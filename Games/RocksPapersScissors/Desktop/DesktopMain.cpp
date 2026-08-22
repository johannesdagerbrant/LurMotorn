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

#include "Lur/Core/CVar.h"         // #147: registry walk for the gameplay-CVar sync seed
#include "Lur/Core/CVarConfig.h"  // persist tuned cvars across runs
#include "Lur/Core/Log.h"
#include "Lur/Input/ConsoleGesture.h"  // #151: the ONE dev-console gesture, shared with the phones
#include "Lur/Net/Session.h"
#include "Lur/Platform/Window.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Save/Store.h"        // persistent per-opponent W-L-D (ScoreBook)
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Loopback.h"
#include "Rps/AiController.h"
#include "OwnerBot.h"   // the scripted owner line the top tier is scored against
#include "Rps/MatchRecord.h"   // #144: --replay a device recording
#include "Lur/Input/ScrollCamera.h"
#include "Rps/GameView.h"
#include "Rps/ViewMetrics.h"
#include "Rps/TouchRouter.h"   // #43 section D: what a touch MEANS, shared with both phone mains   // #43 section D: Ppu / WorldHeightF / WorldToFixed / GhostOffsetPx
#include "Rps/LockstepPeer.h"
#include "Rps/SimRunner.h"
#include "Rps/SoloInput.h"
#include "Rps/ScoreBook.h"
#include "Rps/SessionWiring.h" // the ONE Session->LockstepPeer routing table (#160)
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

// #43 section D: the arithmetic lives in Rps/ViewMetrics.h now — this stayed a nullary wrapper only
// because the desktop window width is a global here and ~30 call sites read Ppu() with no argument.
float Ppu() { return Rps::Ppu(static_cast<float>(kWinW)); }
using Rps::WorldHeightF;
// View-side world (float) -> Fixed for a place event. Only the placing peer computes this; the
// resulting Fixed travels over the wire, so both peers apply the identical position (no float
// crosses into the sim's determinism — the event carries the raw int).
using Rps::WorldToFixed;   // #43 section D: one definition, shared with both phone mains

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
    Lur::Input::ScrollCamera Cam;
    bool CamInit = false;   // first frame parks the camera at MinCam (camp visible)
    uint8_t Team = 0;
    // #43 section D: what a touch MEANS is Rps::TouchRouter now, shared with the solo loop and both
    // phone mains. This window used to carry the OLDEST of the four copies — no ghost offset, no
    // magnetic snap, no press flash, no tap slop (#209) — which mattered because the two-window
    // harness is where placement feel gets judged.
    Rps::TouchRouter Router;
};

// #43 section D: RouteConsolePointer is gone — Rps::TouchRouter owns "an open console eats the
// pointer" for all four mains, which is what its old comment was already asking for ("a second
// recognizer with its own slop is how one console, identical on both, quietly stops being true").

void SendViaSession(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* D, std::size_t N) {
    static_cast<Lur::Net::Session*>(Ctx)->Send(Type, D, N);
}

bool SetupPeer(Peer& P, const char* Title, int X, const std::string& Guid) {
    if (!P.Win.Create(Title, kWinW, kWinH, X, 60)) return false;
    P.Renderer = Lur::Render::VulkanRenderer::Create("OnlyRps");
    if (P.Renderer == nullptr || !P.Renderer->Init(P.Win.NativeHandle())) return false;
    P.View.CreateResources(P.Renderer);
    // #43 section D. No selector hooks: the two-window harness has no opponent selector — each
    // window IS its peer — so an AI-tier or peer-row pick is not reachable here.
    Rps::TouchRouterHooks Hooks;
    Hooks.Emit = [&P](const Rps::InputEvent& E) { P.Lp.QueueLocalEvent(E); };
    P.Router.Init(&P.View, &P.Cam, std::move(Hooks));
    P.Guid = Guid;
    P.Transport.SetDeferred(true);  // deferred delivery: lockstep replies from a receiver never recurse
    // #147/#112: the workbench must carry the SAME message set as a phone, or a bug in the cvar-sync
    // / fingerprint path is invisible in the two-window loopback (its whole point). #160 makes that
    // literal — one shared routing table instead of a per-main copy.
    Rps::RouteSessionToPeer(P.Session, P.Lp);
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

void HandlePeerInput(Peer& P, Lur::Sim::SplitMix64& Rng, bool Auto, uint64_t ElapsedNs,
                     uint64_t& AutoAccumNs) {
    // #139: a pointer-down on a build plate starts a drag-to-place (the ghost follows to the
    // field; a valid release emits a Place event, an invalid one slides back); any other drag
    // pans the camera. The per-building x1/x5/x20 queue taps land in #140.
#if !LUR_SHIPPING
    // The console, per window (#119). The two-window loopback is the build the balance pass
    // actually runs in (#110), and until now its § key was wired only in the solo path — so
    // the one mode with a peer to sync a tuned cvar to was the one mode you could not open
    // the console in. Each Peer owns its Win and View, so § acts on the focused window.
    if (P.Win.TakeConsoleToggle()) P.View.SetDevOverlayOpen(!P.View.DevOverlayOpen());
    for (uint32_t Vk : P.Win.TakeKeys()) P.View.DevKey(Vk);
#else
    for (uint32_t Vk : P.Win.TakeKeys()) (void)Vk;
#endif
    int W = 0, H = 0;
    P.Win.GetSize(&W, &H);
    Rps::TouchFrame F;
    F.ViewW = static_cast<float>(W);
    F.ViewH = static_cast<float>(H);
    F.Team  = P.Team;
    F.Live  = true;   // the loopback harness is only ever driven with a match running
    for (const Lur::Input::TouchEvent& T : P.Win.TakeTouches()) P.Router.Route(T, P.Snap, F);
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
#if LUR_INTERNAL
            // #147/#112: same exchange a phone does. Both windows share this process's globals, so
            // the merged set is a no-op here — but running the real path keeps it exercised.
            A->Lp.SendFingerprint();
            B->Lp.SendFingerprint();
            Lur::Core::CVarRegistry::ForEach([&A, &B](Lur::Core::ICVar* C) {
                if (!C->AffectsGameplay() || !C->Overridden()) return;
                const int Id = Rps::GameplayIdForName(C->Name());
                if (Id < 0) return;
                A->Lp.SeedGameplayCvar(static_cast<uint8_t>(Id), C->RawValue(), C->EditWallMs());
                B->Lp.SeedGameplayCvar(static_cast<uint8_t>(Id), C->RawValue(), C->EditWallMs());
            });
            A->Lp.SendCvarSync();
            B->Lp.SendCvarSync();
#endif
            // Each window's linked row names the OTHER peer, so the two-window build shows the
            // same thing two phones would (#178).
            A->View.SetLinked(true, B->Guid);
            B->View.SetLinked(true, A->Guid);
            A->View.SetBuildMismatch(A->Lp.BuildMismatch());
            B->View.SetBuildMismatch(B->Lp.BuildMismatch());
            Started = true;
            Lur::Log::Info("linked - lockstep started (A=team%d B=team%d)", ATeam, BTeam);
        }

        if (Started) {
            HandlePeerInput(*A, Rng, Auto, ElapsedNs, AutoA);
            HandlePeerInput(*B, Rng, Auto, ElapsedNs, AutoB);
            A->Lp.Tick(ElapsedNs);  // produce + send input, execute up to the ceiling
            B->Lp.Tick(ElapsedNs);
            // #161: the workbench must SHOW the repair the phones show, and in both windows — the
            // two-window loopback is where a recovery gets watched side by side.
            A->View.SetRecovering(A->Lp.Recovering());
            B->View.SetRecovering(B->Lp.Recovering());
            if (A->Lp.Desynced() || B->Lp.Desynced())
                Lur::Log::Error("DESYNC detected (tick A=%u B=%u, recovery attempt %d/%d)",
                                A->Lp.ExecTick(), B->Lp.ExecTick(),
                                A->Lp.RecoveryAttempts() + B->Lp.RecoveryAttempts(),
                                Rps::LockstepPeer::MaxDesyncRecoveries);
        }

        const float DtSec = static_cast<float>(ElapsedNs) / 1.0e9f;
        RenderPeer(*A, Now, DtSec);
        RenderPeer(*B, Now, DtSec);

        if (MaxFrames > 0 && ++Frame >= MaxFrames) {
            // #204: sticky per-match counts, and print them PER PEER. Both must agree — the two-window
            // loopback is the cheapest place to notice if they ever stop agreeing, which is exactly the
            // asymmetry that made a real divergence look like a clean match on one phone.
            Lur::Log::Info("rendered %d frames headless (started=%d ticks A=%u B=%u desyncA=%u "
                           "desyncB=%u) - exiting",
                           Frame, Started ? 1 : 0, A->Lp.ExecTick(), B->Lp.ExecTick(),
                           A->Lp.DesyncsSeen(), B->Lp.DesyncsSeen());
            break;
        }
    }

    A->Renderer->Shutdown();
    B->Renderer->Shutdown();
    Lur::Log::Info("clean exit");
    return 0;
}

// ------------------------------------------------------------------ single-window solo (slice 0)

