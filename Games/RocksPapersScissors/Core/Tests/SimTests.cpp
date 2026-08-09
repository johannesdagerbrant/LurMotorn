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
#include <cstring>    // #171: GameplayNameForId round-trip (strcmp)
#include <initializer_list>
#include <type_traits>   // #159: compile-time guard that hashed sim state carries no floating point

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
    // #146: wipe the ARMY, not the HQ — the win-rule tests below exercise the economic net
    // (wiped + broke), which presumes the home base still stands (razing it is its own decisive rule).
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.Team[I] == Team && !S.IsHomeBase(I)) S.AliveBits[I >> 6] &= ~(1ull << (I & 63));
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

// ---- Per-slot entity identity (Sim::Serial). A slot index is NOT an identity: AllocSlot hands out
// the lowest free slot, so deaths recycle it and a ROLLBACK re-runs every allocation made inside the
// resim window (one extra entity in the corrected timeline slides every later spawn down a slot). The
// view smooths a rollback correction only for a slot that still holds THE SAME entity, and without a
// real identity it eased each new spawn in from its predecessor's position — a swinging arc on
// everything freshly built. The law it needs: a serial identifies one entity for the life of the
// process and is NEVER reused, across recycling, rollback rewinds, or a match restart. ----
static void TestSlotSerialNeverReused() {
    constexpr int Ticks = 400;
    static Sim S;
    S.Init(0xA11CE);   FundForArmyScript(S);

    // Highest serial ever OBSERVED alive. A slot whose serial changed must show a serial above this —
    // i.e. freshly minted — never one that has been in circulation before.
    uint32_t HighWater = 0;
    static uint32_t Seen[MaxUnits];  // per-slot: the last serial seen there (kept across the slot's death)
    for (int32_t K = 0; K < MaxUnits; ++K) Seen[K] = 0;
    bool FreshOnly = true, NonZero = true, NoDuplicates = true;
    int32_t Recycled = 0;            // slots that changed hands at least once — the case under test

    for (int I = 0; I < Ticks && FreshOnly && NonZero && NoDuplicates; ++I) {
        ArmyStep(S, static_cast<uint32_t>(I));
        for (int32_t K = 0; K < S.Count; ++K) {
            if (!S.IsAlive(K)) continue;
            const uint32_t Sr = S.Serial[K];
            if (Sr == 0) { NonZero = false; break; }              // every live entity is identified
            if (Sr != Seen[K]) {                                  // this slot changed hands
                if (Sr <= HighWater) { FreshOnly = false; break; }  // ...to a RECYCLED identity: the bug
                if (Seen[K] != 0) ++Recycled;                       // ...and it had held someone before
                HighWater = Sr;
                Seen[K] = Sr;
            }
        }
        // No two live entities may share an identity in the same tick.
        for (int32_t A = 0; A < S.Count && NoDuplicates; ++A) {
            if (!S.IsAlive(A)) continue;
            for (int32_t B = A + 1; B < S.Count; ++B)
                if (S.IsAlive(B) && S.Serial[A] == S.Serial[B]) { NoDuplicates = false; break; }
        }
    }
    CHECK(NonZero);
    CHECK(FreshOnly);
    CHECK(NoDuplicates);
    CHECK(Recycled > 0);   // the run must actually exercise slot reuse, or the law is vacuous

    // A ROLLBACK rewinds the state but must NOT rewind the identity counter: re-simulating a corrected
    // timeline re-runs the allocations, and a rewound counter would hand the discarded timeline's
    // serials back out — the collision the view cannot see through (it compares serials, and equal
    // means "same entity, smooth the correction"). Restore an old snapshot, spawn again, and the new
    // entity must carry an identity no one has held.
    static Sim Rewound;
    Rewound = S;                      // the "snapshot" (a plain copy — what the ring stores)
    const uint32_t HeadSerial = S.NextSerial;
    for (int I = 0; I < 20; ++I) ArmyStep(S, static_cast<uint32_t>(I));  // speculate on past the snapshot
    CHECK(S.NextSerial > HeadSerial);  // the discarded timeline minted identities...
    S.RestoreFrom(Rewound);            // ...and the rewind must not hand them out again
    const uint32_t AfterRestore = S.NextSerial;
    CHECK(AfterRestore > HeadSerial);          // counter kept the speculative head's position
    CHECK(S.Tick == Rewound.Tick);             // ...while the STATE really did rewind
    CHECK(S.StateHash() == Rewound.StateHash());  // Serial is not hashed: identity rides outside state
}

