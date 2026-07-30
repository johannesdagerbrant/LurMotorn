// Host tests for the single-player AiController (#124-#126). It's a pure function of sim state
// + tick, so no renderer is needed: we drive a Sim directly with the AI filling one team's
// mask and assert determinism, the fixed opening, the counter choice, and that a sharper tier
// reads the enemy sooner than a fuzzier one.
#include <cstdint>
#include <cstdio>

#include "Lur/Core/CVar.h"
#include "Rps/AiController.h"
#include "Rps/MatchRecord.h"
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
    // Medium (staleness 0, exact) facing 6 enemy Rocks, ALREADY has a mining camp, past its
    // opening, gold to spend -> it builds the counter to Rock = Paper (places a Paper building,
    // since it has none yet).
    //
    // MEDIUM, NOT HARD, and the distinction is the point of the test rather than a detail: this
    // asserts the SINGLE-COUNTER (argmax) decision, and since the 2026-07-30 ladder collapse the top
    // rung is the #158 mixed-composition path, which deliberately does NOT answer one Rock army with
    // one Paper building — it produces toward a distribution and opens on the tie-break type. Medium
    // is the old hard verbatim (staleness 0, precision 1), so it is the strongest tier that still
    // runs the behaviour under test. TestTopTierHoldsAMixedComposition covers the other path.
    Sim S;
    S.Init(0x1234);
    Inject(S, /*team*/ 0, UnitRock, 6);            // enemy army
    Inject(S, /*team*/ 1, UnitMiner, 6);           // my economy, past Medium's OpenWorkers (5)
    InjectBuilding(S, 1, UnitMiner, F(17), F(230));// AI already has its camp
    S.Teams[1].Gold = 100000;
    AiController Ai;
    Ai.Init(0x1234, 1, EAiTier::Medium);
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
    // Difficulty = information quality + cadence: Medium (current, exact, fast) locks the counter
    // essentially immediately; Easy (stale, fuzzy, slow) takes many ticks to react to the same
    // army. This IS the fair-but-adjustable design.
    //
    // Medium is the fast end of this comparison, not Hard — same reason as TestCounterChoice: it
    // probes "when does it first place the COUNTER building", which is a question only the argmax
    // path answers, and Medium holds the maximum information any tier gets (staleness 0, precision 1)
    // so the contrast with Easy is undiminished.
    const uint32_t Fast = FirstCounterTick(EAiTier::Medium);
    const uint32_t Easy = FirstCounterTick(EAiTier::Easy);
    CHECK(Fast == 0);            // sees the current board, reacts at once
    CHECK(Easy > 60);            // must wait out its staleness before the mirror shows the army
    CHECK(Fast < Easy);
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

