// The two-peer soak (#211): two LockstepPeers, each driven by its OWN AiController over its OWN
// sim, played through long matches under a deterministic fault schedule — on the host, with no
// window and no Vulkan device.
//
// ---- Why this exists ----
// CLAUDE.md claimed "the whole netcode is proven in two windows on one desktop before a phone is in
// the room". Measured 2026-08-16, that was not true for a long match with real input. The desktop's
// `RunLoopback` is the only mode with two real LockstepPeers and it cannot drive itself (the
// event-based auto-soak was left unwired: `(void)Auto;` … "#137b: re-wires to events in #140"). Every
// mode that CAN drive itself — `--aidiag`, `--aivs`, `--aibeginner` — runs ONE Rps::Sim with two
// AiControllers and contains no netcode at all. So the only way to run two lockstep peers through a
// real match was two physical phones, which is why this class of bug was only ever found on hardware
// at the end of a long day. #210 sat blocked on exactly that: two phones recovered from desync 45
// times and re-diverged at the next anchor every time, with no way to separate "netcode logic" from
// "cross-platform nondeterminism".
//
// ---- What it can and cannot answer ----
// Both peers here run ONE binary: identical compiler, identical struct layout, identical
// floating-point behaviour. So it CANNOT catch cross-toolchain nondeterminism, and it will stay green
// through all of it. It answers #210 by ELIMINATION: green over millions of ticks with the full fault
// schedule means the divergence is not netcode logic, and the search moves to the sim's determinism
// across toolchains (flight recorder + `--recdiff`, plus a deliberate NDK-vs-host StateHash compare).
//
// ---- The assertion that matters ----
// Convergence, not liveness. On 2026-08-16 both phones reported started=1, desync=0 and 60 fps while
// playing DIFFERENT GAMES — Android at tick 3320 with 32 units, iPhone at 3150 with 16. "Both still
// running" was true throughout and would have passed any liveness check. So the check here is: when
// the link has been clean and no recovery is in flight, equal exec ticks MUST mean equal state
// hashes — and the number of times that check actually RAN is asserted too, because a convergence
// check that never fires is indistinguishable from a green one.
//
//   ./rps_two_peer_soak                 # gate run (what CTest runs): 1 seed x 1200 ticks. ~5 s.
//   ./rps_two_peer_soak --long          # soak run: 4 seeds x 8000 ticks, ~4 matches each. ~6 min.
//   ./rps_two_peer_soak --ticks N --seeds N --seed0 N --row <name> --faultperiod N
//   ./rps_two_peer_soak --trace N [--tracefrom T] --ckptrounds N        # diagnosing a failure
//
// Seeds come from a counter, never from the clock: a soak that cannot be replayed from its seed is a
// bug report nobody can act on.
//
// ---- CURRENT STATUS ----
// Both the gate run and `--long` (4 seeds x 8000 ticks x 7 rows = 28 runs, ~6 min) are GREEN:
// 0 of 41 checkpoint failures on every one of the 28 runs, 3-5 matches played per run, and the
// per-tick reading clean on every row except the two that deliberately force divergence.
//
// Getting here took three bugs in LockstepPeer and three in this file, and the split is worth
// remembering before trusting any future red result from it:
//
//   * #212 (netcode): AdvanceConfirmed fabricated anchor hashes from an unrelated tick, the survivor
//     published resync frontiers ahead of its own executed tick, and the anchor-mismatch recovery
//     waited to be rescued instead of asking. Together: a livelock at 15-22 of 41 checkpoints.
//   * #214 (netcode, wire v11): MsgCamp carried no match identity, so a camp from a match the peer
//     had left read as readiness for the next one. Left the pair bit-identical under permanently
//     different MatchIndex/Seed. Now transient and self-correcting (BookkeepingDrift asserts it does
//     not persist, and `--long` shows 3 such windows, all realigned).
//   * #213 was filed as a netcode deadlock and was NOT ONE — it was this file freezing the ahead peer
//     during a settle, so the behind peer could never receive the input it was waiting for. Caught by
//     sabotaging the proposed fix back out and finding the tests still passed.
//
// Two of the three harness bugs produced confident, fully-argued false findings before anyone
// suspected the harness. So: when a row goes red, sabotage the fix and confirm the test still fails
// before believing the diagnosis.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "Rps/AiController.h"
#include "Rps/EventCodec.h"
#include "Rps/LockstepPeer.h"

#include "LockstepHarness.h"

using namespace Rps;
using namespace Rps::Harness;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// ---------------------------------------------------------------------------------------------
// The fault rows
// ---------------------------------------------------------------------------------------------
// Each row is a link fault the hardware has actually produced, driven from the tick number and the
// seed so a failure reproduces from two integers. Ordered so the cheapest hypothesis is tested first,
// with one exception: SimultaneousDrop is the row with a KNOWN historical failure (#210), so it is
// the first fault row after the clean baseline.
enum class ERow {
    Clean,             // baseline determinism — if this is red, nothing below means anything
    SimultaneousDrop,  // #210: a frame lost in BOTH directions on the same exchange
    OneWayDrop,        // #163: a produced frame missing from one direction only
    Duplicate,         // #163: the double-GATT-connection shape — every frame arrives twice
    Forge,             // #159: a frame carrying an event the other peer's stream does not have
    Silence,           // #162: execution parked at the ceiling — must HOLD, never conclude
    SilenceThenResume, // the reconnect edge: long outage, then BeginResync and RebuildFromHistory
};

