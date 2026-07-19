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

// ---- Grid vs brute-force equivalence (spatial grid, design §5) ----
// End-to-end: the same seed + input stream, once on the grid path and once on
// brute force, must produce a bit-identical StateHash EVERY tick. Stronger than a
// single-state check — it exercises the grid across a whole evolving match (spawns,
// clashing armies driving nearest-enemy ring search, dense separation).
static void TestGridEqualsBruteForce() {
    constexpr int Ticks = 800;
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
    std::printf("  stress: %d units, %.3f ms/tick over %d ticks (10 Hz budget = 100 ms)\n",
                S.Count, Ms / Ticks, Ticks);
    CHECK(S.Count > 0);
}
#endif

int main() {
    TestDeterminism();
    TestReplayReproducibility();
    TestGridEqualsBruteForce();
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
