// RocksPapersScissors deterministic simulation core.
//
// The one law: State = Replay(Inputs, Seed) (design doc §1). Init derives the
// world from the seed; Step(mask0, mask1) applies exactly one 10 Hz tick as the
// eight fixed-order phases of spec §6. No wallclock, no float, no allocation ever
// touches this file — that is the precondition for the slice-1 lockstep netcode.
//
// The input DELAY (press at T executes at T+3) is deliberately NOT here: it is a
// netcode concern (slice 1). The sim just applies whatever masks it's handed for
// this tick, which keeps the replay law clean and makes the flight-recorder stream
// literally the (mask0, mask1) sequence.
//
// Neighbour queries are brute-force O(n^2) here ON PURPOSE: this is the reference
// the deterministic spatial grid gets equivalence-tested against (issue #75). The
// grid replaces the inner loops later without changing a single result.
#include "Rps/Sim.h"

#include <cstring>

#include "Lur/Core/Assert.h"
#include "Lur/Sim/Random.h"
#include "Lur/Trace/Trace.h"  // LUR_TRACE_SCOPE — observational only (compiles out in Shipping)

namespace Rps {
namespace {

// Deterministic spawn ring around a camp (radius ~2). SpawnCounter % RingSlots
// picks the offset — no RNG, identical on both peers.
constexpr Fixed RingX[RingSlots] = {F(2), F(0), F(-2), F(0), F(1), F(-1), F(1), F(-1)};
constexpr Fixed RingY[RingSlots] = {F(0), F(2), F(0), F(-2), F(1), F(1), F(-1), F(-1)};

using Lur::Sim::Abs;
using Lur::Sim::Max;

// --- small integer-exact geometry helpers (int64 so squared distances never overflow) ---
int64_t Dist2(Fixed ax, Fixed ay, Fixed bx, Fixed by) {
    const int64_t dx = static_cast<int64_t>(ax.Raw) - bx.Raw;
    const int64_t dy = static_cast<int64_t>(ay.Raw) - by.Raw;
    return dx * dx + dy * dy;
}
int64_t RangeSq(Fixed R) { return static_cast<int64_t>(R.Raw) * R.Raw; }

Fixed ClampAxis(Fixed V, Fixed Hi) {
    if (V.Raw < 0) return Fixed{0};
    return V > Hi ? Hi : V;
}

// ---- Deterministic uniform spatial grid (design §5) ------------------------
// Counting-sort rebuild each tick into fixed arrays in slot order: zero
// allocation, fixed bin-iteration order, ascending-id within a cell (the tie-break).
// It buckets units by their START-OF-TICK position (Pos == Prev at build time, so it
// serves both the nearest-enemy query on Pos and the separation query on Prev), and
// is pure TRANSIENT scratch — never in Sim state or the hash. Cell size is a perf
// knob only: any value gives bit-identical results to brute force.
constexpr int32_t GridCols = (WorldWidth.ToInt() + GridCellSize - 1) / GridCellSize;
constexpr int32_t GridRows = (MaxWorldHeight.ToInt() + GridCellSize - 1) / GridCellSize;
constexpr int32_t GridCells = GridCols * GridRows;
constexpr int64_t CellRaw = static_cast<int64_t>(GridCellSize) * Fixed::One;  // cell width in Q16.16 raw

int32_t CellX(Fixed X) {
    const int32_t C = X.ToInt() / GridCellSize;
    return C < 0 ? 0 : (C >= GridCols ? GridCols - 1 : C);
}
int32_t CellY(Fixed Y) {
    const int32_t C = Y.ToInt() / GridCellSize;
    return C < 0 ? 0 : (C >= GridRows ? GridRows - 1 : C);
}
constexpr int32_t Abs32(int32_t V) { return V < 0 ? -V : V; }

struct Grid {
    int32_t Start[GridCells + 1];  // CSR: cell c's units are Order[Start[c] .. Start[c+1])
    int32_t Order[MaxUnits];