static const char* RowName(ERow R) {
    switch (R) {
        case ERow::Clean:             return "clean";
        case ERow::SimultaneousDrop:  return "simultaneous-drop";
        case ERow::OneWayDrop:        return "one-way-drop";
        case ERow::Duplicate:         return "duplicate";
        case ERow::Forge:             return "forge";
        case ERow::Silence:           return "silence";
        case ERow::SilenceThenResume: return "silence-then-resume";
    }
    return "?";
}

// How often a periodic fault fires, in ticks — one fault every 20 s of play, which is still ~40x the
// worst rate hardware has actually shown (#163's match lost ONE frame in ~12 000 ticks).
//
// Every row passes at every rate measured, 40 through 1000, across 4 seeds — so 200 is a comfortable
// middle rather than an edge. It was not always: before #212 and #214 the boundary looked like a
// PHASE effect (130 and 140 failed while 110, 120 and 150 passed), which is what pointed the
// investigation at the anchor/recovery cycle rather than at load. Worth knowing if it ever returns.
//
// Overridable with `--faultperiod` so a failure can be bisected down to ONE fault: "does a single
// injection recover" and "does injection N+1 arriving mid-recovery recover" are different questions
// with different answers, and a fixed period cannot separate them.
static uint32_t GFaultPeriod = 200;
// How long a Silence row holds the link dark. Must stay well under CeilingStallTimeoutNs (20 s =
// 200 ticks at TickRateHz), because past that #162 deliberately ENDS the match — that is correct
// behaviour, not the property this row is testing.
static constexpr uint32_t SilenceTicks = 30;
// The dark window must stay a MINORITY of the fault period, or the row stops testing what it says.
// At --faultperiod 50 the fixed 30 left the link dark 60% of the time — a link that barely functions,
// not a link with an outage — and the pair could not close the gap between checkpoints. Deriving the
// window from the period keeps the row meaning "an outage" at every rate.
static uint32_t DarkTicks(uint32_t Period) {
    const uint32_t Third = Period / 3;
    return SilenceTicks < Third ? SilenceTicks : Third;
}
// The SilenceThenResume outage: long enough that the peers are far apart and the reconnect path has
// real work to do, still short of the #162 bound.
static constexpr uint32_t OutageTicks = 120;
static constexpr uint32_t OutageStart = 200;

// How often the run stops injecting faults and takes an authoritative convergence reading (see
// SettleAndCompare). Offset so every checkpoint sits at least ~50 ticks after the most recent fault,
// which the traced repair latency (~5 ticks) clears with two orders of margin.
static constexpr uint32_t CheckpointPeriod = 200;
static constexpr uint32_t CheckpointPhase = 150;
// Ticks a checkpoint may spend waiting for the pair to come back together. Generous because a
// recovery round backs off up to RecoveryRetryMaxNs (15 s = 150 ticks) before re-attempting, and a
// checkpoint that gives up before the retry fires would report a slow repair as a broken one.
// Overridable with `--ckptrounds`, which is how "the repair is slow" gets told apart from "the pair is
// stuck": if raising the budget turns a failure green, the bound is wrong; if it does not, the pair is
// livelocked and the bound was reporting the truth.
static int GCheckpointRounds = 400;
static bool GSettleTrace = false;

// Whether the row's fault is supposed to be INVISIBLE to the two sims — not "does it recover", which
// every row must, but "may the two states differ for even one tick".
//
// Only two rows qualify, and the reason is the rollback model. A DUPLICATE frame is discarded the
// moment it arrives (the signed sequence read, #163), so it never reaches an executed tick and cannot
// perturb state. A LOST frame is different: under rollback the hole was already SPECULATED — the peer
// was predicted idle across it — so if the frame carried real input the sim has already diverged
// speculatively, and the repair happens after the fact. A transient disagreement in a drop row is
// therefore the design working, not a bug.
//
// The first draft of this file had it as "everything except forge", and that assertion passed only
// because at the gate's fault rate the dropped frames happened to be EMPTY. Raising the rate produced
// transients immediately. That is a test asserting a coincidence, which is worse than no test: it
// would have gone red on a perfectly correct change to what the AI does with its gold.
static bool RowMustNeverDiverge(ERow R) { return R == ERow::Clean || R == ERow::Duplicate; }

// Whether the row's fault shows up as a detected input-sequence gap. Used to prove the injection
// LANDED — a fault row that silently stopped injecting is a row that tests nothing.
static bool RowExpectsGaps(ERow R) {
    return R == ERow::SimultaneousDrop || R == ERow::OneWayDrop || R == ERow::Silence;
}

struct SoakSpec {
    uint64_t Seed = 0;
    uint32_t Ticks = 0;
    ERow Row = ERow::Clean;
    EAiTier Tier0 = EAiTier::Medium;
    EAiTier Tier1 = EAiTier::Medium;
    // Print one line per tick from the first disagreement onward. The counters alone say a pair
    // diverged; they cannot say whether it was never detected, detected and never repaired, or
    // repaired and immediately re-broken — which are three different bugs. `--trace` is what tells
    // them apart without attaching a debugger to a 20 000-tick run.
    uint32_t TraceTicks = 0;
    uint32_t TraceFrom = 0;   // loop tick to start tracing from; 0 = start at the first disagreement
};

