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

// First alive building of (Team, Type), or -1; and whether the team has an alive miner UNIT.
static int32_t FindTeamBuilding(const Sim& S, uint8_t Team, uint8_t Type) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.IsBuilding(I) && S.Team[I] == Team && S.Type[I] == Type) return I;
    return -1;
}
static bool HasMinerUnit(const Sim& S, uint8_t Team) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && !S.IsBuilding(I) && S.Team[I] == Team && S.Type[I] == UnitMiner) return true;
    return false;
}
// #145/#135: a deterministic, STATE-REACTIVE place+queue schedule the determinism-family tests
// share — armies must spawn under the new match-start rules (open with only gold; forced first
// camp; soldier buildings gated on the first miner UNIT). A PURE function of S (so two runs, and
// the grid vs brute paths, stay bit-identical), emitted TEAM-0-FIRST (Execute's combined-batch
// order). Per team, each tick: place the mining camp if missing; keep its miner queue topped up;
// and once a miner UNIT exists (military unlocked), place a Rock building and keep ITS queue
// topped up. The caller funds both teams so nothing is rejected for cost.
static int32_t ArmyScript(const Sim& S, InputEvent* Out) {
    const Fixed T0Y = F(20), T1Y = F(WorldHeight.ToInt() - 20);
    int32_t N = 0;
    for (uint8_t T = 0; T < 2; ++T) {
        const Fixed Y = T == 0 ? T0Y : T1Y;
        const int32_t Camp = FindTeamBuilding(S, T, UnitMiner);
        if (Camp < 0) { Out[N++] = InputEvent::Place(T, UnitMiner, F(17), Y); continue; }
        if (S.Queue[Camp] < 4) Out[N++] = InputEvent::Queue(T, Camp, 4);   // keep miners flowing
        if (HasMinerUnit(S, T)) {                                          // military unlocked
            const int32_t Rock = FindTeamBuilding(S, T, UnitRock);
            if (Rock < 0) Out[N++] = InputEvent::Place(T, UnitRock, F(10), Y);
            else if (S.Queue[Rock] < 4) Out[N++] = InputEvent::Queue(T, Rock, 4);
        }
    }
    return N;
}
// Step one reactive tick on S (build the combined batch from S's state, apply it).
static void ArmyStep(Sim& S, uint32_t /*I*/) {
    InputEvent Evs[MaxEventsPerTick];
    const int32_t N = ArmyScript(S, Evs);
    S.StepEvents(Evs, N);
}
// Fund both teams so the whole ArmyScript succeeds regardless of cost.
static void FundForArmyScript(Sim& S) { S.Teams[0].Gold = 100000; S.Teams[1].Gold = 100000; }

// ---- 1. Determinism: two independent runs, identical hash every tick ----
static void TestDeterminism() {
    constexpr int Ticks = 600;
    constexpr uint64_t Seed = 0x0123456789ABCDEFull;

    static Sim A, B;  // static: ~200 KB each, keep them off the stack
    A.Init(Seed);   FundForArmyScript(A);
    B.Init(Seed);   FundForArmyScript(B);
    CHECK(A.StateHash() == B.StateHash());  // identical from tick 0

    bool AllMatch = true;
    for (int I = 0; I < Ticks; ++I) {
        ArmyStep(A, I);
        ArmyStep(B, I);
        if (A.StateHash() != B.StateHash()) { AllMatch = false; break; }
    }
    CHECK(AllMatch);
    // The match should actually DO something over 600 ticks (buildings produced armies),
    // otherwise "deterministic" is trivially true over an empty sim.
    CHECK(A.AliveCount(0) > 0 && A.AliveCount(1) > 0);
}

// A fresh sim replaying the same stream must reach the same final hash — the
// replay law (State = Replay(Inputs, Seed)) that resync + the recorder depend on.
static void TestReplayReproducibility() {
    constexpr int Ticks = 300;
    constexpr uint64_t Seed = 0xFEEDBEEFu;
    static Sim Live;
    Live.Init(Seed);   FundForArmyScript(Live);
    for (int I = 0; I < Ticks; ++I) ArmyStep(Live, I);
    const uint64_t Final = Live.StateHash();

    static Sim Replay;
    Replay.Init(Seed);   FundForArmyScript(Replay);
    for (int I = 0; I < Ticks; ++I) ArmyStep(Replay, I);
    CHECK(Replay.StateHash() == Final);
}

