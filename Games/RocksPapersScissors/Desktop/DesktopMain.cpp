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
// --flockdemo: --solo StressFill scene for tuning the flock (#97) — combat ON by default
//   (the clash is part of the feel). Add --nocombat for pure-motion tuning. Pair with --auto.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "Lur/Core/CVarConfig.h"  // #115: persist tuned cvars across runs
#include "Lur/Core/Log.h"
#include "Lur/Net/Session.h"
#include "Lur/Platform/Window.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Loopback.h"
#include "Rps/CameraScroll.h"
#include "Rps/GameView.h"
#include "Rps/LockstepPeer.h"
#include "Rps/SimRunner.h"
#include "Rps/Snapshot.h"
#include "Rps/Tunables.h"
#include "WindowsBleTransport.h"  // #101-E: PC becomes a real BLE opponent to the phone

namespace {

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

int kWinW = 360;   // --winw/--winh: fit small dev screens (portrait phone-ish default)
int kWinH = 780;
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
    Rps::CameraScroll Cam;
    bool CamInit = false;   // first frame parks the camera at MinCam (camp visible)
    uint8_t Team = 0;
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

void RenderPeer(Peer& P, uint64_t Now, float DtSec) {
    int W = 0, H = 0;
    P.Win.GetSize(&W, &H);
    if (W <= 0 || H <= 0) return;
    // Capture the current sim; stamp the publish time only when a new tick landed, so
    // AlphaAt interpolates Prev->Pos across the 100 ms step.
    if (P.Lp.ExecTick() != P.LastTick) { P.LastTick = P.Lp.ExecTick(); P.TickLandedNs = Now; }
    P.Snap.CaptureFrom(P.Lp.GetSim(), P.TickLandedNs, kStepNs);
    const float VisibleH = static_cast<float>(H) / Ppu();
    const float FieldMax = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
    const float MaxCam = FieldMax + P.View.TopHudWorldUnits(static_cast<float>(W));
    const float MinCam = -P.View.BottomHudWorldUnits(static_cast<float>(W));
    if (!P.CamInit) { P.Cam.Y = MinCam; P.CamInit = true; }  // camp clear of the plates on launch
    P.Cam.Update(DtSec, MaxCam, MinCam);
    P.View.Render(P.Renderer, P.Snap, P.Snap.AlphaAt(Now), P.Cam.Y, static_cast<float>(W),
                  static_cast<float>(H), P.Team == 1, DtSec);
}

void HandlePeerInput(Peer& P, Lur::Sim::SplitMix64& Rng, bool Auto, uint64_t ElapsedNs,
                     uint64_t& AutoAccumNs) {
    for (uint32_t Vk : P.Win.TakeKeys())
        if (Vk >= 0x31 && Vk <= 0x34) P.Lp.SetLocalMask(static_cast<uint8_t>(1u << (Vk - 0x31)));
    for (const Lur::Input::TouchEvent& T : P.Win.TakeTouches()) {
        if (T.Phase == Lur::Input::ETouchPhase::Began) P.Cam.Begin(T.YPx);
        else if (T.Phase == Lur::Input::ETouchPhase::Moved) P.Cam.Move(T.YPx, Ppu());
        else if (T.Phase == Lur::Input::ETouchPhase::Ended ||
                 T.Phase == Lur::Input::ETouchPhase::Cancelled) {
            P.Cam.End();
            if (T.Phase == Lur::Input::ETouchPhase::Ended) {
                // HUD first (chess's pattern): plates press units, the selector
                // consumes its own taps; only world taps fall through.
                const int Plate = P.View.OnTap(T.XPx, T.YPx);
                if (Plate >= 0) P.Lp.SetLocalMask(static_cast<uint8_t>(1u << Plate));
            }
        }
    }
#if LUR_INTERNAL
    if (Auto) {
        AutoAccumNs += ElapsedNs;
        if (AutoAccumNs > 200'000'000ull) {
            AutoAccumNs = 0;
            P.Lp.SetLocalMask(static_cast<uint8_t>(1u << Rng.NextBounded(4)));  // random soldier
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
            A->Team = ATeam;
            B->Team = BTeam;
            A->Lp.Init(Seed, ATeam, SendViaSession, &A->Session);
            B->Lp.Init(Seed, BTeam, SendViaSession, &B->Session);
            A->View.SetLinked(true);
            B->View.SetLinked(true);
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

        const float DtSec = static_cast<float>(ElapsedNs) / 1.0e9f;
        RenderPeer(*A, Now, DtSec);
        RenderPeer(*B, Now, DtSec);

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

int RunSolo(bool Auto, int MaxFrames, uint64_t Seed, int Stress, bool FlockDemo, bool NoCombat,
            bool FoeOnly, bool Tune) {
    // --flockdemo (#97): a solo StressFill scene for visual tuning of the flock. Combat is
    // ON by default (playtest: how the counters clash is part of the feel) — pass --nocombat
    // for pure-motion tuning (mixed blobs that never kill each other). Defaults to a healthy
    // unit count if --stress wasn't set.
    if (FlockDemo && Stress <= 0) Stress = 200;
    Lur::Log::Info("RPS desktop: solo (SimRunner, no net)%s%s%s", Auto ? " (auto)" : "",
                   FlockDemo ? " (flockdemo)" : "", NoCombat ? " (combat off)" : "");
    Lur::Platform::Window Win;
    // #115 --tune: a DOUBLE-WIDE window — game in the left half, CVar editor in the right.
    const int WinW = Tune ? kWinW * 2 : kWinW;
    if (!Win.Create("RocksPapersScissors - solo", WinW, kWinH, 200, 60)) return 1;
    Lur::Render::IRenderer* Renderer = Lur::Render::VulkanRenderer::Create();
    if (Renderer == nullptr || !Renderer->Init(Win.NativeHandle())) return 1;
    Rps::GameView View;
    View.CreateResources(Renderer);
#if !LUR_SHIPPING
    // Persist tuned cvars across runs (solo has no peer, so LiveCvLatch applies edits live
    // and a whole-file save on each commit keeps them). Load BEFORE the SimRunner starts so
    // Sim::Init latches the persisted values.
    static const char* kCvarsPath = "rps-cvars.cfg";
    if (const int Loaded = Lur::Core::LoadCVarConfig(kCvarsPath); Loaded > 0)
        Lur::Log::Info("loaded %d persisted cvar override(s) from %s", Loaded, kCvarsPath);
    View.SetCvCommitHook([](void*, Lur::Core::ICVar&) { Lur::Core::SaveCVarConfig(kCvarsPath); },
                         nullptr);
    if (Tune) {
        View.SetTuneMode(true);  // #115: keyboard editing of the CVar panel (arrows below)
        View.SetDevSplit(true, static_cast<float>(kWinW));  // game left, panel in the right half
        Lur::Log::Info("RPS --tune: game LEFT, cvars RIGHT. Up/Down select, Left/Right halve/double, Del resets");
    }
#else
    (void)Tune;
#endif

    SoloInputs In;
    auto Runner = std::make_unique<Rps::SimRunner>();
    Runner->Start(Seed, SampleSolo, &In, static_cast<uint32_t>(Stress < 0 ? 0 : Stress), NoCombat);

    Rps::CameraScroll Cam;
    bool CamInit = false;
    Lur::Sim::SplitMix64 Rng(Seed ^ 0xA11CE);
    uint64_t AutoAccumNs = 0, PrevNs = NowNs();
    static Rps::Snapshot Snap;
    int Frame = 0;

    while (Win.PumpEvents()) {
        const uint64_t Now = NowNs();
        const uint64_t ElapsedNs = Now - PrevNs;
        PrevNs = Now;
        for (uint32_t Vk : Win.TakeKeys()) {
#if !LUR_SHIPPING
            if (Tune) {  // #115: arrows drive the CVar panel (VK_LEFT/UP/RIGHT/DOWN, VK_DELETE)
                if (Vk == 0x26) { View.DevSelectMove(-1); continue; }       // Up
                if (Vk == 0x28) { View.DevSelectMove(+1); continue; }       // Down
                if (Vk == 0x27) { View.DevAdjustSelected(+1); continue; }   // Right: double
                if (Vk == 0x25) { View.DevAdjustSelected(-1); continue; }   // Left: halve
                if (Vk == 0x2E) { View.DevAdjustSelected(0); continue; }    // Delete: reset
            }
#endif
            if (Vk >= 0x31 && Vk <= 0x34) In.P0.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x31)));
            else if (Vk >= 0x35 && Vk <= 0x38) In.P1.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x35)));
        }
        for (const Lur::Input::TouchEvent& T : Win.TakeTouches()) {
            if (T.Phase == Lur::Input::ETouchPhase::Began) Cam.Begin(T.YPx);
            else if (T.Phase == Lur::Input::ETouchPhase::Moved) Cam.Move(T.YPx, Ppu());
            else if (T.Phase == Lur::Input::ETouchPhase::Ended ||
                     T.Phase == Lur::Input::ETouchPhase::Cancelled) {
                Cam.End();
                if (T.Phase == Lur::Input::ETouchPhase::Ended) {
                    const int Plate = View.OnTap(T.XPx, T.YPx);
                    if (Plate >= 0) In.P0.fetch_or(static_cast<uint8_t>(1u << Plate));
                }
            }
        }