    void Build(const Sim& S) {
        for (int32_t C = 0; C <= GridCells; ++C) Start[C] = 0;
        // Count into Start[cell+1], then prefix-sum -> Start[cell] = bucket offset.
        for (int32_t I = 0; I < S.Count; ++I)
            if (S.IsAlive(I)) ++Start[CellY(S.PosY[I]) * GridCols + CellX(S.PosX[I]) + 1];
        for (int32_t C = 1; C <= GridCells; ++C) Start[C] += Start[C - 1];
        // Scatter in ascending slot order so ids stay ascending within each cell.
        int32_t Cursor[GridCells];
        for (int32_t C = 0; C < GridCells; ++C) Cursor[C] = Start[C];
        for (int32_t I = 0; I < S.Count; ++I)
            if (S.IsAlive(I)) {
                const int32_t C = CellY(S.PosY[I]) * GridCols + CellX(S.PosX[I]);
                Order[Cursor[C]++] = I;
            }
    }
};

// --- slot allocation: lowest free slot (deterministic). Reuse != compaction —
//     live units never move, ids stay stable for a unit's whole life. ---
int32_t AllocSlot(const Sim& S) {
    for (int32_t I = 0; I < MaxUnits; ++I)
        if (!S.IsAlive(I)) return I;
    return -1;
}
void SetAlive(Sim& S, int32_t I) { S.AliveBits[I >> 6] |= (1ull << (I & 63)); }
void ClearAlive(Sim& S, int32_t I) { S.AliveBits[I >> 6] &= ~(1ull << (I & 63)); }

void SpawnUnit(Sim& S, uint8_t Team, uint8_t Type) {
    const int32_t I = AllocSlot(S);
    LUR_ASSERT_MSG(I >= 0, "RPS: unit slot exhausted (MaxUnits) — raise the cap");
    if (I < 0) return;

    const int32_t Slot = S.Teams[Team].SpawnCounter % RingSlots;
    ++S.Teams[Team].SpawnCounter;

    S.PosX[I] = CampX + RingX[Slot];
    S.PosY[I] = Sim::CampY(Team) + RingY[Slot];
    S.PrevX[I] = S.PosX[I];
    S.PrevY[I] = S.PosY[I];
    S.Hp[I] = S.Units[Type].MaxHp;
    S.Type[I] = Type;
    S.Team[I] = Team;
    S.Target[I] = -1;
    S.Cooldown[I] = 0;
    S.WorkerState[I] = WorkToMine;
    S.Carry[I] = 0;
    S.WorkerTimer[I] = 0;
    S.Kind[I] = KindUnit;         // #131: reset — this slot may be a recycled dead-building slot
    S.Queue[I] = 0;
    S.BuildProgress[I] = 0;
    SetAlive(S, I);
    if (I + 1 > S.Count) S.Count = I + 1;
}

// --- map: v1 is fixed + mirrored; the seed is derived and stored so later
//     variation is free, exactly like chess derives colours from GUIDs. ---
void BuildMap(Sim& S) {
    // CLUSTERED layout (#108): MinesPerCluster mines spread across the 34-wide field, in
    // ClustersPerTeam rows per team — home/safe/midfield/contested, a risk gradient toward
    // mid (closer to centre = shorter enemy walk = higher risk, spec §2). Rows derive from
    // WorldHeight so they scale with the balance knob. Symmetric top/bottom by construction.
    // Sparse + rich (x20 MineGoldCapacity) instead of the dense grid that crawled the sim.
    const Fixed Xs[MinesPerCluster] = {F(4), F(9), F(14), F(20), F(25), F(30)};
    const int32_t Hi = WorldHeight.ToInt();
    const int32_t Mid = Hi / 2;
    const Fixed ClusterY[ClustersPerTeam * 2] = {
        F(CampInset + 2),        // t0 home      (right at the bottom camp — fast early gold)
        F(CampInset + 6),        // t0 safe      (near the bottom camp)
        F(Hi / 4),               // t0 midfield
        F(Mid - 8),              // t0 contested (toward mid)
        F(Hi - CampInset - 2),   // t1 home      (right at the top camp)
        F(Hi - CampInset - 6),   // t1 safe      (near the top camp)
        F(Hi - Hi / 4),          // t1 midfield
        F(Mid + 8),              // t1 contested (toward mid)
    };
    int32_t Idx = 0;
    for (int G = 0; G < ClustersPerTeam * 2; ++G)
        for (int K = 0; K < MinesPerCluster; ++K) {
            S.MineX[Idx] = Xs[K];
            S.MineY[Idx] = ClusterY[G];
            S.MineGold[Idx] = MineGoldCapacity;  // finite reserve (#84)
            ++Idx;
        }
}

// ---- Phase 0: apply this tick's inputs (caller passes P0 then P1) ----
void TryEnqueue(Sim& S, uint8_t Team, uint8_t Type) {
    TeamState& T = S.Teams[Team];
    const int32_t Cost = S.Units[Type].Cost;
    // Queue at cap or too poor -> the press is DETERMINISTICALLY ignored (a silent
    // no-op is correct here, distinct from an assert-worthy error). No partial
    // reservation: gold is spent only on a successful enqueue.
    if (T.QueueCount[Type] >= PerTypeQueueCap) return;
    if (T.Gold < Cost) return;
    T.Gold -= Cost;
    ++T.QueueCount[Type];
}
void ApplyInput(Sim& S, uint8_t Team, uint8_t Mask) {
    // Bit ty of the mask = a press of unit type ty this tick. Same-type presses
    // collapse to one bit (a built-in one-per-button-per-tick rate limit).
    for (uint8_t Ty = 0; Ty < UnitCount; ++Ty)
        if (Mask & (1u << Ty)) TryEnqueue(S, Team, Ty);
}

// ---- Phase 1: production — four parallel per-type queues with stack
// acceleration (#84). Progress is in integer "queue-ticks": a queue with N units
// stacked advances N per tick, so effective build time = BuildTicks / N and deep
// stacks snowball (the pacing thesis). A very deep stack can complete more than
// one unit in a tick — the while loop spawns them all, in type order (deterministic:
// teams 0 then 1, types 0..3, spawn ring ordered by SpawnCounter). ----
void Production(Sim& S) {
    for (uint8_t T = 0; T < 2; ++T) {
        TeamState& Q = S.Teams[T];
        for (uint8_t Ty = 0; Ty < UnitCount; ++Ty) {
            if (Q.QueueCount[Ty] == 0) { Q.BuildProgress[Ty] = 0; continue; }
            Q.BuildProgress[Ty] += Q.QueueCount[Ty] * S.Cv.QueueMult;
            while (Q.QueueCount[Ty] > 0 && Q.BuildProgress[Ty] >= S.Units[Ty].BuildTicks) {
                Q.BuildProgress[Ty] -= S.Units[Ty].BuildTicks;
                --Q.QueueCount[Ty];
                SpawnUnit(S, T, Ty);
            }
            if (Q.QueueCount[Ty] == 0) Q.BuildProgress[Ty] = 0;  // no banked progress on an empty queue
        }
    }
}

// ---- Phase 2: target acquisition - re-scored EVERY tick (playtest 2026-07-19:
// keep-until-death hysteresis made units run PAST enemies). The score is the
// lexicographic tuple (distance band, TYPE PREFERENCE, exact distance, id): closeness
// dominates in TargetBand-wide Chebyshev bands, but the band is WIDE (playtest
// 2026-07-20), so within an engagement the type-preference ladder decides who to hit:
//   0 = PREY   (the type I beat, 3x damage)      — hunt first
//   1 = mirror (same type)                       — then the even fight
//   2 = neutral (enemy cart, no counter either way)
//   3 = PREDATOR (the type that beats me)        — last resort (the flee force also
//                                                  keeps me away from it spatially)
// exact distance then lowest id break the remaining ties. Deterministic on both peers,
// and identical between the brute and grid paths (rps_sim_tests proves it). Band stays
// PRIMARY so the grid's distance-ring early-exit stays valid — a wider band just scans a
// little more before exiting.
int32_t TargetPrefer(uint8_t Mine, uint8_t Theirs) {
    // Prey and enemy CARTS share the top priority (playtest 2026-07-20): hunt the type you
    // beat AND deny the economy equally, both above an even mirror; the predator is last.
    if (Theirs == UnitMiner || UnitTable[Mine].Beats == Theirs) return 0;  // prey or enemy cart
    if (Mine == Theirs) return 1;                                          // mirror
    return 2;                                                              // predator — last resort
}
struct TargetScore {
    int64_t Band;
    int32_t Prefer;   // 0=prey-or-enemy-cart, 1=mirror, 2=predator (lower = pick first)
    int64_t Dist;     // Chebyshev, raw Q16.16
    int32_t Id;
    bool BetterThan(const TargetScore& O) const {
        if (Band != O.Band) return Band < O.Band;
        if (Prefer != O.Prefer) return Prefer < O.Prefer;
        if (Dist != O.Dist) return Dist < O.Dist;
        return Id < O.Id;
    }
};
constexpr int64_t TargetBandRaw = TargetBand.Raw;
int64_t ChebRaw(const Sim& S, int32_t I, int32_t J) {
    int64_t Dx = static_cast<int64_t>(S.PosX[I].Raw) - S.PosX[J].Raw;
    int64_t Dy = static_cast<int64_t>(S.PosY[I].Raw) - S.PosY[J].Raw;
    if (Dx < 0) Dx = -Dx;
    if (Dy < 0) Dy = -Dy;
    return Dx > Dy ? Dx : Dy;
}

// ---- ThreatBits (guard-lite, #98): a per-tick TRANSIENT bit-set (not sim state, not
// hashed) — one bit per unit, set iff that unit is an enemy SOLDIER within GuardAlertR of
// a miner on the OPPOSITE team (i.e. it is raiding that team's economy). It drives the
// INTERPOSE steering (not targeting): a defender near a flagged raider AND a friendly cart
// moves to the point BETWEEN them, screening the cart — even from a predator it wouldn't
// attack. Setting a bit is idempotent, so the set is order-independent — brute and grid
// produce the identical set (rps_sim_tests' equivalence run proves it). ----
struct ThreatSet {
    uint64_t Bits[(MaxUnits + 63) / 64];
    void Clear() { for (uint64_t& B : Bits) B = 0; }
    void Set(int32_t I) { Bits[I >> 6] |= (1ull << (I & 63)); }
    bool Get(int32_t I) const { return (Bits[I >> 6] >> (I & 63)) & 1ull; }
};
constexpr int64_t GuardAlertRaw = GuardAlertR.Raw;
// One (miner M, unit J) pair: flag J if it's an ENEMY soldier within GuardAlertR of M.
void AddThreat(const Sim& S, int32_t M, int32_t J, ThreatSet& T) {
    if (S.Type[J] == UnitMiner || S.Team[J] == S.Team[M]) return;  // only enemy warriors raid
    if (ChebRaw(S, M, J) <= GuardAlertRaw) T.Set(J);
}
void BuildThreatBrute(const Sim& S, ThreatSet& T) {
    T.Clear();
    for (int32_t M = 0; M < S.Count; ++M) {
        if (!S.IsAlive(M) || S.Type[M] != UnitMiner) continue;
        for (int32_t J = 0; J < S.Count; ++J)
            if (S.IsAlive(J)) AddThreat(S, M, J, T);
    }
}
// Grid twin: per miner, walk only the cells its GuardAlertR box overlaps; AddThreat
// re-tests the radius, so the flagged SET is identical to brute regardless of order.
void BuildThreatGrid(const Sim& S, const Grid& G, ThreatSet& T) {
    T.Clear();
    for (int32_t M = 0; M < S.Count; ++M) {
        if (!S.IsAlive(M) || S.Type[M] != UnitMiner) continue;
        const int32_t Cx0 = CellX(S.PosX[M] - GuardAlertR), Cx1 = CellX(S.PosX[M] + GuardAlertR);
        const int32_t Cy0 = CellY(S.PosY[M] - GuardAlertR), Cy1 = CellY(S.PosY[M] + GuardAlertR);
        for (int32_t Gy = Cy0; Gy <= Cy1; ++Gy)
            for (int32_t Gx = Cx0; Gx <= Cx1; ++Gx) {
                const int32_t C = Gy * GridCols + Gx;
                for (int32_t P = G.Start[C]; P < G.Start[C + 1]; ++P)
                    AddThreat(S, M, G.Order[P], T);
            }
    }
}

TargetScore ScoreOf(const Sim& S, int32_t I, int32_t J) {
    const int64_t D = ChebRaw(S, I, J);
    return {D / TargetBandRaw, TargetPrefer(S.Type[I], S.Type[J]), D, J};
}
int32_t NearestEnemyBrute(const Sim& S, int32_t I) {
    int32_t Best = -1;
    TargetScore BS{};
    // Same Chebyshev cell-box cutoff the capped grid search uses (#92), so the two
    // paths consider the IDENTICAL set of enemies and stay bit-for-bit equivalent.
    const int32_t Cx = CellX(S.PosX[I]), Cy = CellY(S.PosY[I]);
    for (int32_t J = 0; J < S.Count; ++J) {
        if (!S.IsAlive(J) || S.Team[J] == S.Team[I]) continue;
        if (Abs32(CellX(S.PosX[J]) - Cx) > TargetSearchMaxK ||
            Abs32(CellY(S.PosY[J]) - Cy) > TargetSearchMaxK) continue;
        const TargetScore Sc = ScoreOf(S, I, J);
        if (Best < 0 || Sc.BetterThan(BS)) { BS = Sc; Best = J; }
    }
    return Best;
}
// Grid nearest-enemy: expanding Chebyshev ring search. Must reproduce the brute
// result EXACTLY - same TargetScore comparator, and the early exit reasons in BANDS:
// any unit in an unscanned ring K+1 sits at Chebyshev >= K*cellSize, so once that
// bound's band is STRICTLY worse than the best band, nothing farther can win (an equal
// band could still flip on counter-preference, so equality keeps scanning).
int32_t NearestEnemyGrid(const Sim& S, const Grid& G, int32_t I) {
    const int32_t Cx = CellX(S.PosX[I]), Cy = CellY(S.PosY[I]);
    TargetScore BS{};
    int32_t BestId = -1;
    // Cap the ring expansion (#92): stop at TargetSearchMaxK cells even if no enemy was
    // found, so two far-apart armies don't each scan the empty cells between them. The
    // brute path applies the same cell-box cutoff, so grid == brute still holds.
    const int32_t MaxK = TargetSearchMaxK;
    for (int32_t K = 0; K <= MaxK; ++K) {
        bool AnyInGrid = false;
        const int32_t X0 = Cx - K, X1 = Cx + K, Y0 = Cy - K, Y1 = Cy + K;
        for (int32_t Gy = Y0; Gy <= Y1; ++Gy) {
            if (Gy < 0 || Gy >= GridRows) continue;
            const bool EdgeRow = (Gy == Y0 || Gy == Y1);
            for (int32_t Gx = X0; Gx <= X1; ++Gx) {
                if (Gx < 0 || Gx >= GridCols) continue;
                if (!EdgeRow && Gx != X0 && Gx != X1) continue;  // ring K = box perimeter; interior done in earlier K
                AnyInGrid = true;
                const int32_t C = Gy * GridCols + Gx;
                for (int32_t P = G.Start[C]; P < G.Start[C + 1]; ++P) {
                    const int32_t J = G.Order[P];
                    if (S.Team[J] == S.Team[I]) continue;  // J is alive by construction (grid holds only alive)
                    const TargetScore Sc = ScoreOf(S, I, J);
                    if (BestId < 0 || Sc.BetterThan(BS)) { BS = Sc; BestId = J; }
                }
            }
        }
        if (BestId >= 0) {
            const int64_t MinNext = static_cast<int64_t>(K) * CellRaw;  // ring K+1 lower bound
            if (MinNext / TargetBandRaw > BS.Band) break;
        }
        if (K > 0 && !AnyInGrid) break;  // the whole ring is outside the grid - nothing farther can be inside
    }
    return BestId;
}
int32_t NearestFreeMine(const Sim& S, int32_t I, const int32_t* Occ) {
    // Always pick the NEAREST mine (playtest 2026-07-20: carts were hauling past nearby
    // mines). Prefer one under the digger cap; but if every gold-bearing mine is crowded,
    // fall back to the nearest gold-bearing mine anyway — a cart NEVER idles while gold
    // exists somewhere, so it keeps moving instead of standing still. Occupancy is the
    // precomputed per-mine count (O(1) here) — with the dense field, a per-mine unit scan
    // would be O(mines×units) per acquisition.
    int32_t BestFree = -1; int64_t BestFreeD = INT64_MAX;
    int32_t BestAny = -1;  int64_t BestAnyD = INT64_MAX;
    for (int32_t Tr = 0; Tr < NumMines; ++Tr) {
        if (S.MineGold[Tr] <= 0) continue;  // depleted mines are gone (#84)
        const int64_t D = Dist2(S.PosX[I], S.PosY[I], S.MineX[Tr], S.MineY[Tr]);
        if (D < BestAnyD) { BestAnyD = D; BestAny = Tr; }
        if (Occ[Tr] < WorkersPerMine && D < BestFreeD) { BestFreeD = D; BestFree = Tr; }
    }
    return BestFree >= 0 ? BestFree : BestAny;
}
void TargetAcquire(Sim& S, const Grid& G) {
    // Mine occupancy computed ONCE (O(units)) then read O(1)/mine — with the dense field a
    // per-mine unit scan per cart would be O(mines×units). Incremented as carts claim mines
    // in slot order, preserving the deterministic first-come assignment.
    int32_t Occ[NumMines] = {};  // ~2 KB stack scratch — NOT static (two Sims may step on separate threads)
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J) && S.Type[J] == UnitMiner && S.Target[J] >= 0) ++Occ[S.Target[J]];
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        if (S.Type[I] == UnitMiner) {
            if (S.Target[I] < 0) {
                const int32_t M = NearestFreeMine(S, I, Occ);  // nearest gold, prefer uncrowded
                S.Target[I] = M;
                if (M >= 0) ++Occ[M];  // claim it so later carts this tick see the higher count
            }
        } else {
            // Re-scored EVERY tick (playtest): banded closeness + type preference, so units
            // engage what they are passing instead of chasing a first pick.
            S.Target[I] = S.UseBruteForce ? NearestEnemyBrute(S, I) : NearestEnemyGrid(S, G, I);
        }
    }
}

