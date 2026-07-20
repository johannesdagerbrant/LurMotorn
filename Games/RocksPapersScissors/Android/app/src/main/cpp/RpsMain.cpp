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
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
#include "Lur/Trace/Trace.h"
#include "Lur/Transport/Ble.h"
#include "Rps/CameraScroll.h"
#include "Rps/GameView.h"
#include "Rps/LockstepPeer.h"
#include "Rps/Snapshot.h"
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
    uint32_t LastConsumedTick = 0xFFFFFFFFu;      // glue only
};

void SendViaSession(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* D, std::size_t N) {
    static_cast<Lur::Net::Session*>(Ctx)->Send(Type, D, N);
}

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

// Touch: taps go to the HUD first (production plates + opponent selector — the view
// owns the hit rects, #85); a drag in the play area pans the camera (design §9).
// View-only camera; plate presses -> the lockstep input stream.
int32_t HandleInput(android_app* App, AInputEvent* Event) {
    auto* S = static_cast<AppState*>(App->userData);
    if (S == nullptr || !S->Ready || App->window == nullptr) return 0;
    if (AInputEvent_getType(Event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const float W = static_cast<float>(ANativeWindow_getWidth(App->window));
    const float X = AMotionEvent_getX(Event, 0);
    const float Y = AMotionEvent_getY(Event, 0);
    switch (AMotionEvent_getAction(Event) & AMOTION_EVENT_ACTION_MASK) {
        case AMOTION_EVENT_ACTION_DOWN:
            S->Cam.Begin(Y);
            S->DownX = X; S->DownY = Y;
            return 1;
        case AMOTION_EVENT_ACTION_MOVE:
            S->Cam.Move(Y, Ppu(W));  // content-drag (the CameraScroll handles the feel)
            return 1;
        case AMOTION_EVENT_ACTION_UP: {
            S->Cam.End();
            const bool Tap = (X - S->DownX) * (X - S->DownX) + (Y - S->DownY) * (Y - S->DownY) < (24.0f * 24.0f);
            if (Tap && S->Linked.load(std::memory_order_acquire)) {
                const int Plate = S->View.OnTap(X, Y);            // View: glue-only (safe here)
                if (Plate >= 0) S->Lp.SetLocalMask(static_cast<uint8_t>(1u << Plate));  // atomic -> sim
            }
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
    State.Session.Start(Transport, State.DeviceId);
    LOGI("RPS session started (device id %zuB)", State.DeviceId.size());

    // ---- SIM thread (#91): owns Session + Lp; pumps BLE, ticks the sim, publishes
    // snapshots. Runs the datagram-driven service loop OFF the render/input thread. ----
    std::thread SimThread([&State] {
        auto PrevTime = std::chrono::steady_clock::now();
        uint32_t LastPubTick = 0xFFFFFFFFu;
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

            State.Session.Tick(ElapsedNs);  // drain the BLE EventInbox + handshake/liveness

            if (!State.Started && State.Session.IsReady()) {
                // Team from GUID order (both phones derive it identically; smaller = team 0).
                const uint8_t Team = State.DeviceId < State.Session.GetPeerGuid() ? 0 : 1;
                State.Lp.Init(kMatchSeed, Team, SendViaSession, &State.Session);
                State.Started = true;
                State.LinkedTeam.store(Team, std::memory_order_relaxed);
                State.Linked.store(true, std::memory_order_release);  // glue applies View.SetLinked + flip
                LOGI("linked - lockstep started (team %d, peer %.8s)", Team, State.Session.GetPeerGuid().c_str());
            }
#if LUR_INTERNAL
            if (State.Started && AutoPlay) {
                AutoAccumNs += ElapsedNs;
                if (AutoAccumNs > 200'000'000ull) {
                    AutoAccumNs = 0;
                    State.Lp.SetLocalMask(static_cast<uint8_t>(1u << AutoRng.NextBounded(4)));
                }
            }
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
            }
#if LUR_INTERNAL
            if (State.Started) {
                DiagAccumNs += ElapsedNs;
                if (DiagAccumNs > 2'000'000'000ull) {
                    DiagAccumNs = 0;
                    LOGI("LOCKSTEP tick=%u you=%d foe=%d desync=%d presented=%u", State.Lp.ExecTick(),
                         State.Lp.GetSim().AliveCount(0), State.Lp.GetSim().AliveCount(1),
                         State.Lp.Desynced() ? 1 : 0, State.PresentedFrames.load(std::memory_order_relaxed));
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

            if (!ViewLinkedApplied && State.Linked.load(std::memory_order_acquire)) {
                State.View.SetLinked(true);  // opponent selector green dot (#85)
                ViewLinkedApplied = true;
            }
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
            State.Cam.Update(DtSec, MaxCam, MinCam);  // momentum + clamp

            if (State.LastConsumedTick != 0xFFFFFFFFu) {  // have a snapshot to draw
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
