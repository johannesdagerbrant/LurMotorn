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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>   // debug.lur.console edge detection (memcpy/strcmp on the prop value)
#include <ctime>     // #156 timestamped flight-recorder filenames
#include <memory>
#include <string>
#include <thread>

#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Core/CVarConfig.h"  // persist + load tuned cvars
#include "Lur/Core/Log.h"         // the engine logger — routed into logcat below
#include "Lur/Input/ConsoleGesture.h"  // #151: the ONE dev-console gesture, shared with iOS/desktop
#include "Lur/Save/DeviceId.h"
#include "Lur/App/GameHost.h"   // #43: engine-owned identity + session lifecycle
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
#include "Lur/Trace/Trace.h"
#include "Lur/Transport/Ble.h"
#include "Rps/CameraScroll.h"
#include "Rps/GameView.h"
#include "Rps/AgentControl.h"  // LUR_AGENT: assistant remote-control command grammar
#include "Rps/AiController.h"
#include "Rps/MatchRecord.h"   // #144 dev-only solo flight recorder
#include "Rps/LockstepPeer.h"
#include "Rps/ScoreBook.h"     // persistent all-time W-L-D per AI tier / per rival
#include "Rps/SessionWiring.h" // the ONE Session->LockstepPeer routing table (#160)
#include "Rps/Snapshot.h"
#include "Rps/SoloInput.h"
#include "Rps/Tunables.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "OnlyRps", __VA_ARGS__)
// Same "OnlyRps" tag, so `logcat -s OnlyRps:*` still catches it — the LEVEL is the point, for the
// handful of lines that mean "what you just asked for did not happen the way you think" (#170).
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "OnlyRps", __VA_ARGS__)

namespace {

// THE ENGINE LOGGER HAD NO SINK ON EITHER PHONE, so every Lur::Log::Info/Error inside the engine and
// the netcode fell back to stdout/stderr — which Android discards for an app. Everything the shim
// logs itself goes through LOGI and was visible, so the hole was invisible: the engine's own voice
// was simply muted on device.
//
// It cost a real diagnosis (2026-07-30). The two phones were running builds from different commits;
// #112's build-fingerprint gate DID fire and called Lur::Log::Error to say so, and nobody ever saw
// the line. The pair then played for 13 minutes and desynced, and the first question — "were the
// builds even the same?" — had no answer in the log. One function pointer fixes it for every
// engine-side message, present and future.
void EngineLogSink(bool Error, const char* Line, void* /*User*/) {
    __android_log_print(Error ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO, "OnlyRps", "%s", Line);
}

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
    // #43: the engine owns identity + the session lifecycle. RPS uses only that half of GameHost —
    // no record sync: ScoreBook is not an ISaveState, is never sent over the wire, and there is no
    // hijack rule to apply here.
    Lur::App::GameHost Host;                     // SIM only (after Start)
    Rps::LockstepPeer Lp;                        // SIM only; glue touches ONLY SetLocalMask (atomic)
    std::string DeviceId;
    std::string CvarsPath;                       // set once at startup (glue), then read-only
    std::string DataDir;                         // app-private dir (#144 match recordings live here)
    bool Started = false;                        // SIM only

    // sim <-> glue hand-off (the whole cross-thread surface):
    Rps::SnapshotMailbox    Mailbox;             // sim publishes, glue consumes
    Rps::Snapshot           Snap;                // glue's working copy (Consume target)
    std::atomic<bool>       SimRunning{true};
    std::atomic<bool>       Linked{false};       // sim -> glue (drives View.SetLinked + flip)
    std::atomic<uint8_t>    LinkedTeam{0};
    std::atomic<uint32_t>   PublishedTick{0};    // sim -> glue: consume only on a new tick
    std::atomic<uint32_t>   PresentedFrames{0};  // glue -> sim: for the LOCKSTEP diag
    // #161: a desync repair is in flight. Published as an atomic because the glue thread must never
    // touch Lp (it owns input + render only), and the player needs to be told on screen — an
    // unexplained hold that then rewinds a second of play reads as a glitch or as cheating.
    std::atomic<bool>       Recovering{false};   // sim -> glue (drives View.SetRecovering)
    std::atomic<bool>       LinkHalfOpen{false}; // sim -> glue (#163: drives View.SetLinkHalfOpen)
#if LUR_AGENT
    // Agent `gesture`: the console recognizer lives on the GLUE thread (it owns the touch stream), so
    // a sim-thread command hands the request across rather than touching it directly.
    std::atomic<bool>       AgentGestureRequest{false};
#endif

    // #185 touch->photon instrumentation. The EVENT time of the newest touch not yet reflected on
    // screen, on the trace clock; 0 = nothing pending. Glue-thread only (input and render are the
    // same thread here), so a plain field is enough.
    //
    // Anchored on AMotionEvent_getEventTime, NOT on when we were handed the event: that is the
    // input system's own stamp for when the sample happened, so it includes the OS dispatch delay
    // we would otherwise be blind to — and #185 exists to find out which side of that line the
    // A14's lag is on. It is CLOCK_MONOTONIC, the same clock Lur::Trace::NowNs reads, so the
    // subtraction is meaningful (do not swap either end for a wall clock).
    uint64_t PendingTouchNs = 0;                  // glue only

    Rps::CameraScroll Cam;                        // glue only
    bool CamInit = false;
    float DownX = 0.0f, DownY = 0.0f;
    // #151: the console gesture — two-finger triple-tap to open, drag-to-scroll while open — is now
    // ONE shared recognizer (Lur::Input::ConsoleGesture), not a per-platform copy. The three copies
    // had drifted in three different directions: this one could open the panel but not scroll it, the
    // desktop could scroll but used a different slop, and iOS could not open it at all.
    Lur::Input::ConsoleGesture DevGesture;         // glue thread only
    uint32_t LastConsumedTick = 0xFFFFFFFFu;      // glue only