// StateHash is a PINNED VALUE, not just a self-consistent one. Determinism tests above
// only prove two runs in the SAME build agree; this proves the hash the peer compares in
// every keepalive, and the `h` lines in every flight recording, are the same numbers they
// were before. It is the guard that lets the hash implementation be refactored (e.g. onto
// the engine's Lur::Core::Fnv1a64 instead of a private copy of FNV-1a) without silently
// re-baselining every recorded match and every desync comparison.
//
// If this fails: something changed the hashed state, its ORDER, or the mix function. That
// is a build-locked wire change — both phones must ship together — so re-pin deliberately,
// never reflexively.
static void TestStateHashGoldenValues() {
    constexpr uint64_t Seed = 0xFEEDBEEFu;
    static Sim S;
    S.Init(Seed);
    CHECK(S.StateHash() == 0xba5b7041cc851843ull);   // fresh sim, tick 0

    FundForArmyScript(S);
    for (int I = 0; I < 300; ++I) ArmyStep(S, static_cast<uint32_t>(I));
    CHECK(S.StateHash() == 0xec58aac6d9f3d598ull);   // after the 300-tick army script
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

// ---- #171: the id->name map must be the exact inverse of the name->id one ----
// GameplayNameForId exists so --recdiff can print "rps.econ.starting_gold" instead of "id 64",
// and the only way that line is worth anything is if the name it prints is the tunable the id
// actually addresses. Both maps expand from the same X-list, so they can only disagree if one
// grows a hand-written entry — cheap to pin, and a wrong name here would misdirect exactly the
// investigation the tool exists to support (a mislabelled cvar sent the 2026-08-01 two-phone
// desync hunt at cross-compiler nondeterminism instead of the cvar merge).
static void TestGameplayCvarIdNameRoundTrips() {
    for (int Id = 0; Id < CvIdCount; ++Id) {
        const char* Name = GameplayNameForId(Id);
        CHECK(std::strcmp(Name, "?") != 0);       // every wire id names a real cvar
        CHECK(GameplayIdForName(Name) == Id);     // ...and that name maps back to this id
    }
    CHECK(std::strcmp(GameplayNameForId(CvIdCount), "?") == 0);   // out of range stays "?"
    CHECK(GameplayIdForName("rps.dev.flight_recorder") < 0);      // non-gameplay is not on the wire
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
        S.Kind[K] = KindUnit;   // #146: slots 0/1 held home bases from Init — reset or they'd stay static
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
    // NOT F(200): initial_frontier is 40, so FrontierT1 already IS 240-40 = 200 and assigning it
    // changes nothing. The test is "every field is mixed into the hash", so it needs a value the
    // field does not already hold.
    S.FrontierT1 = F(199);                    CHECK(S.StateHash() != H4);
    const uint64_t H5 = S.StateHash();

    // memcpy snapshot (the rollback mechanism) preserves every new field bit-for-bit.
    static Sim Snap;
    Snap = S;  // trivially-copyable assignment == memcpy
    CHECK(Snap.StateHash() == H5);
    CHECK(Snap.Kind[0] == KindBuilding && Snap.IsBuilding(0));
    CHECK(Snap.Queue[0] == 7 && Snap.BuildProgress[0] == 13);
    CHECK(Snap.FrontierT0 == F(42) && Snap.FrontierT1 == F(199));
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
    // Live mine: inside the clearance is rejected, outside it is fine. Keyed on Cv.MineClearance,
    // NOT the footprint (#157) — those were the same number until the clearance was widened so a
    // camp could not cover the mine and hide the carts working it.
    const Fixed Mc = S.Cv.MineClearance;
    S.MineX[0] = F(20); S.MineY[0] = F(12); S.MineGold[0] = MineGoldCapacity;
    CHECK(!S.CanPlaceBuilding(0, UnitRock, F(20), F(12)));
    CHECK(!S.CanPlaceBuilding(0, UnitRock, F(20), F(12) + Mc - F(1)));   // just inside -> refused
    CHECK(S.CanPlaceBuilding(0, UnitRock, F(20), F(12) + Mc + F(1)));    // just outside -> allowed
}

// §5.3 (playtest decision 2026-07-25, replacing #133's monotonic high-water): the frontier tracks a
// team's FRONTMOST LIVE PRESENCE — unit or building — so killing whatever is furthest forward pushes
// that team's build line back with it. Floored at the opening frontier so a beaten team can still
// rebuild.
static void TestFrontierFollowsFrontmostSurvivor() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    CHECK(S.FrontierT0 == S.Cv.InitialFrontier);        // starts at the opening band
    PlaceUnit(S, 0, F(17), F(100), 0, UnitRock);        // a team-0 unit already past the line
    PlaceUnit(S, 1, F(20), F(60), 0, UnitRock);         // ...and one of ours behind it
    S.Count = 2;
    for (int t = 0; t < 10; ++t) S.StepEvents(nullptr, 0);          // both march forward (up)
    const Fixed Adv = S.FrontierT0;
    CHECK(Adv >= F(100));                                // advanced to the leader's reach
    const Fixed Second = S.PosY[1];
    CHECK(Second < Adv);
    S.AliveBits[0] &= ~1ull;                             // the FORWARD unit dies
    S.StepEvents(nullptr, 0);
    CHECK(S.FrontierT0 < Adv);                           // the line gave ground...
    CHECK(S.FrontierT0 >= Second);                       // ...back to the next survivor (which also moved)
    // Everything dies: the line falls back to the opening depth and no further, so the team can
    // still rebuild.
    S.AliveBits[0] = 0;
    S.StepEvents(nullptr, 0);
    CHECK(S.FrontierT0 == S.Cv.InitialFrontier);
    // A BUILDING is presence too: a forward building holds the line with no units at all.
    PlaceBuilding(S, 2, F(17), F(90), 0, UnitRock);
    S.Count = 3;
    S.StepEvents(nullptr, 0);
    CHECK(S.FrontierT0 >= F(90));                        // held by the building alone
    S.AliveBits[0] &= ~(1ull << 2);                      // raze it -> the ground is reclaimed
    S.StepEvents(nullptr, 0);
    CHECK(S.FrontierT0 == S.Cv.InitialFrontier);
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

// Cart spread: two carts with two NEAR-EQUIDISTANT deposits take ONE EACH, so a glance counts
// them. Three legs, because the point is that the spread is free and NOT a cap: (a) slack 3
// spreads across a deposit 2 units farther, (b) slack 1 does NOT — the second cart stacks rather
// than walk, so the budget really is what bounds the travel, (c) with only one deposit in budget
// EVERY cart takes it, however many that is — crowding is preferred against, never forbidden.
static void TestCartsSpreadAcrossNearbyMines() {
    // (a) budget 3 covers the 2-unit-farther deposit -> one cart each.
    {
        static Sim S;
        S.Init(0);
        ClearField(S);                     // wipes every mine; we place two by hand
        S.Cv.MineSpreadSlack = F(3);       // explicit, so the test doesn't ride the default
        S.MineX[0] = F(16); S.MineY[0] = F(15); S.MineGold[0] = MineGoldCapacity;  // travel 5
        S.MineX[1] = F(16); S.MineY[1] = F(13); S.MineGold[1] = MineGoldCapacity;  // travel 7
        PlaceUnit(S, 0, F(16), F(20), 0, UnitMiner);
        PlaceUnit(S, 1, F(17), F(20), 0, UnitMiner);
        S.Count = 2;
        S.StepEvents(nullptr, 0);
        CHECK(S.Target[0] == 0);                    // nearest, and it was empty
        CHECK(S.Target[1] == 1);                    // stepped over to the empty one
    }
    // (b) budget 1 does not reach it -> both stack on the nearest (the old rule).
    {
        static Sim S;
        S.Init(0);
        ClearField(S);
        S.Cv.MineSpreadSlack = F(1);
        S.MineX[0] = F(16); S.MineY[0] = F(15); S.MineGold[0] = MineGoldCapacity;  // travel 5
        S.MineX[1] = F(16); S.MineY[1] = F(13); S.MineGold[1] = MineGoldCapacity;  // travel 7 > 5+1
        PlaceUnit(S, 0, F(16), F(20), 0, UnitMiner);
        PlaceUnit(S, 1, F(17), F(20), 0, UnitMiner);
        S.Count = 2;
        S.StepEvents(nullptr, 0);
        CHECK(S.Target[0] == 0 && S.Target[1] == 0);
    }
    // (c) NO ceiling: 10 carts, one deposit in budget and one far away -> all 10 take the near
    // one. The old 6-per-mine cap sent carts 7..10 on the long walk; nothing does now.
    {
        static Sim S;
        S.Init(0);
        ClearField(S);
        S.Cv.MineSpreadSlack = F(3);
        S.MineX[0] = F(17); S.MineY[0] = F(15); S.MineGold[0] = MineGoldCapacity;  // travel 5
        S.MineX[1] = F(17); S.MineY[1] = F(30); S.MineGold[1] = MineGoldCapacity;  // travel 10 > 5+3
        const int Carts = 10;
        for (int K = 0; K < Carts; ++K) PlaceUnit(S, K, F(17), F(20), 0, UnitMiner);
        S.Count = Carts;
        S.StepEvents(nullptr, 0);
        int32_t OnNear = 0;
        for (int K = 0; K < Carts; ++K) if (S.Target[K] == 0) ++OnNear;
        CHECK(OnNear == Carts);
    }
    // (d) the ROW rule, which is what stops a generous slack from emptying a row into the next
    // one: a deposit in ANOTHER row is taken only when it is no farther than the nearest. Mines
    // are index-contiguous per row, so index MinesPerCluster is the first mine of row 1.
    const int32_t OtherRow = MinesPerCluster;
    {   // farther + another row -> refused even though the slack covers the distance
        static Sim S;
        S.Init(0);
        ClearField(S);
        S.Cv.MineSpreadSlack = F(8);
        S.MineX[0] = F(16); S.MineY[0] = F(15); S.MineGold[0] = MineGoldCapacity;         // travel 5
        S.MineX[OtherRow] = F(16); S.MineY[OtherRow] = F(9); S.MineGold[OtherRow] = MineGoldCapacity;  // 11
        PlaceUnit(S, 0, F(16), F(20), 0, UnitMiner);
        PlaceUnit(S, 1, F(16), F(20), 0, UnitMiner);
        S.Count = 2;
        S.StepEvents(nullptr, 0);
        CHECK(S.Target[0] == 0 && S.Target[1] == 0);   // both stay in their own row
    }
    {   // exact tie across rows -> allowed, because it costs nothing (a camp between two rows)
        static Sim S;
        S.Init(0);
        ClearField(S);
        S.Cv.MineSpreadSlack = F(8);
        S.MineX[0] = F(16); S.MineY[0] = F(15); S.MineGold[0] = MineGoldCapacity;         // travel 5
        S.MineX[OtherRow] = F(16); S.MineY[OtherRow] = F(25); S.MineGold[OtherRow] = MineGoldCapacity;  // 5
        PlaceUnit(S, 0, F(16), F(20), 0, UnitMiner);
        PlaceUnit(S, 1, F(16), F(20), 0, UnitMiner);
        S.Count = 2;
        S.StepEvents(nullptr, 0);
        CHECK(S.Target[0] == 0 && S.Target[1] == OtherRow);
    }
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
        if (S.IsAlive(I) && S.IsBuilding(I) && !S.IsHomeBase(I)) return I;  // #146: skip the HQ
    return -1;
}

static void TestEventPlaceAndQueueApply() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    // Fund this from the LIVE costs, never a hardcoded purse: these are CVars, and a flat 1000 went
    // silently insolvent when the camp went to 600 and miners to 100 — the queue then clamped to
    // what was affordable and this read as a queue-mechanics failure instead of a broke test.
    const int32_t PlaceCost = BuildingCostFor(S.Cv, UnitMiner);
    const int32_t MinerUnitCost = S.Units[UnitMiner].Cost;
    const int32_t Purse = PlaceCost + 5 * MinerUnitCost + 1000;   // + slack for the invalid-place probe
    S.Teams[0].Gold = Purse;
    InputEvent P = InputEvent::Place(0, UnitMiner, F(17), F(10));
    S.StepEvents(&P, 1);
    const int32_t B = FirstBuilding(S);
    CHECK(B >= 0);
    CHECK(S.Team[B] == 0 && S.Type[B] == UnitMiner && S.Kind[B] == KindBuilding);
    CHECK(S.Teams[0].Gold == Purse - PlaceCost);
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

// #146: destroying a team's HOME BASE wins the match at once — the decisive blow, independent of
// that team's army or gold (the old economic rule required both to run dry). Uses a real Init sim
// (both home bases present), gives the loser a big army + wallet so ONLY the base falling can end it.
static void TestHomeBaseDestroyedWins() {
    static Sim S;
    S.Init(0);
    S.Teams[0].Gold = 1000;                                   // solvent...
    PlaceUnit(S, S.Count, F(17), F(20), 0, UnitRock); ++S.Count;  // ...and has an army
    int32_t Hb = -1;
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.IsHomeBase(I) && S.Team[I] == 0) { Hb = I; break; }
    CHECK(Hb >= 0);
    S.Hp[Hb] = 0;                 // razed — Deaths clears it, WinCheck sees no team-0 home base
    S.StepEvents(nullptr, 0);
    CHECK(!S.IsAlive(Hb));
    CHECK(S.Result == ResultTeam1Wins);  // team 0 lost the instant its base fell, army/gold notwithstanding
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
// ---- #159: nothing in the HASHED sim state may be floating point ----
// The remaining unexplained candidate for the 2026-07-30 divergence (13.5 minutes of clean lockstep,
// then two different hashes at tick 8180) is cross-compiler nondeterminism: NDK clang and Apple clang
// compiling the same source, with something in sim state that should be Fixed and is not. The classic
// signature is exactly what was seen — thousands of identical ticks and then a late 1-ULP split.
//
// There is no such field today, which is worth ASSERTING rather than re-establishing by reading the
// header each time the question comes up. These are compile-time checks over the members StateHash
// mixes, so adding a float to hashed state fails the build with this comment attached, on the host,
// before either phone is involved. (Modules/Math floats are for RENDERING and never reach here.)
static void TestNoFloatingPointInHashedSimState() {
    // Fixed is the substrate: integer-backed by construction, so every position/velocity derived from
    // it is exact and identical on both compilers.
    static_assert(std::is_integral_v<decltype(Fixed::Raw)>, "Fixed must stay integer-backed (#159)");
    static_assert(sizeof(Fixed) == sizeof(int32_t), "Fixed must be a thin wrapper, no padding (#159)");

    // Every array StateHash mixes, in its declaration order. Fixed counts as integral by the check
    // above; everything else must be a plain integer type.
    using PosT = std::remove_extent_t<decltype(Sim::PosX)>;
    using HpT = std::remove_extent_t<decltype(Sim::Hp)>;
    using TypeT = std::remove_extent_t<decltype(Sim::Type)>;
    using TargetT = std::remove_extent_t<decltype(Sim::Target)>;
    using CooldownT = std::remove_extent_t<decltype(Sim::Cooldown)>;
    using CarryT = std::remove_extent_t<decltype(Sim::Carry)>;
    using QueueT = std::remove_extent_t<decltype(Sim::Queue)>;
    using ProgressT = std::remove_extent_t<decltype(Sim::BuildProgress)>;
    static_assert(std::is_same_v<PosT, Fixed>, "positions must be Fixed, never float (#159)");
    static_assert(std::is_same_v<std::remove_extent_t<decltype(Sim::PrevX)>, Fixed>, "#159");
    static_assert(std::is_integral_v<HpT> && std::is_integral_v<TypeT>, "#159");
    static_assert(std::is_integral_v<TargetT> && std::is_integral_v<CooldownT>, "#159");
    static_assert(std::is_integral_v<CarryT> && std::is_integral_v<QueueT>, "#159");
    static_assert(std::is_integral_v<ProgressT>, "#159");
    static_assert(std::is_integral_v<decltype(Sim::Tick)>, "#159");
    static_assert(std::is_integral_v<decltype(Sim::Seed)>, "#159");
    // The whole sim must stay memcpy-able too: the snapshot seam and the future rollback both copy it,
    // and a member needing a real copy constructor is a member that can carry non-POD state.
    static_assert(std::is_trivially_copyable_v<Sim>, "Sim must stay POD-copyable (#159, Review #2)");
    CHECK(true);  // the assertions above are the test; this keeps the runner's shape uniform
}

// ---- #162: a MINER must not pay for a flock gather it never reads ----
// Measured on a Galaxy A14 at ~1600 units: sim.step 57 ms of a 100 ms tick, of which sim.move was
// ~50 ms — and the field was carts ("I was able to make the match freeze by making over 1600
// carts"). Movement gathered every miner's whole neighbourhood and then took only the mine-repel
// ring from it, discarding the gather: the single hottest phase in the game was dead work for the
// exact unit type the repro spams.
//
// Asserted as a COUNT rather than a duration, because a wall-clock budget on a -O0 host test is
// either flaky or meaningless. Zero gathers is the property; the timing follows from it.
static void TestMinersDoNotPayForAFlockGather() {
    static Sim S;
    S.Init(0x162);
    S.StressFill(600, UnitMiner);   // carts only — the reported repro
    CHECK(S.Count > 1000);
    S.FlockGathers = 0;
    S.StepEvents(nullptr, 0);
    CHECK(S.FlockGathers == 0);     // not one neighbourhood query for a field of carts

    // And the counter is not simply dead: soldiers DO gather, one query each.
    static Sim T;
    T.Init(0x162);
    T.StressFill(600, UnitRock);
    T.FlockGathers = 0;
    T.StepEvents(nullptr, 0);
    CHECK(T.FlockGathers > 1000);
}

// ---- #162: the INVARIANT that makes the above safe ----
// Skipping the gather is only correct while nothing a miner does depends on its neighbours. Asserted
// directly, so a future change that starts feeding the flock accumulator into cart movement fails
// HERE — loudly, next to the reason — instead of silently un-optimising or silently diverging.
// Combat off, so the crowd cannot affect the cart by killing it rather than by steering it.
static void TestMinerPathIgnoresNeighbours() {
    static Sim Alone, Crowded;
    for (Sim* P : {&Alone, &Crowded}) {
        P->Init(0x1620);
        P->DisableCombat = true;
        ClearField(*P);
        P->Count = 1;
        PlaceUnit(*P, 0, F(17), F(20), 0, UnitMiner);
    }
    // Same cart, but ringed by soldiers of both teams — every force the gather can produce.
    Crowded.Count = 25;
    for (int K = 1; K < 25; ++K)
        PlaceUnit(Crowded, K, F(14 + (K % 7)), F(17 + (K % 5)), static_cast<uint8_t>(K & 1),
                  static_cast<uint8_t>(1 + (K % 3)));

    bool Same = true;
    for (int I = 0; I < 120 && Same; ++I) {
        Alone.StepEvents(nullptr, 0);
        Crowded.StepEvents(nullptr, 0);
        Same = Alone.PosX[0] == Crowded.PosX[0] && Alone.PosY[0] == Crowded.PosY[0];
    }
    CHECK(Same);
}

// #181: soldier production must HOLD at the per-team ceiling, so sim.move is bounded by construction
// rather than chased with constant-factor micro-opts (the P1/aggregate path, which the closely-spaced
// force radii cap at ~4%). Set a tiny cap, queue far more soldiers than it, and assert the live soldier
// count tops out AT the cap (never over), the order is HELD (queue not silently dropped), carts are
// exempt, and a death frees exactly one slot for the held order.
static void TestUnitCeilingHoldsSoldierProduction() {
    static Sim S;
    S.Init(0);
    ClearField(S);
    S.Cv.UnitCeiling = 5;  // tiny cap -> a fast, exact test
    const int Rock = S.Count; PlaceBuilding(S, S.Count, F(17), F(20), 0, UnitRock);  ++S.Count;
    S.Queue[Rock] = 50;    // far more soldiers than the cap
    const int Mine = S.Count; PlaceBuilding(S, S.Count, F(25), F(20), 0, UnitMiner); ++S.Count;
    S.Queue[Mine] = 50;    // carts: uncapped

    for (int T = 0; T < 4000; ++T) S.StepEvents(nullptr, 0);

    int32_t Soldiers = 0, Carts = 0;
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && !S.IsBuilding(I) && S.Team[I] == 0)
            (S.Type[I] == UnitMiner ? Carts : Soldiers) += 1;
    CHECK(Soldiers == 5);          // exactly the cap, never over
    CHECK(S.Queue[Rock] > 0);      // the order is still owed, not silently dropped
    CHECK(Carts > 5);              // carts exempt from the soldier ceiling

    // Free ONE soldier slot -> the held order must pop exactly one replacement, back to the cap.
    const int32_t QBefore = S.Queue[Rock];
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && !S.IsBuilding(I) && S.Type[I] == UnitRock) {
            S.AliveBits[I >> 6] &= ~(1ull << (I & 63)); break;  // POD-is-public kill (no combat here)
        }
    S.StepEvents(nullptr, 0);      // held building spawns immediately (BuildProgress was at completion)
    int32_t After = 0;
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && !S.IsBuilding(I) && S.Team[I] == 0 && S.Type[I] != UnitMiner) ++After;
    CHECK(After == 5);                     // refilled to the cap, not over
    CHECK(S.Queue[Rock] == QBefore - 1);   // and the held order was consumed by exactly one
}

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

    // #162: the same measurement for the shape that actually collapsed on hardware — ~1600 CARTS, not
    // soldiers. Printed next to the soldier figure because the interesting number is the RATIO: carts
    // used to cost the same as soldiers despite reading nothing from the gather.
    static Sim C;
    C.Init(0x57A9E56);
    C.StressFill(800, UnitMiner);
    const auto C0 = std::chrono::steady_clock::now();
    for (int I = 0; I < Ticks; ++I) C.StepEvents(nullptr, 0);
    const auto C1 = std::chrono::steady_clock::now();
    const double CMs = std::chrono::duration<double, std::milli>(C1 - C0).count();
    std::printf("  stress: %d CARTS, %.3f ms/tick (gathers/tick=%lld — #162 expects 0)\n", C.Count,
                CMs / Ticks, static_cast<long long>(C.FlockGathers / Ticks));
    CHECK(C.Count > 1000);
}
#endif