// #139/#140: the solo human plays team 0 — its place/queue events are produced by the drag-place
// UI on the render thread and drained here on the sim thread (SoloInputInbox, the LockstepPeer's
// inbox re-used for the peerless solo path).
void SampleSolo(void* Ctx, const Rps::Sim&, uint32_t, Rps::InputEvent* Out, int Cap, int& Count) {
    Count = static_cast<Rps::SoloInputInbox*>(Ctx)->Drain(Out, Cap);
}

// Human (team 0) vs the single-player AI (team 1). The AI reads the sim on the sim thread —
// right before StepEvents — so its board read is race-free (#124).
struct SoloAiCtx {
    Rps::SoloInputInbox* Human;
    Rps::AiController* Ai;
};
void SampleSoloVsAi(void* Ctx, const Rps::Sim& S, uint32_t Tick, Rps::InputEvent* Out, int Cap,
                    int& Count) {
    SoloAiCtx* C = static_cast<SoloAiCtx*>(Ctx);
    // Human (team 0) events first — team-0-before-team-1 is the Execute order both peers use — then
    // the AI (team 1) fills whatever budget remains this tick. The AI HOLDS until the human commits
    // its first mining camp (feedback), so it doesn't build a lead while you're still setting up.
    Count = C->Human->Drain(Out, Cap);
    if (S.HasMinerCamp(0)) {
        int AiCount = 0;
        C->Ai->DecideEvents(S, Tick, Out + Count, Cap - Count, AiCount);
        Count += AiCount;
    }
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
    
    Lur::Log::Info("AI-vs-AI: team0=%s vs team1=%s, %d matches, cap %d ticks",
                   Rps::AiTierName(TierA), Rps::AiTierName(TierB), Matches, MaxTicks);
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
                   Rps::AiTierName(TierA), Wins[Rps::ResultTeam0Wins],
                   Rps::AiTierName(TierB), Wins[Rps::ResultTeam1Wins], Draws, Resolved, Matches,
                   static_cast<double>(SumA0) / Matches, static_cast<double>(SumA1) / Matches);
    return 0;
}

// #152: headless AI DIAGNOSTIC. The --aivs tally answers "which tier is stronger"; it cannot
// answer "is this tier functional at all", which is the question after an economy change — a tier
// tuned against the old purse can sit broke and idle and still tie another broken tier 0-0. So run
// one mirror match and report WHAT THE AI DID: the tick each milestone was reached (camp, first
// miner, first soldier building, first soldier) plus a periodic census of gold/workers/soldiers/
// buildings. Milestones that never arrive print as "-", which is the failure this exists to catch.
int RunAiDiag(Rps::EAiTier Tier, Rps::EAiTier Tier1, uint64_t Seed, int MaxTicks, int EveryTicks) {
#if !LUR_SHIPPING
    Lur::Core::LoadCVarConfig("rps-cvars.cfg");
#endif
    
    auto S = std::make_unique<Rps::Sim>();   // ~MBs of SoA: heap, not the 1 MB main stack (#94)
    S->Init(Seed);
    Rps::AiController Ai0, Ai1;
    Ai0.Init(Seed, 0, Tier);
    Ai1.Init(Seed, 1, Tier1);   // mirror unless --aidiag a:b named two
    // Census of one team: gold, alive miners, alive soldiers, producing buildings (the HQ excluded —
    // it produces nothing, so counting it would hide "this AI never built anything").
    // Mines/Max are the CART SPREAD (rps.mine.spread_slack): how many distinct deposits this
    // team's carts are assigned to, and the worst pile on one of them. Max is the readability
    // number — it is what the player has to count on screen — and it trades against the economy
    // columns beside it, so the two must be read together when tuning the slack.
    struct Census { int Gold, Workers, Soldiers, Buildings, Mines, Max; };
    auto Look = [&](uint8_t T) {
        Census C{S->Teams[T].Gold, 0, 0, 0, 0, 0};
        int Per[Rps::NumMines] = {};
        for (int32_t I = 0; I < S->Count; ++I) {
            if (!S->IsAlive(I) || S->Team[I] != T) continue;
            if (S->Kind[I] == Rps::KindBuilding) ++C.Buildings;
            else if (S->Kind[I] == Rps::KindUnit) {
                if (S->Type[I] == Rps::UnitMiner) {
                    ++C.Workers;
                    if (S->Target[I] >= 0) ++Per[S->Target[I]];
                } else ++C.Soldiers;
            }
        }
        for (int M = 0; M < Rps::NumMines; ++M) {
            if (Per[M] > 0) ++C.Mines;
            if (Per[M] > C.Max) C.Max = Per[M];
        }
        return C;
    };
    int TCamp = -1, TMiner = -1, TBldg = -1, TSoldier = -1;   // milestone ticks (-1 = never)
    int32_t AskPlace = 0, GotPlace = 0, AskPlaceUnits = 0, AskQueue = 0, AskQueueUnits = 0;
    int32_t IdleTicks[4] = {};
    int64_t IdleGold[4] = {};
    Lur::Log::Info("AI diag: t0=%s t1=%s seed=0x%llx cap=%d ticks (%.0fs at %d Hz)",
                   Rps::AiTierName(Tier), Rps::AiTierName(Tier1), static_cast<unsigned long long>(Seed), MaxTicks,
                   static_cast<double>(MaxTicks) / Rps::TickRateHz, Rps::TickRateHz);
    Lur::Log::Info("  tick |    t0 gold  wrk  sol  bld  mine  max |    t1 gold  wrk  sol  bld  mine  max");
    for (int T = 0; T < MaxTicks && S->Result == Rps::ResultOngoing; ++T) {
        Rps::InputEvent E0[Rps::MaxEventsPerTick], E1[Rps::MaxEventsPerTick];
        int C0 = 0, C1 = 0;
        Ai0.DecideEvents(*S, S->Tick, E0, Rps::MaxEventsPerTick, C0);
        Ai1.DecideEvents(*S, S->Tick, E1, Rps::MaxEventsPerTick, C1);
        Rps::InputEvent Comb[2 * Rps::MaxEventsPerTick];
        int NC = 0;
        for (int I = 0; I < C0; ++I) Comb[NC++] = E0[I];
        for (int I = 0; I < C1; ++I) Comb[NC++] = E1[I];
        // What the AI ASKED for, next to what it GOT. A place event the sim refuses is a silent
        // no-op (ApplyPlace is deliberately a deterministic no-op on an illegal spot), so "it never
        // expanded" and "it asked 900 times and was refused 900 times" look identical from the
        // census — and they are completely different bugs.
        // IDLE ticks, attributed by FSM state. An AI that emits nothing is neither expanding nor
        // producing, and the census cannot distinguish "poor" from "had gold and no legal action".
        if (C0 == 0) { const int St0 = static_cast<int>(Ai0.State()); if (St0 >= 0 && St0 < 4) { ++IdleTicks[St0]; IdleGold[St0] += S->Teams[0].Gold; } }
        const int32_t BldBefore = Look(0).Buildings;
        for (int I = 0; I < C0; ++I) {
            if (Comb[I].Team != 0) continue;
            if (Comb[I].Kind == Rps::EventPlaceBuilding) { ++AskPlace; AskPlaceUnits += Comb[I].Type == Rps::UnitMiner ? 1 : 0; }
            else { ++AskQueue; AskQueueUnits += Comb[I].Y; }
        }
        S->StepEvents(Comb, NC);
        if (Look(0).Buildings > BldBefore) ++GotPlace;
        const Census A = Look(0);
        if (TCamp < 0 && S->HasMinerCamp(0)) TCamp = T;
        if (TMiner < 0 && A.Workers > 0) TMiner = T;
        if (TBldg < 0 && A.Buildings > 1) TBldg = T;      // >1: the miner camp is the first
        if (TSoldier < 0 && A.Soldiers > 0) TSoldier = T;
        if (EveryTicks > 0 && T % EveryTicks == 0) {
            const Census B = Look(1);
            Lur::Log::Info("%6d | %9d %4d %4d %4d %5d %4d | %9d %4d %4d %4d %5d %4d", T, A.Gold,
                           A.Workers, A.Soldiers, A.Buildings, A.Mines, A.Max, B.Gold, B.Workers,
                           B.Soldiers, B.Buildings, B.Mines, B.Max);
        }
    }
    auto Ms = [](int T) { return T < 0 ? -1.0 : static_cast<double>(T) / Rps::TickRateHz; };
    Lur::Log::Info("AI diag RESULT (%s): camp %.1fs | first miner %.1fs | 2nd building %.1fs | "
                   "first soldier %.1fs   (-1.0 = NEVER)",
                   Rps::AiTierName(Tier), Ms(TCamp), Ms(TMiner), Ms(TBldg), Ms(TSoldier));
    const Census A = Look(0), B = Look(1);
    Lur::Log::Info("  t0 ASKED: place x%d (camps %d) -> %d landed | queue x%d for %d units",
                   AskPlace, AskPlaceUnits, GotPlace, AskQueue, AskQueueUnits);
    Lur::Log::Info("  t0 IDLE ticks (emitted nothing) open/build/react/allin %d/%d/%d/%d | avg gold while idle in build %lld",
                   IdleTicks[0], IdleTicks[1], IdleTicks[2], IdleTicks[3],
                   static_cast<long long>(IdleTicks[1] > 0 ? IdleGold[1] / IdleTicks[1] : 0));
    Lur::Log::Info("  final: t0 gold=%d wrk=%d sol=%d bld=%d | t1 gold=%d wrk=%d sol=%d bld=%d | "
                   "result=%u at tick %u",
                   A.Gold, A.Workers, A.Soldiers, A.Buildings, B.Gold, B.Workers, B.Soldiers,
                   B.Buildings, static_cast<unsigned>(S->Result), S->Tick);
    return 0;
}