// ---- #144: the AI must buy CAPACITY, not bank gold ----
// The defect this guards: the AI used to place a building of a type only when it owned NONE, so it
// was capped at four buildings (one per type) while #132 made throughput scale with building COUNT.
// Income then compounded past the ceiling forever — 17k-26k gold banked with a 15-42 unit army.
// Asserted as PROPERTIES (grew past the old cap, and did not end up rich-and-idle), not as exact
// numbers, so balance tuning doesn't churn the test.
// Asserted on whichever side is still STANDING at the end (most buildings), not on team 0. Since the
// frontier began following each team's frontmost survivor, a mirror match is genuinely decisive: the
// side that gets pushed back loses building space and collapses, so pinning the assertions to team 0
// made this test a coin flip on who won. What it guards is that a competently-played AI side buys
// capacity and converts it — not that both sides do.
// Runs on HARD, not easy. Difficulty is carried by a per-tier production-VOLUME ladder, and easy is
// deliberately capped (max_buildings 4, queue_depth 3) — so asserting unbounded capacity growth on it
// would assert against the tiering itself. Hard is uncapped (max_buildings 0), and "buys capacity and
// converts it" is exactly the property the top rung is supposed to have. (This note said "not medium"
// before the 2026-07-30 collapse, when medium was the capped middle rung; medium is now the old hard
// and is uncapped too, so easy is the tier this test could not run on.)
static void TestAiExpandsCapacity() {
    Sim S;
    S.Init(0x144);
    AiController Ai0, Ai1;
    Ai0.Init(0x144, 0, EAiTier::Hard);
    Ai1.Init(0x144, 1, EAiTier::Hard);
    for (int T = 0; T < 2400 && S.Result == ResultOngoing; ++T) {
        InputEvent E0[MaxEventsPerTick], E1[MaxEventsPerTick];
        const int C0 = AiTick(Ai0, S, S.Tick, E0);
        const int C1 = AiTick(Ai1, S, S.Tick, E1);
        InputEvent Comb[2 * MaxEventsPerTick];
        int NC = 0;
        for (int I = 0; I < C0; ++I) Comb[NC++] = E0[I];
        for (int I = 0; I < C1; ++I) Comb[NC++] = E1[I];
        S.StepEvents(Comb, NC);
    }
    int32_t Buildings[2] = {}, Units[2] = {}, Camps[2] = {};
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        const int T = S.Team[I] & 1;
        if (S.IsBuilding(I)) {
            if (S.IsHomeBase(I)) continue;
            ++Buildings[T];
            if (S.Type[I] == UnitMiner) ++Camps[T];
        } else {
            ++Units[T];
        }
    }
    const int W = Buildings[1] > Buildings[0] ? 1 : 0;   // the side still standing
    CHECK(Buildings[W] > 4);   // the old hard cap was one building per type, four total
    CHECK(Camps[W] > 1);       // it expands the ECONOMY too, not just the army
    CHECK(Units[W] > 60);      // capacity actually became army (was 15-42 for a whole match)
    // Not sitting on a fortune it cannot spend. Bounded generously: it legitimately saves between
    // placements, and the failure being caught is a five-figure hoard, not thrift.
    CHECK(S.Teams[W].Gold < 20000);
}

// ---- The building cap must NEVER make the FIRST soldier building unbuildable ----
// max_buildings counts ALL producing buildings, so a tier chasing worker_target can fill it with
// mining camps and then be forbidden its first combat building forever: it wants soldiers, has
// nowhere to make them, and banks the income. Easy shipped like that. Three recorded matches on
// 2026-07-27 (rps-match-20260727-074028/074257/080444) show the whole match in four numbers:
// 4 camps, 0 soldier buildings, 0 soldiers of any type, gold climbing to 30850 untouched.
//
// Two independent guards, one test each, because they fail differently:
//   * the state must not FORM  -> economy stops one short of the cap while no combat building
//     stands (TestCapReservesASlotForCombat);
//   * the state must CLEAR if it forms anyway -> the cap yields by one for a first combat
//     building (this test, which injects the jammed state directly).
// Injected rather than played out, because reaching it in a real match needs a passive opponent
// AND income that outruns the miner queue (the owner's dig_range tuning supplied the second half;
// stock cvars do not, which is why every existing test stayed green while easy was broken).
// THE 4000-GOLD STALL (owner's 2026-07-28 recordings). counter_chest reserves a counter BUILDING's
// price out of the unit queue — but building prices and unit prices differ by two orders of magnitude
// (scissor building 4000, cart 50), so a 100% chest priced out EVERY action the top tier had,
// including the never-stand-idle cart fallback that exists to prevent exactly that. Measured on the
// device: 16-22s of total silence while holding 3695-3990 gold, idle 45% of his fastest (159s) win.
//
// It was STEERABLE, which is what makes it a test and not a tuning note: lead with Paper, the AI locks
// Scissor as its counter, reserves 4000, and stops playing. Reproduce that exact board — Paper army,
// AI owns no Scissor building, gold parked just under the 4000 it is saving for — and demand that the
// AI emits SOMETHING. Silence is the bug.
static void TestChestNeverPricesOutEveryAction() {
    Sim S;
    S.Init(0x1234);
    Inject(S, 0, UnitPaper, 20);                        // Paper army -> counter is Scissor (4000)
    Inject(S, 1, UnitMiner, 40);
    InjectBuilding(S, 1, UnitMiner, F(4), F(230));      // a camp to queue carts at
    // Just under the Scissor building it wants: the whole purse is inside the reserve.
    S.Teams[1].Gold = BuildingCostFor(S.Cv, UnitScissor) - 55;
    AiController Ai;
    Ai.Init(0x1234, 1, EAiTier::Hard);
    int Silent = 0, Longest = 0;
    for (uint32_t T = 0; T < 400; ++T) {
        InputEvent E[MaxEventsPerTick];
        const int C = AiTick(Ai, S, T, E);
        // Gold is pinned: this asks "given it CANNOT yet afford the building, does it still act?"
        S.Teams[1].Gold = BuildingCostFor(S.Cv, UnitScissor) - 55;
        if (C == 0) { if (++Silent > Longest) Longest = Silent; } else { Silent = 0; }
    }
    // Before the clamp this was 400/400 silent. A few quiet ticks are fine (a full queue is not a
    // stall); minutes of nothing while holding a fortune is the failure.
    CHECK(Longest < 40);
}

