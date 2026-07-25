// Android entry point for RocksPapersScissors (Phase 2, slice 2). A thin platform shim
// — like chess's AndroidMain.cpp — that owns the NativeActivity loop, creates the Vulkan
// renderer + BLE transport, and drives ONE lockstep peer (Rps::LockstepPeer) + the shared
// Rps::GameView. Unlike the desktop's two-window loopback, a phone IS a single peer: it
// exchanges per-tick input with the OTHER phone over real BLE (which is reliable/ordered,
// so the same lockstep the host tests prove runs unchanged here).
//
// Copy-pasted platform glue (BleShim / AndroidBleTransport / AndroidVulkanSurface) is
// intentional — the extraction into engine platform modules is #42, earned once this
// second consumer exists.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <sys/system_properties.h>  // debug.lur.autoplay — dev-only stress autospam (#101)
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Core/CVarConfig.h"  // #115: persist + load tuned cvars
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
#include "Lur/Trace/Trace.h"
#include "Lur/Transport/Ble.h"
#include "Rps/CameraScroll.h"
#include "Rps/GameView.h"
#include "Rps/AiController.h"
#include "Rps/LockstepPeer.h"
#include "Rps/Snapshot.h"
#include "Rps/SoloInput.h"
#include "Rps/Tunables.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyRps", __VA_ARGS__)

namespace {

// Both phones derive the SAME match seed so their sims match. The seed is currently
// gameplay-inert (v1 map is fixed + mirrored, no RNG in the tick — spec §2); a
// GUID-derived seed for map variation is the design's later refinement.
constexpr uint64_t kMatchSeed = 0x52505353ull;  // 'RPSS'
constexpr uint64_t kStepNs = 1'000'000'000ull / Rps::TickRateHz;

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

float Ppu(float WidthPx) {
    return WidthPx / (static_cast<float>(Rps::WorldWidth.Raw) / static_cast<float>(Rps::Fixed::One));
}
float WorldHeightF() {
    return static_cast<float>(Rps::WorldHeight.Raw) / static_cast<float>(Rps::Fixed::One);
}
// View-side world (float) -> Fixed for a place event (#139). The raw int travels into the sim /
// over the wire, so no float crosses the determinism boundary — both peers apply the same Fixed.
Rps::Fixed WorldToFixed(float W) {
    if (W < 0.0f) W = 0.0f;
    return Rps::Fixed{static_cast<int32_t>(W * static_cast<float>(Rps::Fixed::One) + 0.5f)};
}

// Threading (#91): a dedicated SIM thread owns Session + Lp (pumps BLE, ticks the
// lockstep sim, publishes snapshots); the GLUE thread (android_main) does only input +
// render. The ONLY state crossing the boundary is the Mailbox (snapshot: sim->glue),
// Lp.SetLocalMask (atomic: input->sim), and the atomics below. Renderer/View/Cam are
// glue-only; Session/Lp/Sim are sim-only after Start. This gets the 10 Hz sim + BLE off
// the input/render thread so a datagram is serviced in ~ms, not up to a rendered frame.
struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;  // glue only (lifecycle in HandleCmd)
    bool Ready = false;                          // glue only
    Rps::GameView View;                          // glue only
    Lur::Net::Session Session;                   // SIM only (after Start)
    Rps::LockstepPeer Lp;                        // SIM only; glue touches ONLY SetLocalMask (atomic)
    std::string DeviceId;
    std::string CvarsPath;                       // set once at startup (glue), then read-only
    bool Started = false;                        // SIM only

    // sim <-> glue hand-off (the whole cross-thread surface):
    Rps::SnapshotMailbox    Mailbox;             // sim publishes, glue consumes
    Rps::Snapshot           Snap;                // glue's working copy (Consume target)
    std::atomic<bool>       SimRunning{true};
    std::atomic<bool>       Linked{false};       // sim -> glue (drives View.SetLinked + flip)
    std::atomic<uint8_t>    LinkedTeam{0};
    std::atomic<uint32_t>   PublishedTick{0};    // sim -> glue: consume only on a new tick
    std::atomic<uint32_t>   PresentedFrames{0};  // glue -> sim: for the LOCKSTEP diag

    Rps::CameraScroll Cam;                        // glue only
    bool CamInit = false;
    float DownX = 0.0f, DownY = 0.0f;
    uint64_t TwoDownNs = 0;                        // two-finger triple-tap recognizer (glue)
    uint64_t LastTwoTapNs = 0;
    int TwoTapCount = 0;
    bool TwoFingerActive = false;
    uint32_t LastConsumedTick = 0xFFFFFFFFu;      // glue only

