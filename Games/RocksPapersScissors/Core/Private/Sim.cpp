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
    S.Hp[I] = UnitTable[Type].MaxHp;
    S.Type[I] = Type;
    S.Team[I] = Team;
    S.Target[I] = -1;
    S.Cooldown[I] = 0;
    S.WorkerState[I] = WorkToMine;
    S.Carry[I] = 0;
    S.WorkerTimer[I] = 0;
    SetAlive(S, I);
    if (I + 1 > S.Count) S.Count = I + 1;
}

// --- map: v1 is fixed + mirrored; the seed is derived and stored so later
//     variation is free, exactly like chess derives colours from GUIDs. ---
void BuildMap(Sim& S) {
    // Four mines per cluster, spread across the 34-wide field.
    const Fixed Xs[MinesPerCluster] = {F(8), F(14), F(20), F(26)};
    // Cluster centre lines (portrait, §9), derived from the field height so they scale
    // with the WorldHeight balance knob: each team's SAFE cluster sits just past its camp,
    // its CONTESTED cluster nearer mid-field (shorter walk, higher risk — spec §2).
    const int32_t Mid = WorldHeight.ToInt() / 2;
    const Fixed ClusterY[4] = {
        F(CampInset + 6),                          // t0 safe   (near the bottom camp)
        F(Mid - 6),                                // t0 contested (toward mid)
        F(WorldHeight.ToInt() - CampInset - 6),    // t1 safe   (near the top camp)
        F(Mid + 6),                                // t1 contested (toward mid)
    };
    int32_t Idx = 0;
    for (int G = 0; G < 4; ++G)
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
    const int32_t Cost = UnitTable[Type].Cost;
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
            Q.BuildProgress[Ty] += Q.QueueCount[Ty];
            while (Q.QueueCount[Ty] > 0 && Q.BuildProgress[Ty] >= UnitTable[Ty].BuildTicks) {
                Q.BuildProgress[Ty] -= UnitTable[Ty].BuildTicks;
                --Q.QueueCount[Ty];
                SpawnUnit(S, T, Ty);
            }
            if (Q.QueueCount[Ty] == 0) Q.BuildProgress[Ty] = 0;  // no banked progress on an empty queue
        }
    }
}

// ---- Phase 2: target acquisition for units without a valid target ----
int32_t NearestEnemyBrute(const Sim& S, int32_t I) {
    int32_t Best = -1;
    int64_t BestD = INT64_MAX;
    for (int32_t J = 0; J < S.Count; ++J) {
        if (!S.IsAlive(J) || S.Team[J] == S.Team[I]) continue;
        const int64_t D = Dist2(S.PosX[I], S.PosY[I], S.PosX[J], S.PosY[J]);
        if (D < BestD) { BestD = D; Best = J; }  // strict < : lowest id wins ties (ascending J)
    }
    return Best;
}
// Grid nearest-enemy: expanding Chebyshev ring search. Must reproduce the brute
// result EXACTLY, including the lowest-id tie-break — so the compare is the (dist,id)
// LEXICOGRAPHIC minimum, not strict-<: an equal-distance unit can sit in a farther
// ring than a higher-id one, and the ring bound below guarantees it's still scanned.
int32_t NearestEnemyGrid(const Sim& S, const Grid& G, int32_t I) {
    const int32_t Cx = CellX(S.PosX[I]), Cy = CellY(S.PosY[I]);
    int64_t BestD = INT64_MAX;
    int32_t BestId = -1;
    const int32_t MaxK = GridCols + GridRows;
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
                    const int64_t D = Dist2(S.PosX[I], S.PosY[I], S.PosX[J], S.PosY[J]);
                    if (D < BestD || (D == BestD && J < BestId)) { BestD = D; BestId = J; }
                }
            }
        }
        // After ring K, the nearest unscanned cell (ring K+1) is >= K*cellSize away.
        // Stop only when that bound STRICTLY exceeds the best (so equal-distance ties
        // one ring out are still visited and can win on lower id).
        if (BestId >= 0) {
            const int64_t R = static_cast<int64_t>(K) * CellRaw;
            if (R * R > BestD) break;
        }
        if (K > 0 && !AnyInGrid) break;  // the whole ring is outside the grid — nothing farther can be inside
    }
    return BestId;
}
int32_t MineOccupancy(const Sim& S, int32_t Mine) {
    int32_t C = 0;
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J) && S.Type[J] == UnitMiner && S.Target[J] == Mine) ++C;
    return C;
}
int32_t NearestFreeMine(const Sim& S, int32_t I) {
    int32_t Best = -1;
    int64_t BestD = INT64_MAX;
    for (int32_t Tr = 0; Tr < NumMines; ++Tr) {
        if (S.MineGold[Tr] <= 0) continue;  // depleted mines are gone (#84)
        if (MineOccupancy(S, Tr) >= WorkersPerMine) continue;
        const int64_t D = Dist2(S.PosX[I], S.PosY[I], S.MineX[Tr], S.MineY[Tr]);
        if (D < BestD) { BestD = D; Best = Tr; }
    }
    return Best;
}
void TargetAcquire(Sim& S, const Grid& G) {
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        if (S.Type[I] == UnitMiner) {
            if (S.Target[I] < 0) S.Target[I] = NearestFreeMine(S, I);  // mines are few — brute is fine; set in slot order
        } else {
            const int32_t T = S.Target[I];
            const bool Valid = T >= 0 && S.IsAlive(T) && S.Team[T] != S.Team[I];
            if (!Valid)  // else keep target until death (hysteresis, no dithering)
                S.Target[I] = S.UseBruteForce ? NearestEnemyBrute(S, I) : NearestEnemyGrid(S, G, I);
        }
    }
}

