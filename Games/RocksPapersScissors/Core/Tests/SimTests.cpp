// Host correctness tests for the RPS sim core. Two things matter most here:
//   1. DETERMINISM — the load-bearing property the whole slice-1 lockstep netcode
//      rests on: same seed + same input stream => bit-identical StateHash sequence,
//      across two independent runs. If this ever fails, lockstep is impossible.
//   2. The spec's rule EDGES — win/draw reachability, and the deterministic no-op
//      of a broke/full production press.
// Same hand-rolled harness as chess's tests (CHECK macro + failure count) — there
// is no shared framework by design.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

#include "Lur/Sim/Random.h"
#include "Rps/Sim.h"

using namespace Rps;
using Lur::Sim::SplitMix64;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// Clear a team's alive bits — sets up win-rule edge states that are otherwise
// tedious to reach through play. Sim state is public POD on purpose.
static void KillTeam(Sim& S, uint8_t Team) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.Team[I] == Team) S.AliveBits[I >> 6] &= ~(1ull << (I & 63));
}

// ---- 1. Determinism: two independent runs, identical hash every tick ----
static void TestDeterminism() {
    constexpr int Ticks = 600;
    constexpr uint64_t Seed = 0x0123456789ABCDEFull;

    // A pre-recorded random input stream (the flight-recorder's payload, in effect).
    static uint8_t M0[Ticks], M1[Ticks];
    SplitMix64 Rng(0xC0FFEE);
    for (int I = 0; I < Ticks; ++I) {
        M0[I] = static_cast<uint8_t>(Rng.NextBounded(16));  // 4-bit button mask
        M1[I] = static_cast<uint8_t>(Rng.NextBounded(16));
    }

    static Sim A, B;  // static: ~200 KB each, keep them off the stack
    A.Init(Seed);
    B.Init(Seed);
    CHECK(A.StateHash() == B.StateHash());  // identical from tick 0

    bool AllMatch = true;
    for (int I = 0; I < Ticks; ++I) {
        A.Step(M0[I], M1[I]);
        B.Step(M0[I], M1[I]);
        if (A.StateHash() != B.StateHash()) { AllMatch = false; break; }
    }
    CHECK(AllMatch);
    // The match should actually DO something over 600 ticks (units spawned/moved),
    // otherwise "deterministic" is trivially true over an empty sim.
    CHECK(A.Count > StartMiners * 2);
}

// A fresh sim replaying the same stream must reach the same final hash — the
// replay law (State = Replay(Inputs, Seed)) that resync + the recorder depend on.
static void TestReplayReproducibility() {
    constexpr int Ticks = 300;
    constexpr uint64_t Seed = 0xFEEDBEEFu;
    static uint8_t M0[Ticks], M1[Ticks];
    SplitMix64 Rng(0x5EED);
    for (int I = 0; I < Ticks; ++I) {
        M0[I] = static_cast<uint8_t>(Rng.NextBounded(16));
        M1[I] = static_cast<uint8_t>(Rng.NextBounded(16));
    }
    static Sim Live;
    Live.Init(Seed);
    for (int I = 0; I < Ticks; ++I) Live.Step(M0[I], M1[I]);
    const uint64_t Final = Live.StateHash();

    static Sim Replay;
    Replay.Init(Seed);
    for (int I = 0; I < Ticks; ++I) Replay.Step(M0[I], M1[I]);
    CHECK(Replay.StateHash() == Final);
}

