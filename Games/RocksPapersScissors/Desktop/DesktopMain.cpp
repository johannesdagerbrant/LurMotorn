// Desktop entry point for RocksPapersScissors (Phase 1). A thin platform shim — like
// chess's DesktopMain.cpp, copy-pasted on purpose (Phase-4 GameHost is what earns the
// extraction). Two drivers:
//
//   * DEFAULT: two-window loopback LOCKSTEP (#76) — two full peers (Window + GameView +
//     the proven Transport/Session/LockstepPeer trio) in one process, linked by a
//     DEFERRED LoopbackTransport, driven on the main thread. Every net-flow bug is
//     reproducible in a debugger with both peers visible (the workbench point). Each
//     window's keys 1-4 queue that peer's units; drag pans that window's camera.
//
//   * --solo: one window driven by the threaded SimRunner (slice 0, no net) — vs
//     yourself with keys 1-4 (you) / 5-8 (foe).
//
// --auto presses random soldiers for both sides; --frames N runs headless (smoke).
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "Lur/Core/Log.h"
#include "Lur/Net/Session.h"
#include "Lur/Platform/Window.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Loopback.h"
#include "Rps/GameView.h"
#include "Rps/LockstepPeer.h"
#include "Rps/SimRunner.h"
#include "Rps/Snapshot.h"
#include "Rps/Tunables.h"

namespace {

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

constexpr int kWinW = 360;
constexpr int kWinH = 780;
constexpr uint64_t kStepNs = 1'000'000'000ull / Rps::TickRateHz;  // 10 Hz

float Ppu() {
    return static_cast<float>(kWinW) /
           (static_cast<float>(Rps::WorldWidth.Raw) / static_cast<float>(Rps::Fixed::One));
}
float WorldHeightF() {
    return static_cast<float>(Rps::WorldHeight.Raw) / static_cast<float>(Rps::Fixed::One);
}

// ------------------------------------------------------------------ two-window lockstep

// One peer: its own window + renderer + view + the netcode trio. Big (a Sim lives inside
// LockstepPeer) — always heap-allocated.
struct Peer {
    Lur::Platform::Window Win;
    Lur::Render::IRenderer* Renderer = nullptr;
    Rps::GameView View;
    Lur::Transport::LoopbackTransport Transport;
    Lur::Net::Session Session;
    Rps::LockstepPeer Lp;
    std::string Guid;

    Rps::Snapshot Snap;
    uint32_t LastTick = 0xFFFFFFFFu;
    uint64_t TickLandedNs = 0;
    float CameraY = 0.0f;
    bool Dragging = false;
    float PrevTouchY = 0.0f;
};

void SendViaSession(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* D, std::size_t N) {
    static_cast<Lur::Net::Session*>(Ctx)->Send(Type, D, N);
}

bool SetupPeer(Peer& P, const char* Title, int X, const std::string& Guid) {
    if (!P.Win.Create(Title, kWinW, kWinH, X, 60)) return false;
    P.Renderer = Lur::Render::VulkanRenderer::Create();
    if (P.Renderer == nullptr || !P.Renderer->Init(P.Win.NativeHandle())) return false;
    P.View.CreateResources(P.Renderer);
    P.Guid = Guid;
    P.Transport.SetDeferred(true);  // deferred delivery: lockstep replies from a receiver never recurse
    P.Session.SetHandler(Rps::MsgInput,
                         [&P](const uint8_t* D, std::size_t N) { P.Lp.OnMessage(Rps::MsgInput, D, N); });
    P.Session.SetHandler(Rps::MsgAnchor,
                         [&P](const uint8_t* D, std::size_t N) { P.Lp.OnMessage(Rps::MsgAnchor, D, N); });
    P.Session.SetHandler(Rps::MsgResyncChunk,
                         [&P](const uint8_t* D, std::size_t N) { P.Lp.OnMessage(Rps::MsgResyncChunk, D, N); });
    // On a reconnect (blip or cold rejoin), offer our history so the peer that's behind
    // rebuilds and both resume in lockstep (proven by rps_net_tests; fires on the phones
    // over real BLE — the loopback never actually disconnects).
    P.Session.SetResyncHandler([&P] { P.Lp.BeginResync(); });
    return true;
}

void RenderPeer(Peer& P, uint64_t Now) {
    int W = 0, H = 0;
    P.Win.GetSize(&W, &H);
    if (W <= 0 || H <= 0) return;
    // Capture the current sim; stamp the publish time only when a new tick landed, so
    // AlphaAt interpolates Prev->Pos across the 100 ms step.
    if (P.Lp.ExecTick() != P.LastTick) { P.LastTick = P.Lp.ExecTick(); P.TickLandedNs = Now; }
    P.Snap.CaptureFrom(P.Lp.GetSim(), P.TickLandedNs, kStepNs);
    const float VisibleH = static_cast<float>(H) / Ppu();
    const float MaxCam = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
    if (P.CameraY < 0.0f) P.CameraY = 0.0f;
    if (P.CameraY > MaxCam) P.CameraY = MaxCam;
    P.View.Render(P.Renderer, P.Snap, P.Snap.AlphaAt(Now), P.CameraY, static_cast<float>(W),
                  static_cast<float>(H));
}

void HandlePeerInput(Peer& P, Lur::Sim::SplitMix64& Rng, bool Auto, uint64_t ElapsedNs,
                     uint64_t& AutoAccumNs) {
    for (uint32_t Vk : P.Win.TakeKeys())
        if (Vk >= 0x31 && Vk <= 0x34) P.Lp.SetLocalMask(static_cast<uint8_t>(1u << (Vk - 0x31)));
    for (const Lur::Input::TouchEvent& T : P.Win.TakeTouches()) {
        if (T.Phase == Lur::Input::ETouchPhase::Began) { P.Dragging = true; P.PrevTouchY = T.YPx; }
        else if (T.Phase == Lur::Input::ETouchPhase::Moved && P.Dragging) {
            P.CameraY -= (T.YPx - P.PrevTouchY) / Ppu();
            P.PrevTouchY = T.YPx;
        } else if (T.Phase == Lur::Input::ETouchPhase::Ended ||
                   T.Phase == Lur::Input::ETouchPhase::Cancelled) {
            P.Dragging = false;
        }
    }
#if LUR_INTERNAL
    if (Auto) {
        AutoAccumNs += ElapsedNs;
        if (AutoAccumNs > 700'000'000ull) {
            AutoAccumNs = 0;
            P.Lp.SetLocalMask(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));  // random soldier
        }
    }
#else
    (void)Auto; (void)ElapsedNs; (void)AutoAccumNs;
#endif
}