// ---- Phase 3: movement + flocking (boids slice A, #96) ----
// Miners keep their state machine (WorkerSeek) with a separation + mine-repel nudge;
// SOLDIERS flock. The structure is one neighbour GATHER pass per unit — the old
// separation walk widened to the largest flock radius (CohAllR) — accumulating every
// neighbour force (friendly + enemy separation, two-tier cohesion) from start-of-tick
// Prev positions, so the sums are ORDER-INDEPENDENT and identical on the brute and grid
// paths (rps_sim_tests proves it). A separate scalar FINALIZE step blends the sums into a
// desired vector, Chebyshev-clamps it to Speed, and integrates. No float, no sqrt, no
// alloc; every falloff/normalize is Chebyshev so Fixed::Sqrt stays unbuilt.
//
// Chebyshev seek (spec §5): step = speed * (dx,dy) / max(|dx|,|dy|). Pure Fixed
// mul/div; an EXPLICIT zero-distance guard before the divide (never relying on
// Fixed::operator/'s silent saturate).
void MoveToward(Sim& S, int32_t I, Fixed Tx, Fixed Ty) {
    const Fixed Dx = Tx - S.PosX[I];
    const Fixed Dy = Ty - S.PosY[I];
    const Fixed M = Max(Abs(Dx), Abs(Dy));
    if (M.Raw == 0) return;  // already there (or overlapping) — zero-distance guard
    const Fixed Sp = S.Units[S.Type[I]].Speed;
    if (M <= Sp) { S.PosX[I] = Tx; S.PosY[I] = Ty; return; }  // arrive, don't overshoot
    S.PosX[I] = S.PosX[I] + Sp * Dx / M;
    S.PosY[I] = S.PosY[I] + Sp * Dy / M;
}
bool Arrived(const Sim& S, int32_t I, Fixed Tx, Fixed Ty) {
    const Fixed M = Max(Abs(Tx - S.PosX[I]), Abs(Ty - S.PosY[I]));
    return M <= S.Units[S.Type[I]].Speed;
}
void WorkerSeek(Sim& S, int32_t I) {
    switch (S.WorkerState[I]) {
        case WorkDig: {
            const int32_t Mn = S.Target[I];
            // The mine can empty under us (an earlier-slot digger took the last carry
            // this same tick order) — abandon the dig and re-target next tick.
            if (Mn < 0 || S.MineGold[Mn] <= 0) {
                S.Target[I] = -1; S.WorkerState[I] = WorkToMine;
                return;
            }
            if (S.WorkerTimer[I] > 0) --S.WorkerTimer[I];
            if (S.WorkerTimer[I] <= 0) {
                // Finite reserve (#84): the carry comes OUT of the mine; the last
                // trip takes whatever is left. Slot order makes ties deterministic.
                const int32_t Take = S.MineGold[Mn] < CarryCapacity ? S.MineGold[Mn] : CarryCapacity;
                S.MineGold[Mn] -= Take;
                S.Carry[I] = Take;
                S.WorkerState[I] = WorkToCamp;
            }
            return;  // stationary while digging
        }
        case WorkToMine: {
            const int32_t Tr = S.Target[I];
            if (Tr < 0) return;  // no free mine this tick — idle
            if (S.MineGold[Tr] <= 0) { S.Target[I] = -1; return; }  // it emptied en route — re-target
            const Fixed Tx = S.MineX[Tr], Ty = S.MineY[Tr];
            // Dig from range (playtest): stop WHERE THE CART STANDS once close enough —
            // no snap onto the deposit; with the mine repulsion the carts ring it.
            if (Max(Abs(Tx - S.PosX[I]), Abs(Ty - S.PosY[I])) <= MineDigRange) {
                S.WorkerState[I] = WorkDig;
                S.WorkerTimer[I] = S.Cv.DigTicks;
                return;
            }
            MoveToward(S, I, Tx, Ty);
            return;
        }
        case WorkToCamp: {
            const Fixed Tx = CampX, Ty = Sim::CampY(S.Team[I]);
            if (Arrived(S, I, Tx, Ty)) {
                S.PosX[I] = Tx; S.PosY[I] = Ty;
                S.DepositBuf[S.Team[I]] += S.Carry[I];  // credited in Economy (phase 6)
                S.Carry[I] = 0; S.Target[I] = -1; S.WorkerState[I] = WorkToMine;
                return;
            }
            MoveToward(S, I, Tx, Ty);
            return;
        }
        default: return;
    }
}
// Per-unit neighbour-force accumulator (Q16.16 raw; int64 so a dense stress pile can
// never overflow the running sum). Separation/enemy are Σ(dir_cheb·falloff·strength);
// each cohesion tier is Σ(neighbour−self) offsets + a count — the centroid (Σoffset/N)
// is formed with ONE Fixed divide per unit in the finalize loop, keeping the gather a
// plain associative sum (order-independent, auto-vectorizable per design-doc §5).
struct FlockAcc {
    int64_t SepX = 0, SepY = 0;                 // friendly separation
    int64_t EneX = 0, EneY = 0;                 // enemy separation
    int64_t SameX = 0, SameY = 0; int32_t SameN = 0;  // same-type cohesion sum + count
    int64_t AllX = 0, AllY = 0;   int32_t AllN = 0;   // army (any-warrior) cohesion
    int64_t AlnX = 0, AlnY = 0;   int32_t AlnN = 0;   // same-type alignment: Σ neighbour velocity Δ (#97)
    int64_t FleeX = 0, FleeY = 0;                     // flee from PREDATORS (enemy type that beats me)
    int64_t CartX = 0, CartY = 0; int32_t CartN = 0;  // friendly carts within InterposeR (#98 interpose)
    int64_t RaidX = 0, RaidY = 0; int32_t RaidN = 0;  // flagged raiders within InterposeR (#98 interpose)
};