// #158: the top tier must hold a DISTRIBUTION, not pick a type. This is the regression test for the
// dominant remaining loss mechanism — measured with --aiowner, the argmax tier fielded ONE soldier
// type (peak one-type share 100%, all three alive at the end in only 4 of 16 matches) against an
// opponent rotating all three, so a third of his army hard-countered everything it had at
// counter_mult 3. It lost 12 of 16 that way.
//
// The assertion is on the SET of types it produces, not on a win rate: a win rate needs a whole match
// and a sparring partner, while "does it ever ask for more than one type" is exactly the property the
// change is about and it is decidable in a few hundred ticks.
//
// Events are applied BY HAND rather than through StepEvents, and deliberately: stepping the sim would
// run combat, and then what the test measured would be attrition rather than the production decision.
// A placement becomes a building and a queue command becomes queue depth, which is all the quota
// scheduler reads — so this drives the real decision loop with none of the noise.
static void TestTopTierHoldsAMixedComposition() {
    // Both arms run in ONE process off the same board, so nothing but the CVar differs.
    const int32_t WasEnabled = CvAiMixEnable.Get();
    int32_t Distinct[2] = {0, 0};
    for (int Arm = 0; Arm < 2; ++Arm) {
        CvAiMixEnable.Set(Arm == 0 ? 0 : 1);          // arm 0 = the old argmax, arm 1 = the mix
        Sim S;
        S.Init(0x158);                                 // latches the CVar into S.Cv
        // A MIXED enemy army, the owner's own measured shape — near-equal thirds. Any single counter
        // is 3x against a third of it and 1/3x against another third, which is the whole problem.
        Inject(S, 0, UnitRock, 18);
        Inject(S, 0, UnitPaper, 17);
        Inject(S, 0, UnitScissor, 15);
        Inject(S, 1, UnitMiner, 40);                   // past worker_target, so it wants soldiers
        InjectBuilding(S, 1, UnitMiner, F(4), F(230)); // its opening camp, so it is past the forced one
        AiController Ai;
        Ai.Init(0x158, 1, EAiTier::Hard);
        bool Asked[3] = {false, false, false};
        // Gold is EARNED and SPENT, not pinned. Pinning it rich broke the first version of this test in
        // a way worth recording: the place branch fires whenever another building is affordable, so an
        // infinite purse made the AI place a building every single tick and never queue — and since the
        // quota reads units plus QUEUED work, its counts stayed at zero, the ranking stayed a tie, and
        // the tie-break picked Rock forever. A budget is what makes the AI alternate between buying
        // capacity and filling it, which is the loop the scheduler actually lives in.
        S.Teams[1].Gold = 2000;
        for (uint32_t T = 0; T < 600; ++T) {
            S.Teams[1].Gold += 300;                    // a strong economy, comfortably able to reach 4000
            InputEvent E[MaxEventsPerTick];
            const int C = AiTick(Ai, S, T, E);
            for (int I = 0; I < C; ++I) {
                if (E[I].Kind == EventPlaceBuilding) {
                    const uint8_t Ty = E[I].Type;
                    S.Teams[1].Gold -= BuildingCostFor(S.Cv, Ty);
                    if (Ty >= UnitRock && Ty <= UnitScissor) Asked[Ty - UnitRock] = true;
                    InjectBuilding(S, 1, Ty, Fixed{E[I].X}, Fixed{E[I].Y});
                } else if (E[I].Kind == EventQueueUnits) {
                    const int32_t Slot = E[I].X;
                    if (Slot >= 0 && Slot < S.Count) {
                        const uint8_t Ty = S.Type[Slot];
                        int32_t N = E[I].Y;
                        const int32_t Room = S.Cv.BuildingQueueMax - S.Queue[Slot];
                        if (N > Room) N = Room;
                        // Queue depth is what the quota scheduler counts as supply in flight.
                        S.Queue[Slot] += N;
                        S.Teams[1].Gold -= N * S.Units[Ty].Cost;
                        if (Ty >= UnitRock && Ty <= UnitScissor) Asked[Ty - UnitRock] = true;
                    }
                }
            }
            if (S.Teams[1].Gold < 0) S.Teams[1].Gold = 0;
        }
        for (int K = 0; K < 3; ++K)
            if (Asked[K]) ++Distinct[Arm];
    }
    CvAiMixEnable.Set(WasEnabled);                     // leave the global as we found it
    // THE POINT OF THE TEST, and it fails with the feature switched off: the argmax arm commits to a
    // single type, the mix arm produces all three. If arm 0 ever reports 3 the mechanism under test
    // has stopped being the thing that makes the difference, and this test has stopped being evidence.
    CHECK(Distinct[1] == 3);
    CHECK(Distinct[0] == 1);
}