// ---- #112: a latched AffectsGameplay CVar override changes the sim deterministically ----
// Exercises both halves of the CVar-determinism design: the per-tick latch (Sim::Step's
// Cv = LatchCvs()) means an override applied between runs takes effect, and folding Cv
// into StateHash means two runs with the same override hash identically while a different
// override diverges. This is the sim-side proof under which the Addendum-C peer sync sits.
static void TestCVarOverrideDeterminism() {
    constexpr int Ticks = 400;
    constexpr uint64_t Seed = 0x112C0DEull;
    static uint8_t M0[Ticks], M1[Ticks];
    SplitMix64 Rng(0x112);
    for (int I = 0; I < Ticks; ++I) {
        M0[I] = static_cast<uint8_t>(Rng.NextBounded(16)) | 0x2;  // bias to soldiers so WSeek bites
        M1[I] = static_cast<uint8_t>(Rng.NextBounded(16)) | 0x4;
    }
    auto Run = [&]() {
        static Sim S;  // static: keep the ~200 KB off the stack
        S.Init(Seed);
        for (int I = 0; I < Ticks; ++I) S.Step(M0[I], M1[I]);
        return S.StateHash();
    };

    CvWSeek.Reset();
    const uint64_t Base = Run();
    CHECK(CvWSeek.SetFromString("3.0"));  // shove the goal-seek weight far from its default
    const uint64_t Over1 = Run();
    const uint64_t Over2 = Run();
    CvWSeek.Reset();
    const uint64_t BaseAgain = Run();

    CHECK(Over1 != Base);      // the gameplay knob genuinely alters the simulation...
    CHECK(Over1 == Over2);     // ...deterministically (same override -> identical hash)
    CHECK(BaseAgain == Base);  // and Reset() restores the exact baseline
}

// ---- Grid vs brute-force equivalence (spatial grid, design §5) ----
// End-to-end: the same seed + input stream, once on the grid path and once on
// brute force, must produce a bit-identical StateHash EVERY tick. Stronger than a
// single-state check — it exercises the grid across a whole evolving match (spawns,
// clashing armies driving nearest-enemy ring search, dense separation).
//
// The brute path is O(n^2)/tick, so the deep-match tail dominates the CI gate (~16s of
// rps_sim_tests at -O0). Any grid/brute divergence surfaces within the first ~100 ticks
// once armies clash and dense separation kicks in, so the everyday (Development) gate runs
// a lean 250-tick sweep across the 3 seeds; the exhaustive 800-tick audit runs under
// LUR_SLOW (the Debugging build) where expensive validation belongs (CLAUDE.md ladder).
static void TestGridEqualsBruteForce() {
    constexpr int Ticks = LUR_SLOW ? 800 : 250;
    for (uint64_t Seed : {uint64_t(1), uint64_t(0xABCDEF), uint64_t(0xDEADBEEF)}) {
        static uint8_t M0[Ticks], M1[Ticks];
        SplitMix64 Rng(Seed ^ 0x9999);
        for (int I = 0; I < Ticks; ++I) {
            // Bias toward soldier presses (bits 1..3) so armies actually clash and
            // the nearest-enemy grid search is genuinely exercised.
            M0[I] = static_cast<uint8_t>(Rng.NextBounded(16)) | 0x2;
            M1[I] = static_cast<uint8_t>(Rng.NextBounded(16)) | 0x4;
        }
        static Sim Grid, Brute;
        Grid.Init(Seed);
        Brute.Init(Seed);
        Brute.UseBruteForce = true;  // after Init (Init resets the flag)

        bool Match = true;
        int FirstDiverge = -1;
        for (int I = 0; I < Ticks; ++I) {
            Grid.Step(M0[I], M1[I]);
            Brute.Step(M0[I], M1[I]);
            if (Grid.StateHash() != Brute.StateHash()) { Match = false; FirstDiverge = I; break; }
        }
        if (!Match) std::printf("  grid!=brute seed=%llu diverged at tick %d\n",
                                static_cast<unsigned long long>(Seed), FirstDiverge);
        CHECK(Match);
        CHECK(Grid.Count > StartMiners * 2);  // the match did something
    }
}