// CORRECTED separation falloff (classic boids; the old code had it inverted): strongest
// at contact, zero at R — dir_cheb × (R − cheb)/R × strength. Chebyshev-normalized
// direction (one axis is ±1), no sqrt. Reads only the passed Prev positions, so the sum
// is order-independent. Shared verbatim by unit separation AND mine repel.
void AddRepel(Fixed Ix, Fixed Iy, Fixed Jx, Fixed Jy, Fixed R, Fixed Strength,
              int64_t& Ax, int64_t& Ay) {
    const Fixed Dx = Ix - Jx, Dy = Iy - Jy;          // away from J
    const Fixed Cheb = Max(Abs(Dx), Abs(Dy));
    if (Cheb.Raw == 0 || Cheb >= R) return;          // exact overlap (no dir) or out of range
    const Fixed Scale = (R - Cheb) / R * Strength;   // (R−cheb)/R · strength
    Ax += (Dx / Cheb * Scale).Raw;                   // dir_cheb · scale
    Ay += (Dy / Cheb * Scale).Raw;
}
// Cohesion gather: accumulate the offset TOWARD each in-range neighbour + a count.
void AddCohesion(Fixed Ix, Fixed Iy, Fixed Jx, Fixed Jy, Fixed R,
                 int64_t& Ax, int64_t& Ay, int32_t& N) {
    const Fixed Dx = Jx - Ix, Dy = Jy - Iy;          // toward J
    if (Max(Abs(Dx), Abs(Dy)) >= R) return;          // out of (Chebyshev) range
    Ax += Dx.Raw; Ay += Dy.Raw; ++N;
}
// Alignment gather (#97): sum the neighbour's velocity Δ = Pos − Prev if it's within
// AlignR (position range). Reads Δ — valid ONLY before the tick's bulk Prev=Pos copy.
void AddAlignment(const Sim& S, int32_t I, int32_t J, FlockAcc& A) {
    const Fixed Ox = S.PosX[I] - S.PosX[J], Oy = S.PosY[I] - S.PosY[J];
    if (Max(Abs(Ox), Abs(Oy)) >= S.Cv.AlignRadius) return;
    A.AlnX += static_cast<int64_t>(S.PosX[J].Raw) - S.PrevX[J].Raw;
    A.AlnY += static_cast<int64_t>(S.PosY[J].Raw) - S.PrevY[J].Raw;
    ++A.AlnN;
}
// One neighbour's WHOLE contribution — the single per-pair function both gather paths
// call, so brute and grid add bit-identical terms (house rule). Reads Pos for spatial
// offsets (slice B: Pos is end-of-last-tick during the gather; Prev is one tick older,
// so it carries velocity) and Δ for alignment.
void AccumFlock(const Sim& S, int32_t I, int32_t J, const ThreatSet& Threat, FlockAcc& A) {
    if (J == I) return;
    const Fixed Ix = S.PosX[I], Iy = S.PosY[I];
    const Fixed Jx = S.PosX[J], Jy = S.PosY[J];
    if (S.Team[J] == S.Team[I]) {
        AddRepel(Ix, Iy, Jx, Jy, S.Cv.SepRadius, S.Cv.SeparationStrength, A.SepX, A.SepY);
        if (S.Type[J] != UnitMiner) {  // cohesion/alignment are WARRIOR affinities (miners never blob)
            AddCohesion(Ix, Iy, Jx, Jy, S.Cv.CohAllRadius, A.AllX, A.AllY, A.AllN);
            if (S.Type[J] == S.Type[I]) {
                AddCohesion(Ix, Iy, Jx, Jy, S.Cv.CohSameRadius, A.SameX, A.SameY, A.SameN);
                AddAlignment(S, I, J, A);
            }
        } else if (Max(Abs(Ix - Jx), Abs(Iy - Jy)) < S.Cv.InterposeRadius) {
            A.CartX += Jx.Raw; A.CartY += Jy.Raw; ++A.CartN;  // a friendly cart to screen (#98)
        }
    } else {
        AddRepel(Ix, Iy, Jx, Jy, S.Cv.EnemySepRadius, S.Cv.EnemySeparationStrength, A.EneX, A.EneY);
        // Flee your PREDATOR — the enemy type that beats me (UnitTable[Type[J]].Beats == my
        // type): steer away, larger radius, so I never walk toward my counter.
        if (UnitTable[S.Type[J]].Beats == S.Type[I])
            AddRepel(Ix, Iy, Jx, Jy, S.Cv.PredatorFleeRadius, S.Cv.WPredatorFlee, A.FleeX, A.FleeY);
        // A flagged RAIDER within InterposeR: note it so I can interpose (#98).
        if (Threat.Get(J) && Max(Abs(Ix - Jx), Abs(Iy - Jy)) < S.Cv.InterposeRadius) {
            A.RaidX += Jx.Raw; A.RaidY += Jy.Raw; ++A.RaidN;
        }
    }
}
void GatherBrute(const Sim& S, int32_t I, const ThreatSet& Threat, FlockAcc& A) {
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J)) AccumFlock(S, I, J, Threat, A);
}
// Grid gather widened to the LARGEST flock radius (FlockGatherR = max of all) so a single
// query feeds every force; each Add re-tests its own (smaller) radius, so the summed SET —
// and thus the sum — is identical to brute. Queried by Pos (== each unit's bucketed
// build-time Pos — nothing has moved yet in the gather pass, so this stays consistent).
void GatherGrid(const Sim& S, const Grid& G, int32_t I, const ThreatSet& Threat, FlockAcc& A) {
    const int32_t Cx0 = CellX(S.PosX[I] - S.GatherR), Cx1 = CellX(S.PosX[I] + S.GatherR);
    const int32_t Cy0 = CellY(S.PosY[I] - S.GatherR), Cy1 = CellY(S.PosY[I] + S.GatherR);
    for (int32_t Gy = Cy0; Gy <= Cy1; ++Gy)
        for (int32_t Gx = Cx0; Gx <= Cx1; ++Gx) {
            const int32_t C = Gy * GridCols + Gx;
            for (int32_t P = G.Start[C]; P < G.Start[C + 1]; ++P)
                AccumFlock(S, I, G.Order[P], Threat, A);
        }
}
// Live deposits are SOFT OBSTACLES (playtest): units within MineRepelRadius get pushed
// outward with the same corrected falloff as unit separation. Reads Pos against static
// mine positions — identical on the brute and grid paths.
void AddMineRepel(const Sim& S, int32_t I, int64_t& Ax, int64_t& Ay) {
    for (int32_t Mn = 0; Mn < NumMines; ++Mn) {
        if (S.MineGold[Mn] <= 0) continue;
        AddRepel(S.PosX[I], S.PosY[I], S.MineX[Mn], S.MineY[Mn],
                 MineRepelRadius, S.Cv.SeparationStrength, Ax, Ay);
    }
}
// Chebyshev-clamp a raw (Q16.16) vector in place to a max magnitude — the sqrt-free
// "don't exceed" used for both the per-tick accel clamp and the final speed clamp.
void ChebClamp(int64_t& X, int64_t& Y, int64_t LimitRaw) {
    const int64_t Ax = X < 0 ? -X : X, Ay = Y < 0 ? -Y : Y;
    const int64_t M = Ax > Ay ? Ax : Ay;
    if (M > LimitRaw) { X = X * LimitRaw / M; Y = Y * LimitRaw / M; }
}
// --- Deterministic fixed-point value noise: the float/sqrt-free analog of Simplex/
// OpenSimplex (playtest 2026-07-20). Smooth per-unit wander — hash lattice points to
// pseudo-gradients in [-1,1), smoothstep-interpolate along the tick axis. A PURE integer
// function of (unit, lattice, axis): both peers compute the identical offset, and because
// it's per-unit (no neighbour reads) it leaves grid≡brute untouched. ---
uint32_t NoiseHash(uint32_t Unit, int32_t Lattice, uint32_t Axis) {
    uint64_t H = static_cast<uint64_t>(Unit) * 0x9E3779B97F4A7C15ull;
    H ^= (static_cast<uint64_t>(static_cast<uint32_t>(Lattice)) + 1) * 0xBF58476D1CE4E5B9ull;
    H ^= (static_cast<uint64_t>(Axis) + 1) * 0x94D049BB133111EBull;
    H ^= H >> 30; H *= 0xBF58476D1CE4E5B9ull; H ^= H >> 27; H *= 0x94D049BB133111EBull; H ^= H >> 31;
    return static_cast<uint32_t>(H);
}
Fixed NoiseGrad(uint32_t Unit, int32_t Lattice, uint32_t Axis) {
    return Fixed{static_cast<int32_t>(NoiseHash(Unit, Lattice, Axis) & 0x1FFFF) - Fixed::One};  // [-1,1)
}
Fixed ValueNoise(uint32_t Unit, Fixed T, uint32_t Axis) {
    const int32_t I = T.ToInt();                              // T >= 0 -> floor
    const Fixed F0 = T - Fixed::FromInt(I);                   // frac in [0,1)
    const Fixed G0 = NoiseGrad(Unit, I, Axis);
    const Fixed G1 = NoiseGrad(Unit, I + 1, Axis);
    const Fixed U = F0 * F0 * (Fixed::FromInt(3) - (F0 + F0)); // smoothstep 3f²−2f³
    return G0 + (G1 - G0) * U;                                // lerp
}
// Fractal (fBm) noise (#123): sum Octaves of ValueNoise, each octave at Lacunarity× the
// frequency and Gain× the amplitude, then normalize back to ~[-1,1). Octaves decorrelate via
// a per-octave hash offset on Unit. Octaves==1 returns EXACTLY ValueNoise(Unit,T,Axis) (Amp and
// Freq start at One, Norm==One), so the default is bit-identical — determinism preserved.
Fixed FbmNoise(uint32_t Unit, Fixed T, uint32_t Axis, int32_t Octaves, Fixed Gain, Fixed Lac) {
    if (Octaves < 1) Octaves = 1;
    Fixed Sum{0}, Amp = Fixed::FromInt(1), Freq = Fixed::FromInt(1), Norm{0};
    for (int32_t O = 0; O < Octaves; ++O) {
        Sum = Sum + Amp * ValueNoise(Unit + static_cast<uint32_t>(O) * 0x68E31DA4u, T * Freq, Axis);
        Norm = Norm + Amp;
        Amp = Amp * Gain;
        Freq = Freq * Lac;
    }
    return Sum / Norm;
}
// Phase 3 (boids slice B, #97). TWO passes with the bulk Prev=Pos copy BETWEEN them:
//   pass 1 GATHERs every unit's forces from end-of-last-tick Pos and velocity Δ=Pos−Prev
//          (both still intact — no Pos has moved, Prev not yet overwritten) and computes
//          a step per unit into StepX/StepY (transient stack scratch, never hashed);
//   then   Prev = Pos (so Prev now = end-of-last-tick, the interpolation source + next
//          tick's Δ base);
//   pass 2 APPLIES the steps (miners run their state machine here — direct movement —
//          then take the nudge). Splitting the passes is what lets the gather read a
//          stable Pos snapshot (grid≡brute holds) while Δ still carries momentum.
void Movement(Sim& S, const Grid& G, const ThreatSet& Threat) {
    // Transient per-unit step scratch (32 KB stack; never hashed, never heap). A stack
    // local — NOT static — so two Sims stepping on different threads (future rollback)
    // can't race. Written for every alive unit in pass 1, read for the same set in pass 2.
    Fixed StepX[MaxUnits], StepY[MaxUnits];
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        FlockAcc A;
        if (S.UseBruteForce) GatherBrute(S, I, Threat, A);  // reads Pos + Δ — nothing has moved yet
        else GatherGrid(S, G, I, Threat, A);

        if (S.Type[I] == UnitMiner) {
            // Miners are DIRECT (no momentum) and get NO separation (playtest 2026-07-20:
            // separation shoved carts off the tight camp deposit point, so they could never
            // bank). Only the mine-repel ring (deposits are soft obstacles) applies here;
            // WorkerSeek runs in pass 2 (it needs the unmoved start-of-tick Pos).
            int64_t Nx = 0, Ny = 0;
            AddMineRepel(S, I, Nx, Ny);
            StepX[I] = Fixed{static_cast<int32_t>(Nx)};
            StepY[I] = Fixed{static_cast<int32_t>(Ny)};
            continue;
        }

        // --- Soldier: blend the neighbour sums into a desired velocity ---
        // Seek goal + in-range test (targeting unchanged). No target -> march on the
        // enemy camp line (#92) so fronts close instead of idling.
        Fixed Tx, Ty;
        bool InRange = false;
        const int32_t T = S.Target[I];
        if (T < 0 || !S.IsAlive(T)) {
            Tx = CampX; Ty = Sim::CampY(S.Team[I] ^ 1);
        } else {
            Tx = S.PosX[T]; Ty = S.PosY[T];
            InRange = Dist2(S.PosX[I], S.PosY[I], Tx, Ty) <= RangeSq(UnitTable[S.Type[I]].Range);
        }

        // Repulsion is ALWAYS on (even in range: engaged lines spread into arcs, not
        // piles; and you always flee your predator). Seek + cohesion + alignment + wander
        // are zeroed in range — hold and fight. Soldiers do NOT mine-repel: mines are
        // economy nodes, not battlefield obstacles — armies march through the dense field.
        int64_t Dx = A.SepX + A.EneX + A.FleeX, Dy = A.SepY + A.EneY + A.FleeY;
        // Interpose (#98): with BOTH a friendly cart and a flagged raider nearby, steer to
        // the point BETWEEN their centroids — screening the cart, even from a predator. On
        // always (a defender blocks whether or not it also has an attack target).
        if (A.CartN > 0 && A.RaidN > 0) {
            const Fixed Mx{static_cast<int32_t>((A.CartX / A.CartN + A.RaidX / A.RaidN) / 2)};
            const Fixed My{static_cast<int32_t>((A.CartY / A.CartN + A.RaidY / A.RaidN) / 2)};
            const Fixed Ddx = Mx - S.PosX[I], Ddy = My - S.PosY[I];   // toward the block point
            const Fixed Ch = Max(Abs(Ddx), Abs(Ddy));
            if (Ch.Raw != 0) {
                Dx += (Ddx / Ch * S.Cv.WInterpose).Raw;
                Dy += (Ddy / Ch * S.Cv.WInterpose).Raw;
            }
        }
        if (!InRange) {
            const Fixed Sdx = Tx - S.PosX[I], Sdy = Ty - S.PosY[I];
            const Fixed Cheb = Max(Abs(Sdx), Abs(Sdy));
            if (Cheb.Raw != 0) {                       // seek: unit Chebyshev dir × WSeek
                Dx += (Sdx / Cheb * S.Cv.WSeek).Raw;
                Dy += (Sdy / Cheb * S.Cv.WSeek).Raw;
            }
            if (A.SameN > 0) {                         // toward same-type centroid
                Dx += (Fixed{static_cast<int32_t>(A.SameX / A.SameN)} * S.Cv.WCohSame).Raw;
                Dy += (Fixed{static_cast<int32_t>(A.SameY / A.SameN)} * S.Cv.WCohSame).Raw;
            }
            if (A.AllN > 0) {                          // toward the army centroid (weak)
                Dx += (Fixed{static_cast<int32_t>(A.AllX / A.AllN)} * S.Cv.WCohAll).Raw;
                Dy += (Fixed{static_cast<int32_t>(A.AllY / A.AllN)} * S.Cv.WCohAll).Raw;
            }
            if (A.AlnN > 0) {                          // match same-type neighbours' heading
                Dx += (Fixed{static_cast<int32_t>(A.AlnX / A.AlnN)} * S.Cv.WAlign).Raw;
                Dy += (Fixed{static_cast<int32_t>(A.AlnY / A.AlnN)} * S.Cv.WAlign).Raw;
            }
            // Organic wander: smooth per-unit value noise (Simplex-style). Tick masked to
            // 15 bits so Fixed::FromInt never overflows (loops ~55 min — cosmetic).
            const uint32_t Uu = static_cast<uint32_t>(I);
            const Fixed Tn = Fixed::FromInt(static_cast<int32_t>(S.Tick & 0x7FFF)) * S.Cv.NoiseTimeScale;
            Dx += (FbmNoise(Uu, Tn, 0, S.Cv.NoiseOctaves, S.Cv.NoiseGain, S.Cv.NoiseLacunarity) * S.Cv.WNoise).Raw;
            Dy += (FbmNoise(Uu, Tn, 1, S.Cv.NoiseOctaves, S.Cv.NoiseGain, S.Cv.NoiseLacunarity) * S.Cv.WNoise).Raw;
        }
        // Verlet finalize: Δ is last tick's velocity (Pos−Prev, still valid pre-copy).
        // Accelerate toward the desired velocity, clamped to MaxAccel; carry damped
        // momentum; clamp the whole step to Speed. Momentum smooths the retarget snaps;
        // Damp<1 (stronger when engaged) bleeds the Verlet dense-pack jitter.
        const int64_t DeltaX = static_cast<int64_t>(S.PosX[I].Raw) - S.PrevX[I].Raw;
        const int64_t DeltaY = static_cast<int64_t>(S.PosY[I].Raw) - S.PrevY[I].Raw;
        int64_t Ax = Dx - DeltaX, Ay = Dy - DeltaY;
        ChebClamp(Ax, Ay, S.Cv.MaxAccel.Raw);
        const int64_t Damp = (InRange ? S.Cv.InRangeDamping : S.Cv.FlockDamping).Raw;
        int64_t Stepx = (Damp * DeltaX >> Fixed::FracBits) + Ax;
        int64_t Stepy = (Damp * DeltaY >> Fixed::FracBits) + Ay;
        ChebClamp(Stepx, Stepy, S.Units[S.Type[I]].Speed.Raw);
        LUR_ASSERT_MSG((Stepx <= S.Units[S.Type[I]].Speed.Raw) &&
                       (-Stepx <= S.Units[S.Type[I]].Speed.Raw) &&
                       (Stepy <= S.Units[S.Type[I]].Speed.Raw) &&
                       (-Stepy <= S.Units[S.Type[I]].Speed.Raw),
                       "RPS: soldier step exceeds Speed after clamp (#97 invariant)");
        StepX[I] = Fixed{static_cast<int32_t>(Stepx)};
        StepY[I] = Fixed{static_cast<int32_t>(Stepy)};
    }

    // The bulk Prev=Pos copy, now AFTER the gather (slice B enabler): Δ has been read for
    // every unit, so Prev can advance to end-of-last-tick. This is the view interpolation
    // source and next tick's Δ base — end-of-Step Prev/Pos are unchanged vs before.
    for (int32_t I = 0; I < S.Count; ++I) { S.PrevX[I] = S.PosX[I]; S.PrevY[I] = S.PosY[I]; }

    // Pass 2: apply. Miners run their state machine (direct move) then take the nudge;
    // soldiers integrate the precomputed step. No unit reads another's Pos here, so the
    // apply order is irrelevant to the result (order-independent).
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        if (S.Type[I] == UnitMiner) WorkerSeek(S, I);
        S.PosX[I] = ClampAxis(S.PosX[I] + StepX[I], WorldWidth);
        S.PosY[I] = ClampAxis(S.PosY[I] + StepY[I], WorldHeight);
    }
}