static void TestCapYieldsForAFirstCombatBuilding() {
    Sim S;
    S.Init(0x1234);
    Inject(S, 0, UnitRock, 6);                 // an enemy army -> it wants a counter
    Inject(S, 1, UnitMiner, 40);               // well past easy's worker_target (24)
    for (int K = 0; K < 4; ++K)                // easy's ENTIRE cap (max_buildings 4), all economy
        InjectBuilding(S, 1, UnitMiner, F(4 + 7 * K), F(230));
    S.Teams[1].Gold = 100000;                  // money is not the constraint
    AiController Ai;
    Ai.Init(0x1234, 1, EAiTier::Easy);
    bool Placed = false;
    for (uint32_t T = 0; T < 512 && !Placed; ++T) {   // easy is stale/slow: give it time to react
        InputEvent E[MaxEventsPerTick];
        const int C = AiTick(Ai, S, T, E);
        for (int I = 0; I < C; ++I)
            if (E[I].Kind == EventPlaceBuilding && E[I].Type != UnitMiner) Placed = true;
    }
    CHECK(Placed);
}

// The other half: with the cap NOT yet full, the economy must leave the last slot alone so the
// jam never forms. Easy again (cap 4): no enemy, so it wants miners forever and would otherwise
// expand into all four slots.
static void TestCapReservesASlotForCombat() {
    Sim S;
    S.Init(0x1234);
    Inject(S, 1, UnitMiner, 2);                          // below worker_target -> it wants miners
    InjectBuilding(S, 1, UnitMiner, F(4), F(230));       // its opening camp exists
    S.Teams[1].Gold = 100000;
    AiController Ai;
    Ai.Init(0x1234, 1, EAiTier::Easy);
    int32_t Camps = 1;
    for (uint32_t T = 0; T < 512; ++T) {
        InputEvent E[MaxEventsPerTick];
        const int C = AiTick(Ai, S, T, E);
        for (int I = 0; I < C; ++I)
            if (E[I].Kind == EventPlaceBuilding && E[I].Type == UnitMiner) {
                // Place it for real, so the next decision sees the higher building count.
                // Event X/Y are Fixed RAW (see InputEvent), hence the explicit Fixed{}.
                InjectBuilding(S, 1, UnitMiner, Fixed{E[I].X}, Fixed{E[I].Y});
                ++Camps;
            }
    }
    CHECK(Camps == 3);   // 4 - 1 reserved for combat; it was 4 (and then nothing, forever)
}