// The OWNER diagnostic: the AI against a scripted re-implementation of the line that actually beats
// it (OwnerBot.h). The tier's target is "he wins about 1 in 10", and this is the only harness that
// reports that number — --aivs plays the wrong opponent and --aibeginner plays no opponent at all.
//
// Read the win column, but read the CAMP and BUILDING columns too: he wins on capacity (34 buildings
// to the AI's 8) and on where his camps go (depth 121 vs 37), so those are the terms that have to
// move, not just the tally.
int RunAiOwner(Rps::EAiTier Tier, uint64_t BaseSeed, int Matches, int MaxTicks) {
#if !LUR_SHIPPING
    Lur::Core::LoadCVarConfig("rps-cvars.cfg");
#endif
    Rps::OwnerBot::Params P{};
    Lur::Log::Info("AI-vs-OWNER: tier=%s, %d matches, cap %d ticks (%.0fs) | owner line: %d camps, "
                   "army from t%d, +%d/+%d batches, 1 act/%d ticks",
                   Rps::AiTierName(Tier), Matches, MaxTicks,
                   static_cast<double>(MaxTicks) / Rps::TickRateHz, P.CampTarget, P.ArmyStartTick,
                   P.EarlyBatch, P.LateBatch, P.ActEveryTicks);
    int OwnerWins = 0, AiWins = 0, Unresolved = 0;
    long long SumTicks = 0, SumOwnerBld = 0, SumAiBld = 0, SumOwnerWrk = 0, SumAiWrk = 0;
    // #158 acceptance evidence, and it has to be measured rather than eyeballed off a 300-tick trace:
    // whether all three soldier types are still alive at the end (a mix that collapses under attrition
    // is not a mix), and the PEAK share any one type reached at any sample — the share cap governs the
    // target, so the realised drift away from it is the number that says whether the cap is doing its
    // job or being outvoted by the fallback.
    long long SumAiSol[3] = {0, 0, 0};
    int AllThreeAlive = 0, PeakSharePct = 0;
    for (int M = 0; M < Matches; ++M) {
        const uint64_t Seed = BaseSeed + static_cast<uint64_t>(M);
        auto S = std::make_unique<Rps::Sim>();
        S->Init(Seed);
        Rps::AiController Ai;
        Ai.Init(Seed, 1, Tier);
        Rps::OwnerBot Owner;
        Owner.Init(0, P);
        int T = 0;
        int MatchPeakShare = 0;
        // One match -> print the census. A win tally says WHO won; only the trajectory says which
        // term lost it, and "the AI ends on 74 workers to the owner's 330" is not the same bug as
        // "the AI ends on 520 workers", which is what the device recordings showed against a human.
        const bool Trace = Matches == 1;
        // The COMPOSITION columns are why this trace exists in its current form (#158). The tally
        // said the AI lost while ahead on workers AND buildings, which rules out every economic
        // explanation and leaves only "what is its army made of" — and an aggregate soldier count
        // cannot answer that. r/p/s are units per type; the AI's target mix (permille -> percent) is
        // printed beside its actuals, so "the plan is wrong" and "it is failing to execute the plan"
        // are visibly different failures.
        if (Trace)
            Lur::Log::Info("  tick | owner: wrk bld | r/p/s | ai: wrk bld | r/p/s | want r/p/s | state");
        for (; T < MaxTicks && S->Result == Rps::ResultOngoing; ++T) {
            if (Trace && (T % 300) == 0) {
                int32_t W[2] = {}, B[2] = {}, Sq[2][3] = {};
                for (int32_t I = 0; I < S->Count; ++I) {
                    if (!S->IsAlive(I)) continue;
                    const int Tm = S->Team[I] & 1;
                    const uint8_t Ty = S->Type[I];
                    if (S->IsBuilding(I)) { if (!S->IsHomeBase(I)) ++B[Tm]; }
                    else if (Ty == Rps::UnitMiner) ++W[Tm];
                    else if (Ty >= Rps::UnitRock && Ty <= Rps::UnitScissor) ++Sq[Tm][Ty - Rps::UnitRock];
                }
                static const char* StN[] = {"open", "build", "react", "allin"};
                const int St = static_cast<int>(Ai.State());
                Lur::Log::Info("  %5d | %14d %3d | %4d/%4d/%4d | %8d %3d | %4d/%4d/%4d | %3d/%3d/%3d | %s",
                               T, W[0], B[0], Sq[0][0], Sq[0][1], Sq[0][2], W[1], B[1], Sq[1][0],
                               Sq[1][1], Sq[1][2], Ai.MixShare(0) / 10, Ai.MixShare(1) / 10,
                               Ai.MixShare(2) / 10, St >= 0 && St < 4 ? StN[St] : "?");
            }
            // Peak-share sampling runs on EVERY match, not just the traced one, and on a finer grid
            // than the trace: a cap breach is a transient, and a 300-tick trace of one seed would
            // miss it. Once the AI has an army worth talking about (a 3-unit army is trivially 100%
            // of one type and says nothing about the mix).
            if ((T % 100) == 0) {
                int32_t Q[3] = {};
                for (int32_t I = 0; I < S->Count; ++I) {
                    if (!S->IsAlive(I) || S->IsBuilding(I) || (S->Team[I] & 1) != 1) continue;
                    const uint8_t Ty = S->Type[I];
                    if (Ty >= Rps::UnitRock && Ty <= Rps::UnitScissor) ++Q[Ty - Rps::UnitRock];
                }
                const int32_t Tot = Q[0] + Q[1] + Q[2];
                if (Tot >= 10)
                    for (int K = 0; K < 3; ++K)
                        if (Q[K] * 100 / Tot > MatchPeakShare) MatchPeakShare = Q[K] * 100 / Tot;
            }
            Rps::InputEvent E[2 * Rps::MaxEventsPerTick];
            int N = 0;
            Owner.DecideEvents(*S, S->Tick, E, Rps::MaxEventsPerTick, N);
            // Same gate the real solo path applies: the AI holds until the player commits a camp.
            if (S->HasMinerCamp(0)) {
                int C = 0;
                Ai.DecideEvents(*S, S->Tick, E + N, static_cast<int>(Rps::MaxEventsPerTick) - N, C);
                N += C;
            }
            S->StepEvents(E, N);
        }
        int32_t Bld[2] = {}, Wrk[2] = {}, AiSol[3] = {};
        for (int32_t I = 0; I < S->Count; ++I) {
            if (!S->IsAlive(I)) continue;
            const int Tm = S->Team[I] & 1;
            const uint8_t Ty = S->Type[I];
            if (S->IsBuilding(I)) { if (!S->IsHomeBase(I)) ++Bld[Tm]; }
            else if (Ty == Rps::UnitMiner) ++Wrk[Tm];
            else if (Tm == 1 && Ty >= Rps::UnitRock && Ty <= Rps::UnitScissor) ++AiSol[Ty - Rps::UnitRock];
        }
        SumTicks += T; SumOwnerBld += Bld[0]; SumAiBld += Bld[1];
        SumOwnerWrk += Wrk[0]; SumAiWrk += Wrk[1];
        for (int K = 0; K < 3; ++K) SumAiSol[K] += AiSol[K];
        if (AiSol[0] > 0 && AiSol[1] > 0 && AiSol[2] > 0) ++AllThreeAlive;
        if (MatchPeakShare > PeakSharePct) PeakSharePct = MatchPeakShare;
        if (S->Result == Rps::ResultTeam0Wins) ++OwnerWins;
        else if (S->Result == Rps::ResultTeam1Wins) ++AiWins;
        else ++Unresolved;
    }
    const double Inv = Matches > 0 ? 1.0 / Matches : 0.0;
    Lur::Log::Info("AI-vs-OWNER RESULT: owner %d | %s %d | unresolved %d  (of %d)", OwnerWins,
                   Rps::AiTierName(Tier), AiWins, Unresolved, Matches);
    Lur::Log::Info("  owner win rate %.0f%% (target ~10%%) | avg %.0fs | avg bld owner=%.1f ai=%.1f "
                   "| avg wrk owner=%.1f ai=%.1f",
                   Matches > 0 ? 100.0 * OwnerWins / Matches : 0.0,
                   SumTicks * Inv / Rps::TickRateHz, SumOwnerBld * Inv, SumAiBld * Inv,
                   SumOwnerWrk * Inv, SumAiWrk * Inv);
    Lur::Log::Info("  ai final army r/p/s = %.1f/%.1f/%.1f | all three alive at end in %d/%d | peak "
                   "one-type share %d%%",
                   SumAiSol[0] * Inv, SumAiSol[1] * Inv, SumAiSol[2] * Inv, AllThreeAlive, Matches,
                   PeakSharePct);
    return 0;
}

