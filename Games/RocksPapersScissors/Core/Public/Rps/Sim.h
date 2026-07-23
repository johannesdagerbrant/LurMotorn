#pragma once
#include <cstdint>
#include <type_traits>

#include "Lur/Sim/Fixed.h"
#include "Rps/Tunables.h"

namespace Rps {

using Lur::Sim::Fixed;

enum EWorkerState : uint8_t { WorkToMine = 0, WorkDig = 1, WorkToCamp = 2 };
enum EResult : uint8_t { ResultOngoing = 0, ResultTeam0Wins = 1, ResultTeam1Wins = 2, ResultDraw = 3 };

// #137 (buildings rework): a tick-stamped player input event — the richer input unit that
// REPLACES the old 4-bit press mask (the four buttons are now building drag-sources + per-
// building queue taps, no longer instantaneous unit-queues). POD; applied in phase 0 of the
// tick in a deterministic order on both peers. Two kinds:
//   * EventPlaceBuilding — drop a building of Type (an EUnit) at world position (X,Y).
//   * EventQueueUnits    — enqueue Count units at the building in slot Slot.
enum EEventKind : uint8_t { EventPlaceBuilding = 0, EventQueueUnits = 1 };
struct InputEvent {
    uint8_t Kind = EventPlaceBuilding;  // EEventKind
    uint8_t Team = 0;                   // which player (0/1) issued it
    uint8_t Type = 0;                   // Place: produced unit type (EUnit). Queue: unused.
    int32_t X = 0;                      // Place: PosX (Fixed raw). Queue: building slot.
    int32_t Y = 0;                      // Place: PosY (Fixed raw). Queue: unit count.

    static InputEvent Place(uint8_t Team, uint8_t Type, Fixed X, Fixed Y) {
        return {EventPlaceBuilding, Team, Type, X.Raw, Y.Raw};
    }
    static InputEvent Queue(uint8_t Team, int32_t Slot, int32_t Count) {
        return {EventQueueUnits, Team, 0, Slot, Count};
    }
};
// Per-tick input cap: a human (or the AI) can't issue more than a few taps per 100 ms tick.
// One home for it (Core), shared by the sim/runner/AI and the Net batch codec.
constexpr int MaxEventsPerTick = 16;

// #131 (buildings rework): a slot is either a mobile UNIT or a static BUILDING. Buildings
// live in the SAME per-unit SoA — a slot with Kind==KindBuilding reuses Hp/Team/Pos/AliveBits
// with building meaning (Type = the unit type it PRODUCES; §7 targeting treats it as that
// type). KindUnit==0 so a zero-initialised (or legacy) slot is a unit by default.
enum EKind : uint8_t { KindUnit = 0, KindBuilding = 1 };

// Per-team production + economy state (#84: four PARALLEL per-type queues).
// QueueCount[ty] includes the unit currently building; BuildProgress[ty] advances
// by QueueCount[ty] per tick (stack acceleration) and spawns every BuildTicks.
struct TeamState {
    int32_t Gold = 0;
    int32_t QueueCount[UnitCount] = {};
    int32_t BuildProgress[UnitCount] = {};
    int32_t SpawnCounter = 0;  // drives the deterministic spawn ring (no RNG)
};

// The entire simulation as Plain Old Data — SoA parallel arrays, trivially
// memcpy-able. Two properties fall out of this and are load-bearing (design §5):
//   1. The future rollback snapshot is a memcpy; StateHash is one pass.
//   2. Arrays of naked fixed-width ints have NO padding bytes, so the
//      cross-compiler struct-padding hash hazard cannot exist here.
// Declaration order of the per-unit arrays is PINNED — it is the StateHash order.
struct Sim {
    // ---- Per-unit SoA (fixed-width / Fixed only; zero-initialised whole) ----
    Fixed    PosX[MaxUnits] = {};
    Fixed    PosY[MaxUnits] = {};
    Fixed    PrevX[MaxUnits] = {};        // last tick's pos — interpolation source AND, since #97,
    Fixed    PrevY[MaxUnits] = {};        //   velocity Δ=Pos−Prev (momentum). HASHED (authoritative).
    int32_t  Hp[MaxUnits] = {};
    uint8_t  Type[MaxUnits] = {};         // EUnit
    uint8_t  Team[MaxUnits] = {};         // 0 or 1
    int32_t  Target[MaxUnits] = {};       // soldier: enemy slot; worker: mine index; -1 = none
    int32_t  Cooldown[MaxUnits] = {};     // soldier attack cooldown, ticks
    uint8_t  WorkerState[MaxUnits] = {};  // EWorkerState
    int32_t  Carry[MaxUnits] = {};        // worker gold in hand
    int32_t  WorkerTimer[MaxUnits] = {};  // worker dig countdown, ticks
    // ---- #131 buildings: appended to the per-unit SoA so the existing unit-field StateHash
    //      layout is undisturbed; the hash is then EXTENDED with these. Zero for unit slots. ----
    uint8_t  Kind[MaxUnits] = {};         // EKind — KindUnit (0) or KindBuilding
    int32_t  Queue[MaxUnits] = {};        // building: units queued here (0 for units). Cap = Cv.BuildingQueueMax
    int32_t  BuildProgress[MaxUnits] = {};// building: current unit's construction progress, ticks (0 for units)
    uint64_t AliveBits[(MaxUnits + 63) / 64] = {};
    int32_t  Count = 0;                   // high-water slot; iterate [0, Count) (deterministic on both peers)

    // ---- Mine positions (derived at Init; not mutated during ticks) ----
    Fixed MineX[NumMines] = {};
    Fixed MineY[NumMines] = {};
    // ---- Mine reserves (#84: MUTATED during ticks -> part of StateHash). A mine
    //      with MineGold <= 0 is gone: never targeted, skipped by the view. ----
    int32_t MineGold[NumMines] = {};