struct SoakResult {
    uint32_t TicksRun = 0;
    uint32_t MatchesResolved = 0;
    uint32_t Draws = 0;
    // ---- The two convergence readings, and why there are two ----
    // OPPORTUNISTIC: whenever the link was clean, no recovery is in flight and the two heads happen to
    // sit on the same tick, the hashes must match. Cheap, per-tick resolution, and the reading that
    // catches a fault the moment it lands. Its blind spot is head SKEW: under rollback the peers run
    // independent wall clocks, and after a staggered recovery reseed B can track A perfectly while
    // sitting one tick behind forever — at which point this reading silently stops firing. Trusting it
    // alone would have reported a converged pair as permanently broken (measured, this file, first
    // draft).
    uint32_t Comparisons = 0;
    uint32_t Disagreements = 0;
    uint32_t FirstDivergentTick = 0;   // exec tick of the first disagreement — where debugging starts
    uint64_t HashA = 0, HashB = 0;
    // AUTHORITATIVE: periodically stop injecting faults, drain the link and tick until the heads meet,
    // then compare. Immune to skew, and the only reading that can distinguish "repaired" from "still
    // broken but no longer comparable".
    //
    // NEITHER reading subsumes the other, which the sabotage pass settled empirically. Reverting the
    // #210 survivor rule, dropping the duplicate discard, or killing the gap detector is caught by the
    // checkpoints. But making the combined batch "mine first" instead of "team 0 first" — a textbook
    // wire-ordering determinism bug — diverges the pair from exec tick 2 and every single checkpoint
    // still PASSES, because the recovery machinery keeps papering over it. Only the per-tick reading
    // sees that one. Keep both.
    uint32_t Checkpoints = 0;
    uint32_t CheckpointFailures = 0;
    uint32_t FirstFailedCheckpoint = 0;
    const char* FirstFailReason = "";
    // Converged in state but disagreeing about which match this is. Tracked apart from convergence
    // because it is a different bug with a different fix (see BookkeepingDrift).
    uint32_t DriftCheckpoints = 0;
    uint32_t FirstDriftTick = 0;
    bool DriftAtEnd = false;
    uint32_t SettleTicks = 0;   // ticks spent inside checkpoints, excluded from the fault schedule
    // The #210 invariant: the survivor (team 0, lower device GUID) has the authoritative timeline,
    // so there is nothing it could legitimately be rebuilding FROM. If this is ever true the pair is
    // in the deadlock 9668799 fixed.
    bool SurvivorAwaited = false;
    uint32_t SurvivorAwaitedTick = 0;
    // Whether execution ever stopped for good. Reported separately from convergence because the
    // 2026-08-16 hardware failure was live-but-divergent, and the #162 failure was the opposite.
    uint32_t LastProgressTick = 0;
    // Netcode counters, summed over the run.
    uint32_t DesyncsSeen = 0;
    uint32_t RecoveryRounds = 0, GapRecoveries = 0, InputGaps = 0, DuplicateFrames = 0;
    uint32_t Rollbacks = 0;
    // Proof that a real match happened rather than two idle sims agreeing about nothing. An empty
    // field hashes identically on both peers forever.
    int32_t Alive0 = 0, Alive1 = 0;
    uint32_t PeakAlive = 0;
    uint32_t ExecTick = 0;
};

// ---------------------------------------------------------------------------------------------
// Driving real input
// ---------------------------------------------------------------------------------------------
// Each peer runs ITS OWN AiController over ITS OWN sim — two independent input producers, exactly
// like two players. Sharing one controller between them would make the two input streams a single
// function of one state and quietly remove the thing being tested.
//
// The AI reads the peer's HEAD state, which under rollback may be speculative. That is correct and
// deliberate: a human looks at the speculative render state too. The AI is an input SOURCE, not part
// of the simulation, so it has no determinism obligation across peers — only the events it emits are
// exchanged, and those are what both sims execute.
static void FeedAi(LockstepPeer& P, AiController& Ai) {
    InputEvent Ev[MaxEventsPerTick];
    int Count = 0;
    Ai.DecideEvents(P.GetSim(), P.ExecTick(), Ev, MaxEventsPerTick, Count);
    for (int I = 0; I < Count; ++I) P.QueueLocalEvent(Ev[I]);
}

// ---------------------------------------------------------------------------------------------
// The fault schedule
// ---------------------------------------------------------------------------------------------
// Returns true when this exchange was CLEAN in both directions — the precondition for comparing
// state, since a fault in flight legitimately puts the two heads in different places (each peer has
// its own real input and predicted the other idle across the hole).
static bool ApplyFault(const SoakSpec& Spec, uint32_t T, Outbox& Qa, Outbox& Qb, LockstepPeer& A,
                       LockstepPeer& B) {
    switch (Spec.Row) {
        case ERow::Clean:
            break;

        case ERow::SimultaneousDrop:
            // BOTH peers gap on the same exchange. This is the one shape a single-direction test
            // cannot express, and #210 was invisible to every single-direction test in the suite:
            // it takes both peers gapping at once for the survivor to end up waiting on the loser.
            if (T % GFaultPeriod == 0 && T > 0) {
                DeliverDroppingNthInput(Qa, B, 0);
                DeliverDroppingNthInput(Qb, A, 0);
                return false;
            }
            break;

        case ERow::OneWayDrop:
            if (T % GFaultPeriod == 0 && T > 0) {
                DeliverDroppingNthInput(Qa, B, 0);
                Deliver(Qb, A);
                return false;
            }
            break;

        case ERow::Duplicate:
            // Sustained for the whole run, one-directional — writes go out from one place and were
            // clean on hardware, so the damage was asymmetric in exactly this direction. A duplicate
            // is not a fault the sims should ever SEE (it is discarded on arrival), so this row still
            // demands convergence on every tick.
            DeliverDuplicatingInputs(Qa, B);
            Deliver(Qb, A);
            return true;

        case ERow::Forge:
            if (T % GFaultPeriod == 0 && T > 0) {
                TamperOneInput(Qa, B, /*ForgedTeam*/ 0);
                Deliver(Qb, A);
                return false;
            }
            break;

        case ERow::Silence:
            // Drop EVERYTHING both ways for a window. Execution must park at the ceiling and HOLD:
            // #162's bound exists so a peer this far behind eventually ends the match, but 30 ticks
            // is nowhere near it and concluding anything here would be a bug.
            if ((T % GFaultPeriod) < DarkTicks(GFaultPeriod) && T > GFaultPeriod) {
                Qa.Q.clear();
                Qb.Q.clear();
                return false;
            }
            break;

        case ERow::SilenceThenResume:
            if (T >= OutageStart && T < OutageStart + OutageTicks) {
                Qa.Q.clear();
                Qb.Q.clear();
                return false;
            }
            if (T == OutageStart + OutageTicks) {
                // The reconnect EDGE — what Session fires on a re-established link. Both peers offer
                // their executed history; whichever is behind rebuilds from the longer one.
                A.BeginResync();
                B.BeginResync();
                Deliver(Qa, B);
                Deliver(Qb, A);
                return false;
            }
            break;
    }
    Deliver(Qa, B);
    Deliver(Qb, A);
    return true;
}