    // Solo AI match (#127): a local Sim + AiController, no peer. The glue sets SoloAiTier when
    // an AI row is picked; the sim thread starts the match, ticks it at 10 Hz, and publishes to
    // the same Mailbox. Human is team 0; SoloIn carries the human's place/queue EVENTS (#139/#140,
    // the drag-place UI, replacing the retired press mask). Additive — when no AI row is picked the
    // normal peer path is untouched.
    std::atomic<int>     SoloAiTier{-1};    // glue -> sim: one-shot AI tier pick -> (re)start solo
    // #157 glue -> sim, one-shot: a gameplay CVar was just edited. The MAP (mine rows) is built at
    // Sim::Init, so an edit is invisible until the next match — but while a fresh match is still
    // WAITING FOR THE FIRST CAMP there is no state to lose, so the sim thread re-Inits and the new
    // layout appears immediately. Deliberately pre-match only: mid-match re-Init is not wanted (it
    // would throw the game away) and is not needed for tuning.
    std::atomic<bool>    RebuildPreMatch{false};
    std::atomic<bool>    SoloActive{false}; // sim -> glue: solo match running (tap routing)
    Rps::SoloInputInbox  SoloIn;            // glue -> sim: the human's place/queue events (thread-safe)
    Rps::Sim             SoloSim;           // SIM only (after SoloActive)
    Rps::AiController    SoloAi;            // SIM only
    // #2 session state machine: a real peer linking, switching from the AI match to the peer, and
    // the per-opponent W-L-D shown in the selector (session-scoped; cross-launch persistence = #15-20).
    std::atomic<bool>    PeerLinked{false};   // sim -> glue: a real peer connected (row + blink)
    std::atomic<bool>    SwitchToLinked{false};  // glue -> sim: player picked the linked-opponent row
    std::atomic<bool>    SelectLinkedRow{false}; // sim -> glue: we switched to the peer; name it in the HUD
    // #139/feedback: sim -> glue, the camp the player committed while the opponent hasn't placed
    // theirs. Pre-match it is NOT in the sim (both camps become tick 0's input together), so the
    // view has to draw it from here or the field looks empty and the drop looks lost. Fixed RAW
    // (the sim's units) so no float crosses the thread boundary in a different form than the wire.
    std::atomic<bool>    PendingCamp{false};
    std::atomic<int32_t> PendingCampX{0}, PendingCampY{0};
    std::atomic<int>     AiWins_[Rps::AiTierCount]{}, AiLosses_[Rps::AiTierCount]{},
                         AiDraws_[Rps::AiTierCount]{};  // vs each AI tier
    std::atomic<int>     PeerWins_{0}, PeerLosses_{0}, PeerDraws_{0};  // vs the linked peer
    // PERSISTENT W-L-D, all-time: the atomics above are the thread-safe DISPLAY channel (sim thread
    // writes, glue thread reads); this is the record on disk behind them. Loaded on the glue thread
    // before the sim thread exists (thread creation is the handoff), and from then on read and
    // written ONLY by the sim thread — which is where a result is decided and where the peer's GUID
    // is known. The glue thread never touches it again.
    Rps::ScoreBook       Scores;
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

#if LUR_AGENT
// Apply one assistant remote-control command. SIM THREAD ONLY — several of these reach straight into
// Lp/Sim, which no other thread may touch. See Rps/AgentControl.h for why this exists and what the
// handover rules are; it is absent from every config including Development.
//
// `place` is the one that matters most: it injects an event at EXACT world coordinates, which the touch
// UI cannot do. Drag-to-place magnetically snaps to the nearest VALID square and emits nothing at all
// on an invalid drop, so a human physically cannot produce a placement whose coordinates equal an
// existing camp's — which is precisely the #160 collision. Reproducing that bug on hardware requires
// bypassing the snap, and this is the bypass.
void ApplyAgentCommand(AppState& S, const Rps::AgentCommand& Cmd) {
    const uint8_t Team = S.LinkedTeam.load(std::memory_order_relaxed);
    // #170: name the misroute instead of letting it pass. Input produced while the app is still in its
    // opening AI match goes to the SOLO sim, so the peer sits at stall=1 waiting for a camp that was
    // never sent — and every step of that reports success. Session is sim-thread-owned and this runs on
    // the sim thread, so IsReady() is safe to ask here.
    auto WarnIfSolo = [&S](const char* What) {
        if (!S.SoloActive.load(std::memory_order_acquire)) return;
        if (S.Host.Session().IsReady())
            LOGE("AGENT %s -> the SOLO sim, not the linked peer (a peer IS linked). The other phone "
                 "will wait forever. Send `linked` first, then re-send this.", What);
        else
            LOGI("AGENT %s -> the solo sim (no peer linked yet)", What);
    };
    switch (Cmd.Kind) {
        case Rps::EAgentCmd::Place: {
            const Rps::InputEvent E = Rps::InputEvent::Place(
                Team, static_cast<uint8_t>(Cmd.C & 3), Rps::F(Cmd.A), Rps::F(Cmd.B));
            LOGI("AGENT place type=%d at (%d,%d) team=%u", Cmd.C, Cmd.A, Cmd.B,
                 static_cast<unsigned>(Team));
            WarnIfSolo("place");
            RouteLocalEvent(&S, E);
            break;
        }
        case Rps::EAgentCmd::Queue:
            LOGI("AGENT queue slot=%d count=%d", Cmd.A, Cmd.B);
            WarnIfSolo("queue");
            RouteLocalEvent(&S, Rps::InputEvent::Queue(Team, Cmd.A, Cmd.B));
            break;
        case Rps::EAgentCmd::Stress: {
            // #162's load scenario. Writes the sim directly rather than going through input, because
            // the point is to reach a unit count the economy would take an hour to fund.
            //
            // Whichever sim is live — SOLO is the one to use for a clean PERF measurement, because
            // filling only this peer diverges a linked pair by construction and the #161 recovery then
            // fires in the middle of the numbers. Linked is the right one for the COLLAPSE scenario,
            // where the divergence is part of what is being tested.
            Rps::Sim& Sm = S.SoloActive.load(std::memory_order_acquire)
                               ? S.SoloSim
                               : const_cast<Rps::Sim&>(S.Lp.GetSim());
            LOGI("AGENT stress %d per team, type %d (solo=%d, count %d -> ...)", Cmd.A, Cmd.B,
                 S.SoloActive.load(std::memory_order_relaxed) ? 1 : 0, Sm.Count);
            Sm.StressFill(Cmd.A, static_cast<uint8_t>(Cmd.B));
            LOGI("AGENT stress done, count=%d", Sm.Count);
            break;
        }
        case Rps::EAgentCmd::Corrupt:
            LOGI("AGENT corrupt gold %+d — forcing a divergence (#161)", Cmd.A);
            S.Lp.AgentCorruptState(Cmd.A);
            break;
        case Rps::EAgentCmd::DropTx:
            LOGI("AGENT drop next %d produced frame(s) — simulating #163's half-open link", Cmd.A);
            S.Lp.AgentDropOutgoing(Cmd.A);
            break;
        case Rps::EAgentCmd::Console:
            LOGI("AGENT console %d", Cmd.A);
            S.View.SetDevOverlayOpen(Cmd.A != 0);
            break;
        case Rps::EAgentCmd::Gesture:
            // Drives the SHARED recognizer the way a real touch stream would, so it exercises the
            // #151 wiring (recognizer -> SetDevOverlayOpen) without needing multitouch — which is
            // unavailable here anyway (SELinux blocks evdev injection on this device).
            LOGI("AGENT gesture: synthetic two-finger triple-tap");
            S.AgentGestureRequest.store(true, std::memory_order_release);
            break;
        case Rps::EAgentCmd::KillOwn: {
            // #160 setup: free the ground under our own camp so it can be REBUILT on the same square.
            // That is the only route by which a produced placement can carry coordinates equal to the
            // opening camp's, which is what the old payload-sniffing re-send check mistook for a
            // re-send. Kills via Hp so the sim's own death handling runs.
            Rps::Sim& Sm = const_cast<Rps::Sim&>(S.Lp.GetSim());
            for (int32_t I = 0; I < Sm.Count; ++I) {
                if (!Sm.IsAlive(I) || Sm.Team[I] != Team) continue;
                if (!Sm.IsBuilding(I) || Sm.IsHomeBase(I)) continue;
                if (Sm.Type[I] != static_cast<uint8_t>(Cmd.A & 3)) continue;
                LOGI("AGENT killown slot=%d type=%d at (%d,%d)", I, Sm.Type[I], Sm.PosX[I].ToInt(),
                     Sm.PosY[I].ToInt());
                Sm.Hp[I] = 0;
                break;
            }
            break;
        }
        case Rps::EAgentCmd::Linked:
            // #170: the ONE route into the linked session a harness can rely on. It sets the same flag
            // the selector's "Linked opponent" row sets, and that route is deliberately exempt from the
            // `!HasMinerCamp(0)` gate the AUTO-switch carries — so it still works after a stray `place`
            // has put a camp in the solo sim, which is the state the auto-switch can never leave.
            // The flag is LATCHED (see the switch site): sending this before the link is up is fine,
            // it takes effect on the frame the peer becomes ready.
            LOGI("AGENT linked -> requesting the switch to the linked opponent");
            S.SwitchToLinked.store(true, std::memory_order_release);
            break;
        case Rps::EAgentCmd::None:
            break;
    }
}
#endif

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
    // #157: ask the sim thread to rebuild the map if the match hasn't started. Only a flag —
    // touching SoloSim from the glue thread would race the tick. The sim thread checks pre-match
    // itself, so a mid-match edit is simply ignored here.
    S->RebuildPreMatch.store(true, std::memory_order_release);
}
#endif