// #155: headless BEGINNER diagnostic — the one question --aivs and --aidiag both refuse to answer.
// Both of those measure the AI against a COMPETENT opponent (another tier), and tier strength there
// is ordered by economy: `easy` is weak on the ladder precisely BECAUSE it starves its economy and
// dumps everything into soldiers, which is an early rush. A rush is a liability against `hard` and a
// massacre against a first-timer, so the ladder's own metric is blind to the thing that decides
// whether a new player ever learns the game.
//
// The proxy for that new player is deliberately the FLOOR: team 0 places one mining camp (which is
// what starts the match — the mains gate the AI on exactly that, DesktopMain.cpp RunSolo) and then
// does nothing at all for the rest of the match. Everything the AI then achieves, it achieves
// against someone who is still reading the screen.
//
// What it reports is the arrival clock, not a win tally: when the AI's first soldier EXISTS, when it
// crosses midfield, and when it reaches the player's build zone — that last one is the moment a
// beginner's match is effectively decided, and it is the number the grace window is specified in.
int RunAiBeginner(Rps::EAiTier Tier, uint64_t BaseSeed, int Matches, int MaxTicks, int EveryTicks) {
#if !LUR_SHIPPING
    Lur::Core::LoadCVarConfig("rps-cvars.cfg");
#endif
    
    // The player's half is everything below midfield; their build zone is the opening frontier depth.
    const int32_t Mid = Rps::WorldHeight.ToInt() / 2;
    Lur::Log::Info("AI-vs-beginner: tier=%s, %d matches, cap %d ticks (%.0fs), beginner = camp then idle",
                   Rps::AiTierName(Tier), Matches, MaxTicks,
                   static_cast<double>(MaxTicks) / Rps::TickRateHz);
    auto Ms = [](int T) { return T < 0 ? -1.0 : static_cast<double>(T) / Rps::TickRateHz; };
    long long SumSoldier = 0, SumMid = 0, SumZone = 0;
    int GotSoldier = 0, GotMid = 0, GotZone = 0, Wiped = 0;
    for (int M = 0; M < Matches; ++M) {
        const uint64_t MatchSeed = BaseSeed + static_cast<uint64_t>(M);
        auto S = std::make_unique<Rps::Sim>();   // ~MBs of SoA: heap, not the 1 MB main stack (#94)
        S->Init(MatchSeed);
        Rps::AiController Ai;
        Ai.Init(MatchSeed, 1, Tier);
        const int32_t Zone = S->Cv.InitialFrontier.ToInt();
        // The beginner's ONLY act: drop a camp on the first legal spot near their own baseline (a
        // first-timer plants it at home, not on a distant mine). Fires as tick 0's input, so the
        // match clock starts here exactly as it does for a human.
        bool Opened = false;
        int TSoldier = -1, TMid = -1, TZone = -1, TWipe = -1;
        // The AI's economy at the milestones the RECORDINGS measured, so its ramp can be compared
        // straight against a real beginner's. From 16 recorded losses (2026-07-26), the better-half
        // beginner ran workers 4.9 / 11.2 / 24.1 / 41.6 and ~1.5 / 2.2 / 3.2 / 5.2 buildings at
        // 60/90/120/150s. Pacing easy to that is the acceptance test for "it builds like a
        // first-timer" — and a win tally cannot see it at all.
        int32_t MWrk[4] = {-1, -1, -1, -1}, MSol[4] = {-1, -1, -1, -1}, MBld[4] = {-1, -1, -1, -1};
        for (int T = 0; T < MaxTicks && S->Result == Rps::ResultOngoing; ++T) {
            Rps::InputEvent E[2 * Rps::MaxEventsPerTick];
            int N = 0;
            if (!Opened) {
                for (int R = 0; R < 6 && !Opened; ++R)
                    for (const int32_t X : {8, 14, 20, 26}) {
                        const Rps::Fixed Px = Rps::F(X), Py = Rps::F(5 + R * 4);
                        if (!S->CanPlaceBuilding(0, Rps::UnitMiner, Px, Py)) continue;
                        E[N++] = Rps::InputEvent::Place(0, Rps::UnitMiner, Px, Py);
                        Opened = true;
                        break;
                    }
            }
            // The AI holds until the player commits that camp — the same gate the real solo path
            // applies, so the AI's economy never starts on time the player has not yet spent.
            if (S->HasMinerCamp(0)) {
                int C = 0;
                Ai.DecideEvents(*S, S->Tick, E + N, static_cast<int>(Rps::MaxEventsPerTick) - N, C);
                N += C;
            }
            S->StepEvents(E, N);
            // Where is the AI's army, and how far has it walked into the player's ground?
            int32_t Front = Rps::WorldHeight.ToInt();   // team 1 advances DOWN, so frontmost = min Y
            int32_t Soldiers = 0, T0Bld = 0, AiWorkers = 0, AiBld = 0;
            bool T0Hq = false;
            for (int32_t I = 0; I < S->Count; ++I) {
                if (!S->IsAlive(I)) continue;
                if (S->Team[I] == 0) {
                    // The HQ specifically, not any building: losing IS the HQ dying (#146), and the
                    // player's mining camp routinely outlives it — so counting all buildings reported
                    // "never razed" for a match the player had already lost.
                    if (S->IsHomeBase(I)) T0Hq = true;
                    if (S->IsBuilding(I)) ++T0Bld;
                    continue;
                }
                if (S->IsBuilding(I)) { if (!S->IsHomeBase(I)) ++AiBld; continue; }
                if (S->Type[I] == Rps::UnitMiner) { ++AiWorkers; continue; }
                ++Soldiers;
                const int32_t Y = S->PosY[I].ToInt();
                if (Y < Front) Front = Y;
            }
            for (int Mi = 0; Mi < 4; ++Mi) {
                const int32_t MT = (60 + 30 * Mi) * static_cast<int32_t>(Rps::TickRateHz);
                if (T == MT) { MWrk[Mi] = AiWorkers; MSol[Mi] = Soldiers; MBld[Mi] = AiBld; }
            }
            if (TSoldier < 0 && Soldiers > 0) TSoldier = T;
            if (TMid < 0 && Soldiers > 0 && Front < Mid) TMid = T;
            if (TZone < 0 && Soldiers > 0 && Front < Zone) TZone = T;
            if (TWipe < 0 && !T0Hq && Opened) TWipe = T;
            if (EveryTicks > 0 && T % EveryTicks == 0 && Matches == 1)
                Lur::Log::Info("%6d | ai gold=%6d sol=%4d front=%3d | player bld=%d hq=%d", T,
                               S->Teams[1].Gold, Soldiers, Front, T0Bld, T0Hq ? 1 : 0);
        }
        if (TSoldier >= 0) { SumSoldier += TSoldier; ++GotSoldier; }
        if (TMid >= 0)     { SumMid += TMid;         ++GotMid; }
        if (TZone >= 0)    { SumZone += TZone;       ++GotZone; }
        if (S->Result == Rps::ResultTeam1Wins) ++Wiped;
        Lur::Log::Info("    AI curve 60/90/120/150s:  workers %d/%d/%d/%d  soldiers %d/%d/%d/%d  "
                       "buildings %d/%d/%d/%d   (better-half beginner: wrk 5/11/24/42, bld 2/2/3/5)",
                       MWrk[0], MWrk[1], MWrk[2], MWrk[3], MSol[0], MSol[1], MSol[2], MSol[3],
                       MBld[0], MBld[1], MBld[2], MBld[3]);
        Lur::Log::Info("  seed %#llx: first soldier %.0fs | midfield %.0fs | PLAYER ZONE %.0fs | "
                       "player HQ dead %.0fs",
                       static_cast<unsigned long long>(MatchSeed), Ms(TSoldier), Ms(TMid), Ms(TZone),
                       Ms(TWipe));
    }
    auto Avg = [&](long long Sum, int N) { return N == 0 ? -1.0 : Ms(static_cast<int>(Sum / N)); };
    Lur::Log::Info("AI-vs-beginner RESULT (%s): avg first soldier %.0fs | avg midfield %.0fs | "
                   "avg PLAYER ZONE %.0fs (%d/%d reached) | player wiped %d/%d",
                   Rps::AiTierName(Tier), Avg(SumSoldier, GotSoldier), Avg(SumMid, GotMid),
                   Avg(SumZone, GotZone), GotZone, Matches, Wiped, Matches);
    return 0;
}