// ---- Phase 3: movement + separation ----
// Chebyshev seek (spec §5): step = speed * (dx,dy) / max(|dx|,|dy|). Pure Fixed
// mul/div; an EXPLICIT zero-distance guard before the divide (never relying on
// Fixed::operator/'s silent saturate).
void MoveToward(Sim& S, int32_t I, Fixed Tx, Fixed Ty) {
    const Fixed Dx = Tx - S.PosX[I];
    const Fixed Dy = Ty - S.PosY[I];
    const Fixed M = Max(Abs(Dx), Abs(Dy));
    if (M.Raw == 0) return;  // already there (or overlapping) — zero-distance guard
    const Fixed Sp = UnitTable[S.Type[I]].Speed;
    if (M <= Sp) { S.PosX[I] = Tx; S.PosY[I] = Ty; return; }  // arrive, don't overshoot
    S.PosX[I] = S.PosX[I] + Sp * Dx / M;
    S.PosY[I] = S.PosY[I] + Sp * Dy / M;
}
bool Arrived(const Sim& S, int32_t I, Fixed Tx, Fixed Ty) {
    const Fixed M = Max(Abs(Tx - S.PosX[I]), Abs(Ty - S.PosY[I]));
    return M <= UnitTable[S.Type[I]].Speed;
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
            if (Arrived(S, I, Tx, Ty)) {
                S.PosX[I] = Tx; S.PosY[I] = Ty;
                S.WorkerState[I] = WorkDig; S.WorkerTimer[I] = DigTicks;
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
void SoldierSeek(Sim& S, int32_t I) {
    const int32_t T = S.Target[I];
    if (T < 0 || !S.IsAlive(T)) return;
    if (Dist2(S.PosX[I], S.PosY[I], S.PosX[T], S.PosY[T]) <= RangeSq(UnitTable[S.Type[I]].Range))
        return;  // in range — hold position and fight
    MoveToward(S, I, S.PosX[T], S.PosY[T]);
}
// Separation reads PrevX/PrevY (start-of-tick positions), so the sum is
// order-independent regardless of iteration order — a determinism-safe reduction.
// One neighbour push, factored out so brute and grid paths add IDENTICAL terms.
void AddSeparation(const Sim& S, int32_t I, int32_t J, int64_t R2, Fixed& Sx, Fixed& Sy) {
    if (J == I || S.Team[J] != S.Team[I]) return;
    const int64_t Dx = static_cast<int64_t>(S.PrevX[I].Raw) - S.PrevX[J].Raw;
    const int64_t Dy = static_cast<int64_t>(S.PrevY[I].Raw) - S.PrevY[J].Raw;
    const int64_t D2 = Dx * Dx + Dy * Dy;
    if (D2 == 0 || D2 >= R2) return;  // exact overlap (skip; no det. direction) or out of range
    // Offset is within radius, so |Dx|,|Dy| < One: reinterpreting as a Fixed Raw is safe.
    Sx = Sx + Fixed{static_cast<int32_t>(Dx)} * SeparationStrength;
    Sy = Sy + Fixed{static_cast<int32_t>(Dy)} * SeparationStrength;
}
void SeparationBrute(const Sim& S, int32_t I, Fixed& Sx, Fixed& Sy) {
    Sx = Fixed{0}; Sy = Fixed{0};
    const int64_t R2 = RangeSq(SeparationRadius);
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J)) AddSeparation(S, I, J, R2, Sx, Sy);
}
// Grid separation visits only the cells the radius overlaps — a superset of the
// in-radius neighbours, then AddSeparation re-tests R2, so the summed set (and thus
// the sum) is identical to brute. Queried by Prev, which equals each unit's bucketed
// build-time Pos, so the cell lookup is consistent.
void SeparationGrid(const Sim& S, const Grid& G, int32_t I, Fixed& Sx, Fixed& Sy) {
    Sx = Fixed{0}; Sy = Fixed{0};
    const int64_t R2 = RangeSq(SeparationRadius);
    const int32_t Cx0 = CellX(S.PrevX[I] - SeparationRadius), Cx1 = CellX(S.PrevX[I] + SeparationRadius);
    const int32_t Cy0 = CellY(S.PrevY[I] - SeparationRadius), Cy1 = CellY(S.PrevY[I] + SeparationRadius);
    for (int32_t Gy = Cy0; Gy <= Cy1; ++Gy)
        for (int32_t Gx = Cx0; Gx <= Cx1; ++Gx) {
            const int32_t C = Gy * GridCols + Gx;
            for (int32_t P = G.Start[C]; P < G.Start[C + 1]; ++P)
                AddSeparation(S, I, G.Order[P], R2, Sx, Sy);
        }
}
void Movement(Sim& S, const Grid& G) {
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        Fixed Sx, Sy;
        if (S.UseBruteForce) SeparationBrute(S, I, Sx, Sy);  // from Prev — before we touch Pos
        else SeparationGrid(S, G, I, Sx, Sy);
        if (S.Type[I] == UnitMiner) WorkerSeek(S, I);
        else SoldierSeek(S, I);
        S.PosX[I] = ClampAxis(S.PosX[I] + Sx, WorldWidth);
        S.PosY[I] = ClampAxis(S.PosY[I] + Sy, WorldHeight);
    }
}