void HandleCmd(android_app* App, int32_t Cmd) {
    auto* S = static_cast<AppState*>(App->userData);
    switch (Cmd) {
        case APP_CMD_INIT_WINDOW:
            if (App->window != nullptr) {
                S->Renderer = Lur::Render::VulkanRenderer::Create("OnlyRps");
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

    // #185: how long the OS took to deliver this sample to us. Everything before this point is the
    // digitiser + input dispatch — a floor we do not control — so measuring it separately is what
    // says whether the A14's scroll lag is ours to fix at all.
    const uint64_t EventNs = static_cast<uint64_t>(AMotionEvent_getEventTime(Event));
    LUR_TRACE_LATENCY("input.dispatch", EventNs);
    // Keep the OLDEST unpresented sample, not the newest: during a drag the events arrive faster
    // than we render, and overwriting each time would measure the last one to squeak in before the
    // frame — flattering, and not what the finger sees. The oldest is the sample whose motion the
    // upcoming frame first reveals.
    if (S->PendingTouchNs == 0) S->PendingTouchNs = EventNs;

#if !LUR_SHIPPING
    // #150: while the console is open it OWNS the pointer — a drag ANYWHERE scrolls the cvar list,
    // and a release that barely moved is a DevTap the overlay hit-tests (rows + numpad + the
    // top-left X that closes it). Swallowing the gesture is the whole point: the console sits on
    // top of a LIVE match, so a scroll must not leak through and pan the camera or start a
    // building drag. Same gesture model as DesktopMain.cpp, so one console behaves identically on
    // both — the drag slop is looser here because a finger jitters where a mouse doesn't.
    if (S->View.DevOverlayOpen()) {
        switch (Action) {
            case AMOTION_EVENT_ACTION_DOWN:
                S->DevGesture.DragBegin(Y);
                break;
            case AMOTION_EVENT_ACTION_MOVE:
                // Thread-safe: DevScroll is an atomic the render thread drains.
                S->View.DevScroll(S->DevGesture.DragMove(Y));
                break;
            case AMOTION_EVENT_ACTION_UP:
                if (S->DevGesture.DragEndIsTap()) S->View.DevTap(X, Y);
                break;
            default:
                break;  // extra fingers / cancel: consumed, but no gameplay side effect
        }
        return 1;
    }
#endif

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
    const float GhostOffPx = (static_cast<float>(S->Snap.Cv.BuildingFootprint.Raw) /
                              static_cast<float>(Rps::Fixed::One)) * 0.5f * Ppu(W);
    const float GhX = X - GhostOffPx, GhY = Y - GhostOffPx;

    switch (Action) {
        case AMOTION_EVENT_ACTION_DOWN: {
            S->DownX = X; S->DownY = Y;
            S->DevGesture.PointersDown(1, NowNs());
            const int Plate = Live ? S->View.PlateAt(X, Y) : -1;  // plate hit-test at the real finger
            if (Plate >= 0) {
                S->View.BeginPlaceDrag(Plate, GhX, GhY);  // sets the ghost type; seed at the offset spot
                float Wx = 0.0f, Wy = 0.0f, Gsx = 0.0f, Gsy = 0.0f;
                const bool V = Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy);
                // Finger point AND snapped point: the ghost sticks to the finger, the snap eases in.
                S->View.UpdatePlaceDrag(GhX, GhY, Gsx, Gsy, V);
            } else {
                // #107 revised (feedback 2026-08-04): units queue on PRESS-DOWN, not release, for
                // immediacy — the queue fires the instant you touch the button instead of waiting out
                // the press. A hit on an x1/x5 button enqueues NOW (and lights up); a miss just primes a
                // pan. Cam.Begin runs regardless, so a drag that happens to start on a button still
                // scrolls — harmless, the unit is already queued. (This deliberately drops the old
                // "a press that turns into a pan queues nothing" guard: a button press IS a queue.)
                int32_t Slot = -1;
                const int Cnt = Live ? S->View.OnProductionButton(X, Y, Slot) : 0;
                if (Cnt > 0) {
                    RouteLocalEvent(S, Rps::InputEvent::Queue(MyTeam, Slot, Cnt));
                    S->View.PressProductionButton(X, Y);  // visual flash on the pressed button
                }
                S->Cam.Begin(Y);
            }
            return 1;
        }
        case AMOTION_EVENT_ACTION_POINTER_DOWN:  // a second finger landed
            S->DevGesture.PointersDown(static_cast<int>(Count), NowNs());
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            if (S->View.IsPlacing()) {
                float Wx = 0.0f, Wy = 0.0f, Gsx = 0.0f, Gsy = 0.0f;
                const bool V = Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy);
                S->View.UpdatePlaceDrag(GhX, GhY, Gsx, Gsy, V);
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
                S->DevGesture.Cancel();        // a placement is not a console tap
                return 1;
            }
            S->Cam.End();
#if !LUR_SHIPPING
            // Two-finger TRIPLE-tap opens the CVar view (dev-only; won't fire during normal
            // one-finger play). The in-panel top-left X closes it. #151: the windows and the chaining
            // live in Lur::Input::ConsoleGesture, shared with the iOS and desktop shims.
            const bool WasTwoFinger = S->DevGesture.TwoFingerActive();
            if (S->DevGesture.LiftAndShouldOpen(NowNs())) {
                S->View.SetDevOverlayOpen(true);
                return 1;
            }
            if (WasTwoFinger) return 1;   // a tap in the chain: do not also hit the HUD underneath
#endif
            const bool Tap = (X - S->DownX) * (X - S->DownX) + (Y - S->DownY) * (Y - S->DownY) < (24.0f * 24.0f);
            if (Tap && !S->DevGesture.TwoFingerActive()) {
                // (No DevTap here: an open console returns at the top of HandleInput — #150.)
                // The opponent selector works pre-match (the AI rows start a solo match, #127).
                const int Hit = S->View.OnTap(X, Y);     // -2 selector consumed, 0..3 plate, -1 world
                const int Tier = S->View.TakeAiTier();   // an AI row was picked -> (re)start solo (#127/#2)
                if (Tier >= 0) {
                    S->SoloAiTier.store(Tier, std::memory_order_release);
                } else if (S->View.TakePeerPick()) {     // #2: linked-opponent row -> switch to the peer match
                    S->SwitchToLinked.store(true, std::memory_order_release);
                }
                // (Per-building x1/x5 queue buttons now fire on ACTION_DOWN — see above — not here.)
            }
            S->DevGesture.Cancel();   // the gesture is over either way
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

    // FIRST: give the engine logger a home, before anything can try to report a problem.
    Lur::Log::Init(&EngineLogSink, "OnlyRps");

    const char* DataDir = App->activity != nullptr ? App->activity->internalDataPath : nullptr;
    State.DataDir = DataDir != nullptr ? DataDir : ".";
    // #43: identity + the session lifecycle come from the engine now. RPS takes ONLY that half of
    // GameHost — no EnableRecordSync, because ScoreBook is not an ISaveState (it Loads/Saves itself
    // and never crosses the wire) and there is no per-opponent record to adopt or hijack-guard.
    {
        Lur::App::GameHost::Config HostCfg;
        HostCfg.SaveDir = DataDir != nullptr ? DataDir : ".";
        HostCfg.Transport = Lur::Transport::CreateBleTransport(Lur::Transport::EBleRole::Central);
        HostCfg.Log = [](const char* M) { LOGI("%s", M); };
        State.Host.Init(HostCfg);
    }
    State.DeviceId = State.Host.DeviceId();   // cached: read-only after startup, used on both threads
    // All-time W-L-D, loaded before the sim thread starts. Seeding the display atomics here is what
    // makes the ladder show your real record the moment the dropdown first opens, instead of 0-0-0
    // until the first match of this session finishes.
    State.Scores.Load(State.Host.Store());
    for (int T = 0; T < Rps::AiTierCount; ++T) {
        const Rps::Tally S = State.Scores.Ai(T);
        State.AiWins_[T].store(static_cast<int>(S.Wins), std::memory_order_relaxed);
        State.AiLosses_[T].store(static_cast<int>(S.Losses), std::memory_order_relaxed);
        State.AiDraws_[T].store(static_cast<int>(S.Draws), std::memory_order_relaxed);
    }

    // Route the framed game messages to the lockstep peer. The message SET lives in
    // Rps/SessionWiring.h — one definition shared with the iOS/desktop mains and the test harness,
    // because an unregistered slot is dropped silently at both ends (#147) and four hand-maintained
    // copies is how three of them end up stale.
    Rps::RouteSessionToPeer(State.Host.Session(), State.Lp);

    // A reconnect rebases the lockstep timeline. This is RPS's whole use of the link hooks — and it
    // is why the host's record-sync half is opt-in: chess's resync means "re-adopt the peer and
    // re-send our record", which has no meaning here.
    Lur::App::GameHost::Hooks Hooks;
    Hooks.OnResync = [&State] { State.Lp.BeginResync(); };
    State.Host.Start(std::move(Hooks));

    // Persisted dev-cvar overrides (per-game cvars.cfg). Load into the globals now
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
        // This thread's own handle onto the save directory. A Store is a directory path plus
        // atomic write-then-rename, so a second handle owns nothing the first one does — and it
        // keeps the score file's ONLY writer on the thread that decides results and knows the peer's
        // GUID. (The glue thread's handle is used once, for the device id, before this thread runs.)
        Lur::Save::Store ScoreStore(State.DataDir);
        auto PrevTime = std::chrono::steady_clock::now();
        uint32_t LastPubTick = 0xFFFFFFFFu;
        bool SoloRunning = false;          // #127 solo AI match active (sim thread)
        uint64_t SoloAccumNs = 0;          //   fixed-timestep accumulator for the local sim
        int  SoloTier_ = -1;               // #2 current solo tier (score attribution)
        bool SoloScored = false;           // #2 this solo match's result already tallied
        uint64_t SoloPostNs = 0;           // #149 wall time held on the solo win/lose screen
        bool LinkedScored = false;         // #2 this linked match's result already tallied
        uint32_t LinkedScoredIdx = 0xFFFFFFFFu;  // #149 which Lp match LinkedScored refers to
        bool PeerEverReady = false;        // #2 rising-edge latch for the peer-link notice
        bool PrevPeerReady = false;        // feedback: link-ESTABLISHED edge for the solo->linked auto-switch
#if LUR_INTERNAL
        // #144 flight recorder for solo matches: one file per match under the app's data dir, pulled
        // off the device with `run-as`.
        //
        // #156: LUR_INTERNAL with a console switch, no longer LUR_AGENT. It was assistant-only
        // because it writes files while someone is playing, but that meant capture required a special
        // build — so the interesting match was always the one that wasn't recorded. What replaces
        // absence-from-the-build is a VISIBLE off switch (rps.dev.flight_recorder, a checkbox in the
        // console) plus Shipping still compiling it out entirely. Contrast the setprop console hook
        // below, which stays LUR_AGENT because it is remote control over the player's input.
        //
        // The switch is read ONCE per match, at Begin: toggling mid-match cannot truncate a file
        // half-written, and every other recorder call is a no-op on an unopened file, so skipping
        // Begin is the whole gate.
        Rps::MatchRecorder SoloRec;
        int SoloMatchNo = 0;
        uint64_t RecCensusNs = 0;
        std::string SoloRecFile;   // path of the CURRENT recording, stamped at Begin
        auto SoloRecBegin = [&State, &SoloRec, &SoloMatchNo, &RecCensusNs, &SoloRecFile](int Tier) {
            SoloRecFile.clear();
            if (!Rps::CvFlightRecorder.Get()) return;
            // Timestamped so recordings survive an app restart and sort chronologically — the old
            // per-session counter restarted at 1 every launch and silently overwrote the previous
            // session's files. The ordinal is kept as a suffix purely to stay collision-proof: two
            // matches CAN start inside one second (re-picking a tier restarts instantly).
            const std::time_t Now = std::time(nullptr);
            std::tm Tm{};
            localtime_r(&Now, &Tm);
            char Stamp[24];
            std::strftime(Stamp, sizeof(Stamp), "%Y%m%d-%H%M%S", &Tm);
            SoloRecFile = State.DataDir + "/rps-match-" + Stamp + "-" +
                          std::to_string(++SoloMatchNo) + ".rec";
            SoloRec.Begin(SoloRecFile.c_str(), State.SoloSim, Tier, /*human*/ 0);
            RecCensusNs = 0;
            LOGI("REC started -> %s", SoloRecFile.c_str());
        };
        // ---- LINKED matches are recorded too (#159) ----
        // A linked match wrote nothing, which made the 2026-07-30 desync unreproducible BY
        // CONSTRUCTION: the one match type where two machines can disagree was the one type with no
        // record of what either machine did. Both phones now write their own file for the same match;
        // because both execute the identical combined stream, diffing the two files says whether the
        // wire lost a frame (the `e` lines differ) or the sims computed different results from the
        // same input (the `e` lines agree and the `h` hashes diverge).
        //
        // Fed by LockstepPeer's per-tick sink rather than by polling RecordedEvents(), so the tick
        // numbers come from the sim that stepped them and a resync re-base cannot slide them.
        Rps::MatchRecorder LinkedRec;
        int LinkedMatchNo = 0;
        std::string LinkedRecFile;
        // Which Lp match index the OPEN recording belongs to. Its own variable, not the tally latch:
        // keying the recorder off LinkedScoredIdx opened two files a millisecond apart for every match
        // (the entry-site Begin, then the latch's initial 0xFFFFFFFF != MatchIndex 0 firing again on
        // the first tick), leaving an orphan .rec that ended before it recorded anything. Whoever
        // pulls the files then has to guess which of the two is the match.
        // #159: "no recording open" must be a value MatchIndex can never take. Explicit here, and it
        // has to be: iOS declared the same field as a zero-initialised ivar, where "unset" and "match
        // 0" were the same value, so the iPhone recorded NOTHING until a restart bumped the index —
        // one-sided capture of the first linked match, which is the one a diff most needs (found on
        // hardware 2026-07-31). Named on both sides now so the default can't mean "match 0" again.
        uint32_t LinkedRecIdx = Rps::NoRecMatchIdx;
        struct LinkedRecCtx { Rps::MatchRecorder* Rec; };
        LinkedRecCtx LinkedCtx{&LinkedRec};
        State.Lp.SetTickSink(
            [](void* C, uint32_t Tick, const Rps::InputEvent* Batch, int Count, uint64_t Hash) {
                Rps::MatchRecorder* R = static_cast<LinkedRecCtx*>(C)->Rec;
                R->Events(Tick, Batch, Count);
                // One hash per ANCHOR cadence, not per tick: it is the cadence the netcode already
                // cross-checks on, so the two peers' files land hashes on identical tick numbers and
                // the diff lines up without interpolation.
                if (Tick % 10 == 0) R->Hash(Tick, Hash);
            },
            &LinkedCtx);
        auto LinkedRecBegin = [&State, &LinkedRec, &LinkedMatchNo, &LinkedRecFile, &LinkedRecIdx] {
            // #180: idempotent. The guard moved in here from the call site when the caller became the
            // match-start EDGE rather than a per-frame poll, so opening twice for one match is a
            // programming error rather than the normal flow it used to be.
            if (LinkedRecIdx == State.Lp.MatchIndex()) return;
            LinkedRec.End(State.Lp.GetSim());   // safe if never opened; finalises a previous match
            LinkedRecFile.clear();
            LinkedRecIdx = State.Lp.MatchIndex();
            if (!Rps::CvFlightRecorder.Get()) return;
            const std::time_t Now = std::time(nullptr);
            std::tm Tm{};
            localtime_r(&Now, &Tm);
            char Stamp[24];
            std::strftime(Stamp, sizeof(Stamp), "%Y%m%d-%H%M%S", &Tm);
            // "-vs-" in the name so a linked capture is never confused with a solo one when both are
            // pulled off the device together.
            LinkedRecFile = State.DataDir + "/rps-vs-" + Stamp + "-" +
                            std::to_string(++LinkedMatchNo) + ".rec";
            LinkedRec.Begin(LinkedRecFile.c_str(), State.Lp.GetSim(), /*tier*/ -1,
                            State.LinkedTeam.load(std::memory_order_relaxed));
            LOGI("REC linked -> %s", LinkedRecFile.c_str());
        };
        // #180: open on the match-start EDGE delivered by the netcode, not by watching MatchStarted()
        // from this loop. The poll lost tick 0 whenever the match started during camp DELIVERY rather
        // than during our own Tick — and tick 0 is the tick that carries both camps, so the file then
        // diffed as "EVENTS differ at tick 0 ... look at the transport" on a clean match. The sink
        // fires inside TryStartMatch: merged cvars in place (which is why this is not done at
        // Lp.Init), camps seeded, nothing executed yet.
        using LinkedRecBeginFn = decltype(LinkedRecBegin);
        State.Lp.SetMatchStartSink(
            [](void* C) { (*static_cast<LinkedRecBeginFn*>(C))(); }, &LinkedRecBegin);
#endif
#if LUR_INTERNAL
        // Developer-facing (stays LUR_INTERNAL, so a dev build a human drives still has it):
        // the periodic LOCKSTEP diagnostic line, and the #101 autospam that a HUMAN opts into
        // with debug.lur.autoplay=1 before launch.
        uint64_t DiagAccumNs = 0, AutoAccumNs = 0;
        char AutoV[PROP_VALUE_MAX] = {};
        const bool AutoPlay = (__system_property_get("debug.lur.autoplay", AutoV) > 0 && AutoV[0] == '1');
        if (AutoPlay) LOGI("autoplay ENABLED (debug.lur.autoplay=1): auto-spamming miners+soldiers");
        Lur::Sim::SplitMix64 AutoRng(0x5059ull ^ State.DeviceId.size());
#endif
#if LUR_AGENT
        // ---- Assistant remote control (CLAUDE.md's LUR_AGENT axis) ----
        // Channel: the system property `debug.lur.agent.cmd`, holding "<seq> <verb> [args]". Polled on
        // the SIM thread, which is the only thread allowed to touch Lp/Sim — the same reason the cvar
        // console commits route through a queue. A system property is the Android idiom here (the
        // pre-existing autoplay hook uses one) and costs a cheap read per poll.
        //
        // This is remote control, so it is absent from every config including Development. Two rules
        // when handing a build over: rebuild WITHOUT -DLUR_AGENT so it is not merely idle, and
        // `adb shell setprop debug.lur.agent.cmd ""` — a stale property fighting a player's own input
        // is what cost the 2026-07-25 playtest an evening.
        Rps::AgentControl AgentCtl;
        uint64_t AgentPollNs = 0;
        LOGI("AGENT CONTROL COMPILED IN (LUR_AGENT=1) — this build must not be handed to a player. "
             "Drive it with: adb shell setprop debug.lur.agent.cmd '<seq> <verb> [args]'");
#endif
        while (State.SimRunning.load(std::memory_order_acquire)) {
            const auto Now = std::chrono::steady_clock::now();
            const uint64_t ElapsedNs =
                std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
            PrevTime = Now;
#if LUR_AGENT
            // Poll the agent channel FIRST, and unconditionally. It must be serviced whatever mode the
            // app is in, and the branches below `continue` out of the loop for solo (which owns it),
            // for a decided match, and for the post-match hold — so anywhere further down is
            // unreachable in exactly the states a scenario most often needs to be driven from. (Learned
            // by putting it further down and watching the property land while nothing happened: the app
            // opens into a solo AI match, which takes the first of those continues.)
            AgentPollNs += ElapsedNs;
            if (AgentPollNs >= 100'000'000ull) {
                AgentPollNs = 0;
                char CmdV[PROP_VALUE_MAX] = {};
                Rps::AgentCommand Cmd;
                if (__system_property_get("debug.lur.agent.cmd", CmdV) > 0 && AgentCtl.Poll(CmdV, Cmd))
                    ApplyAgentCommand(State, Cmd);
            }
#endif

#if LUR_INTERNAL
            // The periodic diagnostic + TRACE line. It lives at the TOP of the loop, above the mode
            // branches, because those `continue` out for solo (which owns the loop), for a decided
            // match and for the post-match hold — so at the bottom it was unreachable in exactly the
            // states worth diagnosing.
            //
            // SOLO counts too. This was linked-only, and the TRACE line rides on it — so the one mode
            // with no peer to perturb the numbers, i.e. the mode you actually want for a PERFORMANCE
            // measurement, was the mode that printed nothing. iOS already covered both
            // (`_Started || _SoloActive`); this is the Android side catching up. Found while measuring
            // #162 on the phone: 1602 carts spawned and not a single TRACE line came out.
            const bool SoloDiag = State.SoloActive.load(std::memory_order_acquire);
            if (State.Started || SoloDiag) {
                DiagAccumNs += ElapsedNs;
                if (DiagAccumNs > 2'000'000'000ull) {
                    DiagAccumNs = 0;
                    // #147: hash + gold + frontier are the CONVERGENCE readout. The anchor cross-check
                    // only starts once the match does, so before either camp is placed a divergence
                    // was completely invisible — the two phones simply showed different numbers and
                    // nobody could tell which state was wrong. Pre-match these MUST be identical on
                    // both peers; comparing one line per phone is the whole test, no taps required.
                    const Rps::Sim& DS = SoloDiag ? State.SoloSim : State.Lp.GetSim();
                    // `badbuild` is here because it was the FIRST question asked when the pair
                    // desynced on 2026-07-30 and the log could not answer it. #112 detects a
                    // fingerprint mismatch and sets BuildMismatch(); nothing read it (the gate's own
                    // comment claims the app aborts on it — no app ever did), and its log line went
                    // to an unsinked stderr. One field on the line everyone already reads.
                    // #163: `gaps`/`gapat` and `stall` are here for the same reason `badbuild` is — they
                    // were the questions the log could not answer. A frame lost inside a link that
                    // reports no error is why locating one missing input needed two flight recordings
                    // and a diff; gaps>0 says it on the line everyone already reads, and gapat names
                    // the tick. stall=1 identifies the half-open pre-match hang, which otherwise looks
                    // to the player (and in the log) exactly like a frozen app.
                    // #204: `desync` is the STICKY per-match count of detected anchor mismatches, which
                    // is what a reader means by "did this match diverge" and is the same on both
                    // phones. `dsgate` is the live gate, which the recovery SURVIVOR clears at once —
                    // printing only that made one real divergence read as 1 here and 0 on the peer.
                    LOGI("%s tick=%u you=%d foe=%d desync=%u dsgate=%d badbuild=%d presented=%u "
                         "hash=%08x gold=%d "
                         "frontier=%d started=%d gaps=%d gapat=%u stall=%d halfopen=%d restarts=%d "
                         "rollbk=%d resim=%u spec=%lld",
                         SoloDiag ? "SOLO" : "LOCKSTEP",
                         SoloDiag ? DS.Tick : State.Lp.ExecTick(), DS.AliveCount(0), DS.AliveCount(1),
                         SoloDiag ? 0u : State.Lp.DesyncsSeen(),
                         (!SoloDiag && State.Lp.Desynced()) ? 1 : 0,
                         State.Lp.BuildMismatch() ? 1 : 0,
                         State.PresentedFrames.load(std::memory_order_relaxed),
                         static_cast<uint32_t>(DS.StateHash() & 0xFFFFFFFFu),
                         DS.Teams[SoloDiag ? 0 : State.LinkedTeam.load(std::memory_order_relaxed)].Gold,
                         DS.FrontierT0.ToInt(),
                         SoloDiag ? 1 : (State.Lp.MatchStarted() ? 1 : 0),
                         SoloDiag ? 0 : State.Lp.InputGaps(),
                         SoloDiag ? 0u : State.Lp.LastInputGapTick(),
                         (!SoloDiag && State.Lp.PreMatchStalled()) ? 1 : 0,
                         // #163: the half-open verdict on the line everyone reads — a wedged notify
                         // path (connected, our writes leave, the peer never notifies) now reads as
                         // halfopen=1 instead of a silent freeze.
                         (!SoloDiag && State.Host.Session().IsLinkHalfOpen()) ? 1 : 0,
                         // #182: hard radio restarts fired this half-open episode (capped at
                         // MaxRadioRestarts). On real hardware this is the ONLY proof the escalation ran
                         // — the wedge isn't host-reproducible, so a climbing restarts= is the test.
                         SoloDiag ? 0 : State.Host.Session().RadioRestartsAttempted(),
                         // Rollback (Docs/Journal/2026-08-03): how often a delivered peer frame
                         // contradicted the prediction and forced a rewind+resim, the total ticks
                         // re-simulated, and how far the head is speculating past the confirmed frontier
                         // (the correction-exposure window). The plan's §correction-frequency + resim-cost.
                         SoloDiag ? 0 : State.Lp.Rollbacks(),
                         SoloDiag ? 0u : State.Lp.ResimTicks(),
                         SoloDiag ? 0LL
                                  : static_cast<long long>(static_cast<int64_t>(State.Lp.ExecTick()) -
                                                           State.Lp.ConfirmedTick()));
                    // #159: the linked recording's periodic census. It carries the economy snapshot
                    // AND it is what FLUSHES the file — without it the whole capture sits in the
                    // stdio buffer until End, so a killed app or a match that never resolves leaves
                    // nothing on disk. That is the failure mode this recorder exists to survive.
                    if (!SoloDiag)
                        LinkedRec.Census(State.Lp.GetSim(),
                                         State.LinkedTeam.load(std::memory_order_relaxed), -1, -1);
                    char TraceLine[512];
                    if (Lur::Trace::FormatLineAndReset(TraceLine, sizeof(TraceLine)) > 0) LOGI("TRACE %s", TraceLine);
                }
            }
#endif
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
#if LUR_INTERNAL
                SoloRecBegin(NewTier);
#endif
            }
#if LUR_INTERNAL
            // #157: a dev CVar edit while a fresh solo match is still WAITING FOR THE FIRST CAMP —
            // re-Init so map knobs (rps.mine.row_*) are visible immediately. Pre-match only: nothing
            // has happened yet, so there is nothing to throw away. Same seed, so only the edited
            // knobs move. Re-latches Cv through Init, which is what rebuilds the mine field.
            if (State.RebuildPreMatch.exchange(false, std::memory_order_acq_rel) && SoloRunning &&
                !State.SoloSim.HasMinerCamp(0) && State.SoloSim.Result == Rps::ResultOngoing) {
                State.SoloSim.Init(kMatchSeed);
                State.SoloAi.Init(kMatchSeed, /*AI team*/ 1,
                                  static_cast<Rps::EAiTier>(SoloTier_ < 0 ? 0 : SoloTier_));
                SoloAccumNs = 0;
                State.Mailbox.Back().CaptureFrom(State.SoloSim, NowNs(), kStepNs);
                State.Mailbox.Publish();
                State.PublishedTick.store(State.SoloSim.Tick, std::memory_order_release);
                LastPubTick = State.SoloSim.Tick;
                LOGI("pre-match map rebuilt from edited cvars (mine rows %d/%d)",
                     State.SoloSim.Cv.MineRowHome.ToInt(), State.SoloSim.Cv.MineRowSafe.ToInt());
            }
#endif

            // Pump the session ALWAYS (even during solo) so a real peer can complete the handshake —
            // that's what raises the "opponent link established" notice + the Linked-opponent row. Lp
            // stays uninitialised until we actually enter the peer match, and no MsgInput flows until
            // both peers do, so this only advances the Session-level handshake here.
            State.Host.Tick(ElapsedNs);
            const bool PeerReady = State.Host.Session().IsReady();
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
            // On the link edge the peer's GUID is finally known, so publish the ALL-TIME record
            // against THIS rival into the display atomics. Without it the linked row would read
            // 0-0-0 until the first match of the session ended, even with a long history on disk.
            if (LinkEdge) {
                const Rps::Tally T = State.Scores.Peer(State.Host.Session().GetPeerGuid(), State.DeviceId);
                State.PeerWins_.store(static_cast<int>(T.Wins), std::memory_order_relaxed);
                State.PeerLosses_.store(static_cast<int>(T.Losses), std::memory_order_relaxed);
                State.PeerDraws_.store(static_cast<int>(T.Draws), std::memory_order_relaxed);
            }
            // #170: the manual pick LATCHES until it can be honoured. It used to be exchange()d away
            // every frame regardless, so a pick that arrived while the link was still coming up was
            // silently dropped — invisible to a human (the row only exists once a peer is linked) but
            // the normal case for a harness, which sends `linked` the moment it launches the app. The
            // flag is only cleared when the switch actually happens, or when there is nothing left to
            // switch out of.
            const bool ManualPick = State.SwitchToLinked.load(std::memory_order_acquire);
            const bool AutoSwitch = LinkEdge && !State.SoloSim.HasMinerCamp(0);  // unstarted AI match only
            if (SoloRunning && PeerReady && (AutoSwitch || ManualPick)) {
                SoloRunning = false;
                State.SwitchToLinked.store(false, std::memory_order_release);
                State.SoloActive.store(false, std::memory_order_release);
                State.SelectLinkedRow.store(true, std::memory_order_release);  // glue: name the peer in the HUD
                LOGI("switch solo -> linked (%s)", AutoSwitch ? "auto: link established, AI match not started"
                                                             : "player picked the linked opponent");
            } else if (ManualPick && !SoloRunning) {
                State.SwitchToLinked.store(false, std::memory_order_release);  // already linked: moot
            }

            if (SoloRunning) {
                // MATCH DECIDED — tally once, hold the result on screen, then rebuild. This runs
                // BEFORE the pre-match gate below, and that order is the whole point: losing normally
                // means your camps are gone, so when this sat after a `!HasMinerCamp(0)` gate a wiped
                // player was stuck on "you lose" forever and the loss was never even tallied. A
                // finished match is its own state; it must not be reachable only through having a camp.
                if (State.SoloSim.Result != Rps::ResultOngoing) {
                    if (!SoloScored && SoloTier_ >= 0) {
                        SoloScored = true;
#if LUR_INTERNAL
                        if (SoloRec.IsOpen()) {
                            SoloRec.Census(State.SoloSim, 0, static_cast<int>(State.SoloAi.State()),
                                           static_cast<int>(State.SoloAi.CounterEnemy()));
                            SoloRec.End(State.SoloSim);   // finalise: complete + replayable from here
                            LOGI("REC match %d finished: result=%u tick=%u -> %s", SoloMatchNo,
                                 static_cast<unsigned>(State.SoloSim.Result), State.SoloSim.Tick,
                                 SoloRecFile.c_str());
                        }
#endif
                        // Record to disk FIRST, then publish to the display atomics from the record —
                        // so what the row shows is what was persisted, and a crash right after a
                        // match cannot leave the two disagreeing.
                        State.Scores.RecordAi(SoloTier_, State.SoloSim.Result, /*MyTeam*/ 0);
                        State.Scores.Save(ScoreStore);
                        const Rps::Tally T = State.Scores.Ai(SoloTier_);
                        State.AiWins_[SoloTier_].store(static_cast<int>(T.Wins), std::memory_order_relaxed);
                        State.AiLosses_[SoloTier_].store(static_cast<int>(T.Losses), std::memory_order_relaxed);
                        State.AiDraws_[SoloTier_].store(static_cast<int>(T.Draws), std::memory_order_relaxed);
                    }
                    // #149: the result stands for PostMatchHoldNs, then a FRESH match at the same tier
                    // in the pre-match state (the AI waits for your camp; the camera re-locks itself).
                    SoloPostNs += ElapsedNs;
                    if (SoloPostNs >= Rps::PostMatchHoldNs && SoloTier_ >= 0) {
                        const uint64_t NextSeed = State.SoloSim.Seed + 1;
                        State.SoloSim.Init(NextSeed);
                        State.SoloAi.Init(NextSeed, /*AI team*/ 1, static_cast<Rps::EAiTier>(SoloTier_));
                        SoloScored = false;  SoloPostNs = 0;  SoloAccumNs = 0;
                        LastPubTick = 0xFFFFFFFFu;
                        LOGI("solo: next match begins (tier %d, seed 0x%llx)", SoloTier_,
                             static_cast<unsigned long long>(NextSeed));
#if LUR_INTERNAL
                        SoloRecBegin(SoloTier_);
#endif
                        // Publish the fresh (empty) sim at once, or the view keeps drawing the old
                        // match's final frame — including its result overlay — until the first tick.
                        State.Mailbox.Back().CaptureFrom(State.SoloSim, NowNs(), kStepNs);
                        State.Mailbox.Publish();
                        State.PublishedTick.store(State.SoloSim.Tick, std::memory_order_release);
                        LastPubTick = State.SoloSim.Tick;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;   // the match is over: nothing to tick
                }
                SoloPostNs = 0;
                // PRE-MATCH HOLD, mirroring the linked path (#139/#149). Until you place your opening
                // camp the clock does NOT run — no ticks, no match timer — and your camp and the AI's
                // land in the SAME tick, exactly as two peers apply both camps at tick 0. Before this,
                // the solo sim ticked from 0 while you were still choosing a spot: the AI's economy
                // effectively started on however long you deliberated, and the timer counted time you
                // had not played. Elapsed time is DROPPED here, not banked, so no catch-up burst
                // follows the placement.
                if (!State.SoloSim.HasMinerCamp(0)) {
                    SoloAccumNs = 0;
                    Rps::InputEvent Evs[Rps::MaxEventsPerTick];
                    const int Drained = State.SoloIn.Drain(Evs, Rps::MaxEventsPerTick);
                    // Pre-match only the opening camp counts (the linked path drops the rest too),
                    // and only if the sim would actually accept it — otherwise an invalid drop would
                    // start the AI's clock while the player still has no camp.
                    int Kept = 0;
                    for (int I = 0; I < Drained; ++I)
                        if (Evs[I].Kind == Rps::EventPlaceBuilding && Evs[I].Type == Rps::UnitMiner &&
                            State.SoloSim.CanPlaceBuilding(0, Rps::UnitMiner, Rps::Fixed{Evs[I].X},
                                                           Rps::Fixed{Evs[I].Y}))
                            Evs[Kept++] = Evs[I];
                    if (Kept > 0) {
                        int AiCount = 0;
                        State.SoloAi.DecideEvents(State.SoloSim, State.SoloSim.Tick, Evs + Kept,
                                                  Rps::MaxEventsPerTick - Kept, AiCount);
                        State.SoloSim.StepEvents(Evs, Kept + AiCount);
#if LUR_INTERNAL
                        SoloRec.Events(State.SoloSim.Tick - 1, Evs, Kept + AiCount);
#endif
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;   // held: publish nothing new, tick nothing
                }
                SoloAccumNs += ElapsedNs;
                while (SoloAccumNs >= kStepNs) {   // fixed 10 Hz, decoupled from the service loop
                    SoloAccumNs -= kStepNs;
                    // #139/#140: the human (team 0) place/queue events drained first (team-0-before-
                    // team-1 is the Execute order), then the AI (team 1) fills the remaining budget.
                    Rps::InputEvent Evs[Rps::MaxEventsPerTick];
                    int Count = State.SoloIn.Drain(Evs, Rps::MaxEventsPerTick);
                    {
                        int AiCount = 0;
                        State.SoloAi.DecideEvents(State.SoloSim, State.SoloSim.Tick, Evs + Count,
                                                  Rps::MaxEventsPerTick - Count, AiCount);
                        Count += AiCount;
                    }
                    State.SoloSim.StepEvents(Evs, Count);
#if LUR_INTERNAL
                    // #144 flight recorder: the COMBINED batch, recorded at the tick it was applied
                    // on, so a dev machine can replay this match bit-for-bit (Rps::ReplayMatch).
                    SoloRec.Events(State.SoloSim.Tick - 1, Evs, Count);
#endif
                }
                const uint32_t T = State.SoloSim.Tick;
                if (T != LastPubTick) {
                    LastPubTick = T;
                    State.Mailbox.Back().CaptureFrom(State.SoloSim, NowNs(), kStepNs);
                    State.Mailbox.Publish();
                    State.PublishedTick.store(T, std::memory_order_release);
                }
#if LUR_INTERNAL
                // #144 telemetry: a census every 2s into BOTH the recording and logcat, so the match
                // is readable live (adb logcat -s OnlyRps) and replayable afterwards. The AI's own
                // state + countered type go in it: a recording that shows only what it BUILT can't
                // distinguish "mis-countered" from "production-bound".
                RecCensusNs += ElapsedNs;
                if (RecCensusNs >= 2'000'000'000ull) {
                    RecCensusNs = 0;
                    const int AiSt = static_cast<int>(State.SoloAi.State());
                    const int AiCt = static_cast<int>(State.SoloAi.CounterEnemy());
                    SoloRec.Census(State.SoloSim, /*human*/ 0, AiSt, AiCt);
                    int32_t W[2] = {0, 0}, So[2] = {0, 0}, B[2] = {0, 0};
                    for (int32_t I = 0; I < State.SoloSim.Count; ++I) {
                        if (!State.SoloSim.IsAlive(I)) continue;
                        const int T2 = State.SoloSim.Team[I] & 1;
                        if (State.SoloSim.IsBuilding(I)) { if (!State.SoloSim.IsHomeBase(I)) ++B[T2]; }
                        else if (State.SoloSim.Type[I] == Rps::UnitMiner) ++W[T2];
                        else ++So[T2];
                    }
                    LOGI("REC t=%u you: g=%d w=%d s=%d b=%d | ai: g=%d w=%d s=%d b=%d state=%d ctr=%d",
                         State.SoloSim.Tick, State.SoloSim.Teams[0].Gold, W[0], So[0], B[0],
                         State.SoloSim.Teams[1].Gold, W[1], So[1], B[1], AiSt, AiCt);
                }
#endif
                // (Result handling — tally, hold, rebuild — is at the TOP of this block, above the
                // pre-match gate, so a wiped player still gets it.)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;  // solo owns the loop; skip Lp
            }

            if (!State.Started && PeerReady) {
                // Team from GUID order (both phones derive it identically; smaller = team 0).
                const uint8_t Team = State.DeviceId < State.Host.Session().GetPeerGuid() ? 0 : 1;
                State.Lp.Init(kMatchSeed, Team, SendViaSession, &State.Host.Session());
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
                LOGI("linked - lockstep started (team %d, peer %.8s)", Team, State.Host.Session().GetPeerGuid().c_str());
            }
#if LUR_INTERNAL
            // #137b: the linked auto-soak spammed a random press mask, retired with the mask.
            // Event-based soak (random place/queue) returns with the input UI in #139/#140.
            (void)AutoPlay; (void)AutoRng; (void)AutoAccumNs;
#endif
            if (State.Started) {
                { LUR_TRACE_SCOPE("net.tick"); State.Lp.Tick(ElapsedNs); }  // produce+send input, execute (sim.step nests)
                State.Recovering.store(State.Lp.Recovering(), std::memory_order_relaxed);  // #161 -> HUD
                State.LinkHalfOpen.store(State.Host.Session().IsLinkHalfOpen(),
                                         std::memory_order_relaxed);  // #163 -> HUD
                // #139/feedback: publish the committed-but-not-yet-simulated camp so the view can
                // show it while we wait for the opponent. Clears itself the moment the match starts
                // (the real camp is then in the snapshot, with its production buttons).
                const bool Pending = State.Lp.HasLocalCamp() && !State.Lp.MatchStarted();
                if (Pending) {
                    State.PendingCampX.store(State.Lp.LocalCamp().X, std::memory_order_relaxed);
                    State.PendingCampY.store(State.Lp.LocalCamp().Y, std::memory_order_relaxed);
                }
                State.PendingCamp.store(Pending, std::memory_order_release);
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
                // #149: one Lp now spans many matches, so the tally latch is per MATCH INDEX — a
                // restart re-arms it exactly once, and a re-entered index can never double-count.
                if (LinkedScoredIdx != State.Lp.MatchIndex()) {
                    LinkedScoredIdx = State.Lp.MatchIndex();
                    LinkedScored = false;
                }
                // #2: tally the linked result ONCE (you are LinkedTeam: your-team win = W, else L; draw = D).
                if (!LinkedScored && State.Lp.GetSim().Result != Rps::ResultOngoing) {
                    LinkedScored = true;
                    const uint8_t Me = State.LinkedTeam.load(std::memory_order_relaxed);
                    const uint8_t R = State.Lp.GetSim().Result;
#if LUR_INTERNAL
                    // #159: close the recording HERE, on the result, not at the next Begin — the end
                    // line stamps the result and tick, and by the next Begin the sim has already been
                    // rebuilt for the following match. A desync-declared draw (e6d6abf) lands here
                    // too, so the file that captured a divergence is always complete.
                    if (LinkedRec.IsOpen()) {
                        LinkedRec.Census(State.Lp.GetSim(), Me, /*no AI*/ -1, -1);
                        LinkedRec.End(State.Lp.GetSim());
                        // #204: the MATCH verdict wants the sticky count — the live gate is often
                        // already cleared by the time a match ends, so this line used to report a
                        // match that had genuinely diverged as desync=0.
                        LOGI("REC linked match finished: result=%u tick=%u desync=%u -> %s",
                             static_cast<unsigned>(R), State.Lp.GetSim().Tick,
                             State.Lp.DesyncsSeen(), LinkedRecFile.c_str());
                    }
#endif
                    // Per-RIVAL and persistent, keyed on the peer's device GUID — so the row reads
                    // "your record against THIS person", not "against whoever is in the room". A
                    // malformed/absent GUID is refused by RecordPeer rather than tallied against a
                    // junk opponent, so fall back to publishing the in-memory count in that case.
                    const std::string& PeerGuid = State.Host.Session().GetPeerGuid();
                    if (State.Scores.RecordPeer(PeerGuid, State.DeviceId, R, Me)) {
                        State.Scores.Save(ScoreStore);
                        const Rps::Tally T = State.Scores.Peer(PeerGuid, State.DeviceId);
                        State.PeerWins_.store(static_cast<int>(T.Wins), std::memory_order_relaxed);
                        State.PeerLosses_.store(static_cast<int>(T.Losses), std::memory_order_relaxed);
                        State.PeerDraws_.store(static_cast<int>(T.Draws), std::memory_order_relaxed);
                    } else {
                        LOGI("peer result not persisted (peer guid %zuB) — session tally only",
                             PeerGuid.size());
                        if (R == Rps::ResultDraw) State.PeerDraws_.fetch_add(1);
                        else if ((R == Rps::ResultTeam0Wins && Me == 0) || (R == Rps::ResultTeam1Wins && Me == 1))
                            State.PeerWins_.fetch_add(1);
                        else State.PeerLosses_.fetch_add(1);
                    }
                }
            }
            // ~500 Hz service: datagram-to-Step latency stays ~ms without busy-spinning a core.
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    // ---- GLUE thread: input (ALooper) + render only. Consumes snapshots; never touches
    // Session/Lp/Sim (except Lp.SetLocalMask, atomic). ----
    bool ViewLinkedApplied = false;
    auto FramePrev = std::chrono::steady_clock::now();
#if LUR_AGENT
    int ConsolePropCountdown = 1;         // frames until the next debug.lur.console poll
    char ConsolePropLast[PROP_VALUE_MAX] = {};   // last value seen, so we act on CHANGES only
    bool ConsolePropSeeded = false;              // first read is a baseline, never an action
#endif
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

#if LUR_AGENT
            // ASSISTANT-ONLY hook: `adb shell setprop debug.lur.console 1` opens the CVar console, 0
            // closes it. It exists because the console's real gesture is a two-finger triple-tap,
            // which `adb shell input` cannot inject (nor can we write evdev directly: Samsung's
            // SELinux denies /dev/input writes even to group `input`), so without it the console is
            // unreachable from an automated on-device check.
            //
            // LUR_AGENT, not LUR_INTERNAL: this is REMOTE CONTROL over the player's input, and
            // LUR_INTERNAL is on in Development — the configuration that gets played. Gating it there
            // is how a stale `setprop` ended up fighting a real two-finger tap. Compiled out of any
            // build not configured with -DLUR_AGENT=ON, so there is nothing left to misbehave.
            // EDGE-triggered, not a standing order. Acting on the property's VALUE every poll made it
            // authoritative, so a leftover `debug.lur.console 0` slammed the console shut twice a
            // second and the two-finger triple-tap could never win — the console "closed immediately"
            // on opening. Now only a CHANGE does anything, and the first read is just a baseline, so a
            // stale property from an earlier session is inert.
            if (--ConsolePropCountdown <= 0) {
                ConsolePropCountdown = 30;  // ~twice a second at 60 fps — a poll, not a per-frame cost
                char ConsoleV[PROP_VALUE_MAX] = {};
                __system_property_get("debug.lur.console", ConsoleV);
                if (!ConsolePropSeeded) {
                    std::memcpy(ConsolePropLast, ConsoleV, sizeof(ConsoleV));
                    ConsolePropSeeded = true;
                } else if (std::strcmp(ConsoleV, ConsolePropLast) != 0) {
                    std::memcpy(ConsolePropLast, ConsoleV, sizeof(ConsoleV));
                    State.View.SetDevOverlayOpen(ConsoleV[0] == '1');
                }
            }
#endif
            // #2: the Linked-opponent ROW + "opponent link established" blink appear only when a real
            // PEER connects (not for a solo match). Fire once on the rising edge.
            if (!ViewLinkedApplied && State.PeerLinked.load(std::memory_order_acquire)) {
                // Label the row with the PEER's device id, not a generic word (#178): with two
                // phones on a table, a label that reads the same on both tells you nothing.
                State.View.SetLinked(true, State.Host.Session().GetPeerGuid());
                State.View.NotifyPeerLinked();    // blink the bar
                ViewLinkedApplied = true;
            }
            // Every frame, not just the link edge: a mismatch is discovered when the peer's
            // fingerprint ARRIVES, which can be well after the link comes up, and it clears on a
            // reinstall. The setter early-outs when unchanged, so this costs a bool compare.
            State.View.SetBuildMismatch(State.Lp.BuildMismatch());
            // feedback: the sim switched us to the peer -> point the selector at that row, so the HUD
            // names who we are actually playing instead of still reading the AI tier.
            // (gated on ViewLinkedApplied so the flag is never consumed before the row exists)
            if (ViewLinkedApplied && State.SelectLinkedRow.exchange(false, std::memory_order_acq_rel))
                State.View.SelectLinkedOpponent();
            // #2: push the per-opponent SESSION scores to the selector (no-op when unchanged, so this
            // only rebuilds the list on a real change).
            for (int T = 0; T < Rps::AiTierCount; ++T)
                State.View.SetAiScore(T, State.AiWins_[T].load(std::memory_order_relaxed),
                                      State.AiLosses_[T].load(std::memory_order_relaxed),
                                      State.AiDraws_[T].load(std::memory_order_relaxed));
            State.View.SetPeerScore(State.PeerWins_.load(std::memory_order_relaxed),
                                    State.PeerLosses_.load(std::memory_order_relaxed),
                                    State.PeerDraws_.load(std::memory_order_relaxed));
            // #161: "resyncing with opponent" while a desync repair is in flight.
            State.View.SetRecovering(State.Recovering.load(std::memory_order_relaxed));
            State.View.SetLinkHalfOpen(State.LinkHalfOpen.load(std::memory_order_relaxed));  // #163
#if LUR_AGENT
            // Agent `gesture`: feed the SHARED recognizer a synthetic two-finger triple-tap on the glue
            // thread, which is where it lives. Three taps, each inside the hold window and inside the
            // chain window, exactly as a finger pair would produce — so this proves the recognizer and
            // its wiring to SetDevOverlayOpen, which is the part #151 got wrong on iOS.
            if (State.AgentGestureRequest.exchange(false, std::memory_order_acquire)) {
                const uint64_t T0 = NowNs();
                bool Opened = false;
                for (int Tap = 0; Tap < Rps::AgentGestureTaps; ++Tap) {
                    const uint64_t Down = T0 + static_cast<uint64_t>(Tap) * 200'000'000ull;
                    State.DevGesture.PointersDown(1, Down);
                    State.DevGesture.PointersDown(2, Down);
                    Opened = State.DevGesture.LiftAndShouldOpen(Down + 40'000'000ull);
                }
                LOGI("AGENT gesture -> recognizer says open=%d (console now %d)", Opened ? 1 : 0,
                     State.View.DevOverlayOpen() ? 1 : 0);
                if (Opened) State.View.SetDevOverlayOpen(true);
            }
#endif
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
                // #139/feedback: your camp, committed and waiting on the opponent's (not in the sim
                // yet). Solo never sets it: there the place applies immediately.
                const bool Pend = State.PendingCamp.load(std::memory_order_acquire);
                constexpr float FixedOne = static_cast<float>(Rps::Fixed::One);
                State.View.SetPendingCamp(
                    Pend,
                    static_cast<float>(State.PendingCampX.load(std::memory_order_relaxed)) / FixedOne,
                    static_cast<float>(State.PendingCampY.load(std::memory_order_relaxed)) / FixedOne);
                State.View.Render(State.Renderer, State.Snap, State.Snap.AlphaAt(NowNs()), State.Cam.Y, W, H,
                                  State.LinkedTeam.load(std::memory_order_relaxed) == 1, DtSec);
            }
            // #185: touch sample -> this frame handed to the presentation engine. Bounds our share
            // of scroll lag: it spans OS dispatch + the wait + input->camera + record + submit, and
            // stops at vkQueuePresentKHR returning. It does NOT include the compositor holding the
            // image until the next scan-out, so real photons are up to one more refresh (~16.6 ms
            // here) later — a constant, and the same constant on both phones, so the DIFFERENCE
            // between the two devices is still honest even though the absolute is a lower bound.
            if (State.PendingTouchNs != 0) {
                LUR_TRACE_LATENCY("input.toPresent", State.PendingTouchNs);
                State.PendingTouchNs = 0;
            }
            State.PresentedFrames.store(State.Renderer != nullptr ? State.Renderer->PresentedFrames() : 0u,
                                        std::memory_order_relaxed);
        }
    }

    State.SimRunning.store(false, std::memory_order_release);  // stop + join the sim thread first
    if (SimThread.joinable()) SimThread.join();
    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