// ---- #112: a latched AffectsGameplay CVar override changes the sim deterministically ----
// Exercises both halves of the CVar-determinism design: the per-tick latch (PreTick's
// Cv = LatchCvs()) means an override applied between runs takes effect, and folding Cv
// into StateHash means two runs with the same override hash identically while a different
// override diverges. This is the sim-side proof under which the Addendum-C peer sync sits.
static void TestCVarOverrideDeterminism() {
    constexpr int Ticks = 400;
    constexpr uint64_t Seed = 0x112C0DEull;
    auto Run = [&]() {
        static Sim S;  // static: keep the ~200 KB off the stack
        S.Init(Seed);   FundForArmyScript(S);
        for (int I = 0; I < Ticks; ++I) ArmyStep(S, I);  // places combat buildings -> soldiers exist
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

#if !LUR_SHIPPING
// ---- #112: the gameplay-CVar wire list must cover EXACTLY the registered set ----
// Guards the one gap the LUR_RPS_GAMEPLAY_CVARS X-list has vs a registry-driven cook: a
// CVar migrated to AffectsGameplay but forgotten in the X-list (so it has no wire id /
// snapshot field) would silently never sync. If these ever diverge, add the CVar to
// LUR_RPS_GAMEPLAY_CVARS (or drop AffectsGameplay). This IS the cook's completeness assert.
static void TestGameplayCvarListComplete() {
    int Registered = 0;
    Lur::Core::CVarRegistry::ForEach(
        [&](Lur::Core::ICVar* C) { if (C->AffectsGameplay()) ++Registered; });
    CHECK(Registered == CvIdCount);
}
#endif

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
        static Sim Grid, Brute;
        // Same PLACE+QUEUE schedule on both paths: buildings produce clashing armies (and the
        // building-repel force) — exercises the nearest-enemy ring search AND building repulsion
        // through the grid, which must reproduce brute bit-for-bit.
        Grid.Init(Seed);    FundForArmyScript(Grid);
        Brute.Init(Seed);   FundForArmyScript(Brute);
        Brute.UseBruteForce = true;  // after Init (Init resets the flag)

        bool Match = true;
        int FirstDiverge = -1;
        for (int I = 0; I < Ticks; ++I) {
            ArmyStep(Grid, I);
            ArmyStep(Brute, I);
            if (Grid.StateHash() != Brute.StateHash()) { Match = false; FirstDiverge = I; break; }
        }
        if (!Match) std::printf("  grid!=brute seed=%llu diverged at tick %d\n",
                                static_cast<unsigned long long>(Seed), FirstDiverge);
        CHECK(Match);
        CHECK(Grid.AliveCount(0) > 0 && Grid.AliveCount(1) > 0);  // the match did something
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
    for (int I = 0; I < 40; ++I) S.StepEvents(nullptr, 0);
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
    FundForArmyScript(S);
    int32_t Prev = S.AliveCount(0) + S.AliveCount(1);
    bool NeverDropped = true;
    for (int I = 0; I < 200; ++I) {
        ArmyStep(S, I);  // both teams place combat buildings + queue warriors that would counter-kill
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
    S.Kind[I] = KindUnit; S.Queue[I] = 0; S.BuildProgress[I] = 0;  // #131: robust vs recycled slots
    S.AliveBits[I >> 6] |= (1ull << (I & 63));
}
static void ClearField(Sim& S) {
    for (int I = 0; I < S.Count; ++I) S.AliveBits[I >> 6] = 0;
    for (int M = 0; M < NumMines; ++M) S.MineGold[M] = 0;  // no mines: idle carts, controlled scenario
    S.Count = 0;
}
// #132: drop a BUILDING (Kind==KindBuilding) that produces `Type` into a slot. No placement
// API yet (#137), so tests seed the SoA directly — same POD-is-public discipline as PlaceUnit.
static void PlaceBuilding(Sim& S, int I, Fixed X, Fixed Y, uint8_t Team, uint8_t Type) {
    S.PosX[I] = X; S.PosY[I] = Y; S.PrevX[I] = X; S.PrevY[I] = Y;
    S.Team[I] = Team; S.Type[I] = Type; S.Hp[I] = BuildingHpFor(S.Cv, Type);
    S.Kind[I] = KindBuilding; S.Queue[I] = 0; S.BuildProgress[I] = 0;
    S.Target[I] = -1; S.Cooldown[I] = 0;
    S.AliveBits[I >> 6] |= (1ull << (I & 63));
}
// Count alive MOBILE units (not buildings) on a team.
static int32_t MobileCount(const Sim& S, uint8_t Team) {
    int32_t C = 0;
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && !S.IsBuilding(I) && S.Team[I] == Team) ++C;
    return C;
}
// #135: the match now opens with only gold (no start-miners, no camp drop-off). The economy
// tests seed a minimal starter economy the way a placed camp would: a miner BUILDING (the only
// deposit point now) at the old bottom/top camp spot + Carts miner units beside it, so gathering
// and deposit work. Appends onto whatever is already in the SoA (grows S.Count).
static void SeedStarterEconomy(Sim& S, uint8_t Team, int Carts) {
    const Fixed Cx = CampX, Cy = Sim::CampY(Team);
    PlaceBuilding(S, S.Count, Cx, Cy, Team, UnitMiner);   ++S.Count;
    for (int K = 0; K < Carts; ++K) { PlaceUnit(S, S.Count, Cx + F(1 + K), Cy, Team, UnitMiner); ++S.Count; }
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
    S.StepEvents(nullptr, 0);
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
    for (int I = 0; I < 25; ++I) { A.StepEvents(nullptr, 0); B.StepEvents(nullptr, 0); }
    CHECK(A.PosY[0] > B.PosY[0]);  // interpose held the defender back (north) vs the free chase south
}

// ---- #131 buildings SoA foundation: the new authoritative fields (Kind / per-building
// Queue / BuildProgress / frontier high-water) must be BOTH hashed AND memcpy-preserved.
// There is no placement API yet (#133), so we mutate the SoA directly — the same POD-is-
// public discipline the win-rule tests use. Two properties are the whole point of #131:
//   (a) each new field is folded into StateHash (flipping it changes the hash), and
//   (b) Sim stays trivially copyable, so a memcpy snapshot round-trips the new state.
static void TestBuildingSoaHashedAndCopyable() {
    static_assert(std::is_trivially_copyable<Sim>::value, "#131: Sim must stay memcpy-able");
    static Sim S;
    S.Init(0);
    ClearField(S);
    // A lone unit — baseline hash with no building state set.
    PlaceUnit(S, 0, F(17), F(20), 0, UnitRock);
    S.Count = 1;
    const uint64_t H0 = S.StateHash();

    // Turn slot 0 into a building (reuse Type as the produced type). Each field flip must
    // move the hash, proving it is mixed in.
    S.Kind[0] = KindBuilding;               CHECK(S.StateHash() != H0);
    const uint64_t H1 = S.StateHash();
    S.Queue[0] = 7;                          CHECK(S.StateHash() != H1);
    const uint64_t H2 = S.StateHash();
    S.BuildProgress[0] = 13;                 CHECK(S.StateHash() != H2);
    const uint64_t H3 = S.StateHash();
    S.FrontierT0 = F(42);                     CHECK(S.StateHash() != H3);
    const uint64_t H4 = S.StateHash();
    S.FrontierT1 = F(200);                    CHECK(S.StateHash() != H4);
    const uint64_t H5 = S.StateHash();

    // memcpy snapshot (the rollback mechanism) preserves every new field bit-for-bit.
    static Sim Snap;
    Snap = S;  // trivially-copyable assignment == memcpy
    CHECK(Snap.StateHash() == H5);
    CHECK(Snap.Kind[0] == KindBuilding && Snap.IsBuilding(0));
    CHECK(Snap.Queue[0] == 7 && Snap.BuildProgress[0] == 13);
    CHECK(Snap.FrontierT0 == F(42) && Snap.FrontierT1 == F(200));
}

// ---- #132 building production: FLAT cadence, no stack acceleration ----
// A building with a deep queue must build at ONE-per-BuildTicks — a big queue does NOT build
// faster (the removed stack-snowball). Trace the exact edges: unit #1 lands at BuildTicks,
// #2 at 2·BuildTicks, #3 at 3·BuildTicks; queue + progress zero out at the end.
static void TestBuildingProducesFlatCadence() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceBuilding(S, 0, F(17), F(20), 0, UnitRock);
    S.Count = 1;
    S.Queue[0] = 3;
    const int Bt = S.Units[UnitRock].BuildTicks;
    for (int t = 1; t <= Bt; ++t) S.StepEvents(nullptr, 0);
    CHECK(MobileCount(S, 0) == 1);        // exactly one after Bt ticks (flat: +1/tick, not +Queue)
    CHECK(S.Queue[0] == 2);
    for (int t = 1; t <= Bt; ++t) S.StepEvents(nullptr, 0);
    CHECK(MobileCount(S, 0) == 2);        // second at 2·Bt
    for (int t = 1; t <= Bt; ++t) S.StepEvents(nullptr, 0);
    CHECK(MobileCount(S, 0) == 3);        // third at 3·Bt
    CHECK(S.Queue[0] == 0 && S.BuildProgress[0] == 0);  // drained, no banked progress
    CHECK(S.IsBuilding(0) && S.IsAlive(0));             // the building persists (it produces, isn't consumed)
}