// ---- Phase 4: attacks (damage buffered, applied SIMULTANEOUSLY) ----
void Attacks(Sim& S) {
    if (S.DisableCombat) return;  // LUR_INTERNAL --flockdemo (#97): watch flocking, no kills
    int32_t Dmg[MaxUnits];
    std::memset(Dmg, 0, sizeof(int32_t) * static_cast<size_t>(S.Count));
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || S.Type[I] == UnitMiner) continue;  // workers ignore combat in v1 (spec §5)
        if (S.Cooldown[I] > 0) --S.Cooldown[I];
        if (S.Cooldown[I] > 0) continue;  // still cooling
        const int32_t T = S.Target[I];
        if (T < 0 || !S.IsAlive(T)) continue;
        if (Dist2(S.PosX[I], S.PosY[I], S.PosX[T], S.PosY[T]) > RangeSq(UnitTable[S.Type[I]].Range)) continue;
        int32_t D = S.Units[S.Type[I]].Attack;
        if (UnitTable[S.Type[I]].Beats == S.Type[T]) D *= S.Cv.CounterMultiplier;
        Dmg[T] += D;
        S.Cooldown[I] = UnitTable[S.Type[I]].Cooldown;
    }
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I)) S.Hp[I] -= Dmg[I];  // simultaneous: mutual kills reachable (win rule needs it)
}