// ---- Flocking (boids slice A, #96) ----
// Cohesion POLARITY: grid≡brute can't catch a toward/away sign error (both paths would be
// wrong identically), so assert the behaviour directly — a same-type group must CONTRACT,
// never explode. Setup: 6 Papers on team 0 spread across X, no enemies, no mines. They
// march up (seek's X-component is ~0 at that distance), so any X-contraction is cohesion;
// a flipped sign would blow the group apart instead. Also a no-overflow guard (positions
// stay finite / in-bounds — the |force sum| bound the plan §8 asks for).
static void TestSameTypeCohesionContracts() {
    using Lur::Sim::Min; using Lur::Sim::Max;
    static Sim S;
    S.Init(0);
    for (int M = 0; M < NumMines; ++M) S.MineGold[M] = 0;          // mines gone: no repel
    for (int I = 0; I < S.Count; ++I) S.AliveBits[I >> 6] = 0;     // clear the starting miners
    S.Count = 0;
    const Fixed Xs[6] = {F(14), F(15), F(16), F(18), F(19), F(20)};  // spread 6, centred on 17
    for (int K = 0; K < 6; ++K) {
        S.PosX[K] = Xs[K]; S.PosY[K] = F(22);
        S.PrevX[K] = S.PosX[K]; S.PrevY[K] = S.PosY[K];
        S.Hp[K] = UnitTable[UnitPaper].MaxHp;
        S.Type[K] = UnitPaper; S.Team[K] = 0; S.Target[K] = -1;
        S.AliveBits[K >> 6] |= (1ull << (K & 63));
    }
    S.Count = 6;
    auto XSpread = [](const Sim& St) {
        Fixed Lo = St.PosX[0], Hi = St.PosX[0];
        for (int I = 1; I < St.Count; ++I) { Lo = Min(Lo, St.PosX[I]); Hi = Max(Hi, St.PosX[I]); }
        return Hi - Lo;
    };
    const Fixed Before = XSpread(S);
    for (int I = 0; I < 40; ++I) S.Step(0, 0);
    const Fixed After = XSpread(S);
    CHECK(After < Before);                 // cohesion pulled the type together (not apart)
    CHECK(After.Raw > 0);                  // separation kept them from collapsing to a point
    for (int I = 0; I < S.Count; ++I) {    // no overflow: every unit stayed on the field
        CHECK(S.PosX[I].Raw >= 0 && S.PosX[I] <= WorldWidth);
        CHECK(S.PosY[I].Raw >= 0 && S.PosY[I] <= WorldHeight);
    }
}

// DisableCombat (#97 --flockdemo): attacks are suppressed, so no unit ever dies from
// combat — the alive count only grows (production) over a clashing match. Guards the
// demo scene's sim behaviour without needing the GPU/window build.
static void TestDisableCombatNoDeaths() {
    static Sim S;
    S.Init(0);
    S.DisableCombat = true;
    S.Teams[0].Gold = 100000; S.Teams[1].Gold = 100000;
    int32_t Prev = S.AliveCount(0) + S.AliveCount(1);
    bool NeverDropped = true;
    for (int I = 0; I < 200; ++I) {
        S.Step(1u << UnitRock, 1u << UnitScissor);  // both spam warriors that would counter-kill
        const int32_t Now = S.AliveCount(0) + S.AliveCount(1);
        if (Now < Prev) { NeverDropped = false; break; }
        Prev = Now;
    }
    CHECK(NeverDropped);              // combat off => no deaths, count is monotone
    CHECK(S.AliveCount(0) > 0 && S.AliveCount(1) > 0);
}

// Scenario helpers: drop a unit into a slot, and wipe a sim to an empty field.
static void PlaceUnit(Sim& S, int I, Fixed X, Fixed Y, uint8_t Team, uint8_t Type) {
    S.PosX[I] = X; S.PosY[I] = Y; S.PrevX[I] = X; S.PrevY[I] = Y;
    S.Team[I] = Team; S.Type[I] = Type; S.Hp[I] = UnitTable[Type].MaxHp;
    S.Target[I] = -1; S.Cooldown[I] = 0; S.WorkerState[I] = WorkToMine;
    S.Carry[I] = 0; S.WorkerTimer[I] = 0;
    S.AliveBits[I >> 6] |= (1ull << (I & 63));
}
static void ClearField(Sim& S) {
    for (int I = 0; I < S.Count; ++I) S.AliveBits[I >> 6] = 0;
    for (int M = 0; M < NumMines; ++M) S.MineGold[M] = 0;  // no mines: idle carts, controlled scenario
    S.Count = 0;
}

