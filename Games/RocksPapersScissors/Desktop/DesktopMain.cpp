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
#include "Lur/Core/CVarConfig.h"  // #115: persist tuned cvars across runs
#include "Lur/Core/Log.h"
#include "Lur/Net/Session.h"
#include "Lur/Platform/Window.h"
#include "Lur/Render/Vulkan/VulkanRenderer.h"
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Loopback.h"
#include "Rps/AiController.h"
#include "Rps/MatchRecord.h"   // #144: --replay a device recording
#include "Rps/CameraScroll.h"
#include "Rps/GameView.h"
#include "Rps/LockstepPeer.h"
#include "Rps/SimRunner.h"
#include "Rps/SoloInput.h"
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
#if LUR_INTERNAL
    // #147/#112: the workbench must carry the SAME message set as a phone, or a bug in the
    // cvar-sync / fingerprint path is invisible in the two-window loopback (its whole point).
    P.Session.SetHandler(Rps::MsgCvar,
                         [&P](const uint8_t* D, std::size_t N) { P.Lp.OnMessage(Rps::MsgCvar, D, N); });
    P.Session.SetHandler(Rps::MsgCvarSync,
                         [&P](const uint8_t* D, std::size_t N) { P.Lp.OnMessage(Rps::MsgCvarSync, D, N); });
    P.Session.SetHandler(Rps::MsgFingerprint,
                         [&P](const uint8_t* D, std::size_t N) { P.Lp.OnMessage(Rps::MsgFingerprint, D, N); });
#endif
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