// #132: throughput scales by BUILDING COUNT, not queue depth. Two buildings each queued 1
// finish BOTH units in Bt ticks (parallel), where one building queued 2 has produced only 1.
static void TestBuildingCountScalesThroughput() {
    static Sim Two, One;
    Two.Init(0); ClearField(Two);
    PlaceBuilding(Two, 0, F(12), F(20), 0, UnitScissor);
    PlaceBuilding(Two, 1, F(22), F(20), 0, UnitScissor);
    Two.Count = 2; Two.Queue[0] = 1; Two.Queue[1] = 1;

    One.Init(0); ClearField(One);
    PlaceBuilding(One, 0, F(17), F(20), 0, UnitScissor);
    One.Count = 1; One.Queue[0] = 2;

    const int Bt = Two.Units[UnitScissor].BuildTicks;
    for (int t = 1; t <= Bt; ++t) { Two.StepEvents(nullptr, 0); One.StepEvents(nullptr, 0); }
    CHECK(MobileCount(Two, 0) == 2);   // two buildings -> both units out in Bt ticks
    CHECK(MobileCount(One, 0) == 1);   // one building -> only the first, second still building
}

// ---- #133 placement validity (§5.1): pure predicate over the hashed sim state ----
static void TestPlacementValidity() {
    static Sim S;
    S.Init(0);
    ClearField(S);  // clears units + mines; frontier keeps its Init value
    const Fixed Fp = S.Cv.BuildingFootprint;
    // Clear field, inside the initial band, in-bounds -> valid.
    CHECK(S.CanPlaceBuilding(0, UnitMiner, F(17), F(10)));
    // Past the team-0 frontier (Y > FrontierT0) -> rejected.
    CHECK(!S.CanPlaceBuilding(0, UnitMiner, F(17), S.FrontierT0 + F(1)));
    // Footprint crosses the west edge -> out of bounds -> rejected.
    CHECK(!S.CanPlaceBuilding(0, UnitMiner, Fp - F(1, 10), F(10)));
    // Existing building: same spot and one-footprint-away both overlap; >2·Fp is clear.
    PlaceBuilding(S, 0, F(10), F(10), 0, UnitMiner);
    S.Count = 1;
    CHECK(!S.CanPlaceBuilding(0, UnitRock, F(10), F(10)));
    CHECK(!S.CanPlaceBuilding(0, UnitRock, F(10) + Fp, F(10)));
    CHECK(S.CanPlaceBuilding(0, UnitRock, F(10) + Fp + Fp + F(1), F(10)));
    // Live mine: footprint over it is rejected; clear of it is fine.
    S.MineX[0] = F(20); S.MineY[0] = F(12); S.MineGold[0] = MineGoldCapacity;
    CHECK(!S.CanPlaceBuilding(0, UnitRock, F(20), F(12)));
    CHECK(S.CanPlaceBuilding(0, UnitRock, F(20), F(12) + Fp + F(1)));
}