int main() {
    TestDeterminism();
    TestStateHashGoldenValues();
    TestReplayReproducibility();
    TestCVarOverrideDeterminism();
#if !LUR_SHIPPING
    TestGameplayCvarListComplete();
    TestGameplayCvarIdNameRoundTrips();   // #171
#endif
    TestGridEqualsBruteForce();
    TestNoFloatingPointInHashedSimState();  // #159
#if LUR_INTERNAL
    TestMinersDoNotPayForAFlockGather();   // #162
#endif
    TestMinerPathIgnoresNeighbours();      // #162 (the invariant the optimisation rests on)
    TestSlotSerialNeverReused();           // per-slot entity identity (the view's rollback-smoothing guard)
    TestSameTypeCohesionContracts();
    TestDisableCombatNoDeaths();
    TestCartPriorityOverMirror();
    TestInterposeScreensCart();
    TestBuildingSoaHashedAndCopyable();
    TestBuildingProducesFlatCadence();
    TestBuildingCountScalesThroughput();
    TestPlacementValidity();
    TestFrontierFollowsFrontmostSurvivor();
    TestBuildingRepelsUnits();
    TestSoldierTargetsEnemyBuildingByType();
    TestScissorDestroysPaperBuildingWithCounter();
    TestCartDepositsAtNearestMinerBuilding();
    TestCartsSpreadAcrossNearbyMines();
    TestCartStrandedWithoutMinerBuilding();
    TestEventPlaceAndQueueApply();
    TestEventQueuePartialByGold();
    TestStepEventsDeterministic();
    TestMutualAnnihilationDraw();
    TestWipeoutLoses();
    TestBuildingsDoNotSaveFromLoss();
    TestHomeBaseDestroyedWins();
    TestRebuyIsNotLoss();
    TestPendingProductionNotLoss();
    TestSoldierBuildingGatedOnMinerUnit();
    TestMineDepletesAndVanishes();
    TestDepletedMinesStopEconomy();
    TestEconomyGathersGold();
#if LUR_INTERNAL
    TestUnitCeilingHoldsSoldierProduction();  // #181
    TestStressTickBudget();
#endif

    if (GFailures == 0) std::printf("rps_sim_tests: ALL PASS\n");
    else std::printf("rps_sim_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