    // ---- Per-team + global ----
    TeamState Teams[2];
    // #131/§5.3 frontier high-water: the furthest-forward Y any of a team's units has EVER
    // reached (team 0 advances up = max Y; team 1 advances down = min Y). Monotonic, HASHED —
    // gates forward placement (§5.1), so both peers must agree. Init to CvInitialFrontier (#133).
    Fixed     FrontierT0{};
    Fixed     FrontierT1{};
    uint32_t  Tick = 0;
    uint8_t   Result = ResultOngoing;     // EResult
    uint64_t  Seed = 0;

    // ---- #112: authoritative gameplay-CVar values, per-Sim state, HASHED ----
    // Latched from the global CVars ONCE at Init, then owned by the Sim: constant within a
    // tick and mutated only at tick boundaries by synced overrides (LockstepPeer applies a
    // resolved MsgCvar at its stamped tick on both peers). Because it lives in the Sim, two
    // peers in one process (loopback / two-window --tune) hold INDEPENDENT overrides that
    // the sync converges — the workbench-faithful model. Folded into StateHash so a
    // mis-apply/mis-sync is an immediate desync alarm. POD -> Sim stays trivially copyable.
    CvSnapshot Cv{};

    // ---- Derived from Cv (NOT hashed): per-type stats with the tunable fields
    //      (cost/hp/speed/damage/build_time) taken from the unit CVars and the rest
    //      (range/cooldown/beats) from UnitTable. Rebuilt by DeriveUnits() whenever Cv
    //      (re)latches — at Init and each Step — so the hot loops read a struct array, not a
    //      per-read CVar lookup. Deterministic (a pure function of the hashed Cv), so leaving
    //      it out of StateHash is safe: both peers derive identical values. ----
    UnitStats Units[UnitCount] = {};
    // Runtime flock-gather radius (#123): max of the gathered soldier-flock radii in Cv,
    // recomputed by DeriveUnits(). The grid neighbour walk uses it so grid==brute holds for
    // ANY radii — raising a radius CVar can't under-cover the grid. Derived, NOT hashed.
    Fixed GatherR{};

    // ---- Transient within a tick (cleared each Step; NOT hashed) ----
    int32_t DepositBuf[2] = {};           // worker deposits buffered in Movement, applied in Economy

    // ---- Config (NOT hashed) — force the brute-force neighbour path instead of the
    //      spatial grid. Grid is the default; this exists so a test can run the same
    //      seed+inputs both ways and assert the StateHash sequences are identical. ----
    bool UseBruteForce = false;

    // ---- Config (NOT hashed) — LUR_INTERNAL --flockdemo (#97): suppress attacks so
    //      mixed blobs march/flock without killing each other, for pure visual tuning
    //      of the flow (momentum smoothing, dense-pack jitter). Never set in real play. ----
    bool DisableCombat = false;

    // ---- Config (NOT hashed) — solo/desktop live CVar tuning (#115). When set, Step
    //      re-latches Cv from the global CVars every tick, so a desktop `--tune` edit
    //      changes the RUNNING sim live. OFF by default: a networked (lockstep) match
    //      keeps Cv per-Sim (latched at Init + synced overrides), so this never affects a
    //      real match or the determinism tests — only the solo SimRunner turns it on. ----
    bool LiveCvLatch = false;

    // ---- API ----
    void Init(uint64_t Seed);
    void Step(uint8_t Mask0, uint8_t Mask1);   // one 10 Hz tick — spec §6's 8 phases, in order
    // #137: one tick driven by the tick's INPUT EVENT batch (both teams interleaved, applied in
    // array order — deterministic on both peers). Replaces the mask on the wire/history/recorder;
    // Step(mask) stays for the legacy camp path + old tests until the flip retires it.
    void StepEvents(const InputEvent* Events, int32_t Count);
    void DeriveUnits();                        // refresh Units[] from Cv (#122); call after any Cv (re)latch
    uint64_t StateHash() const;                // FNV-1a over pinned state (design §5)

    // #133/§5.1: is a building of Type placeable at (X,Y) for Team? PURE function of the
    // current (hashed) sim state — in-bounds, within the team's frontier, and no footprint
    // overlap with an existing building or a live mine. Identical on both peers (no RNG),
    // so applying a place event that fails this is a deterministic no-op. Also read by the
    // view each frame for the ghost's valid/invalid blink (§4.1) — read-only, writes nothing.
    bool CanPlaceBuilding(uint8_t Team, uint8_t Type, Fixed X, Fixed Y) const;

#if LUR_INTERNAL
    // Dev-only stress scene (issue #75): bulk-spawn PerTeam soldiers spread across each
    // half of the field, to prove the tick budget (grid) + one-draw render hold at the
    // raised cap. Deterministic (seed-derived). Compiled out of Shipping — never ships.
    void StressFill(int32_t PerTeam);
#endif

    // ---- Read helpers (tests / view) ----
    bool IsAlive(int32_t I) const { return (AliveBits[I >> 6] >> (I & 63)) & 1ull; }
    bool IsBuilding(int32_t I) const { return Kind[I] == KindBuilding; }   // #131
    int32_t AliveCount(uint8_t TeamId) const;
    int32_t QueuedTotal(uint8_t TeamId) const;   // sum over the four per-type queues
    static Fixed CampY(uint8_t TeamId) { return TeamId == 0 ? Camp0Y : Camp1Y; }
};

static_assert(std::is_trivially_copyable<Sim>::value,
              "Sim must stay memcpy-able: it is the rollback snapshot and the StateHash source");

} // namespace Rps