// ---- Phase 5: deaths (clear the alive bit; slot kept, no compaction) ----
void Deaths(Sim& S) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.Hp[I] <= 0) ClearAlive(S, I);
}

// ---- Phase 6: economy — buffered deposits credit gold ----
void Economy(Sim& S) {
    for (int T = 0; T < 2; ++T) { S.Teams[T].Gold += S.DepositBuf[T]; S.DepositBuf[T] = 0; }
}

// ---- Phase 7: win check (spec §6, edge-proof) ----
void WinCheck(Sim& S) {
    bool Lose[2];
    for (uint8_t T = 0; T < 2; ++T)
        Lose[T] = S.AliveCount(T) == 0 && S.QueuedTotal(T) == 0 && S.Teams[T].Gold < CheapestCost;
    if (Lose[0] && Lose[1]) S.Result = ResultDraw;  // both this tick -> draw (simultaneous damage makes it reachable)
    else if (Lose[0]) S.Result = ResultTeam1Wins;
    else if (Lose[1]) S.Result = ResultTeam0Wins;
}

}  // namespace

void Sim::DeriveUnits() {
    DeriveUnitStats(Cv, Units);  // #122: per-type stats from the latched Cv (shared with the HUD)
    // #123: the flock gather radius = max of every radius tested in the soldier gather, so the
    // grid neighbour box always covers brute's reach (grid==brute) whatever the radii are set to.
    GatherR = Max(Max(Max(Cv.SepRadius, Cv.EnemySepRadius), Max(Cv.CohSameRadius, Cv.CohAllRadius)),
                  Max(Max(Cv.AlignRadius, Cv.PredatorFleeRadius), Cv.InterposeRadius));
}

