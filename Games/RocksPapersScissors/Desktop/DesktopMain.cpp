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
#include "Rps/AiController.h"
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
// View-side world (float) -> Fixed for a place event. Only the placing peer computes this; the
// resulting Fixed travels over the wire, so both peers apply the identical position (no float
// crosses into the sim's determinism — the event carries the raw int).
Rps::Fixed WorldToFixed(float W) {
    if (W < 0.0f) W = 0.0f;
    return Rps::Fixed{static_cast<int32_t>(W * static_cast<float>(Rps::Fixed::One) + 0.5f)};
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
    // #139/§9.1: pre-match the camera is LOCKED at the player's baseline (the starting band where
    // the camp goes); free scrolling begins once the match starts (both camps placed).
    if (!P.Lp.MatchStarted()) P.Cam.Y = MinCam;
    else P.Cam.Update(DtSec, MaxCam, MinCam);
    // #139: show the local camp the instant it's dropped (view-only), before the match starts and
    // the sim reflects it — otherwise it looks invisible until the opponent readies too.
    if (!P.Lp.MatchStarted() && P.Lp.HasLocalCamp()) {
        const Rps::InputEvent& C = P.Lp.LocalCamp();
        const float Inv = 1.0f / static_cast<float>(Rps::Fixed::One);
        P.View.SetPlacedPreview(C.Type, static_cast<float>(C.X) * Inv, static_cast<float>(C.Y) * Inv, true);
    } else {
        P.View.SetPlacedPreview(0, 0.0f, 0.0f, false);
    }
    P.View.Render(P.Renderer, P.Snap, P.Snap.AlphaAt(Now), P.Cam.Y, static_cast<float>(W),
                  static_cast<float>(H), P.Team == 1, DtSec);
}

// #139 drag-to-place: turn a pointer at (XPx,YPx) into a place-event world position + validity,
// asking the authoritative sim (WouldAcceptPlace) so the ghost's red/valid blink can never
// disagree with what the sim will accept. Wx/Wy are the world drop; returns validity.
bool DragValidity(Peer& P, float XPx, float YPx, int W, int H, float& Wx, float& Wy) {
    P.View.ScreenToWorld(XPx, YPx, P.Cam.Y, static_cast<float>(W), static_cast<float>(H),
                         P.Team == 1, Wx, Wy);
    return P.Lp.GetSim().WouldAcceptPlace(P.Team, static_cast<uint8_t>(P.View.PlacingType()),
                                          WorldToFixed(Wx), WorldToFixed(Wy));
}