// Every LockstepPeer diagnostic counter is reset by BeginMatch — they answer "what happened in THIS
// match", which is what a phone's HUD wants. A soak spans many matches (8000 ticks is four of them),
// so reading a counter once at the end reports only the last match and silently understates the run:
// the duplicate row's `dup` read as 300-odd out of 8000 ticks, which looks like the injection had
// mostly stopped. Accumulate the deltas instead, treating any decrease as a match boundary.
struct MatchCounter {
    uint32_t Total = 0;
    uint32_t Prev = 0;
    void Sample(uint32_t Now) {
        Total += Now >= Prev ? Now - Prev : Now;   // a decrease means BeginMatch reset it
        Prev = Now;
    }
};

// ---------------------------------------------------------------------------------------------
// The authoritative convergence reading
// ---------------------------------------------------------------------------------------------
// A quiet moment: no new input, no faults, everything in flight delivered, tick whichever peer is
// behind until the two heads meet — then compare. Both players pausing is an ordinary thing to happen
// in a real match, so this is not an artificial state; it is just the one state in which "are these
// two playing the same game" has an unambiguous answer.
//
// Returns as soon as it sees agreement, so a converged pair costs a couple of rounds. Ticks consumed
// are added to *SettleTicks — they are real sim progress and must not be silently unaccounted for.
// CONVERGED = the two peers are simulating the same game: same tick, same state, nothing in flight.
//
// Deliberately says nothing about MatchIndex or Seed. Those are per-peer BOOKKEEPING, and conflating
// them with convergence hid a real distinction: after a transient divergence at a match boundary the
// pair can be bit-identical at every tick while its match counters sit permanently one apart. That is
// a defect, but it is a different defect, and asserting it here reported "the peers never converge"
// about a pair that was in perfect lockstep. Tracked separately below.
static bool Converged(const LockstepPeer& A, const LockstepPeer& B) {
    return A.ExecTick() == B.ExecTick() && !A.Recovering() && !B.Recovering() &&
           !A.AwaitingResync() && !B.AwaitingResync() && A.MatchStarted() && B.MatchStarted() &&
           A.GetSim().StateHash() == B.GetSim().StateHash();
}

// Converged in state but disagreeing about WHICH match this is. The sim cannot see it today (Seed is
// read only by the LUR_INTERNAL StressFill; mines are placed deterministically and there is no
// gameplay RNG), but two things can: a flight recording's header carries the seed, so a drifted pair
// writes two files --recdiff cannot pair (#208's "unpairable file" by another route), and the mains
// latch "already tallied this match" per index, so the two ScoreBooks disagree about how many matches
// were played. It also stops being latent the moment any real RNG enters the sim.
static bool BookkeepingDrift(const LockstepPeer& A, const LockstepPeer& B) {
    return Converged(A, B) &&
           (A.MatchIndex() != B.MatchIndex() || A.GetSim().Seed != B.GetSim().Seed);
}

// Which clause blocked convergence. Without this a failed checkpoint says only "not converged", and
// "the hashes differ" and "the two peers are in different MATCHES" are completely different bugs.
static const char* WhyNotConverged(const LockstepPeer& A, const LockstepPeer& B) {
    if (!A.MatchStarted() || !B.MatchStarted()) return "a peer is pre-match";
    if (A.Recovering() || B.Recovering()) return "still recovering";
    if (A.AwaitingResync() || B.AwaitingResync()) return "still awaiting history";
    if (A.ExecTick() != B.ExecTick()) return "heads never met";
    return "STATE HASH DIFFERS";
}

