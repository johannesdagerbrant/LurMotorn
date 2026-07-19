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
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "Lur/Net/Session.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Sim/Random.h"
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

struct AppState {
    Lur::Render::IRenderer* Renderer = nullptr;
    bool Ready = false;
    Rps::GameView View;
    Lur::Net::Session Session;
    Rps::LockstepPeer Lp;
    std::string DeviceId;
    bool Started = false;

    Rps::Snapshot Snap;
    uint32_t LastTick = 0xFFFFFFFFu;
    uint64_t TickLandedNs = 0;
    Rps::CameraScroll Cam;
    float DownX = 0.0f, DownY = 0.0f;
    uint8_t Team = 0;
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
                if (S->Ready) S->View.CreateResources(S->Renderer);
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

// Touch: the bottom strip is the 4 production buttons (tap x -> unit type); a drag in
// the play area above pans the camera (design §9). View-only camera; buttons -> the
// lockstep input stream.
int32_t HandleInput(android_app* App, AInputEvent* Event) {
    auto* S = static_cast<AppState*>(App->userData);
    if (S == nullptr || !S->Ready || App->window == nullptr) return 0;
    if (AInputEvent_getType(Event) != AINPUT_EVENT_TYPE_MOTION) return 0;
    const float W = static_cast<float>(ANativeWindow_getWidth(App->window));
    const float H = static_cast<float>(ANativeWindow_getHeight(App->window));
    const float X = AMotionEvent_getX(Event, 0);
    const float Y = AMotionEvent_getY(Event, 0);
    const float StripTop = H * 0.85f;
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
            if (Tap && Y >= StripTop && S->Started) {
                int Btn = static_cast<int>(X / (W / 4.0f));
                if (Btn < 0) Btn = 0;
                if (Btn > 3) Btn = 3;
                S->Lp.SetLocalMask(static_cast<uint8_t>(1u << Btn));
            }
            return 1;
        }
        default:
            return 0;
    }
}

}  // namespace

void android_main(android_app* App) {
    AppState State;
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

    auto PrevTime = std::chrono::steady_clock::now();
#if LUR_INTERNAL
    Lur::Sim::SplitMix64 Rng(0xA11CE ^ State.DeviceId.size());
    uint64_t AutoAccumNs = 0, DiagAccumNs = 0;
#endif
    while (!App->destroyRequested) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;

        State.Session.Tick(ElapsedNs);  // pump the BLE inbox + handshake/liveness

        if (!State.Started && State.Session.IsReady()) {
            // Team from GUID order (both phones derive it identically; smaller = team 0).
            const uint8_t Team = State.DeviceId < State.Session.GetPeerGuid() ? 0 : 1;
            State.Team = Team;  // drives the per-player view flip (team 1 = top, mirror so its camp is at the bottom)
            State.Lp.Init(kMatchSeed, Team, SendViaSession, &State.Session);
            State.Started = true;
            LOGI("linked - lockstep started (team %d, peer %.8s)", Team, State.Session.GetPeerGuid().c_str());
        }
        if (State.Started) State.Lp.Tick(ElapsedNs);  // produce + send input, execute to the ceiling

#if LUR_INTERNAL
        // Dev build: auto-press random soldiers so the cross-platform match plays itself,
        // and log the lockstep tick/desync every ~2 s so sync is observable from logcat.
        if (State.Started) {
            AutoAccumNs += ElapsedNs;
            if (AutoAccumNs > 700'000'000ull) {
                AutoAccumNs = 0;
                State.Lp.SetLocalMask(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));
            }
            DiagAccumNs += ElapsedNs;
            if (DiagAccumNs > 2'000'000'000ull) {
                DiagAccumNs = 0;
                LOGI("LOCKSTEP tick=%u you=%d foe=%d desync=%d", State.Lp.ExecTick(),
                     State.Lp.GetSim().AliveCount(0), State.Lp.GetSim().AliveCount(1),
                     State.Lp.Desynced() ? 1 : 0);
            }
        }
#endif

        int Events = 0;
        android_poll_source* Source = nullptr;
        while (ALooper_pollOnce(State.Ready ? 0 : -1, nullptr, &Events,
                                reinterpret_cast<void**>(&Source)) >= 0) {
            if (Source != nullptr) Source->process(App, Source);
            if (App->destroyRequested) break;
        }

        if (State.Ready && App->window != nullptr) {
            const float W = static_cast<float>(ANativeWindow_getWidth(App->window));
            const float H = static_cast<float>(ANativeWindow_getHeight(App->window));
            const uint64_t NowStamp = NowNs();
            if (State.Lp.ExecTick() != State.LastTick) { State.LastTick = State.Lp.ExecTick(); State.TickLandedNs = NowStamp; }
            State.Snap.CaptureFrom(State.Lp.GetSim(), State.TickLandedNs, kStepNs);
            const float VisibleH = H / Ppu(W);
            const float MaxCam = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
            State.Cam.Update(static_cast<float>(ElapsedNs) / 1.0e9f, MaxCam);  // momentum + clamp
            State.View.Render(State.Renderer, State.Snap, State.Snap.AlphaAt(NowStamp), State.Cam.Y, W, H,
                              State.Team == 1);
        }
    }

    if (State.Renderer != nullptr) State.Renderer->Shutdown();
}