void HandlePeerInput(Peer& P, Lur::Sim::SplitMix64& Rng, bool Auto, uint64_t ElapsedNs,
                     uint64_t& AutoAccumNs) {
    // #139: a pointer-down on a build plate starts a drag-to-place (the ghost follows to the
    // field; a valid release emits a Place event, an invalid one slides back); any other drag
    // pans the camera. The per-building x1/x5/x20 queue taps land in #140.
    for (uint32_t Vk : P.Win.TakeKeys()) (void)Vk;
    int W = 0, H = 0;
    P.Win.GetSize(&W, &H);
    for (const Lur::Input::TouchEvent& T : P.Win.TakeTouches()) {
        if (T.Phase == Lur::Input::ETouchPhase::Began) {
            const int Plate = P.View.PlateAt(T.XPx, T.YPx);
            if (Plate >= 0) {
                P.View.BeginPlaceDrag(Plate, T.XPx, T.YPx);  // seed at the finger (no frame-1 flash)
                float Wx = 0, Wy = 0;
                P.View.UpdatePlaceDrag(T.XPx, T.YPx, DragValidity(P, T.XPx, T.YPx, W, H, Wx, Wy));
            } else {
                P.Cam.Begin(T.YPx);
            }
        } else if (T.Phase == Lur::Input::ETouchPhase::Moved) {
            if (P.View.IsPlacing()) {
                float Wx = 0, Wy = 0;
                const bool Valid = DragValidity(P, T.XPx, T.YPx, W, H, Wx, Wy);
                P.View.UpdatePlaceDrag(T.XPx, T.YPx, Valid);
            } else {
                P.Cam.Move(T.YPx, Ppu());
            }
        } else if (T.Phase == Lur::Input::ETouchPhase::Ended ||
                   T.Phase == Lur::Input::ETouchPhase::Cancelled) {
            if (P.View.IsPlacing()) {
                bool Placed = false;
                if (T.Phase == Lur::Input::ETouchPhase::Ended) {
                    float Wx = 0, Wy = 0;
                    if (DragValidity(P, T.XPx, T.YPx, W, H, Wx, Wy)) {
                        P.Lp.QueueLocalEvent(Rps::InputEvent::Place(
                            P.Team, static_cast<uint8_t>(P.View.PlacingType()),
                            WorldToFixed(Wx), WorldToFixed(Wy)));
                        Placed = true;
                    }
                }
                P.View.EndPlaceDrag(Placed);  // valid -> the real building takes over; else slide back
            } else {
                P.Cam.End();
                if (T.Phase == Lur::Input::ETouchPhase::Ended && P.View.OnTap(T.XPx, T.YPx) == -1) {
                    // Not the HUD/selector -> maybe a per-building x1/x5 button (#140).
                    int32_t Slot = -1;
                    const int Cnt = P.View.OnProductionButton(T.XPx, T.YPx, Slot);
                    if (Cnt > 0) P.Lp.QueueLocalEvent(Rps::InputEvent::Queue(P.Team, Slot, Cnt));
                }
            }
        }
    }
    (void)Auto; (void)Rng; (void)ElapsedNs; (void)AutoAccumNs;  // #137b: auto-soak re-wires to events in #140
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
void SampleSolo(void* Ctx, const Rps::Sim&, uint32_t, Rps::InputEvent*, int, int& Count) {
    // #137b: the solo human's place/queue EVENTS arrive with the drag-place UI (#139/#140);
    // until then solo has no local input (the SimRunner just advances the sim).
    (void)Ctx;
    Count = 0;
}

// Human (team 0) vs the single-player AI (team 1). The AI reads the sim on the sim thread —
// right before StepEvents — so its board read is race-free (#124).
struct SoloAiCtx {
    SoloInputs* In;
    Rps::AiController* Ai;
};
void SampleSoloVsAi(void* Ctx, const Rps::Sim& S, uint32_t Tick, Rps::InputEvent* Out, int Cap,
                    int& Count) {
    SoloAiCtx* C = static_cast<SoloAiCtx*>(Ctx);
    Count = 0;
    C->Ai->DecideEvents(S, Tick, Out, Cap, Count);  // team-1 AI; human (team 0) input joins in #139/#140
}

// #128: headless AI-vs-AI tier-strength harness. Because the AI is a pure InputFn over sim
// state, a match is just a Sim loop driven by two AiControllers — no window, no thread. Runs
// Matches seeded games to resolution (or a tick cap, then army-count tiebreak) and reports the
// win tally: the cheap check that the tiers actually differ in strength before human playtests
// (spec §8 slice 3). Tuned via the same rps.ai.* CVars (loaded from cvars.cfg here).
int RunAiVs(Rps::EAiTier TierA, Rps::EAiTier TierB, uint64_t BaseSeed, int Matches, int MaxTicks) {
#if !LUR_SHIPPING
    Lur::Core::LoadCVarConfig("rps-cvars.cfg");  // use the tuned AI knobs if present
#endif
    const char* Names[] = {"easy", "medium", "hard"};
    Lur::Log::Info("AI-vs-AI: team0=%s vs team1=%s, %d matches, cap %d ticks",
                   Names[static_cast<int>(TierA)], Names[static_cast<int>(TierB)], Matches, MaxTicks);
    int Wins[3] = {0, 0, 0};  // [ongoing unused], team0, team1 -> index by EResult
    int Draws = 0, Resolved = 0;
    long long SumA0 = 0, SumA1 = 0;  // army totals for the continuous strength signal
    for (int M = 0; M < Matches; ++M) {
        const uint64_t MatchSeed = BaseSeed + static_cast<uint64_t>(M);
        Rps::Sim S;
        S.Init(MatchSeed);
        Rps::AiController Ai0, Ai1;
        Ai0.Init(MatchSeed, 0, TierA);
        Ai1.Init(MatchSeed, 1, TierB);
        int T = 0;
        for (; T < MaxTicks && S.Result == Rps::ResultOngoing; ++T) {
            Rps::InputEvent E0[Rps::MaxEventsPerTick], E1[Rps::MaxEventsPerTick];
            int C0 = 0, C1 = 0;
            Ai0.DecideEvents(S, S.Tick, E0, Rps::MaxEventsPerTick, C0);
            Ai1.DecideEvents(S, S.Tick, E1, Rps::MaxEventsPerTick, C1);
            Rps::InputEvent Comb[2 * Rps::MaxEventsPerTick];
            int NC = 0;
            for (int I = 0; I < C0; ++I) Comb[NC++] = E0[I];  // team 0 first (Execute order)
            for (int I = 0; I < C1; ++I) Comb[NC++] = E1[I];
            S.StepEvents(Comb, NC);
        }
        const int A0 = S.AliveCount(0), A1 = S.AliveCount(1);
        SumA0 += A0;
        SumA1 += A1;
        uint8_t Res = S.Result;
        if (Res != Rps::ResultOngoing) ++Resolved;      // real wipeout (not a cap tiebreak)
        else Res = A0 > A1 ? Rps::ResultTeam0Wins : A1 > A0 ? Rps::ResultTeam1Wins : Rps::ResultDraw;
        if (Res == Rps::ResultDraw) ++Draws; else ++Wins[Res];
    }
    // Binary wins are noisy for near-even tiers (cap tiebreak + team0/1 positional bias), so
    // ALSO report resolved-count and average army sizes — the continuous strength signal.
    Lur::Log::Info("AI-vs-AI RESULT: team0(%s) %d wins | team1(%s) %d wins | %d draws | %d/%d resolved | "
                   "avg army: t0=%.0f t1=%.0f",
                   Names[static_cast<int>(TierA)], Wins[Rps::ResultTeam0Wins],
                   Names[static_cast<int>(TierB)], Wins[Rps::ResultTeam1Wins], Draws, Resolved, Matches,
                   static_cast<double>(SumA0) / Matches, static_cast<double>(SumA1) / Matches);
    return 0;
}

int RunSolo(bool Auto, int MaxFrames, uint64_t Seed, int Stress, bool FlockDemo, bool NoCombat,
            bool FoeOnly, Rps::EAiTier AiTier) {
    // --flockdemo (#97): a solo StressFill scene for visual tuning of the flock. Combat is
    // ON by default (playtest: how the counters clash is part of the feel) — pass --nocombat
    // for pure-motion tuning (mixed blobs that never kill each other). Defaults to a healthy
    // unit count if --stress wasn't set.
    if (FlockDemo && Stress <= 0) Stress = 200;
    Lur::Log::Info("RPS desktop: solo (SimRunner, no net)%s%s%s", Auto ? " (auto)" : "",
                   FlockDemo ? " (flockdemo)" : "", NoCombat ? " (combat off)" : "");
    Lur::Platform::Window Win;
    if (!Win.Create("RocksPapersScissors - solo", kWinW, kWinH, 200, 60)) return 1;
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
    Lur::Log::Info("dev console: press the key left of '1' (backtick/paragraph) to open; click cvars to edit");
#endif

    SoloInputs In;
    // Default solo opponent is the AI (team 1); you play team 0 (#124). --auto (soak) and
    // --flockdemo (stress) keep the old random/idle P1 instead.
    Rps::AiController Ai;
    const bool UseAi = !Auto && !FlockDemo;
    if (UseAi) {
        Ai.Init(Seed, /*team*/ 1, AiTier);
        const char* Names[] = {"easy", "medium", "hard"};
        Lur::Log::Info("solo opponent: AI (%s)", Names[static_cast<int>(AiTier)]);
    }
    SoloAiCtx AiCtx{&In, &Ai};
    auto Runner = std::make_unique<Rps::SimRunner>();
    Runner->Start(Seed, UseAi ? &SampleSoloVsAi : &SampleSolo,
                  UseAi ? static_cast<void*>(&AiCtx) : static_cast<void*>(&In),
                  static_cast<uint32_t>(Stress < 0 ? 0 : Stress), NoCombat);

    Rps::CameraScroll Cam;
    bool CamInit = false;
#if !LUR_SHIPPING
    float DevDragY = 0.0f, DevDragMoved = 0.0f;  // drag-to-scroll the console (# 121)
#endif
    Lur::Sim::SplitMix64 Rng(Seed ^ 0xA11CE);
    uint64_t AutoAccumNs = 0, PrevNs = NowNs();
    static Rps::Snapshot Snap;
    int Frame = 0;

    while (Win.PumpEvents()) {
        const uint64_t Now = NowNs();
        const uint64_t ElapsedNs = Now - PrevNs;
        PrevNs = Now;
#if !LUR_SHIPPING
        if (Win.TakeConsoleToggle()) View.SetDevOverlayOpen(!View.DevOverlayOpen());  // § key
#endif
        for (uint32_t Vk : Win.TakeKeys()) {
            if (Vk >= 0x31 && Vk <= 0x34) In.P0.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x31)));
            else if (Vk >= 0x35 && Vk <= 0x38) In.P1.fetch_or(static_cast<uint8_t>(1u << (Vk - 0x35)));
        }
        for (const Lur::Input::TouchEvent& T : Win.TakeTouches()) {
#if !LUR_SHIPPING
            // When the console is open it eats pointer input (no camera pan under it); a click
            // release becomes a DevTap the overlay hit-tests on the render thread — same path
            // as the phone's touch, so desktop drives the identical console.
            if (View.DevOverlayOpen()) {
                // Drag = scroll the cvar list; a click that barely moved = a tap the overlay
                // hit-tests. Same gesture model as the phone (finger drag / tap).
                if (T.Phase == Lur::Input::ETouchPhase::Began) { DevDragY = T.YPx; DevDragMoved = 0.0f; }
                else if (T.Phase == Lur::Input::ETouchPhase::Moved) {
                    View.DevScroll(DevDragY - T.YPx);
                    DevDragMoved += std::fabs(DevDragY - T.YPx);
                    DevDragY = T.YPx;
                } else if (T.Phase == Lur::Input::ETouchPhase::Ended) {
                    if (DevDragMoved < 6.0f) View.DevTap(T.XPx, T.YPx);
                }
                continue;
            }
#endif
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
            const float GameW = static_cast<float>(W);
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
            // #137b: auto-soak spammed a random press mask; that's retired with the mask. The
            // event-based soak (random place/queue) re-lands with the input UI in #139/#140.
            (void)Auto; (void)Rng; (void)AutoAccumNs;
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
    std::string RadioExe = "Tools\\BleDevRig\\BleRadio.exe";  // relative to the repo root
    uint64_t Seed = 0x1234;
    int Stress = 0;
    Rps::EAiTier AiTier = Rps::EAiTier::Medium;  // solo opponent difficulty (#124)
    bool AiVs = false;                           // #128 headless AI-vs-AI tier harness
    Rps::EAiTier AiVsA = Rps::EAiTier::Hard, AiVsB = Rps::EAiTier::Easy;
    int Matches = 9;
    int MaxTicks = 6000;
    auto ParseTier = [](const std::string& T) {
        return T == "easy" ? Rps::EAiTier::Easy : T == "hard" ? Rps::EAiTier::Hard
                                                              : Rps::EAiTier::Medium;
    };
    for (int I = 1; I < argc; ++I) {
        std::string A = argv[I];
        if (A == "--frames" && I + 1 < argc) MaxFrames = std::atoi(argv[++I]);
        else if (A == "--auto") Auto = true;
        else if (A == "--solo") Solo = true;
        else if (A == "--ai" && I + 1 < argc) {
            Solo = true;
            AiTier = ParseTier(argv[++I]);
        }
        else if (A == "--aivs" && I + 1 < argc) {  // "hard:easy" -> team0:team1
            AiVs = true;
            const std::string V = argv[++I];
            const auto C = V.find(':');
            AiVsA = ParseTier(V.substr(0, C));
            AiVsB = ParseTier(C == std::string::npos ? std::string{} : V.substr(C + 1));
        }
        else if (A == "--matches" && I + 1 < argc) Matches = std::atoi(argv[++I]);
        else if (A == "--maxticks" && I + 1 < argc) MaxTicks = std::atoi(argv[++I]);
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
    }

    if (AiVs) return RunAiVs(AiVsA, AiVsB, Seed, Matches, MaxTicks);
    if (Ble) return RunBle(RadioExe.c_str(), Auto, MaxFrames, Seed);
    if (Solo) return RunSolo(Auto, MaxFrames, Seed, Stress, FlockDemo, NoCombat, FoeOnly, AiTier);
    return RunLoopback(Auto, MaxFrames, Seed);
}