#if LUR_INTERNAL
// #144: read back a match recorded on a device (Rps::MatchRecorder) and print it. The sim is
// deterministic and the recording carries the seed + the exact latched CVar set, so the replay is
// the same match bit-for-bit — the census below is the ACTUAL game that was played, not a model of
// it. Prints the recorded live census (which carries the AI's state + countered type, unavailable
// after the fact) alongside a replayed one at the same cadence, then the event profile: what each
// side spent its decisions on, which is where a human's edge over the AI shows up.
// ---- --recdiff A.rec B.rec: compare TWO peers' recordings of the SAME linked match (#159) ----
// The two phones execute the identical combined event stream, so the pair of files answers the one
// question a single recording cannot: when they disagreed, WHO was wrong about what?
//
//   * the event streams differ           -> the wire lost or duplicated a frame; the first differing
//                                           tick is where, and the two lines say what was missing;
//   * the streams agree, the hashes not  -> the sims computed different results from identical input,
//                                           i.e. nondeterminism, and the first differing anchor
//                                           brackets it to within 10 ticks.
// Those are different bugs with different fixes, and telling them apart used to require guessing.
//
// Text-diffing the files with `diff` also works and is the reason the format is line-oriented — but
// the two files legitimately differ in the header (each peer records its OWN team) and in census
// lines (wall-clock timed, so they land on different ticks). This compares only what MUST match.
int RunRecDiff(const char* PathA, const char* PathB) {
    const Rps::MatchRecording A = Rps::LoadMatchRecording(PathA);
    const Rps::MatchRecording B = Rps::LoadMatchRecording(PathB);
    if (!A.Ok || !B.Ok) {
        Lur::Log::Error("recdiff: %s is not a valid recording", !A.Ok ? PathA : PathB);
        return 1;
    }
    Lur::Log::Info("recdiff A=%s B=%s", PathA, PathB);
    Lur::Log::Info("  A: seed=%llx human=team%u build=%s events=%zu hashes=%zu end=tick %u result=%d",
                   static_cast<unsigned long long>(A.Seed), static_cast<unsigned>(A.HumanTeam),
                   A.BuildFp.c_str(), A.Events.size(), A.Hashes.size(), A.EndTick, A.Result);
    Lur::Log::Info("  B: seed=%llx human=team%u build=%s events=%zu hashes=%zu end=tick %u result=%d",
                   static_cast<unsigned long long>(B.Seed), static_cast<unsigned>(B.HumanTeam),
                   B.BuildFp.c_str(), B.Events.size(), B.Hashes.size(), B.EndTick, B.Result);
    int Problems = 0;
    // Preconditions first: these are not "divergence", they are "you are comparing the wrong things",
    // and reporting a tick number for them would send someone hunting a sim bug that isn't there.
    if (A.Seed != B.Seed) { Lur::Log::Error("  SEED differs — not the same match"); ++Problems; }
    if (A.BuildFp != B.BuildFp)
        Lur::Log::Error("  BUILD differs (%s vs %s) — a divergence below may be nothing but that",
                        A.BuildFp.c_str(), B.BuildFp.c_str());
    if (A.HumanTeam == B.HumanTeam)
        Lur::Log::Info("  note: both files record human=team%u — same-side captures, so these are "
                       "probably not the two peers of one match",
                       static_cast<unsigned>(A.HumanTeam));
    // The latched CVar set: if the two peers simulated on different tunables, EVERYTHING after tick 0
    // diverges and the first differing tick is meaningless. Check it before anything else.
    bool CvDiffers = false;
    for (uint8_t Id = 0; Id < Rps::CvIdCount; ++Id) {
        const int32_t Ra = Rps::CvOverrideRaw(A.Cv, Id), Rb = Rps::CvOverrideRaw(B.Cv, Id);
        if (Ra != Rb) {
            Lur::Log::Error("  CV %s (id %u) differs: A=%d B=%d — the peers did not agree on tunables",
                            Rps::GameplayNameForId(Id), static_cast<unsigned>(Id), Ra, Rb);
            CvDiffers = true;
            ++Problems;
        }
    }
    // EVENTS: compare per tick, since a tick with no events writes no line at all.
    auto EventsAt = [](const Rps::MatchRecording& R, std::size_t& I, uint32_t Tick) {
        std::vector<Rps::InputEvent> Out;
        while (I < R.Events.size() && R.Events[I].Tick == Tick) Out.push_back(R.Events[I++].Event);
        return Out;
    };
    const uint32_t Last = A.EndTick > B.EndTick ? A.EndTick : B.EndTick;
    // #208: a recording may START MID-MATCH. A peer that was killed and rejoined resumes at the
    // resync frontier, so its file legitimately has no tick 0 — and comparing from 0 then reports
    // "EVENTS differ at tick 0: A has 0, B has 2 ... the WIRE dropped or duplicated a frame" about a
    // perfectly healthy rejoin. That is the tool inventing a transport bug out of its own choice of
    // starting point, which is the failure mode this diff exists to avoid.
    //
    // Coverage is keyed off the first HASH, not the first event: a from-start recording routinely
    // has no events for dozens of ticks, so "no events yet" says nothing about whether the file
    // covers that tick. Hashes are written on a fixed cadence, so the first one marks where the
    // file's knowledge begins. No hashes at all (solo, or pre-#159) means assume from the top. The
    // hash comparison below needs no such clamp — it already intersects by tick.
    auto FirstCovered = [](const Rps::MatchRecording& R) -> uint32_t {
        return R.Hashes.empty() ? 0u : R.Hashes.front().Tick;
    };
    const uint32_t FirstA = FirstCovered(A), FirstB = FirstCovered(B);
    const uint32_t Begin = FirstA > FirstB ? FirstA : FirstB;
    if (Begin > 0) {
        // Say it, rather than quietly comparing less than the reader assumes.
        Lur::Log::Info("  note: comparing from tick %u — %s starts mid-match (A from %u, B from %u), "
                       "so ticks below that exist in only one file", Begin,
                       FirstA > FirstB ? PathA : PathB, FirstA, FirstB);
    }
    std::size_t Ia = 0, Ib = 0;
    int32_t FirstEventDiff = -1;
    for (uint32_t T = Begin; T <= Last; ++T) {
        while (Ia < A.Events.size() && A.Events[Ia].Tick < T) ++Ia;
        while (Ib < B.Events.size() && B.Events[Ib].Tick < T) ++Ib;
        const std::vector<Rps::InputEvent> Ea = EventsAt(A, Ia, T), Eb = EventsAt(B, Ib, T);
        bool Same = Ea.size() == Eb.size();
        for (std::size_t K = 0; Same && K < Ea.size(); ++K)
            Same = Ea[K].Team == Eb[K].Team && Ea[K].Kind == Eb[K].Kind && Ea[K].Type == Eb[K].Type &&
                   Ea[K].X == Eb[K].X && Ea[K].Y == Eb[K].Y;
        if (!Same) {
            FirstEventDiff = static_cast<int32_t>(T);
            Lur::Log::Error("  EVENTS differ at tick %u: A has %zu, B has %zu — the WIRE dropped or "
                            "duplicated a frame (not a sim bug)", T, Ea.size(), Eb.size());
            for (const Rps::InputEvent& E : Ea)
                Lur::Log::Info("    A: team=%u kind=%u type=%u x=%d y=%d", static_cast<unsigned>(E.Team),
                               static_cast<unsigned>(E.Kind), static_cast<unsigned>(E.Type), E.X, E.Y);
            for (const Rps::InputEvent& E : Eb)
                Lur::Log::Info("    B: team=%u kind=%u type=%u x=%d y=%d", static_cast<unsigned>(E.Team),
                               static_cast<unsigned>(E.Kind), static_cast<unsigned>(E.Type), E.X, E.Y);
            ++Problems;
            break;
        }
    }
    // HASHES: only meaningful once the inputs are known identical up to that tick, so this is
    // reported second and explicitly framed against the event result.
    int32_t FirstHashDiff = -1;
    for (const Rps::RecordedHash& Ha : A.Hashes) {
        for (const Rps::RecordedHash& Hb : B.Hashes) {
            if (Hb.Tick != Ha.Tick) continue;
            if (Ha.Hash != Hb.Hash) {
                FirstHashDiff = static_cast<int32_t>(Ha.Tick);
                Lur::Log::Error("  HASH differs at tick %u: A=%016llx B=%016llx", Ha.Tick,
                                static_cast<unsigned long long>(Ha.Hash),
                                static_cast<unsigned long long>(Hb.Hash));
                ++Problems;
            }
            break;
        }
        if (FirstHashDiff >= 0) break;
    }
    if (A.Hashes.empty() || B.Hashes.empty())
        Lur::Log::Info("  note: no hash lines in %s — a solo or pre-#159 recording, so only the event "
                       "streams could be compared", A.Hashes.empty() ? PathA : PathB);
    // The verdict, stated so it points at ONE of the two bugs rather than at "something is wrong".
    //
    // A tunables (or seed) mismatch OUTRANKS a hash divergence, and that ordering is the whole point:
    // two sims fed different Cv diverge immediately and legitimately, so the hashes differing is the
    // SYMPTOM, not the finding. The first real two-phone pair ever diffed (2026-08-01 10:39, #171) hit
    // exactly this — miner-building 400 vs 600, starting-gold 750 vs 800, hashes apart at tick 0 — and
    // the verdict read "the sim is not deterministic across these two builds/platforms" on a pair whose
    // build fingerprints were IDENTICAL. That sends the reader hunting cross-compiler nondeterminism
    // (#159's candidate 2) when the answer, printed four lines above, is that the cvar sync never
    // converged (#169). A tool that buries its own finding under a confident wrong headline is worse
    // than one that says nothing.
    //
    // An EVENT divergence still outranks the tunables mismatch, though: the recorded batch is what
    // each peer EXECUTED, and both peers execute the identical combined stream by construction — so
    // the two streams differing is a wire fact that different Cv does not explain, and it is the
    // finding with the shorter path to a fix.
    if (FirstEventDiff >= 0) {
        Lur::Log::Info("VERDICT: input streams diverged first, at tick %d. Look at the transport, not "
                       "the sim.", FirstEventDiff);
        if (CvDiffers || A.Seed != B.Seed)
            Lur::Log::Info("         (the peers ALSO disagree on %s — a separate bug, fix both)",
                           A.Seed != B.Seed ? "seeds" : "tunables");
    } else if (CvDiffers || A.Seed != B.Seed)
        Lur::Log::Info("VERDICT: the peers did not simulate the same match — %s (above). Everything "
                       "downstream, hashes included%s, follows from that; fix the mismatch and re-run "
                       "before reading any tick number here as a sim bug.",
                       A.Seed != B.Seed ? "different SEEDS" : "different TUNABLES",
                       FirstHashDiff >= 0 ? "" : " (which happen to still agree)");
    else if (FirstHashDiff >= 0)
        Lur::Log::Info("VERDICT: identical inputs and identical tunables, state diverged by tick %d "
                       "(so between tick %d and %d). The sim is not deterministic across these two "
                       "builds/platforms.", FirstHashDiff, FirstHashDiff - 10, FirstHashDiff);
    else if (Problems > 0)
        // Preconditions failed but nothing diverged. This printed NO verdict at all on its first real
        // run (two phones whose headers disagreed on tunables), which reads as "the tool gave up" —
        // and the honest answer is the opposite: the streams agreed, so look at the headers.
        Lur::Log::Info("VERDICT: no divergence in the events or hashes, but the two files disagree on "
                       "their PRECONDITIONS (above). Fix that first — a header that misstates the "
                       "match makes every tick number here meaningless.");
    else
        Lur::Log::Info("VERDICT: the two recordings agree on every compared tick.");
    return Problems == 0 ? 0 : 2;
}