    // Solo AI match (#127): a local Sim + AiController, no peer. The glue sets SoloAiTier when
    // an AI row is picked; the sim thread starts the match, ticks it at 10 Hz, and publishes to
    // the same Mailbox. Human is team 0; SoloIn carries the human's place/queue EVENTS (#139/#140,
    // the drag-place UI, replacing the retired press mask). Additive — when no AI row is picked the
    // normal peer path is untouched.
    std::atomic<int>     SoloAiTier{-1};    // glue -> sim: one-shot AI tier pick -> (re)start solo
    std::atomic<bool>    SoloActive{false}; // sim -> glue: solo match running (tap routing)
    Rps::SoloInputInbox  SoloIn;            // glue -> sim: the human's place/queue events (thread-safe)
    Rps::Sim             SoloSim;           // SIM only (after SoloActive)
    Rps::AiController    SoloAi;            // SIM only
    // #2 session state machine: a real peer linking, switching from the AI match to the peer, and
    // the per-opponent W-L-D shown in the selector (session-scoped; cross-launch persistence = #15-20).
    std::atomic<bool>    PeerLinked{false};   // sim -> glue: a real peer connected (row + blink)
    std::atomic<bool>    SwitchToLinked{false};  // glue -> sim: player picked the linked-opponent row
    std::atomic<bool>    SelectLinkedRow{false}; // sim -> glue: we switched to the peer; name it in the HUD
    std::atomic<int>     AiWins_[3]{}, AiLosses_[3]{}, AiDraws_[3]{};  // vs each AI tier
    std::atomic<int>     PeerWins_{0}, PeerLosses_{0}, PeerDraws_{0};  // vs the linked peer
};

void SendViaSession(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* D, std::size_t N) {
    static_cast<Lur::Net::Session*>(Ctx)->Send(Type, D, N);
}

// #139/#140: route a local place/queue event (produced on the GLUE thread by the drag-place UI)
// to whichever sim is live. Solo -> the thread-safe SoloIn inbox (drained by the sim thread's
// solo tick); linked -> LockstepPeer::QueueLocalEvent (its own glue->sim inbox, #91). Both are
// safe off the sim thread. SoloActive implies Linked, so it's checked first.
void RouteLocalEvent(AppState* S, const Rps::InputEvent& E) {
    if (S->SoloActive.load(std::memory_order_acquire)) S->SoloIn.Push(E);
    else if (S->Linked.load(std::memory_order_acquire)) S->Lp.QueueLocalEvent(E);
}

#if LUR_INTERNAL
// A dev CVar edit committed via the numpad/keyboard (GLUE thread). GameView already set the
// global (browser display); route it to the SIM thread (Lp queue -> MsgCvar, synced to the
// peer at a stamped tick), timestamp it, and persist. QueueGameplayCvar is the one Lp method
// (besides SetLocalMask) safe to call off the sim thread.
void OnCvarCommit(void* Ctx, Lur::Core::ICVar& Cv) {
    auto* S = static_cast<AppState*>(Ctx);
    const uint64_t Ms = NowNs() / 1000000ull;
    Cv.SetEditWallMs(Ms);
    const int Id = Rps::GameplayIdForName(Cv.Name());
    if (Id >= 0) S->Lp.QueueGameplayCvar(static_cast<uint8_t>(Id), Cv.RawValue(), Ms);
    Lur::Core::SaveCVarConfig(S->CvarsPath.c_str());
    Rps::DeriveUnitStats(Rps::LatchCvs(), S->Snap.Units);  // reflect the edit in the pre-match HUD
}
#endif