// Targeting (#98): an enemy CART shares the top priority with prey, so it's chosen over a
// NEARER same-type mirror in the same band — economy denial ranks above the even fight.
static void TestCartPriorityOverMirror() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceUnit(S, 0, F(17), F(20), 0, UnitRock);    // defender
    PlaceUnit(S, 1, F(17), F(26), 1, UnitMiner);   // enemy CART (farther)
    PlaceUnit(S, 2, F(17), F(24), 1, UnitRock);    // enemy mirror (nearer)
    S.Count = 3;
    S.Step(0, 0);
    CHECK(S.Target[0] == 1);  // the cart beats the nearer mirror
}

// Interpose (#98): a defender with a friendly cart AND a flagged raider nearby is pulled
// toward the point BETWEEN them (screening the cart), even while its ATTACK target is a
// prey in the opposite direction. Differential: the same setup WITHOUT the cart has no
// interpose, so the defender chases the prey freely — it must end up farther that way.
static void TestInterposeScreensCart() {
    auto Setup = [](Sim& S, bool WithCart) {
        S.Init(0);
        ClearField(S);
        S.DisableCombat = true;                       // isolate movement (no deaths)
        PlaceUnit(S, 0, F(17), F(14), 0, UnitRock);   // defender
        PlaceUnit(S, 1, F(17), F(25), 1, UnitRock);   // raider (mirror) to the NORTH
        PlaceUnit(S, 2, F(17), F(10), 1, UnitScissor);// prey (defender's attack target) to the SOUTH
        int32_t N = 3;
        if (WithCart) { PlaceUnit(S, 3, F(17), F(20), 0, UnitMiner); N = 4; }  // cart to screen (flags the raider)
        S.Count = N;
    };
    static Sim A, B;
    Setup(A, true);
    Setup(B, false);
    for (int I = 0; I < 25; ++I) { A.Step(0, 0); B.Step(0, 0); }
    CHECK(A.PosY[0] > B.PosY[0]);  // interpose held the defender back (north) vs the free chase south
}

// ---- 2. Win rule (spec §6, edge-proof) ----
static void TestMutualAnnihilationDraw() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    KillTeam(S, 1);
    S.Teams[0].Gold = 0;
    S.Teams[1].Gold = 0;
    S.Step(0, 0);  // win check runs at phase 7
    CHECK(S.Result == ResultDraw);
}

static void TestWipeoutLoses() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    S.Teams[0].Gold = 0;  // no units, no queue, can't rebuy -> team 0 loses
    S.Step(0, 0);
    CHECK(S.Result == ResultTeam1Wins);
}

static void TestRebuyIsNotLoss() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    S.Teams[0].Gold = CheapestCost;  // zero units but can still rebuy -> NOT a loss
    S.Step(0, 0);
    CHECK(S.Result == ResultOngoing);
}

// ---- 3. Production press edges (deterministic no-op) ----
static void TestBrokePressIgnored() {
    static Sim S;
    S.Init(0);
    S.Teams[0].Gold = UnitTable[UnitRock].Cost - 1;  // one short
    const int32_t GoldBefore = S.Teams[0].Gold;
    S.Step(1u << UnitRock, 0);
    CHECK(S.Teams[0].QueueCount[UnitRock] == 0);
    CHECK(S.Teams[0].Gold == GoldBefore);  // no partial reservation
}