int RunReplay(const char* Path, int EveryTicks) {
    const Rps::MatchRecording R = Rps::LoadMatchRecording(Path);
    if (!R.Ok) {
        Lur::Log::Error("replay: %s is not a valid recording", Path);
        return 1;
    }
    
    Lur::Log::Info("replay %s: seed=%llx tier=%s human=team%u events=%zu census=%zu end=tick %u result=%d",
                   Path, static_cast<unsigned long long>(R.Seed),
                   R.Tier >= 0 && R.Tier < Rps::AiTierCount ? Rps::AiTierName(static_cast<Rps::EAiTier>(R.Tier)) : "?", static_cast<unsigned>(R.HumanTeam),
                   R.Events.size(), R.Census.size(), R.EndTick, R.Result);
    Lur::Log::Info("  build: %s", R.BuildFp.c_str());

    auto S = std::make_unique<Rps::Sim>();
    const uint64_t Hash = Rps::ReplayMatch(R, *S);
    Lur::Log::Info("  replayed to tick %u, hash %016llx", S->Tick, static_cast<unsigned long long>(Hash));

    // ---- SHADOW AI: what would TODAY's AI decide on the board this match actually reached? ----
    // The one question a win tally cannot answer. --aivs measures the AI against another AI and
    // --aibeginner against someone who does nothing; neither plays like the person who beat it. The
    // recording does — it IS that person's match — so we re-step it and run a fresh AiController
    // alongside, fed the real board every tick, with its events THROWN AWAY (the recorded events
    // drive the sim, so the board stays bit-identical to the match that happened).
    //
    // That makes it an observer, not a counterfactual: it cannot tell you the AI would have WON,
    // only what it would have decided at each moment the loser was losing. For a decision defect —
    // committing to an attack the enemy's production answers on its own — that is the whole
    // question. Reading "allin at 150s" against a player holding 11 soldiers and 12 buildings is
    // the bug, in one line, on the actual board.
    {
        auto Sh = std::make_unique<Rps::Sim>();
        Sh->InitWithCvs(R.Seed, R.Cv);
        const uint8_t AiTeam = static_cast<uint8_t>(1 - R.HumanTeam);
        Rps::AiController Shadow;
        Shadow.Init(R.Seed, AiTeam, R.Tier >= 0 && R.Tier < Rps::AiTierCount ? static_cast<Rps::EAiTier>(R.Tier)
                                                              : Rps::EAiTier::Hard);
        std::size_t Next = 0, NextCen = 0;
        int32_t FirstAllin = -1, Diverged = -1;
        int StateTicks[4] = {};
        int32_t IdleTicks = 0, CurSilence = 0, LongestSilence = 0, SilenceGold = 0;
        for (uint32_t T = 0; T < R.EndTick; ++T) {
            // FIDELITY GATE. A recording is only replayable by the build that made it: the sim is
            // deterministic, so ANY sim change (a different cart-targeting tie-break is enough)
            // sends the re-simulation down a different match from the same seed and events, and a
            // self-consistent StateHash will not tell you — it only proves two replays agree with
            // each other. The RECORDED census is the live truth, so compare against that. Past the
            // first disagreement the shadow is watching a board that never existed, and its verdict
            // means nothing.
            while (NextCen < R.Census.size() && R.Census[NextCen].Tick < T) ++NextCen;
            if (Diverged < 0 && NextCen < R.Census.size() && R.Census[NextCen].Tick == T) {
                const Rps::RecordedCensus& C = R.Census[NextCen];
                int32_t W[2] = {}, So[2] = {}, B[2] = {};
                for (int32_t I = 0; I < Sh->Count; ++I) {
                    if (!Sh->IsAlive(I)) continue;
                    const int Tm = Sh->Team[I] & 1;
                    if (Sh->Kind[I] == Rps::KindBuilding) ++B[Tm];
                    else if (Sh->Kind[I] == Rps::KindUnit) {
                        if (Sh->Type[I] == Rps::UnitMiner) ++W[Tm]; else ++So[Tm];
                    }
                }
                const int H = R.HumanTeam & 1, A = 1 - H;
                if (W[H] != C.Workers[0] || So[H] != C.Soldiers[0] || B[H] != C.Buildings[0] ||
                    W[A] != C.Workers[1] || So[A] != C.Soldiers[1] || B[A] != C.Buildings[1])
                    Diverged = static_cast<int32_t>(T);
            }
            Rps::InputEvent Ev[2 * Rps::MaxEventsPerTick];
            int N = 0;
            Shadow.DecideEvents(*Sh, Sh->Tick, Ev, Rps::MaxEventsPerTick, N);
            // N is the whole point, not a leftover: a tick on which the AI emits NOTHING is the
            // failure this harness exists to catch. The 2026-07-28 recordings had the top tier silent
            // for 16-22s at a stretch while holding ~3945 gold (a scissor building is 4000), idle 45%
            // of the owner's fastest win — and a state-occupancy readout cannot see that at all,
            // because "Reacting" looks identical whether it is acting or frozen. So count silence, and
            // count the LONGEST run of it, which is what actually loses the match.
            if (N == 0) {
                ++IdleTicks;
                if (++CurSilence > LongestSilence) {
                    LongestSilence = CurSilence;
                    SilenceGold = Sh->Teams[AiTeam].Gold;
                }
            } else {
                CurSilence = 0;
            }
            const int St = static_cast<int>(Shadow.State());
            if (St >= 0 && St < 4) ++StateTicks[St];
            if (Shadow.State() == Rps::AiController::EState::AllIn && FirstAllin < 0)
                FirstAllin = static_cast<int32_t>(T);
            N = 0;
            while (Next < R.Events.size() && R.Events[Next].Tick == T) {
                if (N < static_cast<int>(2 * Rps::MaxEventsPerTick)) Ev[N++] = R.Events[Next].Event;
                ++Next;
            }
            Sh->StepEvents(Ev, N);
        }
        if (Diverged >= 0) {
            Lur::Log::Info("  shadow AI: UNUSABLE — re-simulation left the recorded match at tick %d "
                           "(%.1fs). This build's sim is not the one that recorded it (%s); the census "
                           "above is still live truth, the shadow verdict is not.",
                           Diverged, static_cast<double>(Diverged) / Rps::TickRateHz, R.BuildFp.c_str());
        } else {
            Lur::Log::Info("  shadow AI (today's logic, on the REAL board — census matched all "
                           "%zu samples): first ALL-IN %s | ticks open/build/react/allin %d/%d/%d/%d",
                           R.Census.size(),
                           FirstAllin < 0 ? "never"
                                          : (std::to_string(static_cast<double>(FirstAllin) / Rps::TickRateHz) + "s").c_str(),
                           StateTicks[0], StateTicks[1], StateTicks[2], StateTicks[3]);
            Lur::Log::Info("  shadow AI silence: idle %d/%u ticks (%d%%) | longest unbroken %.1fs, "
                           "holding %d gold at that moment",
                           IdleTicks, R.EndTick,
                           R.EndTick > 0 ? 100 * IdleTicks / static_cast<int32_t>(R.EndTick) : 0,
                           static_cast<double>(LongestSilence) / Rps::TickRateHz, SilenceGold);
        }
    }

    // The recorded census: the live truth, including the AI's internals.
    const char* States[] = {"open", "build", "react", "allin"};
    const char* Types[] = {"miner", "rock", "paper", "scissor"};
    Lur::Log::Info("  tick |  you: gold  wrk  sol  bld |   ai: gold  wrk  sol  bld | ai state / countering");
    for (const Rps::RecordedCensus& C : R.Census) {
        if (EveryTicks > 0 && C.Tick % static_cast<uint32_t>(EveryTicks) >= 20) continue;  // thin it out
        const char* St = C.AiState >= 0 && C.AiState < 4 ? States[C.AiState] : "?";
        const char* Ct = C.AiCounter >= 0 && C.AiCounter < 4 ? Types[C.AiCounter] : "none";
        Lur::Log::Info("%6u | %10d %4d %4d %4d | %10d %4d %4d %4d | %-5s %s", C.Tick, C.Gold[0],
                       C.Workers[0], C.Soldiers[0], C.Buildings[0], C.Gold[1], C.Workers[1],
                       C.Soldiers[1], C.Buildings[1], St, Ct);
    }
    // Decision profile per side: placements by type + queue volume. A human who wins with far fewer
    // decisions is exploiting something structural, not out-clicking the AI.
    int32_t Places[2][4] = {}, Queues[2][2] = {};   // [team][type], [team][{batches, units}]
    for (const Rps::RecordedEvent& E : R.Events) {
        const int T = E.Event.Team & 1;
        if (E.Event.Kind == Rps::EventPlaceBuilding) {
            if (E.Event.Type < 4) ++Places[T][E.Event.Type];
        } else {
            ++Queues[T][0];
            Queues[T][1] += E.Event.Y;   // Queue packs the count in Y
        }
    }
    for (int T = 0; T < 2; ++T)
        Lur::Log::Info("  %s placed camp=%d rock=%d paper=%d scissor=%d | queued %d batches / %d units",
                       T == R.HumanTeam ? "you" : "ai ", Places[T][0], Places[T][1], Places[T][2],
                       Places[T][3], Queues[T][0], Queues[T][1]);
    return 0;
}
#endif

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
    Lur::Render::IRenderer* Renderer = Lur::Render::VulkanRenderer::Create("OnlyRps");
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
    // #157: a commit also REQUESTS a pre-match map rebuild. The map (mine rows) is built in
    // Sim::Init, so a knob edit is otherwise invisible until the next match — but while a fresh
    // match still waits for your opening camp there is nothing to lose by re-Initing, so the new
    // layout shows at once. Only a flag here: the restart is a Runner Stop/Start and belongs on the
    // main loop, not inside a console callback. Ctx-passed rather than a global.
    bool RebuildPreMatch = false;
    View.SetCvCommitHook(
        [](void* Ctx, Lur::Core::ICVar&) {
            Lur::Core::SaveCVarConfig(kCvarsPath);
            *static_cast<bool*>(Ctx) = true;
        },
        &RebuildPreMatch);
    Lur::Log::Info("dev console: press the key left of '1' (backtick/paragraph) to open; click cvars to edit");