int RunLoopback(bool Auto, int MaxFrames, uint64_t Seed) {
    Lur::Log::Info("RPS desktop: two-window loopback lockstep%s", Auto ? " (auto)" : "");
    auto A = std::make_unique<Peer>();
    auto B = std::make_unique<Peer>();
    if (!SetupPeer(*A, "RocksPapersScissors - Peer A", 160, "rps-peer-a") ||
        !SetupPeer(*B, "RocksPapersScissors - Peer B", 160 + kWinW + 20, "rps-peer-b")) {
        Lur::Log::Error("peer setup failed");
        return 1;
    }
    Lur::Transport::LoopbackTransport::Link(A->Transport, B->Transport);
    A->Session.Start(&A->Transport, A->Guid);
    B->Session.Start(&B->Transport, B->Guid);

    bool Started = false;
    Lur::Sim::SplitMix64 Rng(Seed ^ 0xA11CE);
    uint64_t AutoA = 0, AutoB = 0;
    uint64_t PrevNs = NowNs();
    int Frame = 0;

    while (A->Win.PumpEvents() && B->Win.PumpEvents()) {
        const uint64_t Now = NowNs();
        const uint64_t ElapsedNs = Now - PrevNs;
        PrevNs = Now;

        A->Session.Tick(ElapsedNs);  // pump transports (deliver queued datagrams) + handshake
        B->Session.Tick(ElapsedNs);

        if (!Started && A->Session.IsReady() && B->Session.IsReady()) {
            // Each peer derives its team from the two GUIDs identically (smaller = team 0).
            const uint8_t ATeam = A->Guid < A->Session.GetPeerGuid() ? 0 : 1;
            const uint8_t BTeam = B->Guid < B->Session.GetPeerGuid() ? 0 : 1;
            A->Lp.Init(Seed, ATeam, SendViaSession, &A->Session);
            B->Lp.Init(Seed, BTeam, SendViaSession, &B->Session);
            Started = true;
            Lur::Log::Info("linked - lockstep started (A=team%d B=team%d)", ATeam, BTeam);
        }

        if (Started) {
            HandlePeerInput(*A, Rng, Auto, ElapsedNs, AutoA);
            HandlePeerInput(*B, Rng, Auto, ElapsedNs, AutoB);
            A->Lp.Tick(ElapsedNs);  // produce + send input, execute up to the ceiling
            B->Lp.Tick(ElapsedNs);
            if (A->Lp.Desynced() || B->Lp.Desynced())
                Lur::Log::Error("DESYNC detected (tick A=%u B=%u)", A->Lp.ExecTick(), B->Lp.ExecTick());
        }

        RenderPeer(*A, Now);
        RenderPeer(*B, Now);

        if (MaxFrames > 0 && ++Frame >= MaxFrames) {
            Lur::Log::Info("rendered %d frames headless (started=%d ticks A=%u B=%u desync=%d) - exiting",
                           Frame, Started ? 1 : 0, A->Lp.ExecTick(), B->Lp.ExecTick(),
                           (A->Lp.Desynced() || B->Lp.Desynced()) ? 1 : 0);
            break;
        }
    }

    A->Renderer->Shutdown();
    B->Renderer->Shutdown();
    Lur::Log::Info("clean exit");
    return 0;
}