// #133/§5.3: the frontier is a MONOTONIC high-water mark — it advances with a team's forward
// units and never retreats when they die.
static void TestFrontierMonotonicHighWater() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    CHECK(S.FrontierT0 == S.Cv.InitialFrontier);        // seeded at the initial band
    PlaceUnit(S, 0, F(17), F(100), 0, UnitRock);        // a team-0 unit already past the line
    S.Count = 1;
    for (int t = 0; t < 10; ++t) S.StepEvents(nullptr, 0);          // it marches forward (up, toward enemy camp)
    const Fixed Adv = S.FrontierT0;
    CHECK(Adv >= F(100));                                // advanced to the unit's reach
    CHECK(Adv > S.Cv.InitialFrontier);
    S.AliveBits[0] &= ~1ull;                             // the forward unit dies
    for (int t = 0; t < 10; ++t) S.StepEvents(nullptr, 0);
    CHECK(S.FrontierT0 == Adv);                          // ground held — no retreat
}

// #133/§5.2: a building repels nearby units (they flow around it) and never moves itself.
// Differential: the same unit drifts measurably farther from the building's axis WITH the
// building present than without.
static void TestBuildingRepelsUnits() {
    auto Setup = [](Sim& S, bool WithBuilding) {
        S.Init(0);
        ClearField(S);
        S.DisableCombat = true;
        int N = 0;
        if (WithBuilding) { PlaceBuilding(S, 0, F(15), F(20), 0, UnitScissor); N = 1; }
        PlaceUnit(S, N, F(16), F(20), 0, UnitScissor);  // just east of the building's axis
        S.Count = N + 1;
        return N;
    };
    static Sim A, B;
    const int Ia = Setup(A, true);
    const int Ib = Setup(B, false);
    for (int t = 0; t < 20; ++t) { A.StepEvents(nullptr, 0); B.StepEvents(nullptr, 0); }
    CHECK(A.PosX[Ia] > B.PosX[Ib] + F(1, 2));           // building shoved the unit measurably east
    CHECK(A.IsBuilding(0) && A.PosX[0] == F(15) && A.PosY[0] == F(20));  // the building never moved
}