#if LUR_INTERNAL
// ---- #144: a recording must replay to a BIT-IDENTICAL match, or it is not evidence ----
// The whole value of the flight recorder is that the file IS the match: seed + latched CVar set +
// every tick's combined event batch, replayed through the same deterministic sim (design §1's
// replay law). If the hash differed, every conclusion drawn from a recording would be a guess.
static void TestMatchRecordingReplaysIdentically() {
    const char* Path = "rps_test_recording.rec";
    Sim Live;
    Live.Init(0x1440);
    AiController Ai0, Ai1;
    Ai0.Init(0x1440, 0, EAiTier::Hard);
    Ai1.Init(0x1440, 1, EAiTier::Easy);
    MatchRecorder Rec;
    CHECK(Rec.Begin(Path, Live, static_cast<int>(EAiTier::Easy), /*human*/ 0));
    for (int T = 0; T < 400 && Live.Result == ResultOngoing; ++T) {
        InputEvent E0[MaxEventsPerTick], E1[MaxEventsPerTick];
        const int C0 = AiTick(Ai0, Live, Live.Tick, E0);
        const int C1 = AiTick(Ai1, Live, Live.Tick, E1);
        InputEvent Comb[2 * MaxEventsPerTick];
        int NC = 0;
        for (int I = 0; I < C0; ++I) Comb[NC++] = E0[I];
        for (int I = 0; I < C1; ++I) Comb[NC++] = E1[I];
        const uint32_t At = Live.Tick;      // the tick these events are applied ON
        Live.StepEvents(Comb, NC);
        Rec.Events(At, Comb, NC);
        if (At % 100 == 0) Rec.Census(Live, 0, static_cast<int>(Ai1.State()), Ai1.CounterEnemy());
    }
    const uint64_t LiveHash = Live.StateHash();
    const uint32_t LiveTick = Live.Tick;
    Rec.End(Live);

    const MatchRecording R = LoadMatchRecording(Path);
    CHECK(R.Ok);
    CHECK(R.Seed == 0x1440 && R.EndTick == LiveTick);
    CHECK(!R.Events.empty() && !R.Census.empty());
    Sim Replayed;
    const uint64_t ReplayHash = ReplayMatch(R, Replayed);
    CHECK(Replayed.Tick == LiveTick);
    CHECK(ReplayHash == LiveHash);        // the file is the match
    CHECK(Replayed.Result == Live.Result);
    std::remove(Path);
}
#endif

int main() {
    Lur::Core::CVarEnterMain();  // CVars may not be read before main() (spec §1.1)
    TestDeterminism();
    TestOpening();
    TestCounterChoice();
    TestTierReactionSpeed();
    TestAiVsAiDeterminism();
    TestAiExpandsCapacity();
    TestChestNeverPricesOutEveryAction();
    TestTopTierHoldsAMixedComposition();
    TestCapYieldsForAFirstCombatBuilding();
    TestCapReservesASlotForCombat();
#if LUR_INTERNAL
    TestMatchRecordingReplaysIdentically();
#endif
    if (GFailures == 0) { std::printf("rps_ai_tests: ALL PASS\n"); return 0; }
    std::printf("rps_ai_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
