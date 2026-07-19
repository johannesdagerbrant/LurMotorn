// Host correctness tests for the RPS sim core. Two things matter most here:
//   1. DETERMINISM — the load-bearing property the whole slice-1 lockstep netcode
//      rests on: same seed + same input stream => bit-identical StateHash sequence,
//      across two independent runs. If this ever fails, lockstep is impossible.
//   2. The spec's rule EDGES — win/draw reachability, and the deterministic no-op
//      of a broke/full production press.
// Same hand-rolled harness as chess's tests (CHECK macro + failure count) — there
// is no shared framework by design.
#include <cstdint>
#include <cstdio>

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
    CHECK(A.Count > StartLumberjacks * 2);
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

// ---- 2. Win rule (spec §6, edge-proof) ----
static void TestMutualAnnihilationDraw() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    KillTeam(S, 1);
    S.Teams[0].Wood = 0;
    S.Teams[1].Wood = 0;
    S.Step(0, 0);  // win check runs at phase 7
    CHECK(S.Result == ResultDraw);
}

static void TestWipeoutLoses() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    S.Teams[0].Wood = 0;  // no units, no queue, can't rebuy -> team 0 loses
    S.Step(0, 0);
    CHECK(S.Result == ResultTeam1Wins);
}

static void TestRebuyIsNotLoss() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    S.Teams[0].Wood = CheapestCost;  // zero units but can still rebuy -> NOT a loss
    S.Step(0, 0);
    CHECK(S.Result == ResultOngoing);
}

// ---- 3. Production press edges (deterministic no-op) ----
static void TestBrokePressIgnored() {
    static Sim S;
    S.Init(0);
    S.Teams[0].Wood = UnitTable[UnitRock].Cost - 1;  // one short
    const int32_t WoodBefore = S.Teams[0].Wood;
    S.Step(1u << UnitRock, 0);
    CHECK(S.Teams[0].QueueLen == 0);
    CHECK(S.Teams[0].Wood == WoodBefore);  // no partial reservation
}

static void TestQueueFullIgnored() {
    static Sim S;
    S.Init(0);
    S.Teams[0].Wood = 100000;
    for (int K = 0; K < QueueDepth; ++K) S.Teams[0].Queue[K] = UnitScissor;
    S.Teams[0].QueueLen = QueueDepth;
    S.Teams[0].BuildTimer = 100;  // high, so nothing spawns/pops this tick
    const int32_t WoodBefore = S.Teams[0].Wood;
    S.Step(1u << UnitLumberjack, 0);
    CHECK(S.Teams[0].QueueLen == QueueDepth);   // still full — press ignored
    CHECK(S.Teams[0].Wood == WoodBefore);       // and nothing was spent
}

// ---- Tick-phase behaviour sanity ----
static void TestProductionSpawnsAfterBuildTime() {
    static Sim S;
    S.Init(0);
    const int32_t Before = S.AliveCount(0);
    S.Step(1u << UnitScissor, 0);  // enqueue a Scissor (cost 50, build 50 ticks)
    CHECK(S.Teams[0].QueueLen == 1);
    // Enqueue tick already decremented the head once; BuildTicks-1 more to spawn.
    for (int I = 0; I < UnitTable[UnitScissor].BuildTicks; ++I) S.Step(0, 0);
    CHECK(S.AliveCount(0) == Before + 1);
    CHECK(S.Teams[0].QueueLen == 0);
}

static void TestEconomyGathersWood() {
    static Sim S;
    S.Init(0);
    const int32_t Before = S.Teams[0].Wood;
    for (int I = 0; I < 300; ++I) S.Step(0, 0);  // idle: the 3 starting workers just gather
    CHECK(S.Teams[0].Wood > Before);  // at least one full round trip deposited
}

int main() {
    TestDeterminism();
    TestReplayReproducibility();
    TestMutualAnnihilationDraw();
    TestWipeoutLoses();
    TestRebuyIsNotLoss();
    TestBrokePressIgnored();
    TestQueueFullIgnored();
    TestProductionSpawnsAfterBuildTime();
    TestEconomyGathersWood();

    if (GFailures == 0) std::printf("rps_sim_tests: ALL PASS\n");
    else std::printf("rps_sim_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