// ---- #134/§7: an enemy building is a valid target, scored as the unit type it produces ----
static void TestSoldierTargetsEnemyBuildingByType() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceUnit(S, 0, F(17), F(20), 0, UnitScissor);   // Scissor beats Paper -> the building is prey
    PlaceBuilding(S, 1, F(17), F(23), 1, UnitPaper); // enemy PAPER building
    S.Count = 2;
    S.StepEvents(nullptr, 0);
    CHECK(S.Target[0] == 1);  // targeted the building as if a Paper unit
}

// #134/§7: a building takes damage like a stationary enemy unit of its Type — INCLUDING the
// counter multiplier — and is destroyed at Hp<=0 (alive bit clears). Buildings never hit back.
static void TestScissorDestroysPaperBuildingWithCounter() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceUnit(S, 0, F(17), F(20), 0, UnitScissor);
    PlaceBuilding(S, 1, F(17), F(21), 1, UnitPaper);  // adjacent, inside Scissor range
    S.Count = 2;
    const int32_t Hp0 = S.Hp[1];
    S.StepEvents(nullptr, 0);
    // First engaged hit is counter-multiplied (Scissor beats Paper -> 3x).
    CHECK(S.Hp[1] == Hp0 - S.Units[UnitScissor].Attack * S.Cv.CounterMultiplier);
    CHECK(S.Hp[0] == UnitTable[UnitScissor].MaxHp);  // the building did NOT fight back
    for (int t = 0; t < 800 && S.IsAlive(1); ++t) S.StepEvents(nullptr, 0);
    CHECK(!S.IsAlive(1));  // economy/production building razed
}

// #134/§12.4: a gold-carrying cart deposits at the NEAREST own miner building (not the far
// one), and the gold is credited to the team.
static void TestCartDepositsAtNearestMinerBuilding() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceBuilding(S, 0, F(8),  F(20), 0, UnitMiner);  // far camp
    PlaceBuilding(S, 1, F(26), F(20), 0, UnitMiner);  // near camp
    PlaceUnit(S, 2, F(24), F(20), 0, UnitMiner);      // a cart right by the near camp
    S.Count = 3;
    S.WorkerState[2] = WorkToCamp; S.Carry[2] = CarryCapacity; S.Target[2] = -1;
    const int32_t Before = S.Teams[0].Gold;
    for (int t = 0; t < 100 && S.Carry[2] != 0; ++t) S.StepEvents(nullptr, 0);
    CHECK(S.Carry[2] == 0);
    CHECK(S.Teams[0].Gold == Before + CarryCapacity);  // banked at a miner building
    CHECK(S.PosX[2] > F(20));                           // went to the NEAR camp (x=26), not the far (x=8)
}