void Sim::Init(uint64_t InSeed) {
#if !LUR_SHIPPING
    Lur::Core::CVarEnterMain();  // Init always runs post-main; arm the no-read-before-main guard (dev-only)
#endif
    *this = Sim{};  // value-init: zeroes every array/field (Init-time, not the hot path)
    Seed = InSeed;
    // #112: latch the AffectsGameplay CVars into per-Sim state ONCE, at match start, from
    // the current (default, or solo-console) global values. Thereafter Cv is authoritative
    // Sim state — constant within a tick and mutated ONLY at tick boundaries by synced
    // overrides (LockstepPeer), so two peers in one process hold independent Cv. Hashed.
    Cv = LatchCvs();
    DeriveUnits();  // #122: fill Units[] from the just-latched Cv before anything spawns
    BuildMap(*this);
    for (uint8_t T = 0; T < 2; ++T) {
        Teams[T].Gold = StartGold;
        for (int K = 0; K < StartMiners; ++K) SpawnUnit(*this, T, UnitMiner);
    }
}

void Sim::Step(uint8_t Mask0, uint8_t Mask1) {
    if (Result != ResultOngoing) return;  // match decided: freeze (still deterministic on both peers)
    LUR_TRACE_SCOPE("sim.step");           // pure observer — never reads back into sim state
    // NB: Cv is NOT re-latched here — it is per-Sim state set at Init and mutated only at
    // tick boundaries by synced overrides (#112), so it stays constant across this tick.
    // Exception: solo/desktop live tuning (#115) opts into re-latching from the globals so
    // a `--tune` edit moves the running sim — no peer means no desync risk.
    if (LiveCvLatch) Cv = LatchCvs();
    DeriveUnits();  // #122: reflect this tick's Cv (Init latch, live-tune, or a synced override)

    // NOTE (slice B, #97): the bulk Prev=Pos copy is NO LONGER here. It moved INSIDE
    // Movement, after the gather, so the gather can read Δ=Pos−Prev (last tick's
    // velocity). Nothing between here and Movement reads Prev (grid + target acq read
    // Pos), so removing it from the top is safe.
    ApplyInput(*this, 0, Mask0);  // phase 0: P0 then P1
    ApplyInput(*this, 1, Mask1);
    Production(*this);            // phase 1 (spawns) — grid must see the new units

    // Build the spatial grid AFTER production (so spawns are bucketed) but before any
    // movement, capturing start-of-tick positions. Both target acq and the flock gather
    // read it on Pos (nothing has moved yet), so the buckets stay consistent.
    Grid G;
    { LUR_TRACE_SCOPE("sim.grid"); G.Build(*this); }

    // Guard-lite (#98): flag raiders (enemy soldiers near a team's miners); Movement's
    // interpose steering reads the bits. Transient — never in Sim state or the hash. Same
    // brute/grid split as the neighbour queries so the two paths stay equivalent.
    ThreatSet Threat;  // ~0.5 KB stack scratch — NOT static (two Sims may step on separate threads)
    if (UseBruteForce) BuildThreatBrute(*this, Threat);
    else BuildThreatGrid(*this, G, Threat);

    { LUR_TRACE_SCOPE("sim.acq");  TargetAcquire(*this, G); }          // phase 2
    { LUR_TRACE_SCOPE("sim.move"); Movement(*this, G, Threat); }       // phase 3
    { LUR_TRACE_SCOPE("sim.atk");  Attacks(*this); }           // phase 4
    Deaths(*this);                // phase 5
    Economy(*this);               // phase 6
    WinCheck(*this);              // phase 7
    ++Tick;                       // phase 8 (hash) is computed on demand via StateHash()
}