static bool SettleAndCompare(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb,
                             AiController& Ai0, AiController& Ai1, int Rounds,
                             uint32_t* SettleTicks) {
    bool Standstill = false;   // neither head moved last round -> release the leader too
    for (int I = 0; I < Rounds; ++I) {
        if (Converged(A, B)) return true;
        // A quiet moment means no NEW orders — it does not mean no input at all, because the opening
        // camp IS input and the match clock does not start until both peers have placed one. A peer
        // sitting in the pre-match handshake therefore never starts, the settle can never converge, and
        // no amount of extra rounds helps: the failure count was identical at 400, 2000 and 6000
        // rounds, which is what gave this away. Feed the AI only while a peer is pre-match, so the
        // handshake completes and the moment stays quiet for the part that is actually being measured.
        if (!A.MatchStarted()) FeedAi(A, Ai0);
        if (!B.MatchStarted()) FeedAi(B, Ai1);
        if (GSettleTrace && (I % 40) == 0)
            std::printf("      settle I=%d A[t=%u rec=%d awa=%d dsy=%d] B[t=%u rec=%d awa=%d dsy=%d]\n",
                        I, A.ExecTick(), A.Recovering() ? 1 : 0, A.AwaitingResync() ? 1 : 0,
                        A.Desynced() ? 1 : 0, B.ExecTick(), B.Recovering() ? 1 : 0,
                        B.AwaitingResync() ? 1 : 0, B.Desynced() ? 1 : 0);
        // Hold the AHEAD peer so the heads can meet — except when the pair made no progress at all last
        // round, in which case release both. Three earlier versions of these four lines were each wrong
        // in a way that produced a confident false finding, so the reasoning is worth keeping:
        //
        //   * reading the second head AFTER ticking the first is an off-by-one: a pair one tick apart
        //     alternates forever, which read as "the netcode never reconverges";
        //   * snapshotting the heads first but still freezing the ahead peer deadlocks by construction —
        //     a peer stalled at its ceiling waits for the OTHER peer's input, and the other peer only
        //     produces input when ticked. That reported "both peers wedged for 400 s with a healthy
        //     link" about a pair that was not wedged;
        //   * ticking both unconditionally (what two real phones do) fixes that but stops the heads ever
        //     MEETING: a peer that bursts out of a stall overshoots the other, and the settle's
        //     equal-tick condition is never satisfied even though the pair is fine.
        //
        // So: converge by holding the leader, and break a genuine standstill by releasing it. Only a
        // pair that stays frozen with both peers running is actually stuck.
        const uint32_t Ta = A.ExecTick();
        const uint32_t Tb = B.ExecTick();
        if (Ta <= Tb || Standstill) A.Tick(OneTickNs);
        if (Tb <= Ta || Standstill) B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
        Standstill = A.ExecTick() == Ta && B.ExecTick() == Tb;
        ++*SettleTicks;
    }
    return Converged(A, B);
}