// #135/§12.4: with NO own miner building, a gold-carrying cart is STRANDED — it holds the gold
// (there is no camp fallback now) and idles until a miner building exists, then deposits.
static void TestCartStrandedWithoutMinerBuilding() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceUnit(S, 0, F(17), F(20), 0, UnitMiner);
    S.Count = 1;
    S.WorkerState[0] = WorkToCamp; S.Carry[0] = CarryCapacity; S.Target[0] = -1;
    const int32_t Before = S.Teams[0].Gold;
    for (int t = 0; t < 60; ++t) S.StepEvents(nullptr, 0);
    CHECK(S.Carry[0] == CarryCapacity);   // still holding — nowhere to deposit
    CHECK(S.Teams[0].Gold == Before);     // no gold credited (no camp fallback)
    // Give the team a miner building right where the cart stands -> it resumes and deposits.
    PlaceBuilding(S, 1, F(17), F(20), 0, UnitMiner);
    S.Count = 2;
    for (int t = 0; t < 60 && S.Carry[0] != 0; ++t) S.StepEvents(nullptr, 0);
    CHECK(S.Carry[0] == 0);
    CHECK(S.Teams[0].Gold == Before + CarryCapacity);  // stranded gold banked once a camp exists
}

// ---- #137: StepEvents — place + queue events mutate the sim, deterministically ----
static int32_t FirstBuilding(const Sim& S) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.IsBuilding(I)) return I;
    return -1;
}

static void TestEventPlaceAndQueueApply() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    S.Teams[0].Gold = 1000;
    const int32_t PlaceCost = BuildingCostFor(S.Cv, UnitMiner);
    InputEvent P = InputEvent::Place(0, UnitMiner, F(17), F(10));
    S.StepEvents(&P, 1);
    const int32_t B = FirstBuilding(S);
    CHECK(B >= 0);
    CHECK(S.Team[B] == 0 && S.Type[B] == UnitMiner && S.Kind[B] == KindBuilding);
    CHECK(S.Teams[0].Gold == 1000 - PlaceCost);
    // Queue 5 miners at it — gold deducted per unit.
    const int32_t GoldPreQ = S.Teams[0].Gold;
    InputEvent Q = InputEvent::Queue(0, B, 5);
    S.StepEvents(&Q, 1);
    CHECK(S.Queue[B] == 5);
    CHECK(S.Teams[0].Gold == GoldPreQ - 5 * S.Units[UnitMiner].Cost);
    // An INVALID place (past the frontier) is a deterministic no-op: no building, no gold spent.
    const int32_t GoldPreBad = S.Teams[0].Gold;
    InputEvent Bad = InputEvent::Place(0, UnitRock, F(17), S.FrontierT0 + F(5));
    S.StepEvents(&Bad, 1);
    CHECK(S.Teams[0].Gold == GoldPreBad);
}

// Queue clamps to gold: a batch bigger than the wallet enqueues only what gold covers (partial).
// Uses a miner building — the forced-first-building rule (#135) rejects a non-miner first camp.
static void TestEventQueuePartialByGold() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    const int32_t MinerCost = S.Units[UnitMiner].Cost;
    S.Teams[0].Gold = BuildingCostFor(S.Cv, UnitMiner) + 2 * MinerCost + 10;  // camp + exactly 2 miners + change
    InputEvent P = InputEvent::Place(0, UnitMiner, F(17), F(10));
    S.StepEvents(&P, 1);
    const int32_t B = FirstBuilding(S);
    CHECK(B >= 0);
    InputEvent Q = InputEvent::Queue(0, B, 5);  // ask for 5, afford 2
    S.StepEvents(&Q, 1);
    CHECK(S.Queue[B] == 2);
    CHECK(S.Teams[0].Gold == 10);
}

// StepEvents is deterministic: two runs of the same event schedule hash identically, and the
// placed building actually produces.
static void TestStepEventsDeterministic() {
    auto Script = [](Sim& S) {
        S.Init(0x1234);
        S.Teams[0].Gold = 1000;  // fund the placement + a full queue
        InputEvent P = InputEvent::Place(0, UnitMiner, F(17), F(20));  // slot 0 (match starts empty)
        S.StepEvents(&P, 1);
        const int32_t B = FirstBuilding(S);
        InputEvent Q = InputEvent::Queue(0, B, 10);
        S.StepEvents(&Q, 1);
        for (int t = 0; t < 300; ++t) S.StepEvents(nullptr, 0);  // idle ticks (empty batch)
    };
    static Sim A, B;
    Script(A);
    Script(B);
    CHECK(A.StateHash() == B.StateHash());
    CHECK(A.AliveCount(0) > 0);  // the building produced miners from an empty start
}

