// Host tests for the single-player AiController (#124-#126). It's a pure function of sim state
// + tick, so no renderer is needed: we drive a Sim directly with the AI filling one team's
// mask and assert determinism, the fixed opening, the counter choice, and that a sharper tier
// reads the enemy sooner than a fuzzier one.
#include <cstdint>
#include <cstdio>

#include "Lur/Core/CVar.h"
#include "Rps/AiController.h"
#include "Rps/Sim.h"
#include "Rps/Tunables.h"

using namespace Rps;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// Append N alive units of (Team, Type) directly to the board (bypasses production) so a test
// can set up an arbitrary composition to react to.
static void Inject(Sim& S, uint8_t Team, uint8_t Type, int N) {
    for (int K = 0; K < N; ++K) {
        const int32_t I = S.Count++;
        S.Type[I] = Type;
        S.Team[I] = Team;
        S.Hp[I] = S.Units[Type].MaxHp;
        S.AliveBits[I >> 6] |= (1ull << (I & 63));
    }
}

// Drop a BUILDING (produces Type) onto the board so a test can put the AI past the forced
// first-camp placement and into its production/counter logic.
static int32_t InjectBuilding(Sim& S, uint8_t Team, uint8_t Type, Fixed X, Fixed Y) {
    const int32_t I = S.Count++;
    S.PosX[I] = X; S.PosY[I] = Y; S.PrevX[I] = X; S.PrevY[I] = Y;
    S.Type[I] = Type; S.Team[I] = Team; S.Hp[I] = BuildingHpFor(S.Cv, Type);
    S.Kind[I] = KindBuilding; S.Queue[I] = 0; S.BuildProgress[I] = 0;
    S.Target[I] = -1; S.Cooldown[I] = 0;
    S.AliveBits[I >> 6] |= (1ull << (I & 63));
    return I;
}

// The AI's events this tick (Count set); Out[0] is the primary action.
static int AiTick(AiController& Ai, const Sim& S, uint32_t Tick, InputEvent* Out) {
    int C = 0;
    Ai.DecideEvents(S, Tick, Out, MaxEventsPerTick, C);
    return C;
}

// The AI plays team 1; team 0 is idle. Run Ticks steps, return the final StateHash.
static uint64_t RunAiVsIdle(uint64_t Seed, EAiTier Tier, int Ticks) {
    Sim S;
    S.Init(Seed);
    AiController Ai;
    Ai.Init(Seed, 1, Tier);
    for (int T = 0; T < Ticks; ++T) {
        InputEvent E[MaxEventsPerTick];
        const int C = AiTick(Ai, S, S.Tick, E);
        S.StepEvents(E, C);
    }
    return S.StateHash();
}

static void TestDeterminism() {
    // Same seed + tier -> identical AI play -> identical final state (the load-bearing property).
    const uint64_t A = RunAiVsIdle(0x1234, EAiTier::Medium, 300);
    const uint64_t B = RunAiVsIdle(0x1234, EAiTier::Medium, 300);
    CHECK(A == B);
    // (Tier *differentiation* is proven by TestTierReactionSpeed — it needs an enemy to react
    // to; vs an idle foe the tiers legitimately play the same opening, so no != assertion here.)
}

static void TestOpening() {
    // Fresh board, no buildings yet. The AI's FORCED first action is to place a mining camp
    // (all tiers, full-speed) — the pre-match ready move.
    Sim S;
    S.Init(0x1234);
    AiController Ai;
    Ai.Init(0x1234, 1, EAiTier::Medium);
    InputEvent E[MaxEventsPerTick];
    const int C = AiTick(Ai, S, 0, E);
    CHECK(C == 1 && E[0].Kind == EventPlaceBuilding && E[0].Type == UnitMiner);
}

