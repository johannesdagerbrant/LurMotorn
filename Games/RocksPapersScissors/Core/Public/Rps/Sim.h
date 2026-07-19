#pragma once
#include <cstdint>
#include <type_traits>

#include "Lur/Sim/Fixed.h"
#include "Rps/Tunables.h"

namespace Rps {

using Lur::Sim::Fixed;

enum EWorkerState : uint8_t { WorkToTree = 0, WorkChop = 1, WorkToCamp = 2 };
enum EResult : uint8_t { ResultOngoing = 0, ResultTeam0Wins = 1, ResultTeam1Wins = 2, ResultDraw = 3 };

// Per-team production + economy state (spec §4).
struct TeamState {
    int32_t Wood = 0;
    uint8_t Queue[QueueDepth] = {};
    int32_t QueueLen = 0;
    int32_t BuildTimer = 0;    // ticks remaining on the head unit
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
    Fixed    PrevX[MaxUnits] = {};        // last tick's pos — interpolation source (view only)
    Fixed    PrevY[MaxUnits] = {};        //   excluded from StateHash: it's a pure copy of last Pos
    int32_t  Hp[MaxUnits] = {};
    uint8_t  Type[MaxUnits] = {};         // EUnit
    uint8_t  Team[MaxUnits] = {};         // 0 or 1
    int32_t  Target[MaxUnits] = {};       // soldier: enemy slot; worker: tree index; -1 = none
    int32_t  Cooldown[MaxUnits] = {};     // soldier attack cooldown, ticks
    uint8_t  WorkerState[MaxUnits] = {};  // EWorkerState
    int32_t  Carry[MaxUnits] = {};        // worker wood in hand
    int32_t  WorkerTimer[MaxUnits] = {};  // worker chop countdown, ticks
    uint64_t AliveBits[(MaxUnits + 63) / 64] = {};
    int32_t  Count = 0;                   // high-water slot; iterate [0, Count) (deterministic on both peers)

    // ---- Static map (derived at Init; not mutated during ticks) ----
    Fixed TreeX[NumTrees] = {};
    Fixed TreeY[NumTrees] = {};

    // ---- Per-team + global ----
    TeamState Teams[2];
    uint32_t  Tick = 0;
    uint8_t   Result = ResultOngoing;     // EResult
    uint64_t  Seed = 0;

    // ---- Transient within a tick (cleared each Step; NOT hashed) ----
    int32_t DepositBuf[2] = {};           // worker deposits buffered in Movement, applied in Economy

    // ---- Config (NOT hashed) — force the brute-force neighbour path instead of the
    //      spatial grid. Grid is the default; this exists so a test can run the same
    //      seed+inputs both ways and assert the StateHash sequences are identical. ----
    bool UseBruteForce = false;

    // ---- API ----
    void Init(uint64_t Seed);
    void Step(uint8_t Mask0, uint8_t Mask1);   // one 10 Hz tick — spec §6's 8 phases, in order
    uint64_t StateHash() const;                // FNV-1a over pinned state (design §5)

    // ---- Read helpers (tests / view) ----
    bool IsAlive(int32_t I) const { return (AliveBits[I >> 6] >> (I & 63)) & 1ull; }
    int32_t AliveCount(uint8_t TeamId) const;
    static Fixed CampY(uint8_t TeamId) { return TeamId == 0 ? Camp0Y : Camp1Y; }
};

static_assert(std::is_trivially_copyable<Sim>::value,
              "Sim must stay memcpy-able: it is the rollback snapshot and the StateHash source");

} // namespace Rps