#endif

    // You play team 0; drag-place buildings + tap x1/x5 to queue (#139/#140). Events go into the
    // inbox on this (render/input) thread and are drained by the sim thread's InputFn. Default solo
    // opponent is the AI (team 1, #124); --auto (soak) / --flockdemo (stress) run without an AI.
    Rps::SoloInputInbox Human;
    Rps::AiController Ai;
    const bool UseAi = !Auto && !FlockDemo;
    if (UseAi) {
        Ai.Init(Seed, /*team*/ 1, AiTier);
        
        Lur::Log::Info("solo opponent: AI (%s)", Rps::AiTierName(AiTier));
    }
    SoloAiCtx AiCtx{&Human, &Ai};
    auto Runner = std::make_unique<Rps::SimRunner>();
    // PreMatchTeam 0 = hold the clock until YOU place your opening camp, like a linked match
    // (#139/#149). Not for the stress/flock scenes: they have no camp and would never tick.
    const int SoloGate = (Stress > 0 || FlockDemo) ? -1 : 0;
    Runner->Start(Seed, UseAi ? &SampleSoloVsAi : &SampleSolo,
                  UseAi ? static_cast<void*>(&AiCtx) : static_cast<void*>(&Human),
                  static_cast<uint32_t>(Stress < 0 ? 0 : Stress), NoCombat, SoloGate);

    Lur::Input::ScrollCamera Cam;
    bool CamInit = false;
    // #43 section D: one dispatch for all four RPS mains. PickAiTier only LATCHES — the restart it
    // triggers stops and respawns the sim thread, and doing that from inside a touch handler would
    // move it off the frame boundary it has always run on.
    int PendingAiTier = -1;
    Rps::TouchRouter Router;
    {
        Rps::TouchRouterHooks Hooks;
        Hooks.Emit = [&Human](const Rps::InputEvent& E) { Human.Push(E); };
        Hooks.PickAiTier = [&PendingAiTier](int Tier) { PendingAiTier = Tier; };
        Router.Init(&View, &Cam, std::move(Hooks));
    }
    uint64_t PrevNs = NowNs();
    static Rps::Snapshot Snap;
    int Frame = 0;
    (void)Auto; (void)FoeOnly;  // #137b: the mask-based --auto/--autofoe soak retired (event soak = #144+)
    // PERSISTENT W-L-D per AI tier (desktop has no peer row), the same ScoreBook the phones use, in
    // the desktop's gitignored save dir next to chess's. It is the ONE source of truth here: the
    // in-memory tally arrays this replaced meant the ladder reset every launch, which is the whole
    // reason the numbers were worth nothing.
    Lur::Save::Store ScoreStore(".lur-desktop-save/rps");
    Rps::ScoreBook Scores;
    Scores.Load(ScoreStore);
    int CurTier = static_cast<int>(AiTier);
    bool Scored = false;
    uint64_t PostMatchNs = 0;   // #149 wall time held on the win/lose screen

    while (Win.PumpEvents()) {
        const uint64_t Now = NowNs();
        const uint64_t ElapsedNs = Now - PrevNs;
        PrevNs = Now;
        int W = 0, H = 0;
        Win.GetSize(&W, &H);
        // Latest sim state for BOTH the drag-place ghost validity and the render below (a full
        // ~90 KB copy each frame — fine on desktop; the phone consumes only on a new tick).
        const bool HaveSnap = Runner->LatestSnapshot(Snap);
#if !LUR_SHIPPING
        if (Win.TakeConsoleToggle()) View.SetDevOverlayOpen(!View.DevOverlayOpen());  // § key
#endif
        // Keys no longer drive units (#137b: events). They go to the console when it is open
        // (#119) and are dropped otherwise — DevKey claims a key only while the console shows,
        // so the game's input path is untouched when it is closed.
        for (uint32_t Vk : Win.TakeKeys()) {
#if !LUR_SHIPPING
            View.DevKey(Vk);
#else
            (void)Vk;
#endif
        }
        Rps::TouchFrame TF;
        TF.ViewW = static_cast<float>(W);
        TF.ViewH = static_cast<float>(H);
        TF.Team  = 0;              // you are always team 0 in the solo harness
        TF.Live  = HaveSnap;       // no snapshot yet = pre-match: nothing to place into
        for (const Lur::Input::TouchEvent& T : Win.TakeTouches()) Router.Route(T, Snap, TF);

        // #157: a cvar was edited and the match hasn't started — rebuild so map knobs
        // (rps.mine.row_*) are visible while you tune. Pre-match ONLY, and gated on having a
        // snapshot to test: mid-match tuning deliberately does not restart the game.
        if (RebuildPreMatch) {
            RebuildPreMatch = false;
            if (HaveSnap && Snap.Result == Rps::ResultOngoing && !Snap.HasMinerCamp(0)) {
                Runner->Stop();
                Runner->Start(Seed, UseAi ? &SampleSoloVsAi : &SampleSolo, &AiCtx,
                              static_cast<uint32_t>(Stress < 0 ? 0 : Stress), NoCombat, SoloGate);
                Lur::Log::Info("pre-match map rebuilt from edited cvars (mine rows %d/%d, clearance %d)",
                               Rps::CvMineRowHome.Get().ToInt(), Rps::CvMineRowSafe.Get().ToInt(),
                               Rps::CvMineClearance.Get().ToInt());
            }
        }
        // #2: picking an AI tier from the selector (re)starts a fresh match at that difficulty at
        // ANY time; each tier keeps a session W-L-D shown in its row.
        if (const int NewTier = PendingAiTier; NewTier >= 0) {
            PendingAiTier = -1;
            CurTier = NewTier; Scored = false;
            Ai.Init(Seed, /*team*/ 1, static_cast<Rps::EAiTier>(NewTier));
            Runner->Stop();
            Runner->Start(Seed, &SampleSoloVsAi, &AiCtx, static_cast<uint32_t>(Stress < 0 ? 0 : Stress),
                          NoCombat, SoloGate);
            CamInit = false;
            
            Lur::Log::Info("solo AI match restarted (%s)", Rps::AiTierName(static_cast<Rps::EAiTier>(NewTier)));
        }
        // Tally the match result once it resolves, then keep the selector rows' scores current.
        // Written straight through to disk: a result is worth persisting the instant it happens, and
        // "save on exit" loses exactly the match you were curious about when the app is killed.
        // UseAi gates the RECORD, not just the tally: --auto / --flockdemo have no AI opponent at all,
        // so a result there is a soak/stress artefact and would otherwise write a permanent win
        // against whatever tier happened to be the default.
        if (!Scored && HaveSnap && Snap.Result != Rps::ResultOngoing) {
            Scored = true;
            if (UseAi) {
                Scores.RecordAi(CurTier, Snap.Result, /*MyTeam*/ 0);
                Scores.Save(ScoreStore);
            }
        }
        // #149: hold the win/lose screen for PostMatchHoldNs, then start a fresh match at the same
        // tier from Seed+1 — back in the pre-match state, waiting for your camp. Same restart path
        // the tier pick uses; the camera re-locks itself on !HasMinerCamp, so nothing else to reset.
        if (HaveSnap && Snap.Result != Rps::ResultOngoing) {
            PostMatchNs += ElapsedNs;
            if (PostMatchNs >= Rps::PostMatchHoldNs) {
                PostMatchNs = 0;
                Scored = false;
                ++Seed;
                Ai.Init(Seed, /*team*/ 1, static_cast<Rps::EAiTier>(CurTier));
                Runner->Stop();
                Runner->Start(Seed, &SampleSoloVsAi, &AiCtx,
                              static_cast<uint32_t>(Stress < 0 ? 0 : Stress), NoCombat, SoloGate);
                CamInit = false;
                Lur::Log::Info("solo: next match begins (seed 0x%llx)",
                               static_cast<unsigned long long>(Seed));
            }
        } else {
            PostMatchNs = 0;
        }
        for (int T = 0; T < Rps::AiTierCount; ++T) {
            const Rps::Tally S = Scores.Ai(T);
            View.SetAiScore(T, static_cast<int>(S.Wins), static_cast<int>(S.Losses),
                            static_cast<int>(S.Draws));
        }
        if (HaveSnap && W > 0 && H > 0) {
            const float GameW = static_cast<float>(W);
            const float VisibleH = static_cast<float>(H) / Ppu();
            const float FieldMax = WorldHeightF() - VisibleH > 0.0f ? WorldHeightF() - VisibleH : 0.0f;
            const float MaxCam = FieldMax + View.TopHudWorldUnits(GameW);
            const float MinCam = -View.BottomHudWorldUnits(GameW);
            if (!CamInit) { Cam.Y = MinCam; CamInit = true; }
            // Camera LOCKED at the baseline until you place your first mining camp; a fresh match
            // (no camp yet) therefore snaps the view back to the bottom. Free scroll after.
            if (!Snap.HasMinerCamp(0)) Cam.Y = MinCam;
            else Cam.Update(static_cast<float>(ElapsedNs) / 1.0e9f, MaxCam, MinCam);
            View.Render(Renderer, Snap, Snap.AlphaAt(Now), Cam.Y, GameW,
                        static_cast<float>(H), /*FlipY=*/false,
                        static_cast<float>(ElapsedNs) / 1.0e9f);  // solo = team-0 view
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
    Lur::DevRig::WindowsBleTransport Ble(RadioExe);   // #42: UUID comes from BleServiceUuid now
    Ble.SetLogger([](const char* M) { Lur::Log::Info("%s", M); });
    if (!Ble.Start()) {
        Lur::Log::Error("BLE radio failed to start - build it: "
                        "powershell -File Tools\\BleDevRig\\build.ps1 -Source BleRadio.cs");
        return 1;
    }

    Lur::Net::Session Session;
    Rps::LockstepPeer Lp;
    const std::string Guid = "rps-pc-ble-peer";  // stable; the phone's GUID orders the teams
    // #147: the gameplay-CVar sync + build fingerprint (#112) were once wired on the Android peer
    // only, so this rig peer DROPPED the phone's MsgCvarSync and never sent its own — and the desktop
    // DOES load rps-cvars.cfg (above), so a tuned PC vs an untuned phone simulated different Cv and
    // desynced at the first anchor. Both halves must exist on every peer, which is why the set is now
    // one shared table (Rps/SessionWiring.h) rather than a copy per main.
    Rps::RouteSessionToPeer(Session, Lp);
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
#if LUR_INTERNAL
            // #147/#112: refuse a mismatched build, then exchange our (persisted rps-cvars.cfg)
            // override set so both peers converge on ONE merged Cv before tick 0.
            Lp.SendFingerprint();
            Lur::Core::CVarRegistry::ForEach([&Lp](Lur::Core::ICVar* C) {
                if (!C->AffectsGameplay() || !C->Overridden()) return;
                const int Id = Rps::GameplayIdForName(C->Name());
                if (Id >= 0) Lp.SeedGameplayCvar(static_cast<uint8_t>(Id), C->RawValue(), C->EditWallMs());
            });
            Lp.SendCvarSync();
#endif
            Started = true;
            Lur::Log::Info("linked - lockstep started (team %d, peer %.8s)", Team,
                           Session.GetPeerGuid().c_str());
        }
        if (Started) {
            // #137b: auto-soak spammed a random press mask; that's retired with the mask. The
            // event-based soak (random place/queue) re-lands with the input UI in #139/#140.
            (void)Auto; (void)Rng; (void)AutoAccumNs;
            Lp.Tick(ElapsedNs);  // produce + send input, execute to the ceiling
            // No View here — the BLE rig peer is headless. The log line is the readout (#161).
            if (Lp.Desynced())
                Lur::Log::Error("DESYNC (tick %u, recovery attempt %d/%d)", Lp.ExecTick(),
                                Lp.RecoveryAttempts(), Rps::LockstepPeer::MaxDesyncRecoveries);
        }

        QualAccumNs += ElapsedNs;
        if (Started && QualAccumNs > 2'000'000'000ull) {  // ~0.5 Hz liveness line
            QualAccumNs = 0;
            Lur::Log::Info("BLE tick=%u you=%d foe=%d desync=%u dsgate=%d txB=%llu rxB=%llu",
                           Lp.ExecTick(), Lp.GetSim().AliveCount(0), Lp.GetSim().AliveCount(1),
                           Lp.DesyncsSeen(), Lp.Desynced() ? 1 : 0,   // #204: sticky count + live gate
                           (unsigned long long)Ble.GetBytesOut(),
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
    bool AiDiag = false;                         // #152 headless AI milestone/census diagnostic
    bool AiOwner = false;                        // tier vs the scripted owner line (OwnerBot.h)
    bool AiBeginner = false;                     // #155 headless AI-vs-first-timer arrival clock
    Rps::EAiTier AiDiagTier1 = Rps::EAiTier::Medium;  // --aidiag a:b -> team 1's tier
    int DiagEvery = 300;                         //   census cadence in ticks (300 = every 30 s)
    const char* ReplayPath = nullptr;            // #144 --replay <file>: read a device recording
    const char* RecDiffA = nullptr;               // #159 --recdiff a.rec b.rec: two peers, one match
    const char* RecDiffB = nullptr;
    Rps::EAiTier AiVsA = Rps::EAiTier::Hard, AiVsB = Rps::EAiTier::Easy;
    int Matches = 9;
    int MaxTicks = 6000;
    auto ParseTier = [](const std::string& T) {
        if (T == "easy") return Rps::EAiTier::Easy;
        if (T == "hard") return Rps::EAiTier::Hard;
        // "impossible" is NOT accepted as an alias for the rung it named until 2026-07-30. It would
        // now silently mean "hard", which is the same build — but so would the old "hard", and that
        // one now means the rung BELOW. A command line in a week-old note must fail, not quietly
        // measure a different pairing than the note claims.
        return Rps::EAiTier::Medium;
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
        else if (A == "--aidiag" && I + 1 < argc) {  // #152 milestones + census; "a:b" = cross-tier
            AiDiag = true;
            const std::string V = argv[++I];
            const auto C = V.find(':');
            AiTier = ParseTier(V.substr(0, C));
            // A MIRROR hides exactly the question a new tier raises ("is it better than the rung
            // below?"), because both sides play the same build. Naming two tiers watches the
            // matchup itself — the census then shows WHERE one pulls ahead, which a win tally
            // from --aivs cannot.
            AiDiagTier1 = C == std::string::npos ? AiTier : ParseTier(V.substr(C + 1));
        }
        else if (A == "--aiowner" && I + 1 < argc) {   // tier vs the owner's measured fast line
            AiOwner = true;
            AiTier = ParseTier(argv[++I]);
        }
        else if (A == "--aibeginner" && I + 1 < argc) {  // #155 tier vs a camp-then-idle first-timer
            AiBeginner = true;
            AiTier = ParseTier(argv[++I]);
        }
        else if (A == "--every" && I + 1 < argc) DiagEvery = std::atoi(argv[++I]);
        else if (A == "--replay" && I + 1 < argc) ReplayPath = argv[++I];  // #144 read a device recording
        else if (A == "--recdiff" && I + 2 < argc) {   // #159 compare two peers' recordings
            RecDiffA = argv[++I];
            RecDiffB = argv[++I];
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

#if LUR_INTERNAL
    if (RecDiffA != nullptr && RecDiffB != nullptr) return RunRecDiff(RecDiffA, RecDiffB);
    if (ReplayPath != nullptr) return RunReplay(ReplayPath, DiagEvery);
#endif
    if (AiDiag) return RunAiDiag(AiTier, AiDiagTier1, Seed, MaxTicks, DiagEvery);
    if (AiOwner) return RunAiOwner(AiTier, Seed, Matches, MaxTicks);
    if (AiBeginner) return RunAiBeginner(AiTier, Seed, Matches, MaxTicks, DiagEvery);
    if (AiVs) return RunAiVs(AiVsA, AiVsB, Seed, Matches, MaxTicks);
    if (Ble) return RunBle(RadioExe.c_str(), Auto, MaxFrames, Seed);
    if (Solo) return RunSolo(Auto, MaxFrames, Seed, Stress, FlockDemo, NoCombat, FoeOnly, AiTier);
    return RunLoopback(Auto, MaxFrames, Seed);
}