// ---------------------------------------------------------------------------------------------
// The loop
// ---------------------------------------------------------------------------------------------
static SoakResult RunSoak(const SoakSpec& Spec) {
    Outbox Qa, Qb;
    LockstepPeer A, B;   // A = team 0 = the survivor by the device-id tie-break
    A.Init(Spec.Seed, 0, Enqueue, &Qa);
    B.Init(Spec.Seed, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);

    AiController Ai0, Ai1;
    // Distinct RNG streams per side. Salting from the same seed keeps the whole run replayable from
    // (Seed, Row) alone.
    Ai0.Init(Spec.Seed ^ 0xA10A10A1ull, 0, Spec.Tier0);
    Ai1.Init(Spec.Seed ^ 0xB20B20B2ull, 1, Spec.Tier1);

    SoakResult R;
    // Consecutive clean exchanges. A misprediction is corrected by the delivery that contradicts it,
    // so one clean exchange is enough for the heads to agree again — but a RECOVERY spans several,
    // and the streak is what tells the two apart without asking the peer.
    int CleanStreak = 0;
    uint32_t LastMatchIndex = A.MatchIndex();
    uint32_t PrevExec = A.ExecTick();
    uint32_t TraceLeft = 0;
    bool Traced = false;
    bool CheckpointDue = false;
    MatchCounter Dsy, Rnd, Gap, Igp, Dup, Rbk;   // per-match counters, accumulated across matches

    for (uint32_t T = 0; T < Spec.Ticks; ++T) {
        FeedAi(A, Ai0);
        FeedAi(B, Ai1);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        const bool Clean = ApplyFault(Spec, T, Qa, Qb, A, B);

        R.TicksRun = T + 1;
        // A match restart REWINDS the exec tick to 0, so ">" alone stops counting progress the moment
        // the first match ends and the freeze check then fires on a perfectly healthy multi-match run.
        // Any change is progress; only standing still is not.
        if (A.ExecTick() != PrevExec) {
            PrevExec = A.ExecTick();
            R.LastProgressTick = T;
        }
        Dsy.Sample(A.DesyncsSeen() + B.DesyncsSeen());
        Rnd.Sample(static_cast<uint32_t>(A.RecoveryRounds() + B.RecoveryRounds()));
        Gap.Sample(static_cast<uint32_t>(A.GapRecoveries() + B.GapRecoveries()));
        Igp.Sample(static_cast<uint32_t>(A.InputGaps() + B.InputGaps()));
        Dup.Sample(static_cast<uint32_t>(A.DuplicateFrames() + B.DuplicateFrames()));
        Rbk.Sample(static_cast<uint32_t>(A.Rollbacks() + B.Rollbacks()));
        if (A.AwaitingResync() && !R.SurvivorAwaited) {
            R.SurvivorAwaited = true;
            R.SurvivorAwaitedTick = T;
        }
        if (A.MatchIndex() != LastMatchIndex) {
            ++R.MatchesResolved;
            LastMatchIndex = A.MatchIndex();
        }
        // A draw must never come out of the netcode (owner ruling, 2026-08-16). The GAMEPLAY
        // simultaneous-kill draw in Sim.cpp is legitimate and stays — but an AI-vs-AI match wiping
        // both sides to zero on the same tick is vanishingly unlikely, so a draw here is the
        // netcode's, and it is the one outcome that makes a broken run look finished.
        if (A.GetSim().Result == ResultDraw || B.GetSim().Result == ResultDraw) ++R.Draws;

        // ---- The convergence check ----
        // Only meaningful when the link is clean, no recovery is in flight, both peers are inside the
        // same match, and their heads are at the same tick. Everything else is a state the two are
        // ALLOWED to differ in, and asserting there would produce noise rather than a signal.
        CleanStreak = Clean ? CleanStreak + 1 : 0;
        const bool Comparable = CleanStreak >= 1 && !A.Recovering() && !B.Recovering() &&
                                !A.AwaitingResync() && !B.AwaitingResync() && A.MatchStarted() &&
                                B.MatchStarted() && A.MatchIndex() == B.MatchIndex() &&
                                A.ExecTick() == B.ExecTick();
        if (Comparable) {
            ++R.Comparisons;
            const uint64_t Ha = A.GetSim().StateHash();
            const uint64_t Hb = B.GetSim().StateHash();
            if (Ha != Hb) {
                if (R.Disagreements == 0) {
                    R.FirstDivergentTick = A.ExecTick();
                    R.HashA = Ha;
                    R.HashB = Hb;
                }
                ++R.Disagreements;
            }
        }
        const uint32_t Live = static_cast<uint32_t>(A.GetSim().AliveCount(0) + A.GetSim().AliveCount(1));
        if (Live > R.PeakAlive) R.PeakAlive = Live;

        // ---- The authoritative reading ----
        // Due on the phase, but DEFERRED to the next clean exchange: a checkpoint that landed inside a
        // silence window or on a dropped frame would read the outage as a failure to converge. Pending
        // rather than skipped, because at a high fault rate the phase tick is frequently dirty and
        // simply skipping cost the run all but its final checkpoint — which then reported "too few
        // readings" instead of the netcode result it was there to measure.
        if (T % CheckpointPeriod == CheckpointPhase) CheckpointDue = true;
        if (Clean && CheckpointDue) {
            CheckpointDue = false;
            ++R.Checkpoints;
            if (!SettleAndCompare(A, B, Qa, Qb, Ai0, Ai1, GCheckpointRounds, &R.SettleTicks)) {
                if (R.CheckpointFailures == 0) {
                    R.FirstFailedCheckpoint = T;
                    R.FirstFailReason = WhyNotConverged(A, B);
                }
                ++R.CheckpointFailures;
                if (Spec.TraceTicks)
                    std::printf(
                        "    ckpt FAIL T=%u  A[t=%u m=%u seed=%llx h=%08x] B[t=%u m=%u seed=%llx "
                        "h=%08x]  %s\n",
                        T, A.ExecTick(), A.MatchIndex(),
                        static_cast<unsigned long long>(A.GetSim().Seed),
                        static_cast<uint32_t>(A.GetSim().StateHash()), B.ExecTick(),
                        B.MatchIndex(), static_cast<unsigned long long>(B.GetSim().Seed),
                        static_cast<uint32_t>(B.GetSim().StateHash()), WhyNotConverged(A, B));
            } else if (BookkeepingDrift(A, B)) {
                if (Spec.TraceTicks)
                    std::printf("    drift T=%u  A[t=%u m=%u seed=%llx] B[t=%u m=%u seed=%llx]\n", T,
                                A.ExecTick(), A.MatchIndex(),
                                static_cast<unsigned long long>(A.GetSim().Seed), B.ExecTick(),
                                B.MatchIndex(), static_cast<unsigned long long>(B.GetSim().Seed));
                // Converged, so the checkpoint PASSES — but the two peers label this match
                // differently, which is its own defect and must not hide behind a green convergence.
                if (R.DriftCheckpoints == 0) R.FirstDriftTick = T;
                ++R.DriftCheckpoints;
            }
            PrevExec = A.ExecTick();
            R.LastProgressTick = T;
        }

        if (Spec.TraceTicks && TraceLeft == 0 && !Traced &&
            (Spec.TraceFrom ? T >= Spec.TraceFrom : R.Disagreements != 0))
            TraceLeft = Spec.TraceTicks;
        if (TraceLeft) {
            Traced = true;
            --TraceLeft;
            std::printf(
                "    T=%-5u A[t=%-5u h=%08x rec=%d awa=%d] B[t=%-5u h=%08x rec=%d awa=%d] %s\n", T,
                A.ExecTick(), static_cast<uint32_t>(A.GetSim().StateHash()), A.Recovering() ? 1 : 0,
                A.AwaitingResync() ? 1 : 0, B.ExecTick(),
                static_cast<uint32_t>(B.GetSim().StateHash()), B.Recovering() ? 1 : 0,
                B.AwaitingResync() ? 1 : 0, Clean ? "" : "FAULT");
        }

        // The Silence row's own property: nothing may be CONCLUDED while the link is dark.
        // #162's property for the Silence row — nothing may be CONCLUDED while the link is dark —
        // is asserted as "no DRAW", not "still ongoing". Over a long run matches finish legitimately
        // (8000 ticks is four of them), and a team winning during a dark window is gameplay, not the
        // netcode giving up. The draw is the netcode's outcome, and R.Draws already counts it for the
        // whole run. The freeze half of #162 is covered by LastProgressTick.
    }

    // A final checkpoint, so a run never ends on an unresolved reading — the state the pair is left in
    // is the state that matters, and a run that stopped mid-repair would otherwise report nothing.
    ++R.Checkpoints;
    if (!SettleAndCompare(A, B, Qa, Qb, Ai0, Ai1, GCheckpointRounds, &R.SettleTicks)) {
        if (R.CheckpointFailures == 0) R.FirstFailedCheckpoint = R.TicksRun;
        ++R.CheckpointFailures;
    }

    Dsy.Sample(A.DesyncsSeen() + B.DesyncsSeen());
    Rnd.Sample(static_cast<uint32_t>(A.RecoveryRounds() + B.RecoveryRounds()));
    Gap.Sample(static_cast<uint32_t>(A.GapRecoveries() + B.GapRecoveries()));
    Igp.Sample(static_cast<uint32_t>(A.InputGaps() + B.InputGaps()));
    Dup.Sample(static_cast<uint32_t>(A.DuplicateFrames() + B.DuplicateFrames()));
    Rbk.Sample(static_cast<uint32_t>(A.Rollbacks() + B.Rollbacks()));
    R.DriftAtEnd = BookkeepingDrift(A, B);
    R.DesyncsSeen = Dsy.Total;
    R.RecoveryRounds = Rnd.Total;
    R.GapRecoveries = Gap.Total;
    R.InputGaps = Igp.Total;
    R.DuplicateFrames = Dup.Total;
    R.Rollbacks = Rbk.Total;
    R.Alive0 = A.GetSim().AliveCount(0);
    R.Alive1 = A.GetSim().AliveCount(1);
    R.ExecTick = A.ExecTick();
    return R;
}