#if LUR_INTERNAL
        if (Auto) {
            AutoAccumNs += ElapsedNs;
            if (AutoAccumNs > 200'000'000ull) {
                AutoAccumNs = 0;
                if (!FoeOnly) In.P0.fetch_or(static_cast<uint8_t>(1u << Rng.NextBounded(4)));  // you: manual if FoeOnly
                In.P1.fetch_or(static_cast<uint8_t>(1u << Rng.NextBounded(4)));                // the opponent always mashes
            }
        }
#else
        (void)Auto;
#endif
        if (Runner->LatestSnapshot(Snap)) {
            int W = 0, H = 0;
            Win.GetSize(&W, &H);
            // --tune: the window is double-wide; the game occupies the LEFT half, so it lays
            // out to GameW (GameView renders it into the left-half viewport, panel right).
            const float GameW = Tune ? static_cast<float>(W) * 0.5f : static_cast<float>(W);
            if (W > 0 && H > 0) {
                const float VisibleH = static_cast<float>(H) / Ppu();
                const float FieldMax = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
                const float MaxCam = FieldMax + View.TopHudWorldUnits(GameW);
                const float MinCam = -View.BottomHudWorldUnits(GameW);
                if (!CamInit) { Cam.Y = MinCam; CamInit = true; }
                Cam.Update(static_cast<float>(ElapsedNs) / 1.0e9f, MaxCam, MinCam);
                View.Render(Renderer, Snap, Snap.AlphaAt(Now), Cam.Y, GameW,
                            static_cast<float>(H), /*FlipY=*/false,
                            static_cast<float>(ElapsedNs) / 1.0e9f);  // solo = team-0 view
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

// ------------------------------------------------------------------ BLE peer vs the phone (#101-E)

// One RPS lockstep peer, headless (no window/renderer — the PC is just the OPPONENT),
// driven over real BLE via WindowsBleTransport so TraceAndroid can capture from the
// phone during a live PC-vs-phone match. --auto presses a random soldier ~1.4/s (the
// only sensible mode for an unattended opponent). Runs until MaxFrames (0 = forever).
int RunBle(const char* RadioExe, bool Auto, int MaxFrames, uint64_t Seed) {
    Lur::Log::Info("RPS desktop: BLE peer vs phone (radio=%s, auto=%d)", RadioExe, Auto ? 1 : 0);
    // RPS's per-game service UUID (…7371, distinct from chess's …7370) — the radio must
    // scan for THIS or it never discovers the RPS phone (matches the Android CMake).
    Lur::DevRig::WindowsBleTransport Ble(RadioExe, "4C55524D-4F54-4F52-4E00-5472616E7371");
    Ble.SetLogger([](const char* M) { Lur::Log::Info("%s", M); });
    if (!Ble.Start()) {
        Lur::Log::Error("BLE radio failed to start - build it: "
                        "powershell -File Tools\\BleDevRig\\build.ps1 -Source BleRadio.cs");
        return 1;
    }

    Lur::Net::Session Session;
    Rps::LockstepPeer Lp;
    const std::string Guid = "rps-pc-ble-peer";  // stable; the phone's GUID orders the teams
    Session.SetHandler(Rps::MsgInput,
                       [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(Rps::MsgInput, D, N); });
    Session.SetHandler(Rps::MsgAnchor,
                       [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(Rps::MsgAnchor, D, N); });
    Session.SetHandler(Rps::MsgResyncChunk,
                       [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(Rps::MsgResyncChunk, D, N); });
    Session.SetResyncHandler([&Lp] { Lp.BeginResync(); });
    Session.Start(&Ble, Guid);
    Lur::Log::Info("session started (id %.8s); waiting for the phone to advertise", Guid.c_str());

    bool Started = false;
    Lur::Sim::SplitMix64 Rng(Seed ^ 0xB1E);
    uint64_t AutoAccumNs = 0, QualAccumNs = 0;
    int Frame = 0;
    auto PrevTime = std::chrono::steady_clock::now();
    for (;;) {
        const auto Now = std::chrono::steady_clock::now();
        const uint64_t ElapsedNs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Now - PrevTime).count();
        PrevTime = Now;

        Session.Tick(ElapsedNs);  // pump the radio inbox + handshake/liveness

        if (!Started && Session.IsReady()) {
            const uint8_t Team = Guid < Session.GetPeerGuid() ? 0 : 1;
            Lp.Init(Seed, Team, SendViaSession, &Session);
            Started = true;
            Lur::Log::Info("linked - lockstep started (team %d, peer %.8s)", Team,
                           Session.GetPeerGuid().c_str());
        }
        if (Started) {
            (void)Auto; (void)Rng;
#if LUR_INTERNAL
            if (Auto) {
                AutoAccumNs += ElapsedNs;
                if (AutoAccumNs > 200'000'000ull) {  // ~1.4 presses/s, a random soldier type
                    AutoAccumNs = 0;
                    Lp.SetLocalMask(static_cast<uint8_t>(1u << Rng.NextBounded(4)));
                }
            }
#endif
            Lp.Tick(ElapsedNs);  // produce + send input, execute to the ceiling
            if (Lp.Desynced()) Lur::Log::Error("DESYNC (tick %u)", Lp.ExecTick());
        }

        QualAccumNs += ElapsedNs;
        if (Started && QualAccumNs > 2'000'000'000ull) {  // ~0.5 Hz liveness line
            QualAccumNs = 0;
            Lur::Log::Info("BLE tick=%u you=%d foe=%d desync=%d txB=%llu rxB=%llu",
                           Lp.ExecTick(), Lp.GetSim().AliveCount(0), Lp.GetSim().AliveCount(1),
                           Lp.Desynced() ? 1 : 0, (unsigned long long)Ble.GetBytesOut(),
                           (unsigned long long)Ble.GetBytesIn());
        }

        ++Frame;
        if (MaxFrames > 0 && Frame >= MaxFrames) {
            Lur::Log::Info("ran %d frames (linked=%d tick=%u) - exiting", Frame, Started ? 1 : 0,
                           Lp.ExecTick());
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));  // ~250 Hz service, don't busy-spin
    }
    Lur::Log::Info("clean exit");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Lur::Log::Init(nullptr, "RpsDesktop");

    int MaxFrames = 0;
    bool Auto = false, Solo = false, Ble = false, FlockDemo = false, NoCombat = false, FoeOnly = false;
    bool Tune = false;  // #115: keyboard CVar tuning panel (implies --solo)
    std::string RadioExe = "Tools\\BleDevRig\\BleRadio.exe";  // relative to the repo root
    uint64_t Seed = 0x1234;
    int Stress = 0;
    for (int I = 1; I < argc; ++I) {
        std::string A = argv[I];
        if (A == "--frames" && I + 1 < argc) MaxFrames = std::atoi(argv[++I]);
        else if (A == "--auto") Auto = true;
        else if (A == "--solo") Solo = true;
        else if (A == "--flockdemo") { Solo = true; FlockDemo = true; }  // #97 visual tuning (combat ON)
        else if (A == "--nocombat") NoCombat = true;                     // pure-motion tuning (no kills)
        else if (A == "--autofoe") { Solo = true; Auto = true; FoeOnly = true; }  // you play, only the foe mashes
        else if (A == "--ble") {
            Ble = true;
            if (I + 1 < argc && argv[I + 1][0] != '-') RadioExe = argv[++I];  // optional radio path
        }
        else if (A == "--seed" && I + 1 < argc) Seed = std::strtoull(argv[++I], nullptr, 0);
        else if (A == "--stress" && I + 1 < argc) Stress = std::atoi(argv[++I]);
        else if (A == "--winw" && I + 1 < argc) kWinW = std::atoi(argv[++I]);
        else if (A == "--winh" && I + 1 < argc) kWinH = std::atoi(argv[++I]);
        else if (A == "--tune") { Solo = true; Tune = true; }  // #115 CVar tuning panel
    }

    if (Ble) return RunBle(RadioExe.c_str(), Auto, MaxFrames, Seed);
    if (Solo) return RunSolo(Auto, MaxFrames, Seed, Stress, FlockDemo, NoCombat, FoeOnly, Tune);
    return RunLoopback(Auto, MaxFrames, Seed);
}