// #152: headless AI DIAGNOSTIC. The --aivs tally answers "which tier is stronger"; it cannot
// answer "is this tier functional at all", which is the question after an economy change — a tier
// tuned against the old purse can sit broke and idle and still tie another broken tier 0-0. So run
// one mirror match and report WHAT THE AI DID: the tick each milestone was reached (camp, first
// miner, first soldier building, first soldier) plus a periodic census of gold/workers/soldiers/
// buildings. Milestones that never arrive print as "-", which is the failure this exists to catch.
int RunAiDiag(Rps::EAiTier Tier, uint64_t Seed, int MaxTicks, int EveryTicks) {
#if !LUR_SHIPPING
    Lur::Core::LoadCVarConfig("rps-cvars.cfg");
#endif
    const char* Names[] = {"easy", "medium", "hard"};
    auto S = std::make_unique<Rps::Sim>();   // ~MBs of SoA: heap, not the 1 MB main stack (#94)
    S->Init(Seed);
    Rps::AiController Ai0, Ai1;
    Ai0.Init(Seed, 0, Tier);
    Ai1.Init(Seed, 1, Tier);
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
    Lur::Log::Info("AI diag: tier=%s seed=0x%llx cap=%d ticks (%.0fs at %d Hz)",
                   Names[static_cast<int>(Tier)], static_cast<unsigned long long>(Seed), MaxTicks,
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
        S->StepEvents(Comb, NC);
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
                   Names[static_cast<int>(Tier)], Ms(TCamp), Ms(TMiner), Ms(TBldg), Ms(TSoldier));
    const Census A = Look(0), B = Look(1);
    Lur::Log::Info("  final: t0 gold=%d wrk=%d sol=%d bld=%d | t1 gold=%d wrk=%d sol=%d bld=%d | "
                   "result=%u at tick %u",
                   A.Gold, A.Workers, A.Soldiers, A.Buildings, B.Gold, B.Workers, B.Soldiers,
                   B.Buildings, static_cast<unsigned>(S->Result), S->Tick);
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
    const char* Names[] = {"easy", "medium", "hard"};
    // The player's half is everything below midfield; their build zone is the opening frontier depth.
    const int32_t Mid = Rps::WorldHeight.ToInt() / 2;
    Lur::Log::Info("AI-vs-beginner: tier=%s, %d matches, cap %d ticks (%.0fs), beginner = camp then idle",
                   Names[static_cast<int>(Tier)], Matches, MaxTicks,
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
                   Names[static_cast<int>(Tier)], Avg(SumSoldier, GotSoldier), Avg(SumMid, GotMid),
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
int RunReplay(const char* Path, int EveryTicks) {
    const Rps::MatchRecording R = Rps::LoadMatchRecording(Path);
    if (!R.Ok) {
        Lur::Log::Error("replay: %s is not a valid recording", Path);
        return 1;
    }
    const char* Tiers[] = {"easy", "medium", "hard"};
    Lur::Log::Info("replay %s: seed=%llx tier=%s human=team%u events=%zu census=%zu end=tick %u result=%d",
                   Path, static_cast<unsigned long long>(R.Seed),
                   R.Tier >= 0 && R.Tier < 3 ? Tiers[R.Tier] : "?", static_cast<unsigned>(R.HumanTeam),
                   R.Events.size(), R.Census.size(), R.EndTick, R.Result);
    Lur::Log::Info("  build: %s", R.BuildFp.c_str());

    auto S = std::make_unique<Rps::Sim>();
    const uint64_t Hash = Rps::ReplayMatch(R, *S);
    Lur::Log::Info("  replayed to tick %u, hash %016llx", S->Tick, static_cast<unsigned long long>(Hash));

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
        const char* Names[] = {"easy", "medium", "hard"};
        Lur::Log::Info("solo opponent: AI (%s)", Names[static_cast<int>(AiTier)]);
    }
    SoloAiCtx AiCtx{&Human, &Ai};
    auto Runner = std::make_unique<Rps::SimRunner>();
    // PreMatchTeam 0 = hold the clock until YOU place your opening camp, like a linked match
    // (#139/#149). Not for the stress/flock scenes: they have no camp and would never tick.
    const int SoloGate = (Stress > 0 || FlockDemo) ? -1 : 0;
    Runner->Start(Seed, UseAi ? &SampleSoloVsAi : &SampleSolo,
                  UseAi ? static_cast<void*>(&AiCtx) : static_cast<void*>(&Human),
                  static_cast<uint32_t>(Stress < 0 ? 0 : Stress), NoCombat, SoloGate);

    Rps::CameraScroll Cam;
    bool CamInit = false;
#if !LUR_SHIPPING
    float DevDragY = 0.0f, DevDragMoved = 0.0f;  // drag-to-scroll the console (# 121)
#endif
    uint64_t PrevNs = NowNs();
    static Rps::Snapshot Snap;
    int Frame = 0;
    (void)Auto; (void)FoeOnly;  // #137b: the mask-based --auto/--autofoe soak retired (event soak = #144+)
    // #2 session W-L-D per AI tier (desktop has no peer row); the current match's tier + a scored latch.
    int SW[3] = {}, SL[3] = {}, SD[3] = {};
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
        // #1: lift the dragged ghost UP-LEFT of the finger by ~its footprint size so the thumb
        // doesn't hide it. The SAME offset feeds the ghost draw, the validity read, and the drop, so
        // the building lands exactly where you SEE it (above-left of the finger), not under the thumb.
        const float GhostOffPx = (static_cast<float>(Snap.Cv.BuildingFootprint.Raw) /
                                  static_cast<float>(Rps::Fixed::One)) * 0.5f * Ppu();
        // #148 magnetic drag-to-place: the (thumb-offset) desired point snaps to the nearest valid
        // spot within ~the icon size. ResolvePlacement returns the snapped world drop (Wx,Wy) + where
        // to draw the ghost (Gsx,Gsy — the snapped spot when valid, else the offset point for the red
        // blink). You are team 0. One home in GameView so desktop/Android/iOS feel identical.
        auto Resolve = [&](float DesX, float DesY, float& Wx, float& Wy, float& Gsx, float& Gsy) -> bool {
            return View.ResolvePlacement(DesX, DesY, Cam.Y, static_cast<float>(W), static_cast<float>(H),
                                         /*FlipY=*/false, Snap, /*Team*/ 0, Wx, Wy, Gsx, Gsy);
        };
        for (uint32_t Vk : Win.TakeKeys()) (void)Vk;  // keys no longer drive units (#137b: events)
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
            // #139/#140 mirror of the loopback's HandlePeerInput: a pointer-down on a build plate
            // starts a drag-to-place (ghost follows, valid release emits a Place event); any other
            // drag pans the camera; a tap on a building's x1/x5 button queues units.
            const float GhX = T.XPx - GhostOffPx, GhY = T.YPx - GhostOffPx;  // #1 offset placement point
            if (T.Phase == Lur::Input::ETouchPhase::Began) {
                const int Plate = View.PlateAt(T.XPx, T.YPx);  // plate hit-test at the real finger
                if (Plate >= 0) {
                    View.BeginPlaceDrag(Plate, GhX, GhY);  // sets the ghost type; seed at the offset spot
                    float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
                    const bool V = Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy);
                    // Finger point AND snapped point: ghost on the finger, snap eased (visual only).
                    View.UpdatePlaceDrag(GhX, GhY, Gsx, Gsy, V);
                } else {
                    // #107: a press on an x1/x5 button lights up NOW; the enqueue still commits on
                    // release (below), so a press that turns into a camera pan queues nothing.
                    View.PressProductionButton(T.XPx, T.YPx);
                    Cam.Begin(T.YPx);
                }
            } else if (T.Phase == Lur::Input::ETouchPhase::Moved) {
                if (View.IsPlacing()) {
                    float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
                    const bool V = Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy);
                    View.UpdatePlaceDrag(GhX, GhY, Gsx, Gsy, V);
                } else {
                    Cam.Move(T.YPx, Ppu());
                }
            } else if (T.Phase == Lur::Input::ETouchPhase::Ended ||
                       T.Phase == Lur::Input::ETouchPhase::Cancelled) {
                if (View.IsPlacing()) {
                    bool Placed = false;
                    if (T.Phase == Lur::Input::ETouchPhase::Ended) {
                        float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
                        if (Resolve(GhX, GhY, Wx, Wy, Gsx, Gsy)) {
                            Human.Push(Rps::InputEvent::Place(0, static_cast<uint8_t>(View.PlacingType()),
                                                             WorldToFixed(Wx), WorldToFixed(Wy)));
                            Placed = true;
                        }
                    }
                    View.EndPlaceDrag(Placed);  // valid -> the real building takes over; else slide back
                } else {
                    Cam.End();
                    if (T.Phase == Lur::Input::ETouchPhase::Ended && View.OnTap(T.XPx, T.YPx) == -1) {
                        // Not the HUD/selector -> maybe a per-building x1/x5 button (#140).
                        int32_t Slot = -1;
                        const int Cnt = View.OnProductionButton(T.XPx, T.YPx, Slot);
                        if (Cnt > 0) Human.Push(Rps::InputEvent::Queue(0, Slot, Cnt));
                    }
                }
            }
        }
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
        if (const int NewTier = View.TakeAiTier(); NewTier >= 0) {
            CurTier = NewTier; Scored = false;
            Ai.Init(Seed, /*team*/ 1, static_cast<Rps::EAiTier>(NewTier));
            Runner->Stop();
            Runner->Start(Seed, &SampleSoloVsAi, &AiCtx, static_cast<uint32_t>(Stress < 0 ? 0 : Stress),
                          NoCombat, SoloGate);
            CamInit = false;
            const char* Names[] = {"easy", "medium", "hard"};
            Lur::Log::Info("solo AI match restarted (%s)", Names[NewTier]);
        }
        // Tally the match result once it resolves, then keep the selector rows' scores current.
        if (!Scored && HaveSnap && Snap.Result != Rps::ResultOngoing) {
            Scored = true;
            if (Snap.Result == Rps::ResultTeam0Wins) ++SW[CurTier];
            else if (Snap.Result == Rps::ResultTeam1Wins) ++SL[CurTier];
            else ++SD[CurTier];
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
        for (int T = 0; T < 3; ++T) View.SetAiScore(T, SW[T], SL[T], SD[T]);
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
#if LUR_INTERNAL
    // #147: the gameplay-CVar sync + build fingerprint (#112) were wired on the Android peer only,
    // so this rig peer DROPPED the phone's MsgCvarSync and never sent its own — and the desktop
    // DOES load rps-cvars.cfg (above), so a tuned PC vs an untuned phone simulated different Cv
    // and desynced at the first anchor. Both halves of the exchange must exist on every peer.
    Session.SetHandler(Rps::MsgCvar,
                       [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(Rps::MsgCvar, D, N); });
    Session.SetHandler(Rps::MsgCvarSync,
                       [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(Rps::MsgCvarSync, D, N); });
    Session.SetHandler(Rps::MsgFingerprint,
                       [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(Rps::MsgFingerprint, D, N); });
#endif
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
    bool AiDiag = false;                         // #152 headless AI milestone/census diagnostic
    bool AiBeginner = false;                     // #155 headless AI-vs-first-timer arrival clock
    int DiagEvery = 300;                         //   census cadence in ticks (300 = every 30 s)
    const char* ReplayPath = nullptr;            // #144 --replay <file>: read a device recording
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
        else if (A == "--aidiag" && I + 1 < argc) {  // #152 one mirror match, milestones + census
            AiDiag = true;
            AiTier = ParseTier(argv[++I]);
        }
        else if (A == "--aibeginner" && I + 1 < argc) {  // #155 tier vs a camp-then-idle first-timer
            AiBeginner = true;
            AiTier = ParseTier(argv[++I]);
        }
        else if (A == "--every" && I + 1 < argc) DiagEvery = std::atoi(argv[++I]);
        else if (A == "--replay" && I + 1 < argc) ReplayPath = argv[++I];  // #144 read a device recording
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
    if (ReplayPath != nullptr) return RunReplay(ReplayPath, DiagEvery);
#endif
    if (AiDiag) return RunAiDiag(AiTier, Seed, MaxTicks, DiagEvery);
    if (AiBeginner) return RunAiBeginner(AiTier, Seed, Matches, MaxTicks, DiagEvery);
    if (AiVs) return RunAiVs(AiVsA, AiVsB, Seed, Matches, MaxTicks);
    if (Ble) return RunBle(RadioExe.c_str(), Auto, MaxFrames, Seed);
    if (Solo) return RunSolo(Auto, MaxFrames, Seed, Stress, FlockDemo, NoCombat, FoeOnly, AiTier);
    return RunLoopback(Auto, MaxFrames, Seed);
}