// ---------------------------------------------------------------------------------------------
// Assertions per row
// ---------------------------------------------------------------------------------------------
static void CheckRow(const SoakSpec& Spec, const SoakResult& R) {
    std::printf(
        "  %-20s seed=0x%llx ticks=%u(+%u) exec=%u cmp=%u/%u ckpt=%u/%u match=%u alive=%d/%d peak=%u "
        "desync=%u rounds=%d gaps=%d dup=%d rb=%d\n",
        RowName(Spec.Row), static_cast<unsigned long long>(Spec.Seed), R.TicksRun, R.SettleTicks,
        R.ExecTick, R.Disagreements, R.Comparisons, R.CheckpointFailures, R.Checkpoints,
        R.MatchesResolved, R.Alive0, R.Alive1, R.PeakAlive, R.DesyncsSeen, R.RecoveryRounds,
        R.InputGaps, R.DuplicateFrames, R.Rollbacks);

    if (R.Disagreements)
        std::printf("    first divergence at exec tick %u: A=0x%016llx B=0x%016llx\n",
                    R.FirstDivergentTick, static_cast<unsigned long long>(R.HashA),
                    static_cast<unsigned long long>(R.HashB));
    if (R.CheckpointFailures)
        std::printf("    NOT CONVERGED at checkpoint, first at tick %u — %s\n",
                    R.FirstFailedCheckpoint, R.FirstFailReason);

    // The authoritative reading, and it applies to EVERY row without exception. Whatever the fault
    // was, a quiet moment must find the two peers playing the same game.
    CHECK(R.CheckpointFailures == 0);

    // And playing the same NUMBERED game — but the assertion is that drift does not PERSIST, not that
    // it never appears. Two peers whose sims end a match a few ticks apart genuinely ARE in different
    // matches for that window, and #214's ordinal exchange corrects the label at the next boundary
    // rather than preventing the window. What breaks recordings and score tallies is a session that
    // ENDS disagreeing, because nothing realigns it after that. Before #214 that was the normal
    // outcome — 21 of 41 checkpoints drifted and the seeds never re-converged.
    if (R.DriftCheckpoints)
        std::printf("    match bookkeeping drift from tick %u (%u checkpoints, %s) — states agree, "
                    "MatchIndex/Seed do not\n", R.FirstDriftTick, R.DriftCheckpoints,
                    R.DriftAtEnd ? "STILL DRIFTED AT END" : "realigned");
    CHECK(!R.DriftAtEnd);

    // Nothing these rows inject ever reaches an executed tick, so a single disagreeing hash is a bug.
    if (RowMustNeverDiverge(Spec.Row)) CHECK(R.Disagreements == 0);

    // ---- Proof the injection LANDED ----
    // Every fault row needs one, or a row whose injection silently stopped working (a codec change, a
    // slot that is no longer live, a schedule that no longer fires) goes green having tested nothing.
    // That is the exact trap the 2026-08-16 test files fell into, and it is cheap to close.
    if (RowExpectsGaps(Spec.Row)) CHECK(R.InputGaps > 0);
    if (Spec.Row == ERow::Duplicate) CHECK(R.DuplicateFrames > R.TicksRun / 2);
    if (Spec.Row == ERow::Forge) {
        CHECK(R.Disagreements > 0);   // the forged event really did move executed state
        CHECK(R.DesyncsSeen > 0);     // and the netcode's own cross-check noticed, not just this file
    }
    if (Spec.Row == ERow::SilenceThenResume) {
        // The outage cost real wall time, so the pair must end up meaningfully behind the tick count a
        // clean run reaches. Without this the row would pass with the outage window never firing.
        CHECK(R.ExecTick + OutageTicks / 2 < R.TicksRun);
    }

    // The survivor never adopts (#210). Its timeline IS the tie-break winner, so there is nothing it
    // could rebuild from — waiting means the pair is deadlocked on the loser.
    if (R.SurvivorAwaited) std::printf("    SURVIVOR AWAITED RESYNC at tick %u\n", R.SurvivorAwaitedTick);
    CHECK(!R.SurvivorAwaited);

    // A netcode draw is not an acceptable outcome of an RPS match (owner ruling, 2026-08-16).
    CHECK(R.Draws == 0);

    // The checks must have RUN. Without this the file goes green the moment a reading stops being
    // satisfiable — which is exactly how three test files written on 2026-08-16 passed against
    // deliberately sabotaged implementations. The per-tick floor is per-row because the fault rows
    // spend real time in recovery and inside a silence window, where comparing state is not valid.
    CHECK(R.Checkpoints >= 2);
    // ONE floor for every row, not a tuned fraction per row. The floor exists to catch a run that
    // compared NOTHING — the vacuous-green failure mode — and an eighth of the run (plus an absolute
    // minimum for short debugging runs) is nowhere near vacuous: 1000 comparisons at the soak length.
    // Per-row fractions were three numbers that had to be re-tuned every time a fault rate changed,
    // and each re-tune was indistinguishable from tuning the test to pass.
    CHECK(R.Comparisons > 200 && R.Comparisons > R.TicksRun / 8);

    // The match must have MOVED. A pair that converges by both freezing is the #162 failure, and it
    // satisfies every hash comparison above — "alive" was true throughout the 2026-08-16 hardware
    // failure too, in both directions.
    CHECK(R.LastProgressTick + 250 > R.TicksRun);

    // And it must be a real MATCH, not two empty fields agreeing about nothing. An idle sim hashes
    // identically on both peers forever, so without this the whole soak is vacuous. Scaled to the
    // duration because the army takes a few hundred ticks to exist at all — a short `--ticks` run is a
    // debugging tool, not a proof.
    if (R.TicksRun >= 600) CHECK(R.PeakAlive > 20);
    CHECK(R.Rollbacks > 0);   // real events crossed the wire and contradicted the idle prediction
}