static void TestCounterChoice() {
    // Hard (staleness 0, exact) facing 6 enemy Rocks, ALREADY has a mining camp, past its
    // opening, gold to spend -> it builds the counter to Rock = Paper (places a Paper building,
    // since it has none yet).
    Sim S;
    S.Init(0x1234);
    Inject(S, /*team*/ 0, UnitRock, 6);            // enemy army
    Inject(S, /*team*/ 1, UnitMiner, 6);           // my economy, past Hard's OpenWorkers (5)
    InjectBuilding(S, 1, UnitMiner, F(17), F(230));// AI already has its camp
    S.Teams[1].Gold = 100000;
    AiController Ai;
    Ai.Init(0x1234, 1, EAiTier::Hard);
    InputEvent E[MaxEventsPerTick];
    const int C = AiTick(Ai, S, 0, E);
    CHECK(C == 1 && E[0].Kind == EventPlaceBuilding && E[0].Type == UnitPaper);
}

// Tick at which the tier first acts on the correct counter (a Paper building) against a static
// Rock army. The board is held fixed (no Step) to isolate the AI's reaction timing.
static uint32_t FirstCounterTick(EAiTier Tier) {
    Sim S;
    S.Init(0x1234);
    Inject(S, 0, UnitRock, 6);
    Inject(S, 1, UnitMiner, 6);
    InjectBuilding(S, 1, UnitMiner, F(17), F(230));  // camp present -> AI is in production logic
    S.Teams[1].Gold = 100000;
    AiController Ai;
    Ai.Init(0x1234, 1, Tier);
    for (uint32_t T = 0; T < 512; ++T) {
        InputEvent E[MaxEventsPerTick];
        const int C = AiTick(Ai, S, T, E);
        if (C == 1 && E[0].Kind == EventPlaceBuilding && E[0].Type == UnitPaper) return T;
    }
    return 0xFFFFFFFFu;  // never
}

static void TestTierReactionSpeed() {
    // Difficulty = information quality + cadence: Hard (current, exact, fast) locks the counter
    // essentially immediately; Easy (stale, fuzzy, slow) takes many ticks to react to the same
    // army. This IS the fair-but-adjustable design.
    const uint32_t Hard = FirstCounterTick(EAiTier::Hard);
    const uint32_t Easy = FirstCounterTick(EAiTier::Easy);
    CHECK(Hard == 0);            // sees the current board, reacts at once
    CHECK(Easy > 60);            // must wait out its staleness before the mirror shows the army
    CHECK(Hard < Easy);
}

// Two AIs in one process (the #128 harness) must be deterministic: same seeds -> same match.
static uint64_t RunAiVsAi(uint64_t Seed, EAiTier A, EAiTier B, int MaxTicks, uint8_t& OutResult) {
    Sim S;
    S.Init(Seed);
    AiController Ai0, Ai1;
    Ai0.Init(Seed, 0, A);
    Ai1.Init(Seed, 1, B);
    for (int T = 0; T < MaxTicks && S.Result == ResultOngoing; ++T) {
        InputEvent E0[MaxEventsPerTick], E1[MaxEventsPerTick];
        const int C0 = AiTick(Ai0, S, S.Tick, E0);
        const int C1 = AiTick(Ai1, S, S.Tick, E1);
        InputEvent Comb[2 * MaxEventsPerTick];
        int NC = 0;
        for (int I = 0; I < C0; ++I) Comb[NC++] = E0[I];  // team 0 first (matches Execute order)
        for (int I = 0; I < C1; ++I) Comb[NC++] = E1[I];
        S.StepEvents(Comb, NC);
    }
    OutResult = S.Result;
    return S.StateHash();
}

static void TestAiVsAiDeterminism() {
    uint8_t R1 = 0, R2 = 0;
    const uint64_t H1 = RunAiVsAi(0x1234, EAiTier::Hard, EAiTier::Easy, 500, R1);
    const uint64_t H2 = RunAiVsAi(0x1234, EAiTier::Hard, EAiTier::Easy, 500, R2);
    CHECK(H1 == H2);
    CHECK(R1 == R2);
}

int main() {
    Lur::Core::CVarEnterMain();  // CVars may not be read before main() (spec §1.1)
    TestDeterminism();
    TestOpening();
    TestCounterChoice();
    TestTierReactionSpeed();
    TestAiVsAiDeterminism();
    if (GFailures == 0) { std::printf("rps_ai_tests: ALL PASS\n"); return 0; }
    std::printf("rps_ai_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