static void TestQueueCapIgnored() {
    static Sim S;
    S.Init(0);
    S.Teams[0].Gold = 100000;
    S.Teams[0].QueueCount[UnitScissor] = PerTypeQueueCap;  // at the sanity cap
    const int32_t GoldBefore = S.Teams[0].Gold;
    S.Step(1u << UnitScissor, 0);
    // The press is ignored (cap); production still ran — a cap-deep stack (64) beats
    // BuildTicks (50) in one tick, so exactly one scissor spawned from the stack.
    CHECK(S.Teams[0].QueueCount[UnitScissor] == PerTypeQueueCap - 1);
    CHECK(S.Teams[0].Gold == GoldBefore);  // nothing was spent on the ignored press
}

// ---- Tick-phase behaviour sanity ----
static void TestProductionSpawnsAfterBuildTime() {
    static Sim S;
    S.Init(0);
    const int32_t Before = S.AliveCount(0);
    S.Step(1u << UnitScissor, 0);  // enqueue a Scissor (cost 50, build 50 ticks)
    CHECK(S.Teams[0].QueueCount[UnitScissor] == 1);
    // The enqueue tick already advanced progress once (count 1 => +1/tick); a solo
    // unit therefore lands BuildTicks-1 idle ticks later — check the exact edge.
    for (int I = 0; I < UnitTable[UnitScissor].BuildTicks - 2; ++I) S.Step(0, 0);
    CHECK(S.AliveCount(0) == Before);        // one tick early: not yet
    S.Step(0, 0);
    CHECK(S.AliveCount(0) == Before + 1);    // exactly BuildTicks queue-ticks
    CHECK(S.Teams[0].QueueCount[UnitScissor] == 0);
}

// ---- 4. Parallel queues + stack acceleration (#84) ----
static void TestParallelQueuesProgressIndependently() {
    static Sim S;
    S.Init(0);
    S.Teams[0].Gold = 1000;
    const int32_t Before = S.AliveCount(0);
    S.Step((1u << UnitRock) | (1u << UnitPaper), 0);  // both queues start the same tick
    CHECK(S.Teams[0].QueueCount[UnitRock] == 1);
    CHECK(S.Teams[0].QueueCount[UnitPaper] == 1);
    for (int I = 0; I < UnitTable[UnitRock].BuildTicks; ++I) S.Step(0, 0);
    // A single serial queue would need 2x BuildTicks; parallel queues finish together.
    CHECK(S.AliveCount(0) == Before + 2);
}

static void TestStackAccelerationSpawnsFaster() {
    static Sim A, B;
    A.Init(0);
    B.Init(0);
    A.Teams[0].Gold = 1000;
    B.Teams[0].Gold = 1000;
    A.Step(1u << UnitScissor, 0);  // A stacks two scissors...
    A.Step(1u << UnitScissor, 0);
    B.Step(1u << UnitScissor, 0);  // ...B queues one (same elapsed ticks)
    B.Step(0, 0);
    const int32_t ABefore = A.AliveCount(0), BBefore = B.AliveCount(0);
    int32_t AFirst = -1, BFirst = -1;
    for (int I = 0; I < UnitTable[UnitScissor].BuildTicks + 2; ++I) {
        A.Step(0, 0);
        B.Step(0, 0);
        if (AFirst < 0 && A.AliveCount(0) > ABefore) AFirst = I;
        if (BFirst < 0 && B.AliveCount(0) > BBefore) BFirst = I;
    }
    CHECK(AFirst >= 0 && BFirst >= 0);
    CHECK(AFirst < BFirst);  // the deeper stack builds STRICTLY faster (the pacing thesis)
}