void HandleCmd(android_app* App, int32_t Cmd) {
    auto* S = static_cast<AppState*>(App->userData);
    switch (Cmd) {
        case APP_CMD_INIT_WINDOW:
            if (App->window != nullptr) {
                S->Renderer = Lur::Render::VulkanRenderer::Create();
                S->Ready = S->Renderer && S->Renderer->Init(App->window);
                LOGI("Renderer init: %s", S->Ready ? "ok" : "failed");
                if (S->Ready) {
                    S->View.CreateResources(S->Renderer);
                    // OS safe areas (#85 feedback): status bar above the HUD, nav bar
                    // below the plates. dp values scaled by the device density — the
                    // proper WindowInsets seam is the phase-2 window-metrics item.
                    const float Dpx = static_cast<float>(AConfiguration_getDensity(App->config)) / 160.0f;
                    S->View.SetInsets(28.0f * Dpx, 56.0f * Dpx);
                }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            if (S->Renderer != nullptr) S->Renderer->Shutdown();
            S->Ready = false;
            break;
        default:
            break;
    }
}

// Touch (#139/#140, mirror of the desktop HandlePeerInput): once a match is live (solo vs the AI,
// or linked vs a peer) a pointer-down on a build plate starts a drag-to-place — the ghost follows
// the finger, blinking red where the drop is invalid, and a valid release emits a Place event; any
// other drag pans the camera (design §9); a tap on a building's x1/x5 button queues units. Taps
// still reach the HUD first (opponent selector + dev console). View/camera are glue-only; unit
// input crosses to the sim via RouteLocalEvent (solo inbox / Lp inbox).
int32_t HandleInput(android_app* App, AInputEvent* Event) {
    auto* S = static_cast<AppState*>(App->userData);
    if (S == nullptr || !S->Ready || App->window == nullptr) return 0;
    if (AInputEvent_getType(Event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const float W = static_cast<float>(ANativeWindow_getWidth(App->window));
    const float H = static_cast<float>(ANativeWindow_getHeight(App->window));
    const float X = AMotionEvent_getX(Event, 0);
    const float Y = AMotionEvent_getY(Event, 0);
    const int32_t Action = AMotionEvent_getAction(Event) & AMOTION_EVENT_ACTION_MASK;
    const size_t Count = AMotionEvent_getPointerCount(Event);

    // A match is live once SoloActive (vs AI) or Linked (vs a peer). You play LinkedTeam (0 in
    // solo). Ghost validity is the render-thread WouldAcceptPlace over the last snapshot (the
    // mirror of the sim's predicate), so the red/valid blink can't disagree with the sim.
    const bool Live = S->Linked.load(std::memory_order_acquire);
    const uint8_t MyTeam = S->LinkedTeam.load(std::memory_order_relaxed);
    // #148 magnetic drag-to-place: the (thumb-offset) desired point snaps to the nearest valid spot
    // within ~the icon size. ResolvePlacement returns the snapped world drop (Wx,Wy) + where to draw
    // the ghost (Gsx,Gsy — snapped when valid, else the offset point for the red blink).
    auto Resolve = [&](float DesX, float DesY, float& Wx, float& Wy, float& Gsx, float& Gsy) -> bool {
        return S->View.ResolvePlacement(DesX, DesY, S->Cam.Y, W, H, MyTeam == 1, S->Snap, MyTeam,
                                        Wx, Wy, Gsx, Gsy);
    };
    // #1: lift the dragged ghost UP-LEFT of the finger by ~its footprint size so the thumb doesn't
    // hide it (the desired point handed to Resolve; snapping refines from there).
    const float GhostOffPx = (static_cast<float>(S->Snap.BuildingFootprint.Raw) /
                              static_cast<float>(Rps::Fixed::One)) * 0.5f * Ppu(W);
    const float GhX = X - GhostOffPx, GhY = Y - GhostOffPx;

    switch (Action) {
        case AMOTION_EVENT_ACTION_DOWN: {
            S->DownX = X; S->DownY = Y;
            S->TwoFingerActive = false;
            const int Plate = Live ? S->View.PlateAt(X, Y) : -1;  // plate hit-test at the real finger
            if (Plate >= 0) {
                S->View.BeginPlaceDrag(Plate, GhX, GhY);  // sets the ghost type; seed at the offset spot
                float Wx = 0.0f, Wy = 0.0f, Gsx = 0.0f, Gsy = 0.0f;
                const bool V = Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy);
                S->View.UpdatePlaceDrag(Gsx, Gsy, V);  // hop the ghost to the snapped spot
            } else {
                S->Cam.Begin(Y);
            }
            return 1;
        }
        case AMOTION_EVENT_ACTION_POINTER_DOWN:  // a second finger landed
            if (Count == 2) { S->TwoFingerActive = true; S->TwoDownNs = NowNs(); }
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            if (S->View.IsPlacing()) {
                float Wx = 0.0f, Wy = 0.0f, Gsx = 0.0f, Gsy = 0.0f;
                const bool V = Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy);
                S->View.UpdatePlaceDrag(Gsx, Gsy, V);
            } else if (Count == 1) {
                S->Cam.Move(Y, Ppu(W));  // one finger = scroll; 2+ = a gesture, no scroll
            }
            return 1;
        case AMOTION_EVENT_ACTION_POINTER_UP:
            return 1;  // the whole gesture is decided at ACTION_UP (last finger up)
        case AMOTION_EVENT_ACTION_UP: {
            // A placement in progress commits (valid drop -> Place event) or slides back.
            if (S->View.IsPlacing()) {
                bool Placed = false;
                float Wx = 0.0f, Wy = 0.0f, Gsx = 0.0f, Gsy = 0.0f;
                if (Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy)) {
                    RouteLocalEvent(S, Rps::InputEvent::Place(MyTeam,
                                        static_cast<uint8_t>(S->View.PlacingType()),
                                        WorldToFixed(Wx), WorldToFixed(Wy)));
                    Placed = true;
                }
                S->View.EndPlaceDrag(Placed);  // valid -> the real building takes over; else slide back
                S->TwoFingerActive = false;
                return 1;
            }
            S->Cam.End();
#if !LUR_SHIPPING
            // Two-finger TRIPLE-tap opens the CVar view (dev-only; won't fire during normal
            // one-finger play). The in-panel top-left X closes it. Each quick two-finger tap
            // within the window increments the count; the 3rd opens.
            if (S->TwoFingerActive && (NowNs() - S->TwoDownNs) < 350'000'000ull) {
                S->TwoTapCount = (NowNs() - S->LastTwoTapNs < 600'000'000ull) ? S->TwoTapCount + 1 : 1;
                S->LastTwoTapNs = NowNs();
                if (S->TwoTapCount >= 3) {
                    S->View.SetDevOverlayOpen(true);
                    S->TwoTapCount = 0;
                }
                S->TwoFingerActive = false;
                return 1;
            }
#endif
            const bool Tap = (X - S->DownX) * (X - S->DownX) + (Y - S->DownY) * (Y - S->DownY) < (24.0f * 24.0f);
            if (Tap && !S->TwoFingerActive) {
#if !LUR_SHIPPING
                S->View.DevTap(X, Y);  // dev CVar-browser tap (no-op when hidden / off a row)
#endif
                // The opponent selector works pre-match (the AI rows start a solo match, #127).
                const int Hit = S->View.OnTap(X, Y);     // -2 selector consumed, 0..3 plate, -1 world
                const int Tier = S->View.TakeAiTier();   // an AI row was picked -> (re)start solo (#127/#2)
                if (Tier >= 0) {
                    S->SoloAiTier.store(Tier, std::memory_order_release);
                } else if (S->View.TakePeerPick()) {     // #2: linked-opponent row -> switch to the peer match
                    S->SwitchToLinked.store(true, std::memory_order_release);
                } else if (Live && Hit == -1) {
                    // Not the HUD/selector -> maybe a per-building x1/x5 queue button (#140).
                    int32_t Slot = -1;
                    const int Cnt = S->View.OnProductionButton(X, Y, Slot);
                    if (Cnt > 0) RouteLocalEvent(S, Rps::InputEvent::Queue(MyTeam, Slot, Cnt));
                }
            }
            S->TwoFingerActive = false;
            return 1;
        }
        default:
            return 0;
    }
}

}  // namespace