// ---- 2. Win rule (spec §6, edge-proof) ----
static void TestMutualAnnihilationDraw() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    KillTeam(S, 1);
    S.Teams[0].Gold = 0;
    S.Teams[1].Gold = 0;
    S.StepEvents(nullptr, 0);  // win check runs at phase 7
    CHECK(S.Result == ResultDraw);
}

static void TestWipeoutLoses() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    S.Teams[0].Gold = 0;  // no units, no queue, can't rebuy -> team 0 loses
    S.StepEvents(nullptr, 0);
    CHECK(S.Result == ResultTeam1Wins);
}

// #136/§12.1: buildings DO NOT enter the loss test. A team with buildings standing (even a
// combat building) but no alive units and no rebuy gold is doomed and loses anyway.
static void TestBuildingsDoNotSaveFromLoss() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceBuilding(S, 0, F(17), F(10), 0, UnitMiner);   // a mining camp stands...
    PlaceBuilding(S, 1, F(17), F(14), 0, UnitRock);    // ...and a combat building
    S.Count = 2;
    S.Teams[0].Gold = 0;        // but no units and can't afford the cheapest -> doomed
    S.Teams[1].Gold = 1000;     // team 1 solvent, so it does not also lose
    S.StepEvents(nullptr, 0);
    CHECK(S.AliveCount(0) == 0);          // buildings are not counted as units
    CHECK(S.Result == ResultTeam1Wins);   // team 0 loses despite its buildings
}

static void TestRebuyIsNotLoss() {
    static Sim S;
    S.Init(0);
    KillTeam(S, 0);
    S.Teams[0].Gold = CheapestCost;  // zero units but can still rebuy -> NOT a loss
    S.StepEvents(nullptr, 0);
    CHECK(S.Result == ResultOngoing);
}

// #135: a broke team (gold < cheapest) with 0 units is NOT lost while a PAID unit is still
// building — the queued unit is coming (gold spent at enqueue). This guards the normal opening
// (place camp -> queue miners -> go broke -> first miner pops) that the removed start-miners
// used to mask; without the carve-out the win check froze the match on a spurious draw.
static void TestPendingProductionNotLoss() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    PlaceBuilding(S, 0, F(17), F(10), 0, UnitMiner);  // team 0's camp
    S.Count = 1;
    S.Queue[0] = 1;                 // one miner PAID and building
    S.Teams[0].Gold = 0;            // broke, 0 alive units
    S.Teams[1].Gold = 1000;         // team 1 solvent, so it never also loses
    S.StepEvents(nullptr, 0);
    CHECK(S.Result == ResultOngoing);          // pending production saves it — not doomed
    for (int t = 0; t < S.Units[UnitMiner].BuildTicks + 2; ++t) S.StepEvents(nullptr, 0);
    CHECK(S.AliveCount(0) >= 1);               // the miner popped -> a real unit exists
    CHECK(S.Result == ResultOngoing);
}

// #135 (refined): SOLDIER (non-miner) buildings are disabled until the team's first miner UNIT has
// spawned — enforces the camp -> miners -> military opening. A miner CAMP is always placeable.
static void TestSoldierBuildingGatedOnMinerUnit() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    S.Teams[0].Gold = 100000;
    // No miner unit yet: a soldier building is a deterministic no-op (nothing placed, gold kept).
    const int32_t Gold0 = S.Teams[0].Gold;
    InputEvent Rock = InputEvent::Place(0, UnitRock, F(17), F(10));
    S.StepEvents(&Rock, 1);
    CHECK(FirstBuilding(S) < 0);
    CHECK(S.Teams[0].Gold == Gold0);
    // A miner CAMP IS allowed as the first building; but a soldier building is STILL blocked
    // (a placed camp isn't a miner UNIT yet).
    InputEvent Camp = InputEvent::Place(0, UnitMiner, F(17), F(10));
    S.StepEvents(&Camp, 1);
    CHECK(FindTeamBuilding(S, 0, UnitMiner) >= 0);
    InputEvent Rock2 = InputEvent::Place(0, UnitRock, F(10), F(10));
    S.StepEvents(&Rock2, 1);
    CHECK(FindTeamBuilding(S, 0, UnitRock) < 0);
    // Spawn a miner UNIT -> military unlocks and the soldier building places.
    PlaceUnit(S, S.Count, F(24), F(10), 0, UnitMiner); ++S.Count;
    InputEvent Rock3 = InputEvent::Place(0, UnitRock, F(10), F(10));
    S.StepEvents(&Rock3, 1);
    CHECK(FindTeamBuilding(S, 0, UnitRock) >= 0);
}