// ---- 5. Finite mines (#84) ----
static void TestMineDepletesAndVanishes() {
    static Sim S;
    S.Init(0);
    // Leave a single near-empty mine: exactly one carry in it. Total income is then
    // exactly that carry, and the mine must read as gone (gold 0) afterwards.
    for (int M = 0; M < NumMines; ++M) S.MineGold[M] = 0;
    S.MineGold[0] = CarryCapacity;
    const int32_t Before0 = S.Teams[0].Gold, Before1 = S.Teams[1].Gold;
    for (int I = 0; I < 400; ++I) S.Step(0, 0);
    CHECK(S.MineGold[0] == 0);
    CHECK(S.Teams[0].Gold == Before0 + CarryCapacity);  // mine 0 is team 0's safe cluster
    CHECK(S.Teams[1].Gold == Before1);                  // the far team never got a carry
}

static void TestDepletedMinesStopEconomy() {
    static Sim S;
    S.Init(0);
    for (int M = 0; M < NumMines; ++M) S.MineGold[M] = 0;
    const int32_t Before = S.Teams[0].Gold;
    for (int I = 0; I < 300; ++I) S.Step(0, 0);
    CHECK(S.Teams[0].Gold == Before);          // no phantom income from dead mines
    CHECK(S.Result == ResultOngoing);          // gold >= CheapestCost: still a rebuy, not a loss
}

static void TestEconomyGathersGold() {
    static Sim S;
    S.Init(0);
    const int32_t Before = S.Teams[0].Gold;
    for (int I = 0; I < 300; ++I) S.Step(0, 0);  // idle: the 3 starting workers just gather
    CHECK(S.Teams[0].Gold > Before);  // at least one full round trip deposited
}

#if LUR_INTERNAL
// Stress scene (issue #75): the tick budget at the raised cap. Prints ms/tick — the
// measurement IS the proof; no hard time assert (machine-dependent, would flake). This
// is where the spatial grid earns its keep: at 2048 units the O(n^2) scans would be a
// wall, the grid keeps the tick cheap.
static void TestStressTickBudget() {
    static Sim S;
    S.Init(0x57A9E55);
    S.StressFill(1024);  // per team -> ~2048 units
    CHECK(S.Count > 1500);
    constexpr int Ticks = 60;
    const auto T0 = std::chrono::steady_clock::now();
    for (int I = 0; I < Ticks; ++I) S.Step(0, 0);
    const auto T1 = std::chrono::steady_clock::now();
    const double Ms = std::chrono::duration<double, std::milli>(T1 - T0).count();
    // The flock GATHER is the hot phase (plan §6): each unit visits a cell box sized by
    // FlockGatherR (the largest force radius). Log it — this is the knob to watch on device.
    const int32_t CellK = (FlockGatherR.ToInt() + GridCellSize) / GridCellSize;  // ceil-ish half-width
    const int32_t Box = 2 * CellK + 1;
    std::printf("  stress: %d units, %.3f ms/tick over %d ticks (10 Hz budget = 100 ms); "
                "flock gather = %dx%d cells/unit (FlockGatherR=%d, GridCellSize=%d)\n",
                S.Count, Ms / Ticks, Ticks, Box, Box, FlockGatherR.ToInt(), GridCellSize);
    CHECK(S.Count > 0);
}
#endif

int main() {
    TestDeterminism();
    TestReplayReproducibility();
    TestCVarOverrideDeterminism();
    TestGridEqualsBruteForce();
    TestSameTypeCohesionContracts();
    TestDisableCombatNoDeaths();
    TestCartPriorityOverMirror();
    TestInterposeScreensCart();
    TestMutualAnnihilationDraw();
    TestWipeoutLoses();
    TestRebuyIsNotLoss();
    TestBrokePressIgnored();
    TestQueueCapIgnored();
    TestProductionSpawnsAfterBuildTime();
    TestParallelQueuesProgressIndependently();
    TestStackAccelerationSpawnsFaster();
    TestMineDepletesAndVanishes();
    TestDepletedMinesStopEconomy();
    TestEconomyGathersGold();
#if LUR_INTERNAL
    TestStressTickBudget();
#endif

    if (GFailures == 0) std::printf("rps_sim_tests: ALL PASS\n");
    else std::printf("rps_sim_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