// ---------------------------------------------------------------------------------------------
int main(int Argc, char** Argv) {
    // Gate run: cheap enough for build.ps1, long enough that a one-index timeline skew cannot hide in
    // a run of empty batches (the way the hardware skew stayed invisible for ~13 minutes).
    uint32_t Ticks = 1200;
    int Seeds = 1;
    uint64_t Seed0 = 0x50AC0000ull;
    const char* OnlyRow = nullptr;
    uint32_t TraceTicks = 0;
    uint32_t TraceFrom = 0;

    for (int I = 1; I < Argc; ++I) {
        const char* Arg = Argv[I];
        const bool HasNext = I + 1 < Argc;
        if (std::strcmp(Arg, "--long") == 0) {
            // 8000 ticks is ~4 full matches, so the soak covers the post-match restart path several
            // times per seed rather than exercising one long opening. Four seeds keeps it around six
            // minutes: long enough to be worth running by hand, short enough that it gets run.
            Ticks = 8000;
            Seeds = 4;
        } else if (std::strcmp(Arg, "--ticks") == 0 && HasNext) {
            Ticks = static_cast<uint32_t>(std::strtoul(Argv[++I], nullptr, 0));
        } else if (std::strcmp(Arg, "--seeds") == 0 && HasNext) {
            Seeds = static_cast<int>(std::strtol(Argv[++I], nullptr, 0));
        } else if (std::strcmp(Arg, "--seed0") == 0 && HasNext) {
            Seed0 = std::strtoull(Argv[++I], nullptr, 0);
        } else if (std::strcmp(Arg, "--row") == 0 && HasNext) {
            OnlyRow = Argv[++I];
        } else if (std::strcmp(Arg, "--ckptrounds") == 0 && HasNext) {
            GCheckpointRounds = static_cast<int>(std::strtol(Argv[++I], nullptr, 0));
        } else if (std::strcmp(Arg, "--faultperiod") == 0 && HasNext) {
            GFaultPeriod = static_cast<uint32_t>(std::strtoul(Argv[++I], nullptr, 0));
        } else if (std::strcmp(Arg, "--trace") == 0 && HasNext) {
            TraceTicks = static_cast<uint32_t>(std::strtoul(Argv[++I], nullptr, 0));
        } else if (std::strcmp(Arg, "--settletrace") == 0) {
            GSettleTrace = true;
        } else if (std::strcmp(Arg, "--tracefrom") == 0 && HasNext) {
            TraceFrom = static_cast<uint32_t>(std::strtoul(Argv[++I], nullptr, 0));
        } else {
            std::printf("rps_two_peer_soak: unknown argument '%s'\n", Arg);
            return 2;
        }
    }

    const ERow Rows[] = {ERow::Clean,   ERow::SimultaneousDrop, ERow::OneWayDrop,
                         ERow::Duplicate, ERow::Forge,          ERow::Silence,
                         ERow::SilenceThenResume};

    for (int S = 0; S < Seeds; ++S) {
        const uint64_t Seed = Seed0 + static_cast<uint64_t>(S);
        for (ERow Row : Rows) {
            if (OnlyRow && std::strcmp(OnlyRow, RowName(Row)) != 0) continue;
            SoakSpec Spec;
            Spec.Seed = Seed;
            Spec.Ticks = Ticks;
            Spec.Row = Row;
            Spec.TraceTicks = TraceTicks;
            Spec.TraceFrom = TraceFrom;
            const SoakResult R = RunSoak(Spec);
            CheckRow(Spec, R);
        }
    }

    if (GFailures == 0) std::printf("rps_two_peer_soak: ALL PASS\n");
    else std::printf("rps_two_peer_soak: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