void android_main(android_app* App) {
    // Heap-allocate AppState (#94): Sim-in-Lp + Snapshot + GameView instances stack to
    // ~hundreds of KB on the ~1 MB glue thread — fine today, one cap-raise from a stack
    // overflow. Heap-owned + a budget assert keeps that growth visible instead of silent.
    static_assert(sizeof(AppState) < 4u * 1024u * 1024u, "AppState exceeds its size budget");
    auto StateOwned = std::make_unique<AppState>();
    AppState& State = *StateOwned;
    LOGI("AppState heap-allocated: %zu bytes", sizeof(AppState));
    App->userData = &State;
    App->onAppCmd = HandleCmd;
    App->onInputEvent = HandleInput;

    const char* DataDir = App->activity != nullptr ? App->activity->internalDataPath : nullptr;
    Lur::Save::Store Store(DataDir != nullptr ? DataDir : ".");
    State.DeviceId = Lur::Save::LoadOrCreateDeviceId(Store);

    auto* Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Central);
    State.Session.SetLogger([](const char* M) { LOGI("Net: %s", M); });

    // Route the framed game messages to the lockstep peer; a reconnect triggers resync.
    State.Session.SetHandler(Rps::MsgInput,
                             [&State](const uint8_t* D, std::size_t N) { State.Lp.OnMessage(Rps::MsgInput, D, N); });
    State.Session.SetHandler(Rps::MsgAnchor,
                             [&State](const uint8_t* D, std::size_t N) { State.Lp.OnMessage(Rps::MsgAnchor, D, N); });
    State.Session.SetHandler(Rps::MsgResyncChunk,
                             [&State](const uint8_t* D, std::size_t N) { State.Lp.OnMessage(Rps::MsgResyncChunk, D, N); });
    State.Session.SetResyncHandler([&State] { State.Lp.BeginResync(); });
#if LUR_INTERNAL
    // Dev-only gameplay-CVar sync + build-fingerprint (#112) over the framed slots.
    State.Session.SetHandler(Rps::MsgCvar,
                             [&State](const uint8_t* D, std::size_t N) { State.Lp.OnMessage(Rps::MsgCvar, D, N); });
    State.Session.SetHandler(Rps::MsgCvarSync,
                             [&State](const uint8_t* D, std::size_t N) { State.Lp.OnMessage(Rps::MsgCvarSync, D, N); });
    State.Session.SetHandler(Rps::MsgFingerprint,
                             [&State](const uint8_t* D, std::size_t N) { State.Lp.OnMessage(Rps::MsgFingerprint, D, N); });