// ---- 5. Finite mines (#84) ----
static void TestMineDepletesAndVanishes() {
    static Sim S;
    S.Init(0);
    // Leave a single near-empty mine: exactly one carry in it. Total income is then
    // exactly that carry, and the mine must read as gone (gold 0) afterwards.
    for (int M = 0; M < NumMines; ++M) S.MineGold[M] = 0;
    S.MineGold[0] = CarryCapacity;
    SeedStarterEconomy(S, 0, 2);  // #135: a camp + carts for team 0 (team 1 gets none)
    const int32_t Before0 = S.Teams[0].Gold, Before1 = S.Teams[1].Gold;
    for (int I = 0; I < 400; ++I) S.StepEvents(nullptr, 0);
    CHECK(S.MineGold[0] == 0);
    CHECK(S.Teams[0].Gold == Before0 + CarryCapacity);  // mine 0 is team 0's safe cluster
    CHECK(S.Teams[1].Gold == Before1);                  // the far team never got a carry
}

static void TestDepletedMinesStopEconomy() {
    static Sim S;
    S.Init(0);
    for (int M = 0; M < NumMines; ++M) S.MineGold[M] = 0;
    const int32_t Before = S.Teams[0].Gold;
    for (int I = 0; I < 300; ++I) S.StepEvents(nullptr, 0);
    CHECK(S.Teams[0].Gold == Before);          // no phantom income from dead mines
    CHECK(S.Result == ResultOngoing);          // gold >= CheapestCost: still a rebuy, not a loss
}

static void TestEconomyGathersGold() {
    static Sim S;
    S.Init(0);
    SeedStarterEconomy(S, 0, 3);  // #135: a camp + 3 carts (no start economy from Init now)
    const int32_t Before = S.Teams[0].Gold;
    for (int I = 0; I < 300; ++I) S.StepEvents(nullptr, 0);  // carts gather from the home cluster
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
    for (int I = 0; I < Ticks; ++I) S.StepEvents(nullptr, 0);
    const auto T1 = std::chrono::steady_clock::now();
    const double Ms = std::chrono::duration<double, std::milli>(T1 - T0).count();
    // The flock GATHER is the hot phase (plan §6): each unit visits a cell box sized by the
    // runtime gather radius (#123: Sim::GatherR = max force radius). Log it — the device knob.
    const int32_t CellK = (S.GatherR.ToInt() + GridCellSize) / GridCellSize;  // ceil-ish half-width
    const int32_t Box = 2 * CellK + 1;
    std::printf("  stress: %d units, %.3f ms/tick over %d ticks (10 Hz budget = 100 ms); "
                "flock gather = %dx%d cells/unit (GatherR=%d, GridCellSize=%d)\n",
                S.Count, Ms / Ticks, Ticks, Box, Box, S.GatherR.ToInt(), GridCellSize);
    CHECK(S.Count > 0);
}
#endif

int main() {
    TestDeterminism();
    TestReplayReproducibility();
    TestCVarOverrideDeterminism();
#if !LUR_SHIPPING
    TestGameplayCvarListComplete();
#endif
    TestGridEqualsBruteForce();
    TestSameTypeCohesionContracts();
    TestDisableCombatNoDeaths();
    TestCartPriorityOverMirror();
    TestInterposeScreensCart();
    TestBuildingSoaHashedAndCopyable();
    TestBuildingProducesFlatCadence();
    TestBuildingCountScalesThroughput();
    TestPlacementValidity();
    TestFrontierMonotonicHighWater();
    TestBuildingRepelsUnits();
    TestSoldierTargetsEnemyBuildingByType();
    TestScissorDestroysPaperBuildingWithCounter();
    TestCartDepositsAtNearestMinerBuilding();
    TestCartStrandedWithoutMinerBuilding();
    TestEventPlaceAndQueueApply();
    TestEventQueuePartialByGold();
    TestStepEventsDeterministic();
    TestMutualAnnihilationDraw();
    TestWipeoutLoses();
    TestBuildingsDoNotSaveFromLoss();
    TestRebuyIsNotLoss();
    TestPendingProductionNotLoss();
    TestSoldierBuildingGatedOnMinerUnit();
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