// ---- Phase 4: attacks (damage buffered, applied SIMULTANEOUSLY) ----
void Attacks(Sim& S) {
    int32_t Dmg[MaxUnits];
    std::memset(Dmg, 0, sizeof(int32_t) * static_cast<size_t>(S.Count));
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || S.Type[I] == UnitMiner) continue;  // workers ignore combat in v1 (spec §5)
        if (S.Cooldown[I] > 0) --S.Cooldown[I];
        if (S.Cooldown[I] > 0) continue;  // still cooling
        const int32_t T = S.Target[I];
        if (T < 0 || !S.IsAlive(T)) continue;
        if (Dist2(S.PosX[I], S.PosY[I], S.PosX[T], S.PosY[T]) > RangeSq(UnitTable[S.Type[I]].Range)) continue;
        int32_t D = UnitTable[S.Type[I]].Attack;
        if (UnitTable[S.Type[I]].Beats == S.Type[T]) D *= CounterMultiplier;
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

void Sim::Init(uint64_t InSeed) {
    *this = Sim{};  // value-init: zeroes every array/field (Init-time, not the hot path)
    Seed = InSeed;
    BuildMap(*this);
    for (uint8_t T = 0; T < 2; ++T) {
        Teams[T].Gold = StartGold;
        for (int K = 0; K < StartMiners; ++K) SpawnUnit(*this, T, UnitMiner);
    }
}

void Sim::Step(uint8_t Mask0, uint8_t Mask1) {
    if (Result != ResultOngoing) return;  // match decided: freeze (still deterministic on both peers)

    for (int32_t I = 0; I < Count; ++I) { PrevX[I] = PosX[I]; PrevY[I] = PosY[I]; }

    ApplyInput(*this, 0, Mask0);  // phase 0: P0 then P1
    ApplyInput(*this, 1, Mask1);
    Production(*this);            // phase 1 (spawns) — grid must see the new units

    // Build the spatial grid AFTER production (so spawns are bucketed) but before any
    // movement, capturing start-of-tick positions. Both target acq (on Pos) and
    // separation (on Prev) read it; they're consistent because Pos == Prev right now.
    Grid G;
    G.Build(*this);

    TargetAcquire(*this, G);      // phase 2
    Movement(*this, G);           // phase 3
    Attacks(*this);               // phase 4
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
            Hp[I] = UnitTable[Ty].MaxHp;
            Type[I] = Ty;
            Team[I] = T;
            Target[I] = -1;
            Cooldown[I] = 0;
            WorkerState[I] = WorkToMine;
            Carry[I] = 0;
            WorkerTimer[I] = 0;
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
    // little-endian target (host x86, Android/iOS ARM — all LE). PrevX/PrevY and the
    // transient DepositBuf are deliberately excluded (view copy / within-tick scratch).
    uint64_t H = 1469598103934665603ull;
    auto Mix = [&](const void* P, size_t N) {
        const uint8_t* B = static_cast<const uint8_t*>(P);
        for (size_t K = 0; K < N; ++K) { H ^= B[K]; H *= 1099511628211ull; }
    };
    const size_t N = static_cast<size_t>(Count);
    Mix(PosX, sizeof(Fixed) * N);
    Mix(PosY, sizeof(Fixed) * N);
    Mix(Hp, sizeof(int32_t) * N);
    Mix(Type, N);
    Mix(Team, N);
    Mix(Target, sizeof(int32_t) * N);
    Mix(Cooldown, sizeof(int32_t) * N);
    Mix(WorkerState, N);
    Mix(Carry, sizeof(int32_t) * N);
    Mix(WorkerTimer, sizeof(int32_t) * N);
    Mix(AliveBits, sizeof(uint64_t) * ((N + 63) / 64));
    for (int T = 0; T < 2; ++T) {
        const TeamState& Q = Teams[T];
        Mix(&Q.Gold, sizeof(int32_t));
        Mix(Q.QueueCount, sizeof(int32_t) * UnitCount);
        Mix(Q.BuildProgress, sizeof(int32_t) * UnitCount);
        Mix(&Q.SpawnCounter, sizeof(int32_t));
    }
    Mix(MineGold, sizeof(int32_t) * NumMines);  // mutable reserves (#84) — MineX/Y stay excluded (static)
    Mix(&Tick, sizeof(uint32_t));
    Mix(&Result, sizeof(uint8_t));
    return H;
}

}  // namespace Rps