#endif
    State.Session.Start(Transport, State.DeviceId);
    LOGI("RPS session started (device id %zuB)", State.DeviceId.size());

    // #115: persisted dev-cvar overrides (per-game cvars.cfg). Load into the globals now
    // (before the sim thread's match-start Init latches them); route commits back to disk +
    // the peer sync via the GameView hook. Dev-only.
    State.CvarsPath = std::string(DataDir != nullptr ? DataDir : ".") + "/rps-cvars.cfg";
#if LUR_INTERNAL
    if (const int N = Lur::Core::LoadCVarConfig(State.CvarsPath.c_str()); N > 0)
        LOGI("loaded %d persisted cvar override(s) from %s", N, State.CvarsPath.c_str());
    // The persisted overrides are now in the globals; arm the read guard and seed the
    // pre-match HUD stats from them (before any match Init latches), so the plates show the
    // loaded/tuned costs instead of the compile-time defaults. A live match overwrites
    // Snap.Units from the synced sim each tick; OnCvarCommit refreshes this pre-match copy.
    Lur::Core::CVarEnterMain();
    Rps::DeriveUnitStats(Rps::LatchCvs(), State.Snap.Units);
    State.View.SetCvCommitHook(&OnCvarCommit, &State);
#endif

    // #2: open straight into a match vs the Easy AI (the one-shot the sim thread consumes on its
    // first iteration). The player can pick another tier — or the linked opponent — any time.
    State.SoloAiTier.store(static_cast<int>(Rps::EAiTier::Easy), std::memory_order_release);

    // ---- SIM thread (#91): owns Session + Lp; pumps BLE, ticks the sim, publishes
    // snapshots. Runs the datagram-driven service loop OFF the render/input thread. ----
    std::thread SimThread([&State] {
        auto PrevTime = std::chrono::steady_clock::now();
        uint32_t LastPubTick = 0xFFFFFFFFu;
        bool SoloRunning = false;          // #127 solo AI match active (sim thread)
        uint64_t SoloAccumNs = 0;          //   fixed-timestep accumulator for the local sim
        int  SoloTier_ = -1;               // #2 current solo tier (score attribution)
        bool SoloScored = false;           // #2 this solo match's result already tallied
        bool LinkedScored = false;         // #2 this linked match's result already tallied
        bool PeerEverReady = false;        // #2 rising-edge latch for the peer-link notice
        bool PrevPeerReady = false;        // feedback: link-ESTABLISHED edge for the solo->linked auto-switch
#if LUR_INTERNAL
        uint64_t DiagAccumNs = 0, AutoAccumNs = 0;
        // Dev-only autospam (#101): debug.lur.autoplay=1 (set before launch) floods our own
        // team with random plates incl. miners, so a PC-vs-phone match with BOTH ends armed
        // maxes units. Off by default — phones are for human playtesting.
        char AutoV[PROP_VALUE_MAX] = {};
        const bool AutoPlay = (__system_property_get("debug.lur.autoplay", AutoV) > 0 && AutoV[0] == '1');
        if (AutoPlay) LOGI("autoplay ENABLED (debug.lur.autoplay=1): auto-spamming miners+soldiers");
        Lur::Sim::SplitMix64 AutoRng(0x5059ull ^ State.DeviceId.size());
#endif
        while (State.SimRunning.load(std::memory_order_acquire)) {
            const auto Now = std::chrono::steady_clock::now();
            const uint64_t ElapsedNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
            PrevTime = Now;

            // ---- Solo AI match (#127/#2): a local Sim + AiController, no peer. Picking ANY AI tier
            // (re)starts a fresh match AT ONCE — even mid-match — via the one-shot SoloAiTier the
            // glue sets on each selector pick. App-open stores Easy, so a match is live immediately. ----
            const int NewTier = State.SoloAiTier.exchange(-1, std::memory_order_acq_rel);
            if (NewTier >= 0) {
                State.SoloSim.Init(kMatchSeed);
                State.SoloAi.Init(kMatchSeed, /*AI team*/ 1, static_cast<Rps::EAiTier>(NewTier));
                SoloRunning = true;  SoloTier_ = NewTier;  SoloScored = false;
                SoloAccumNs = 0;  LastPubTick = 0xFFFFFFFFu;
                State.Started = false;                                  // drop any linked match — solo takes over
                State.SoloActive.store(true, std::memory_order_release);
                State.LinkedTeam.store(0, std::memory_order_relaxed);   // you are team 0 (no view flip)
                State.Linked.store(true, std::memory_order_release);
                State.Mailbox.Back().CaptureFrom(State.SoloSim, NowNs(), kStepNs);  // publish tick 0 now
                State.Mailbox.Publish();
                State.PublishedTick.store(State.SoloSim.Tick, std::memory_order_release);
                LOGI("solo AI match (re)started (tier %d)", NewTier);
            }

            // Pump the session ALWAYS (even during solo) so a real peer can complete the handshake —
            // that's what raises the "opponent link established" notice + the Linked-opponent row. Lp
            // stays uninitialised until we actually enter the peer match, and no MsgInput flows until
            // both peers do, so this only advances the Session-level handshake here.
            State.Session.Tick(ElapsedNs);
            const bool PeerReady = State.Session.IsReady();
            if (PeerReady && !PeerEverReady) {
                PeerEverReady = true;
                State.PeerLinked.store(true, std::memory_order_release);  // glue: View.SetLinked + blink
            }
            // AUTO-switch solo -> linked, on the EDGE where the link establishes and ONLY out of an
            // AI match the player has not started (no mine camp dragged in yet). Two freshly opened
            // phones therefore pair and front-load the match with zero taps, and because both peers
            // switch on the same Session-ready edge they enter lockstep within ~a frame of each other
            // — the proven direct-link timing (a manual tap drifted the two Lp.Init calls SECONDS
            // apart, so the peer that switched first ran lockstep alone and desynced at tick 10, #147).
            //
            // Never auto-switch out of a STARTED AI match (that would destroy a game in progress),
            // and never out of a linked match, started or not: SoloRunning is false there, and a
            // re-link after a blip must resync rather than re-Init. That leaves the selector as the
            // deliberate way across — honoured below from ANY state, including a started AI match.
            const bool LinkEdge = PeerReady && !PrevPeerReady;
            PrevPeerReady = PeerReady;
            const bool ManualPick = State.SwitchToLinked.exchange(false, std::memory_order_acq_rel);
            const bool AutoSwitch = LinkEdge && !State.SoloSim.HasMinerCamp(0);  // unstarted AI match only
            if (SoloRunning && PeerReady && (AutoSwitch || ManualPick)) {
                SoloRunning = false;
                State.SoloActive.store(false, std::memory_order_release);
                State.SelectLinkedRow.store(true, std::memory_order_release);  // glue: name the peer in the HUD
                LOGI("switch solo -> linked (%s)", AutoSwitch ? "auto: link established, AI match not started"
                                                             : "player picked the linked opponent");
            }

            if (SoloRunning) {
                SoloAccumNs += ElapsedNs;
                while (SoloAccumNs >= kStepNs) {   // fixed 10 Hz, decoupled from the service loop
                    SoloAccumNs -= kStepNs;
                    // #139/#140: the human (team 0) place/queue events drained first (team-0-before-
                    // team-1 is the Execute order), then the AI (team 1) fills the remaining budget.
                    Rps::InputEvent Evs[Rps::MaxEventsPerTick];
                    int Count = State.SoloIn.Drain(Evs, Rps::MaxEventsPerTick);
                    // The AI holds until the human commits its first mining camp (feedback) — no
                    // building a lead while the player is still placing their opening camp.
                    if (State.SoloSim.HasMinerCamp(0)) {
                        int AiCount = 0;
                        State.SoloAi.DecideEvents(State.SoloSim, State.SoloSim.Tick, Evs + Count,
                                                  Rps::MaxEventsPerTick - Count, AiCount);
                        Count += AiCount;
                    }
                    State.SoloSim.StepEvents(Evs, Count);
                }
                const uint32_t T = State.SoloSim.Tick;
                if (T != LastPubTick) {
                    LastPubTick = T;
                    State.Mailbox.Back().CaptureFrom(State.SoloSim, NowNs(), kStepNs);
                    State.Mailbox.Publish();
                    State.PublishedTick.store(T, std::memory_order_release);
                }
                // #2: tally the result ONCE (you are team 0: team-0 win = W, team-1 win = L, draw = D).
                if (!SoloScored && State.SoloSim.Result != Rps::ResultOngoing && SoloTier_ >= 0) {
                    SoloScored = true;
                    if (State.SoloSim.Result == Rps::ResultTeam0Wins) State.AiWins_[SoloTier_].fetch_add(1);
                    else if (State.SoloSim.Result == Rps::ResultTeam1Wins) State.AiLosses_[SoloTier_].fetch_add(1);
                    else State.AiDraws_[SoloTier_].fetch_add(1);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;  // solo owns the loop; skip Lp
            }

            if (!State.Started && PeerReady) {
                // Team from GUID order (both phones derive it identically; smaller = team 0).
                const uint8_t Team = State.DeviceId < State.Session.GetPeerGuid() ? 0 : 1;
                State.Lp.Init(kMatchSeed, Team, SendViaSession, &State.Session);
#if LUR_INTERNAL
                // #112: exchange the build fingerprint (refuse mismatched builds) and our
                // persisted gameplay-CVar overrides, so a designer's tuning reaches the peer
                // and both converge before play. (Two-phone convergence is verified on
                // hardware; single-device this is a no-op beyond the seed.)
                State.Lp.SendFingerprint();
                Lur::Core::CVarRegistry::ForEach([&State](Lur::Core::ICVar* C) {
                    if (C->AffectsGameplay() && C->Overridden()) {
                        const int Id = Rps::GameplayIdForName(C->Name());
                        if (Id >= 0)
                            State.Lp.SeedGameplayCvar(static_cast<uint8_t>(Id), C->RawValue(),
                                                      C->EditWallMs());
                    }
                });
                State.Lp.SendCvarSync();
#endif
                // #148: reconcile with the peer on ENTERING the match, not only on a reconnect
                // edge. A freshly launched app never takes that edge (it connects before the Hello
                // handshake completes, so Session sees no not-connected -> connected transition
                // while Ready), so it never offered its frontier — and the peer that kept running
                // sat in Awaiting forever waiting for it. For a genuinely fresh pair this is a
                // cheap no-op exchange: both send an empty history and marker F=0, and each sees
                // 0 > 0 fail and resumes. Done AFTER Init so Init can't wipe it.
                State.Lp.BeginResync();
                State.Started = true;
                LinkedScored = false;  // #2 tally this match's result once
                State.LinkedTeam.store(Team, std::memory_order_relaxed);
                State.Linked.store(true, std::memory_order_release);  // glue applies the view flip
                LOGI("linked - lockstep started (team %d, peer %.8s)", Team, State.Session.GetPeerGuid().c_str());
            }
#if LUR_INTERNAL
            // #137b: the linked auto-soak spammed a random press mask, retired with the mask.
            // Event-based soak (random place/queue) returns with the input UI in #139/#140.
            (void)AutoPlay; (void)AutoRng; (void)AutoAccumNs;
#endif
            if (State.Started) {
                { LUR_TRACE_SCOPE("net.tick"); State.Lp.Tick(ElapsedNs); }  // produce+send input, execute (sim.step nests)
                // Publish a snapshot only when a NEW tick landed (per-tick, 10 Hz — not the
                // old per-frame ~90 KB capture on the render thread).
                const uint32_t T = State.Lp.ExecTick();
                if (T != LastPubTick) {
                    LastPubTick = T;
                    { LUR_TRACE_SCOPE("snap.capture");
                      State.Mailbox.Back().CaptureFrom(State.Lp.GetSim(), NowNs(), kStepNs); }
                    State.Mailbox.Publish();
                    State.PublishedTick.store(T, std::memory_order_release);
                }
                // #2: tally the linked result ONCE (you are LinkedTeam: your-team win = W, else L; draw = D).
                if (!LinkedScored && State.Lp.GetSim().Result != Rps::ResultOngoing) {
                    LinkedScored = true;
                    const uint8_t Me = State.LinkedTeam.load(std::memory_order_relaxed);
                    const uint8_t R = State.Lp.GetSim().Result;
                    if (R == Rps::ResultDraw) State.PeerDraws_.fetch_add(1);
                    else if ((R == Rps::ResultTeam0Wins && Me == 0) || (R == Rps::ResultTeam1Wins && Me == 1))
                        State.PeerWins_.fetch_add(1);
                    else State.PeerLosses_.fetch_add(1);
                }
            }
#if LUR_INTERNAL
            if (State.Started) {
                DiagAccumNs += ElapsedNs;
                if (DiagAccumNs > 2'000'000'000ull) {
                    DiagAccumNs = 0;
                    // #147: hash + gold + frontier are the CONVERGENCE readout. The anchor cross-check
                    // only starts once the match does, so before either camp is placed a divergence
                    // was completely invisible — the two phones simply showed different numbers and
                    // nobody could tell which state was wrong. Pre-match these MUST be identical on
                    // both peers; comparing one line per phone is the whole test, no taps required.
                    const Rps::Sim& DS = State.Lp.GetSim();
                    LOGI("LOCKSTEP tick=%u you=%d foe=%d desync=%d presented=%u hash=%08x gold=%d "
                         "frontier=%d started=%d",
                         State.Lp.ExecTick(), DS.AliveCount(0), DS.AliveCount(1),
                         State.Lp.Desynced() ? 1 : 0, State.PresentedFrames.load(std::memory_order_relaxed),
                         static_cast<uint32_t>(DS.StateHash() & 0xFFFFFFFFu),
                         DS.Teams[State.LinkedTeam.load(std::memory_order_relaxed)].Gold,
                         DS.FrontierT0.ToInt(), State.Lp.MatchStarted() ? 1 : 0);
                    char TraceLine[512];
                    if (Lur::Trace::FormatLineAndReset(TraceLine, sizeof(TraceLine)) > 0) LOGI("TRACE %s", TraceLine);
                }
            }
#endif
            // ~500 Hz service: datagram-to-Step latency stays ~ms without busy-spinning a core.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // ---- GLUE thread: input (ALooper) + render only. Consumes snapshots; never touches
    // Session/Lp/Sim (except Lp.SetLocalMask, atomic). ----
    bool ViewLinkedApplied = false;
    auto FramePrev = std::chrono::steady_clock::now();
    while (!App->destroyRequested) {
        // WAIT-EARLY, SAMPLE-LATE: spend the GPU fence-wait idle up front, BEFORE polling
        // input, so the touch we render is the freshest possible for this present (cuts
        // ~1 frame of scroll/touch latency — the wait no longer sits between input & draw).
        if (State.Ready && State.Renderer != nullptr) {
            LUR_TRACE_SCOPE("gpu.wait");
            State.Renderer->WaitForFrame();
        }

        int Events = 0;
        android_poll_source* Source = nullptr;
        while (ALooper_pollOnce(State.Ready ? 0 : -1, nullptr, &Events,
                                reinterpret_cast<void**>(&Source)) >= 0) {
            if (Source != nullptr) Source->process(App, Source);
            if (App->destroyRequested) break;
        }

        if (State.Ready && App->window != nullptr) {
            LUR_TRACE_SCOPE("frame.render");
            const auto Now = std::chrono::steady_clock::now();
            const float DtSec =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Now - FramePrev).count() / 1.0e9f;
            FramePrev = Now;
            const float W = static_cast<float>(ANativeWindow_getWidth(App->window));
            const float H = static_cast<float>(ANativeWindow_getHeight(App->window));

            // #2: the Linked-opponent ROW + "opponent link established" blink appear only when a real
            // PEER connects (not for a solo match). Fire once on the rising edge.
            if (!ViewLinkedApplied && State.PeerLinked.load(std::memory_order_acquire)) {
                State.View.SetLinked(true);       // adds the Linked-opponent row (green dot)
                State.View.NotifyPeerLinked();    // blink the bar
                ViewLinkedApplied = true;
            }
            // feedback: the sim switched us to the peer -> point the selector at that row, so the HUD
            // names who we are actually playing instead of still reading the AI tier.
            // (gated on ViewLinkedApplied so the flag is never consumed before the row exists)
            if (ViewLinkedApplied && State.SelectLinkedRow.exchange(false, std::memory_order_acq_rel))
                State.View.SelectLinkedOpponent();
            // #2: push the per-opponent SESSION scores to the selector (no-op when unchanged, so this
            // only rebuilds the list on a real change).
            for (int T = 0; T < 3; ++T)
                State.View.SetAiScore(T, State.AiWins_[T].load(std::memory_order_relaxed),
                                      State.AiLosses_[T].load(std::memory_order_relaxed),
                                      State.AiDraws_[T].load(std::memory_order_relaxed));
            State.View.SetPeerScore(State.PeerWins_.load(std::memory_order_relaxed),
                                    State.PeerLosses_.load(std::memory_order_relaxed),
                                    State.PeerDraws_.load(std::memory_order_relaxed));
            // Copy the latest published tick out only when it CHANGED — between ticks we
            // re-render the held snapshot with a fresh alpha (deletes the per-frame copy).
            const uint32_t Pub = State.PublishedTick.load(std::memory_order_acquire);
            if (Pub != State.LastConsumedTick && State.Mailbox.Consume(State.Snap))
                State.LastConsumedTick = Pub;

            const float VisibleH = H / Ppu(W);
            const float FieldMax = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
            const float MaxCam = FieldMax + State.View.TopHudWorldUnits(W);
            const float MinCam = -State.View.BottomHudWorldUnits(W);
            if (!State.CamInit) { State.Cam.Y = MinCam; State.CamInit = true; }
            // Camera LOCKED at the baseline until the local team places its first mining camp; a
            // fresh match (no camp yet) therefore snaps the view back to the bottom. Free scroll after.
            if (!State.Snap.HasMinerCamp(State.LinkedTeam.load(std::memory_order_relaxed)))
                State.Cam.Y = MinCam;
            else
                State.Cam.Update(DtSec, MaxCam, MinCam);  // momentum + clamp

            // Always render — before the first published snapshot, State.Snap is the
            // default (empty) sim, which draws the field + HUD (the menu). Gating on a
            // published tick left the pre-link screen black (a #91 regression).
            {
                LUR_TRACE_SCOPE("render.view");
                State.View.Render(State.Renderer, State.Snap, State.Snap.AlphaAt(NowNs()), State.Cam.Y, W, H,
                                  State.LinkedTeam.load(std::memory_order_relaxed) == 1, DtSec);
            }
            State.PresentedFrames.store(State.Renderer != nullptr ? State.Renderer->PresentedFrames() : 0u,
                                        std::memory_order_relaxed);
        }
    }

    State.SimRunning.store(false, std::memory_order_release);  // stop + join the sim thread first
    if (SimThread.joinable()) SimThread.join();
    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