#if LUR_INTERNAL
void Sim::StressFill(int32_t PerTeam) {
    Lur::Sim::SplitMix64 R(Seed ^ 0x57A9E55ull);
    const int32_t Wi = WorldWidth.ToInt(), Hi = WorldHeight.ToInt();
    for (uint8_t T = 0; T < 2; ++T) {
        // Team 0 fills the lower half, team 1 the upper — soldiers only (types 1..3).
        const int32_t Y0 = T == 0 ? 4 : Hi / 2;
        for (int32_t K = 0; K < PerTeam; ++K) {
            const int32_t I = AllocSlot(*this);
            if (I < 0) return;  // hit the cap
            const uint8_t Ty = static_cast<uint8_t>(1 + R.NextBounded(3));
            PosX[I] = F(2 + static_cast<int32_t>(R.NextBounded(static_cast<uint32_t>(Wi - 4))));
            PosY[I] = F(Y0 + static_cast<int32_t>(R.NextBounded(static_cast<uint32_t>(Hi / 2 - 4))));
            PrevX[I] = PosX[I];
            PrevY[I] = PosY[I];
            Hp[I] = Units[Ty].MaxHp;
            Type[I] = Ty;
            Team[I] = T;
            Target[I] = -1;
            Cooldown[I] = 0;
            WorkerState[I] = WorkToMine;
            Carry[I] = 0;
            WorkerTimer[I] = 0;
            Kind[I] = KindUnit;         // #131: reset recycled slot
            Queue[I] = 0;
            BuildProgress[I] = 0;
            SetAlive(*this, I);
            if (I + 1 > Count) Count = I + 1;
        }
    }
}
#endif

int32_t Sim::AliveCount(uint8_t TeamId) const {
    int32_t C = 0;
    for (int32_t I = 0; I < Count; ++I)
        if (IsAlive(I) && Team[I] == TeamId) ++C;
    return C;
}

int32_t Sim::QueuedTotal(uint8_t TeamId) const {
    int32_t C = 0;
    for (uint8_t Ty = 0; Ty < UnitCount; ++Ty) C += Teams[TeamId].QueueCount[Ty];
    return C;
}

uint64_t Sim::StateHash() const {
    // FNV-1a over the pinned authoritative state, in declaration order. Assumes a
    // little-endian target (host x86, Android/iOS ARM — all LE). PrevX/PrevY ARE hashed
    // as of slice B (#97): Δ=Pos−Prev now feeds behaviour (momentum/alignment), so Prev
    // is authoritative — a Pos-only write or snapshot-restore that skipped it would
    // silently diverge with the anchor alarm blind. The transient DepositBuf stays
    // excluded (within-tick scratch). Build-LOCKED, not a wire change: the mask/event
    // codec is untouched, so ProtocolVersion is unchanged — a mixed-build session just
    // trips the anchor-hash alarm within a second (both peers must run the same build).
    uint64_t H = 1469598103934665603ull;
    auto Mix = [&](const void* P, size_t N) {
        const uint8_t* B = static_cast<const uint8_t*>(P);
        for (size_t K = 0; K < N; ++K) { H ^= B[K]; H *= 1099511628211ull; }
    };
    const size_t N = static_cast<size_t>(Count);
    Mix(PosX, sizeof(Fixed) * N);
    Mix(PosY, sizeof(Fixed) * N);
    Mix(PrevX, sizeof(Fixed) * N);  // #97: authoritative (feeds velocity Δ)
    Mix(PrevY, sizeof(Fixed) * N);
    Mix(Hp, sizeof(int32_t) * N);
    Mix(Type, N);
    Mix(Team, N);
    Mix(Target, sizeof(int32_t) * N);
    Mix(Cooldown, sizeof(int32_t) * N);
    Mix(WorkerState, N);
    Mix(Carry, sizeof(int32_t) * N);
    Mix(WorkerTimer, sizeof(int32_t) * N);
    Mix(Kind, N);                              // #131 buildings (0 = unit)
    Mix(Queue, sizeof(int32_t) * N);           // #131 per-building production queue
    Mix(BuildProgress, sizeof(int32_t) * N);   // #131 per-building construction progress
    Mix(AliveBits, sizeof(uint64_t) * ((N + 63) / 64));
    for (int T = 0; T < 2; ++T) {
        const TeamState& Q = Teams[T];
        Mix(&Q.Gold, sizeof(int32_t));
        Mix(Q.QueueCount, sizeof(int32_t) * UnitCount);
        Mix(Q.BuildProgress, sizeof(int32_t) * UnitCount);
        Mix(&Q.SpawnCounter, sizeof(int32_t));
    }
    Mix(MineGold, sizeof(int32_t) * NumMines);  // mutable reserves (#84) — MineX/Y stay excluded (static)
    Mix(&FrontierT0, sizeof(Fixed));            // #131/§5.3 frontier high-water (gates placement)
    Mix(&FrontierT1, sizeof(Fixed));
    Mix(&Cv, sizeof(Cv));  // #112: latched gameplay-CVar snapshot — a mis-latch surfaces as a desync
    Mix(&Tick, sizeof(uint32_t));
    Mix(&Result, sizeof(uint8_t));
    return H;
}

}  // namespace Rps