// ------------------------------------------------------------------ single-window solo (slice 0)

struct SoloInputs {
    std::atomic<uint8_t> P0{0};
    std::atomic<uint8_t> P1{0};
};
void SampleSolo(void* Ctx, uint32_t, uint8_t& M0, uint8_t& M1) {
    SoloInputs* In = static_cast<SoloInputs*>(Ctx);
    M0 = In->P0.exchange(0, std::memory_order_relaxed);
    M1 = In->P1.exchange(0, std::memory_order_relaxed);
}

int RunSolo(bool Auto, int MaxFrames, uint64_t Seed, int Stress) {
    Lur::Log::Info("RPS desktop: solo (SimRunner, no net)%s", Auto ? " (auto)" : "");
    Lur::Platform::Window Win;
    if (!Win.Create("RocksPapersScissors - solo", kWinW, kWinH, 200, 60)) return 1;
    Lur::Render::IRenderer* Renderer = Lur::Render::VulkanRenderer::Create();
    if (Renderer == nullptr || !Renderer->Init(Win.NativeHandle())) return 1;
    Rps::GameView View;
    View.CreateResources(Renderer);

    SoloInputs In;
    auto Runner = std::make_unique<Rps::SimRunner>();
    Runner->Start(Seed, SampleSolo, &In, static_cast<uint32_t>(Stress < 0 ? 0 : Stress));

    float CameraY = 0.0f;
    bool Dragging = false;
    float PrevTouchY = 0.0f;
    Lur::Sim::SplitMix64 Rng(Seed ^ 0xA11CE);
    uint64_t AutoAccumNs = 0, PrevNs = NowNs();
    static Rps::Snapshot Snap;
    int Frame = 0;

    while (Win.PumpEvents()) {
        const uint64_t Now = NowNs();
        const uint64_t ElapsedNs = Now - PrevNs;
        PrevNs = Now;
        for (uint32_t Vk : Win.TakeKeys()) {
            if (Vk >= 0x31 && Vk <= 0x34) In.P0.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x31)));
            else if (Vk >= 0x35 && Vk <= 0x38) In.P1.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x35)));
        }
        for (const Lur::Input::TouchEvent& T : Win.TakeTouches()) {
            if (T.Phase == Lur::Input::ETouchPhase::Began) { Dragging = true; PrevTouchY = T.YPx; }
            else if (T.Phase == Lur::Input::ETouchPhase::Moved && Dragging) {
                CameraY -= (T.YPx - PrevTouchY) / Ppu();
                PrevTouchY = T.YPx;
            } else if (T.Phase == Lur::Input::ETouchPhase::Ended ||
                       T.Phase == Lur::Input::ETouchPhase::Cancelled) {
                Dragging = false;
            }
        }
#if LUR_INTERNAL
        if (Auto) {
            AutoAccumNs += ElapsedNs;
            if (AutoAccumNs > 700'000'000ull) {
                AutoAccumNs = 0;
                In.P0.fetch_or(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));
                In.P1.fetch_or(static_cast<uint8_t>(1u << (1 + Rng.NextBounded(3))));
            }
        }
#else
        (void)Auto;
#endif
        if (Runner->LatestSnapshot(Snap)) {
            int W = 0, H = 0;
            Win.GetSize(&W, &H);
            if (W > 0 && H > 0) {
                const float VisibleH = static_cast<float>(H) / Ppu();
                const float MaxCam = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
                if (CameraY < 0.0f) CameraY = 0.0f;
                if (CameraY > MaxCam) CameraY = MaxCam;
                View.Render(Renderer, Snap, Snap.AlphaAt(Now), CameraY, static_cast<float>(W),
                            static_cast<float>(H));
            }
        }
        if (MaxFrames > 0 && ++Frame >= MaxFrames) {
            Lur::Log::Info("rendered %d frames headless (tick %u) - exiting", Frame,
                           Runner->PublishedTick());
            break;
        }
    }
    Runner->Stop();
    Renderer->Shutdown();
    Lur::Log::Info("clean exit");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Lur::Log::Init(nullptr, "RpsDesktop");

    int MaxFrames = 0;
    bool Auto = false, Solo = false;
    uint64_t Seed = 0x1234;
    int Stress = 0;
    for (int I = 1; I < argc; ++I) {
        std::string A = argv[I];
        if (A == "--frames" && I + 1 < argc) MaxFrames = std::atoi(argv[++I]);
        else if (A == "--auto") Auto = true;
        else if (A == "--solo") Solo = true;
        else if (A == "--seed" && I + 1 < argc) Seed = std::strtoull(argv[++I], nullptr, 0);
        else if (A == "--stress" && I + 1 < argc) Stress = std::atoi(argv[++I]);
    }

    if (Solo) return RunSolo(Auto, MaxFrames, Seed, Stress);
    return RunLoopback(Auto, MaxFrames, Seed);
}
