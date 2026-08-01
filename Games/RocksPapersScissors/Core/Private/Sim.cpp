// RocksPapersScissors deterministic simulation core.
//
// The one law: State = Replay(Inputs, Seed) (design doc §1). Init derives the
// world from the seed; StepEvents(events) applies exactly one 10 Hz tick as the
// eight fixed-order phases of spec §6. No wallclock, no float, no allocation ever
// touches this file — that is the precondition for the slice-1 lockstep netcode.
//
// The input DELAY (press at T executes at T+3) is deliberately NOT here: it is a
// netcode concern (slice 1). The sim just applies whatever input-event batch it's
// handed for this tick, which keeps the replay law clean and makes the flight-
// recorder stream literally the per-tick event sequence (#145: the old 4-bit press
// mask is gone — production is spatial buildings now).
//
// Neighbour queries are brute-force O(n^2) here ON PURPOSE: this is the reference
// the deterministic spatial grid gets equivalence-tested against (issue #75). The
// grid replaces the inner loops later without changing a single result.
#include "Rps/Sim.h"

#include <cstring>

#include "Lur/Core/Assert.h"
#include "Lur/Core/Log.h"      // #157: the starter-row seal is a WARNING, not an abort
#include "Lur/Sim/Random.h"
#include "Rps/Placement.h"     // #157: the one shared placement predicate (Sim + Snapshot)
#include "Lur/Trace/Trace.h"  // LUR_TRACE_SCOPE — observational only (compiles out in Shipping)

namespace Rps {
namespace {

// Deterministic spawn ring around a spawn center (radius ~2). A per-building counter
// (Cooldown[], #132) or the start-miner index picks the offset % RingSlots — no RNG,
// identical on both peers.
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

// Spawn a mobile unit at an explicit position (#132). The one place a unit slot is filled;
// callers pick the position — the legacy camp+ring path and building production both route
// here. Returns the slot (-1 if the cap is hit).
int32_t SpawnUnitAt(Sim& S, uint8_t Team, uint8_t Type, Fixed X, Fixed Y) {
    const int32_t I = AllocSlot(S);
    LUR_ASSERT_MSG(I >= 0, "RPS: unit slot exhausted (MaxUnits) — raise the cap");
    if (I < 0) return -1;

    S.PosX[I] = X;
    S.PosY[I] = Y;
    S.PrevX[I] = X;
    S.PrevY[I] = Y;
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
    return I;
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
    // #157: the two rows nearest each end sit HARD AGAINST the map edge, so nothing can be built
    // BEHIND them (below the bottom pair / above the top pair). They used to sit at CampInset+2 and
    // +6 (8 and 12), which left a usable strip behind them — camps went up there, off to the side of
    // the economy, and the row nearest the edge became the awkward one to serve.
    //
    // The invariant, and why these two numbers: a building's lowest legal centre is
    // Edge = 1.5 x footprint (CanPlaceBuilding keeps the whole ICON on-map), and it must also clear
    // every live mine by Cv.MineClearance. So a row at Ym has no room behind it exactly when
    //     Ym - MineClearance < 1.5 x footprint
    // At footprint 3 / clearance 6 that is Ym < 10.5 — both 3 and 9 qualify. Asserted below rather
    // than left as a comment, because it silently depends on two CVars.
    // #157: the two starter rows are CVars (rps.mine.row_home / row_safe), as a distance in from
    // each team's own end so they stay mirrored whatever they are set to. midfield/contested remain
    // derived from WorldHeight — freezing them at an absolute Y would stop them scaling with the map.
    const Fixed EdgeRow = S.Cv.MineRowHome;
    const Fixed NextRow = S.Cv.MineRowSafe;
    const Fixed ClusterY[ClustersPerTeam * 2] = {
        EdgeRow,                 // t0 home      (nearest the bottom edge)
        NextRow,                 // t0 safe      (one row in)
        F(Hi / 4),               // t0 midfield
        F(Mid - 8),              // t0 contested (toward mid)
        WorldHeight - EdgeRow,   // t1 home      (nearest the top edge)
        WorldHeight - NextRow,   // t1 safe      (one row in)
        F(Hi - Hi / 4),          // t1 midfield
        F(Mid + 8),              // t1 contested (toward mid)
    };
    // The #157 sealing property, now a WARNING and not an assert: these rows are a tuning knob, so a
    // value that stops sealing is a choice to be told about, not a programmer error to abort on.
    // Warn once per Init; it is a handful of lines per match, only in a dev build.
    const Fixed EdgeMargin = S.Cv.BuildingFootprint * F(3, 2);   // lowest legal building centre
    const Fixed Seal = EdgeMargin + S.Cv.MineClearance;
    if (!(EdgeRow < Seal) || !(NextRow < Seal))
        Lur::Log::Info("RPS map: starter rows %d/%d do NOT seal the edge (need < %d = 1.5x footprint "
                       "+ mine_clearance) — ground behind them is buildable",
                       EdgeRow.ToInt(), NextRow.ToInt(), Seal.ToInt());
    int32_t Idx = 0;
    for (int G = 0; G < ClustersPerTeam * 2; ++G)
        for (int K = 0; K < MinesPerCluster; ++K) {
            S.MineX[Idx] = Xs[K];
            S.MineY[Idx] = ClusterY[G];
            S.MineGold[Idx] = MineGoldCapacity;  // finite reserve (#84)
            ++Idx;
        }
}

// ---- Phase 1 (buildings, #132): per-BUILDING production — FLAT, no stack acceleration.
// Each alive building with Queue>0 advances BuildProgress by +1/tick (NOT += Queue), so a
// deep queue never builds faster: throughput scales by BUILDING COUNT, not stack depth
// (the deliberate lever handoff, spec §3). At BuildTicks it spawns ONE unit of the
// building's Type adjacent to itself and decrements the queue. Buildings are visited in
// slot order [0, Count) — the same determinism guarantee as the legacy spawn loop.
//
// The per-building spawn-ring index reuses Cooldown[] (buildings never attack, so it is free
// building state) — no global SpawnCounter. Flat cadence means at most one spawn per building
// per tick (BuildProgress can't leap past BuildTicks in one +1), so no inner loop.
void ProductionBuildings(Sim& S) {
    for (int32_t B = 0; B < S.Count; ++B) {
        if (!S.IsAlive(B) || !S.IsBuilding(B)) continue;
        if (S.Queue[B] <= 0) { S.BuildProgress[B] = 0; continue; }  // no banked progress
        const uint8_t Ty = S.Type[B];
        S.BuildProgress[B] += 1;  // FLAT
        if (S.BuildProgress[B] >= S.Units[Ty].BuildTicks) {
            S.BuildProgress[B] -= S.Units[Ty].BuildTicks;
            const int32_t Slot = static_cast<int32_t>(static_cast<uint32_t>(S.Cooldown[B]) % RingSlots);
            ++S.Cooldown[B];
            SpawnUnitAt(S, S.Team[B], Ty, S.PosX[B] + RingX[Slot], S.PosY[B] + RingY[Slot]);
            --S.Queue[B];
            if (S.Queue[B] <= 0) S.BuildProgress[B] = 0;
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
// A protected asset raises the raider alert: a miner-type slot (cart or camp, the original set) or
// (#146) the HOME BASE. Enemy soldiers within GuardAlertR of one are flagged so friendly defenders
// interpose to screen it. (Type==UnitMiner preserves the pre-#146 set verbatim; home base is added.)
bool IsGuardedAsset(const Sim& S, int32_t M) {
    return S.Type[M] == UnitMiner || S.IsHomeBase(M);
}
void BuildThreatBrute(const Sim& S, ThreatSet& T) {
    T.Clear();
    for (int32_t M = 0; M < S.Count; ++M) {
        if (!S.IsAlive(M) || !IsGuardedAsset(S, M)) continue;
        for (int32_t J = 0; J < S.Count; ++J)
            if (S.IsAlive(J)) AddThreat(S, M, J, T);
    }
}
// Grid twin: per miner, walk only the cells its GuardAlertR box overlaps; AddThreat
// re-tests the radius, so the flagged SET is identical to brute regardless of order.
void BuildThreatGrid(const Sim& S, const Grid& G, ThreatSet& T) {
    T.Clear();
    for (int32_t M = 0; M < S.Count; ++M) {
        if (!S.IsAlive(M) || !IsGuardedAsset(S, M)) continue;
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
    // #146: the enemy HOME BASE is PREY to every soldier (no RPS counter) — top target tier,
    // regardless of the attacker's type. Its Type is UnitNone, so score it by Kind, not TargetPrefer.
    const int32_t Prefer = S.IsHomeBase(J) ? 0 : TargetPrefer(S.Type[I], S.Type[J]);
    return {D / TargetBandRaw, Prefer, D, J};
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
// Travel time to a mine. CHEBYSHEV, not Euclidean, because movement is Chebyshev-normalized
// (design §5: a step covers Speed in max(|dx|,|dy|), so a diagonal is ~1.4x faster in Euclidean
// terms). This metric therefore IS ticks-to-arrive, which is what a travel budget must be
// measured in — and it matches the dig-range test in WorkerSeek, which is Chebyshev too. No
// sqrt, so Fixed::Sqrt stays unbuilt.
Fixed TravelTo(const Sim& S, int32_t I, int32_t Mine) {
    return Max(Abs(S.MineX[Mine] - S.PosX[I]), Abs(S.MineY[Mine] - S.PosY[I]));
}
int32_t NearestLeastCrowdedMine(const Sim& S, int32_t I, const int32_t* Occ) {
    // Nearest-always (playtest 2026-07-20: carts were hauling past nearby mines), refined by a
    // READABILITY rule: among the deposits that are ABOUT AS CLOSE as the nearest one, take the
    // one with the fewest carts already assigned. That is the whole rule — there is deliberately
    // NO per-mine cap. A pile of carts on one deposit is uncountable at a glance; spreading them
    // over the two or three near-equidistant deposits of a cluster (~5 units apart, so from a
    // camp they differ by ~3) makes the number readable, and costs a fraction of a second.
    //
    // Two bounds on "about as close", because a pure distance budget CANNOT express the rule
    // that matters. BuildMap lays mines out in rows of MinesPerCluster: ~5-6 apart along a row,
    // and the starter rows are 6 apart (row_home 3 / row_safe 9). So the sideways neighbour and
    // a mine in the next row are the SAME distance away — no threshold separates them. What
    // separates them is the layout itself:
    //   * SAME ROW as the nearest deposit: the full MineSpreadSlack budget. A cart sitting on
    //     top of a mine is ~5-6 from its neighbour, so the budget has to be that big to spread
    //     at all, and spending it stays inside the cluster the cart is already working.
    //   * ANOTHER ROW: only if it is no FARTHER than the nearest deposit. That admits the free
    //     case (a camp between two rows, both equidistant) and forbids the expensive one — a
    //     cart abandoning a row it is standing in to cross to the next one.
    // Rows are index-contiguous out of BuildMap, so "same row" is one integer divide.
    //
    // Occupancy is the precomputed per-mine count (O(1) here); a per-mine unit scan would be
    // O(mines x units) per acquisition. Ties break to the closer deposit, then — iteration
    // order with strict < — to the lower mine index, so the choice is deterministic for lockstep.
    Fixed Near{0}; int32_t NearId = -1;
    for (int32_t Tr = 0; Tr < NumMines; ++Tr) {
        if (S.MineGold[Tr] <= 0) continue;  // depleted mines are gone (#84)
        const Fixed D = TravelTo(S, I, Tr);
        if (NearId < 0 || D < Near) { Near = D; NearId = Tr; }
    }
    if (NearId < 0) return -1;   // the map is mined out — nothing to acquire
    const Fixed Budget = Near + S.Cv.MineSpreadSlack;
    const int32_t NearRow = NearId / MinesPerCluster;
    int32_t Best = -1, BestOcc = 0; Fixed BestD{0};
    for (int32_t Tr = 0; Tr < NumMines; ++Tr) {
        if (S.MineGold[Tr] <= 0) continue;
        const Fixed D = TravelTo(S, I, Tr);
        if (D > Budget) continue;                                        // not worth the walk
        if (Tr / MinesPerCluster != NearRow && D > Near) continue;        // don't leave the row
        if (Best < 0 || Occ[Tr] < BestOcc || (Occ[Tr] == BestOcc && D < BestD)) {
            Best = Tr; BestOcc = Occ[Tr]; BestD = D;
        }
    }
    return Best;   // >= 0: the budget always contains the nearest deposit
}
void TargetAcquire(Sim& S, const Grid& G) {
    // Mine occupancy computed ONCE (O(units)) then read O(1)/mine — with the dense field a
    // per-mine unit scan per cart would be O(mines×units). Incremented as carts claim mines
    // in slot order, preserving the deterministic first-come assignment.
    int32_t Occ[NumMines] = {};  // ~2 KB stack scratch — NOT static (two Sims may step on separate threads)
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J) && !S.IsBuilding(J) && S.Type[J] == UnitMiner && S.Target[J] >= 0) ++Occ[S.Target[J]];
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        if (S.IsBuilding(I)) continue;  // #133: buildings don't acquire targets (they're targeted — #134)
        if (S.Type[I] == UnitMiner) {
            if (S.Target[I] < 0) {
                const int32_t M = NearestLeastCrowdedMine(S, I, Occ);  // nearest gold, prefer uncrowded
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
// #134/§12.4: a gold-carrying cart deposits at the NEAREST own MINER BUILDING (fixed-point
// distance, lowest-slot tie-break). Returns the building slot, or -1 if the team has none —
// in which case the caller STRANDS the gold (the cart holds it until a miner building exists
// again, §12.4). Static buildings -> the scan is order-independent and grid==brute-safe.
int32_t NearestMinerBuilding(const Sim& S, int32_t I, uint8_t Team) {
    int32_t Best = -1;
    int64_t BestD = INT64_MAX;
    for (int32_t B = 0; B < S.Count; ++B) {
        if (!S.IsAlive(B) || !S.IsBuilding(B)) continue;
        if (S.Team[B] != Team || S.Type[B] != UnitMiner) continue;
        const int64_t D = Dist2(S.PosX[I], S.PosY[I], S.PosX[B], S.PosY[B]);
        if (D < BestD) { BestD = D; Best = B; }  // strict < -> lowest slot wins ties
    }
    return Best;
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
            if (Max(Abs(Tx - S.PosX[I]), Abs(Ty - S.PosY[I])) <= S.Cv.MineDigRange) {
                S.WorkerState[I] = WorkDig;
                S.WorkerTimer[I] = S.Cv.DigTicks;
                return;
            }
            MoveToward(S, I, Tx, Ty);
            return;
        }
        case WorkToCamp: {
            // #135/§12.4: deposit ONLY at the nearest own MINER BUILDING (camps are gone). With
            // none — the team's last miner building died while this cart carried gold — the gold
            // is STRANDED: the cart holds it and idles here until a miner building exists again.
            const int32_t B = NearestMinerBuilding(S, I, S.Team[I]);
            if (B < 0) return;  // stranded: hold the carry, wait for a drop-off (no camp fallback)
            const Fixed Tx = S.PosX[B], Ty = S.PosY[B];
            // Deposit within the building's footprint (its repulsion keeps soldiers out, but
            // carts ignore separation, so they settle at the footprint edge and bank there).
            if (Max(Abs(Tx - S.PosX[I]), Abs(Ty - S.PosY[I])) <= S.Cv.BuildingFootprint) {
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
    // #162: one Chebyshev test up front against the WIDEST radius, before any per-force work. Every
    // Add below re-tests its own radius, and GatherR is by construction the max of all of them
    // (DeriveUnits), so a neighbour at or beyond it contributes exactly nothing — this is a pure
    // early-out, not a behaviour change (grid==brute and the determinism tests both hold it).
    //
    // It pays because the grid walks whole CELLS: the queried box is ceil(GatherR/CellSize) cells in
    // each direction, so it reaches up to about GatherR + CellSize and most of the pairs it yields are
    // out of range of every force. Those used to run the full body — several branches and up to four
    // separate Chebyshev computations of the same distance — to conclude nothing.
    if (Max(Abs(Ix - Jx), Abs(Iy - Jy)) >= S.GatherR) return;
    // #133/§5.2: a BUILDING is a big static separation source — units flow AROUND it (no
    // pathfinding). Reuse the corrected separation falloff, its own radius/strength, on ALL
    // buildings (friend or foe). Buildings carry no cohesion/alignment/flee/cart semantics,
    // so short-circuit before the unit-affinity logic.
    if (S.IsBuilding(J)) {
        AddRepel(Ix, Iy, Jx, Jy, S.Cv.BuildingRepelRadius, S.Cv.BuildingRepelStrength,
                 A.SepX, A.SepY);
        // #146: a friendly HOME BASE is a protected asset like a cart — a defender that also sees a
        // raider steers to screen it (reuses the #98 interpose accumulator, so no new force/tuning).
        if (S.IsHomeBase(J) && S.Team[J] == S.Team[I] &&
            Max(Abs(Ix - Jx), Abs(Iy - Jy)) < S.Cv.InterposeRadius) {
            A.CartX += Jx.Raw; A.CartY += Jy.Raw; ++A.CartN;
        }
        return;
    }
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
                 S.Cv.MineRepelRadius, S.Cv.SeparationStrength, Ax, Ay);
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
        if (S.IsBuilding(I)) continue;  // #133: buildings never move (StepX/Y unread for them)

        if (S.Type[I] == UnitMiner) {
            // Miners are DIRECT (no momentum) and get NO separation (playtest 2026-07-20:
            // separation shoved carts off the tight camp deposit point, so they could never
            // bank). Only the mine-repel ring (deposits are soft obstacles) applies here;
            // WorkerSeek runs in pass 2 (it needs the unmoved start-of-tick Pos).
            //
            // #162: and therefore NO FLOCK GATHER — this used to sit AFTER the gather, which computed
            // a whole FlockAcc for every cart and then read nothing out of it. That dead work was the
            // measured collapse: at ~1600 units on a Galaxy A14, sim.move was ~50 ms of a 100 ms tick
            // and ~90% of the sim, and the field was carts ("I was able to make the match freeze by
            // making over 1600 carts"). Bit-identical — the gather is pure (const Sim, writes only its
            // accumulator), so not calling it cannot change a result. TestMinerPathIgnoresNeighbours
            // pins the invariant this rests on, so a future force that DOES want neighbours fails
            // there rather than silently reintroducing the cost.
            int64_t Nx = 0, Ny = 0;
            AddMineRepel(S, I, Nx, Ny);
            StepX[I] = Fixed{static_cast<int32_t>(Nx)};
            StepY[I] = Fixed{static_cast<int32_t>(Ny)};
            continue;
        }

        FlockAcc A;
        if (S.UseBruteForce) GatherBrute(S, I, Threat, A);  // reads Pos + Δ — nothing has moved yet
        else GatherGrid(S, G, I, Threat, A);
#if LUR_INTERNAL
        ++S.FlockGathers;  // #162: dev-only counter, so "a cart issues none" is testable exactly
#endif

        // --- Soldier: blend the neighbour sums into a desired velocity ---
        // Seek goal + in-range test (targeting unchanged). No target -> march on the ENEMY
        // BASELINE (#92/#135: camps are gone) so fronts still close instead of idling: team 0
        // pushes up to Y=WorldHeight, team 1 down to Y=0, both centred on the field.
        Fixed Tx, Ty;
        bool InRange = false;
        const int32_t T = S.Target[I];
        if (T < 0 || !S.IsAlive(T)) {
            Tx = F(WorldWidth.ToInt(), 2);  // world-centre X
            Ty = S.Team[I] == 0 ? WorldHeight : Fixed{0};
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
        if (S.IsBuilding(I)) continue;  // #133: buildings are static (Prev==Pos already)
        if (S.Type[I] == UnitMiner) WorkerSeek(S, I);
        S.PosX[I] = ClampAxis(S.PosX[I] + StepX[I], WorldWidth);
        S.PosY[I] = ClampAxis(S.PosY[I] + StepY[I], WorldHeight);
    }
}

// ---- §5.3 frontier: each team's buildable depth is where its FRONTMOST LIVE PRESENCE stands —
// any alive unit OR building. Team 0 advances up (max Y), team 1 down (min Y). Recomputed from
// scratch every tick from post-Movement Pos, so it is identical on both peers and on the grid/brute
// paths, and safe as hashed state. Gates forward placement (CanPlaceBuilding).
//
// NOT a monotonic high-water mark any more (playtest decision, 2026-07-25). It used to only ever
// extend, which meant ground once touched was yours forever: a raid that died the instant it
// arrived still bought permanent building rights deep in enemy territory, and there was no way to
// take that back. Now the line FOLLOWS your frontmost survivor, so killing whatever is furthest
// forward pushes the enemy's build line back with it — territory becomes contestable, and forward
// buildings are worth what they cost (they hold ground precisely because they are presence, and an
// enemy must destroy them to reclaim it).
//
// The floor is the OPENING frontier: you can be pushed back to your starting depth, never behind
// it. Without that floor a team wiped down to its home base could not rebuild at all, which turns a
// bad fight into an unrecoverable one and takes the comeback out of the game.
void UpdateFrontier(Sim& S) {
    Fixed F0 = S.Cv.InitialFrontier;                 // floors, not seeds
    Fixed F1 = WorldHeight - S.Cv.InitialFrontier;
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        if (S.Team[I] == 0) { if (S.PosY[I] > F0) F0 = S.PosY[I]; }
        else                { if (S.PosY[I] < F1) F1 = S.PosY[I]; }
    }
    S.FrontierT0 = F0;
    S.FrontierT1 = F1;
}

// ---- Phase 4: attacks (damage buffered, applied SIMULTANEOUSLY) ----
void Attacks(Sim& S) {
    if (S.DisableCombat) return;  // LUR_INTERNAL --flockdemo (#97): watch flocking, no kills
    int32_t Dmg[MaxUnits];
    std::memset(Dmg, 0, sizeof(int32_t) * static_cast<size_t>(S.Count));
    for (int32_t I = 0; I < S.Count; ++I) {
        // #134/§7: buildings do NOT fight back (production, not combat) — skip them as
        // attackers BEFORE the Cooldown decrement (buildings reuse Cooldown as the #132
        // spawn-ring counter, which the attack cooldown logic must not touch). They remain
        // valid TARGETS below, taking damage like a stationary enemy unit of their Type.
        if (!S.IsAlive(I) || S.IsBuilding(I) || S.Type[I] == UnitMiner) continue;  // workers ignore combat (spec §5)
        if (S.Cooldown[I] > 0) --S.Cooldown[I];
        if (S.Cooldown[I] > 0) continue;  // still cooling
        const int32_t T = S.Target[I];
        if (T < 0 || !S.IsAlive(T)) continue;
        if (Dist2(S.PosX[I], S.PosY[I], S.PosX[T], S.PosY[T]) > RangeSq(UnitTable[S.Type[I]].Range)) continue;
        int32_t D = S.Units[S.Type[I]].Attack;
        // Counter bonus keys off the target's Type — for a building that is its PRODUCED type,
        // so a Scissor hits a Paper building for the 3x counter (spec §7). Same code, no branch.
        if (UnitTable[S.Type[I]].Beats == S.Type[T]) D *= S.Cv.CounterMultiplier;
        Dmg[T] += D;
        S.Cooldown[I] = UnitTable[S.Type[I]].Cooldown;
    }
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I)) S.Hp[I] -= Dmg[I];  // simultaneous: mutual kills reachable (win rule needs it)
}

// ---- Phase 5: deaths (clear the alive bit; slot kept, no compaction) ----
// #134/§7: a BUILDING at Hp<=0 dies through this same path — its alive bit clears, so it
// stops producing (ProductionBuildings skips dead slots -> its queue "evaporates") and stops
// being a deposit drop-off (NearestMinerBuilding skips it). Stale Queue/BuildProgress on the
// dead slot are inert and reset when the slot is recycled by SpawnUnitAt.
void Deaths(Sim& S) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.Hp[I] <= 0) ClearAlive(S, I);
}

// ---- Phase 6: economy — buffered deposits credit gold ----
void Economy(Sim& S) {
    for (int T = 0; T < 2; ++T) { S.Teams[T].Gold += S.DepositBuf[T]; S.DepositBuf[T] = 0; }
}

// A team with a PAID unit IN PRODUCTION (any alive building with Queue>0) is not doomed: gold is
// deducted at ENQUEUE (ApplyQueue), so that unit will spawn regardless of the current wallet.
// #135 exposed this — with the start-miners gone, a normal opening (place camp, queue miners)
// drops gold below CheapestCost with 0 units for the ~30 ticks before the first miner pops, and
// without this carve-out the win check would wrongly declare a loss/draw and freeze the match.
bool HasPendingProduction(const Sim& S, uint8_t Team) {
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.IsBuilding(I) && S.Team[I] == Team && S.Queue[I] > 0) return true;
    return false;
}

// ---- Phase 7: win check (#136/§12.1, edge-proof; refined by #135) ----
// A team LOSES iff it has NO alive units AND cannot afford even the cheapest unit
// (Gold < CheapestCost = a miner) AND has no PAID unit still building (see above). BUILDINGS DO
// NOT otherwise enter: an idle building can't gather (only miner UNITS mine) and can't produce
// without gold, so with no units, no in-flight production, and no rebuy money the player can
// never make a unit or gold again — doomed, however many buildings stand. A gold-carrying cart
// IS a unit (AliveCount counts it), so stranded-gold needs no special case.
void WinCheck(Sim& S) {
    // #146: the DECISIVE condition — a team whose HOME BASE is destroyed loses at once (the killing
    // blow). Both bases stand from Init, so this can't false-fire pre-match. The old economic
    // exhaustion (no units + can't rebuy + nothing queued) stays only as a STALEMATE net (both
    // sides mined out with no army/base contact), so a starved match still resolves.
    bool HomeAlive[2] = {false, false};
    for (int32_t I = 0; I < S.Count; ++I)
        if (S.IsAlive(I) && S.IsHomeBase(I)) HomeAlive[S.Team[I]] = true;
    // Only apply the home-base rule in a real match — one where home bases exist (Init places both).
    // Synthetic/degenerate fields with NO home base (unit-tests' ClearField scenarios) fall back to
    // the economic net alone, so a base-less test sim never trips a spurious loss/draw.
    const bool AnyHome = HomeAlive[0] || HomeAlive[1];
    bool Lose[2];
    for (uint8_t T = 0; T < 2; ++T)
        Lose[T] = (AnyHome && !HomeAlive[T])  // home base razed = decisive loss
               || (S.AliveCount(T) == 0 && S.Teams[T].Gold < CheapestCost && !HasPendingProduction(S, T));
    if (Lose[0] && Lose[1]) S.Result = ResultDraw;  // both this tick -> draw (simultaneous damage makes it reachable)
    else if (Lose[0]) S.Result = ResultTeam1Wins;
    else if (Lose[1]) S.Result = ResultTeam0Wins;
}

// ---- Phase 0 (#137): apply one input event. Both mutations are deterministic no-ops on an
// invalid/unaffordable request (identical on both peers — pure functions of the hashed state). ----
// EventPlaceBuilding: validate (§5.1), deduct the placement cost, and drop a building slot.
void ApplyPlace(Sim& S, uint8_t Team, uint8_t Type, Fixed X, Fixed Y) {
    if (!S.WouldAcceptPlace(Team, Type, X, Y)) return;   // invalid/unaffordable/gated -> no-op
    const int32_t Cost = BuildingCostFor(S.Cv, Type);
    const int32_t I = AllocSlot(S);
    LUR_ASSERT_MSG(I >= 0, "RPS: slot exhausted placing a building");
    if (I < 0) return;
    S.Teams[Team].Gold -= Cost;
    S.PosX[I] = X;  S.PosY[I] = Y;  S.PrevX[I] = X;  S.PrevY[I] = Y;  // static -> Δ=0
    S.Type[I] = Type;  S.Team[I] = Team;  S.Hp[I] = BuildingHpFor(S.Cv, Type);
    S.Kind[I] = KindBuilding;  S.Queue[I] = 0;  S.BuildProgress[I] = 0;
    S.Target[I] = -1;  S.Cooldown[I] = 0;   // Cooldown = the #132 per-building spawn-ring counter
    S.WorkerState[I] = WorkToMine;  S.Carry[I] = 0;  S.WorkerTimer[I] = 0;
    SetAlive(S, I);
    if (I + 1 > S.Count) S.Count = I + 1;
}
// EventQueueUnits: enqueue up to Count units at an own building, clamped to the queue cap and
// gold — a deterministic PARTIAL enqueue if gold runs out mid-batch (as many as gold covers).
void ApplyQueue(Sim& S, uint8_t Team, int32_t Slot, int32_t Count) {
    if (Slot < 0 || Slot >= S.Count) return;
    if (!S.IsAlive(Slot) || !S.IsBuilding(Slot) || S.Team[Slot] != Team) return;
    if (S.IsHomeBase(Slot)) return;  // #146: the HQ produces nothing (Type is UnitNone — no Units[] index)
    const uint8_t Ty = S.Type[Slot];
    const int32_t UnitCost = S.Units[Ty].Cost;
    const int32_t Cap = S.Cv.BuildingQueueMax;
    for (int32_t K = 0; K < Count; ++K) {
        if (S.Queue[Slot] >= Cap) break;             // queue full -> clamp
        if (S.Teams[Team].Gold < UnitCost) break;    // out of gold -> partial enqueue
        S.Teams[Team].Gold -= UnitCost;
        ++S.Queue[Slot];
    }
}

// #146: auto-place a team's HOME BASE (the HQ) at its baseline centre. Inert structure — no
// production/gathering/attack — with Cv.HomeBaseHp and Type=UnitNone (produces nothing). Called
// twice at Init (slots 0/1). Destroying it wins the match (WinCheck).
void SpawnHomeBase(Sim& S, uint8_t Team) {
    const int32_t I = AllocSlot(S);
    LUR_ASSERT_MSG(I >= 0, "RPS: slot exhausted placing a home base");
    if (I < 0) return;
    // #146/feedback: sit the HQ ~3/4 up the buildable band (which corresponds to the locked bottom
    // camera view) — above the two low gold clusters, clear of the bottom plates — not jammed at
    // the baseline. Depth measured from each team's end, so it's symmetric.
    const Fixed Depth = S.Cv.InitialFrontier * F(3, 4);
    const Fixed X = CampX, Y = Team == 0 ? Depth : WorldHeight - Depth;
    S.PosX[I] = X;  S.PosY[I] = Y;  S.PrevX[I] = X;  S.PrevY[I] = Y;  // static -> Δ=0
    S.Hp[I] = S.Cv.HomeBaseHp;
    S.Type[I] = UnitNone;          // produces nothing (never indexes Units[]; targeting is prey by Kind)
    S.Team[I] = Team;
    S.Kind[I] = KindHomeBase;
    S.Target[I] = -1;  S.Cooldown[I] = 0;  S.Queue[I] = 0;  S.BuildProgress[I] = 0;
    S.WorkerState[I] = WorkToMine;  S.Carry[I] = 0;  S.WorkerTimer[I] = 0;
    SetAlive(S, I);
    if (I + 1 > S.Count) S.Count = I + 1;
}

// The tick body shared by Step(mask) and StepEvents — everything after phase-0 input. Kept as
// one function so the two entry points can never drift in phase order or content (spec §6).
void PreTick(Sim& S) {
    // Cv is per-Sim state (latched at Init + synced overrides, #112); only solo live tuning
    // opts into re-latching from the globals so a console edit moves the running sim.
    if (S.LiveCvLatch) S.Cv = LatchCvs();
    S.DeriveUnits();  // reflect this tick's Cv (Init latch, live-tune, or a synced override)
}
void RunTick(Sim& S) {
    ProductionBuildings(S);   // phase 1 (#132): per-building production (the only production now)
    Grid G;
    { LUR_TRACE_SCOPE("sim.grid"); G.Build(S); }  // after production so spawns are bucketed
    ThreatSet Threat;
    if (S.UseBruteForce) BuildThreatBrute(S, Threat);
    else BuildThreatGrid(S, G, Threat);
    { LUR_TRACE_SCOPE("sim.acq");  TargetAcquire(S, G); }          // phase 2
    { LUR_TRACE_SCOPE("sim.move"); Movement(S, G, Threat); }       // phase 3
    UpdateFrontier(S);        // phase 3b (#133): extend the per-team high-water build line
    { LUR_TRACE_SCOPE("sim.atk");  Attacks(S); }               // phase 4
    Deaths(S);                // phase 5
    Economy(S);               // phase 6
    WinCheck(S);              // phase 7
    ++S.Tick;                 // phase 8 (hash) is computed on demand via StateHash()
}

}  // namespace

void Sim::DeriveUnits() {
    DeriveUnitStats(Cv, Units);  // #122: per-type stats from the latched Cv (shared with the HUD)
    // #123: the flock gather radius = max of every radius tested in the soldier gather, so the
    // grid neighbour box always covers brute's reach (grid==brute) whatever the radii are set to.
    GatherR = Max(Max(Max(Cv.SepRadius, Cv.EnemySepRadius), Max(Cv.CohSameRadius, Cv.CohAllRadius)),
                  Max(Max(Cv.AlignRadius, Cv.PredatorFleeRadius),
                      Max(Cv.InterposeRadius, Cv.BuildingRepelRadius)));  // #133: cover building repel too
}

bool Sim::CanPlaceBuilding(uint8_t Team, uint8_t Type, Fixed X, Fixed Y) const {
    (void)Type;  // one shared footprint for all building types (§12.2); Type reserved for later
    // #157: the rule itself lives in Rps/Placement.h and is shared VERBATIM with the render-thread
    // preview (Snapshot::CanPlaceBuilding). It used to be duplicated here and mirrored there, and
    // the mirror fell out of step the first time the rule changed — the ghost went green where the
    // sim refused. One predicate, two data sources, no lockstep-by-comment.
    return PlacementAccepts(*this, Cv.BuildingFootprint, Cv.MineClearance, Team, X, Y);
}

// #135/§9 opening gate, location-independent: a miner CAMP is always placeable, but SOLDIER
// (non-miner) buildings are locked until the team's first miner UNIT has spawned (a placed camp
// isn't enough). Split out so the HUD can grey out + un-arm a locked plate instead of letting it
// look available and silently refuse every drop. Mirrored by Snapshot::IsBuildingUnlocked — change
// both together.
bool Sim::IsBuildingUnlocked(uint8_t Team, uint8_t Type) const {
    if (Type >= UnitCount || Team > 1) return false;
    if (Type == UnitMiner) return true;
    for (int32_t J = 0; J < Count; ++J)
        if (IsAlive(J) && !IsBuilding(J) && this->Team[J] == Team && this->Type[J] == UnitMiner)
            return true;
    return false;
}

bool Sim::WouldAcceptPlace(uint8_t Team, uint8_t Type, Fixed X, Fixed Y) const {
    if (Type >= UnitCount || Team > 1) return false;
    if (!IsBuildingUnlocked(Team, Type)) return false;          // §9 opening gate
    if (!CanPlaceBuilding(Team, Type, X, Y)) return false;      // spatial validity (§5.1)
    return Teams[Team].Gold >= BuildingCostFor(Cv, Type);       // affordable
}

void Sim::Init(uint64_t InSeed) {
#if !LUR_SHIPPING
    Lur::Core::CVarEnterMain();  // Init always runs post-main; arm the no-read-before-main guard (dev-only)
#endif
    // #112: latch the AffectsGameplay CVars into per-Sim state ONCE, at match start, from
    // the current (default, or solo-console) global values. Thereafter Cv is authoritative
    // Sim state — constant within a tick and mutated ONLY at tick boundaries by synced
    // overrides (LockstepPeer), so two peers in one process hold independent Cv. Hashed.
    InitWithCvs(InSeed, LatchCvs());
}

// #147: the whole of Init downstream of the Cv latch — every initial value DERIVED from Cv is
// built here, so re-running this with a different Cv rebuilds them all. Nothing in here may read
// the global CVars (that is Init's job above), or the peer sync could not converge it.
void Sim::InitWithCvs(uint64_t InSeed, CvSnapshot InCv) {
    *this = Sim{};  // value-init: zeroes every array/field (Init-time, not the hot path)
    Seed = InSeed;
    Cv = InCv;
    DeriveUnits();  // #122: fill Units[] from the just-latched Cv before anything spawns
    // #133/§5.3: seed each team's frontier at the world-space initial buildable depth (NOT
    // pixel-derived). Team 0 builds up to CvInitialFrontier from its baseline (Y=0); team 1
    // down to the same depth from the top (Y=WorldHeight). Advances from here as units push.
    FrontierT0 = Cv.InitialFrontier;
    FrontierT1 = WorldHeight - Cv.InitialFrontier;
    BuildMap(*this);
    // #146: each team's HOME BASE (the HQ) is placed at its baseline centre before tick 0 (slots
    // 0/1). It's the win target — destroy the enemy's to win. Auto-placed (never dragged), so it's
    // there from the first frame while the player still drags mining camps to build the economy.
    for (uint8_t T = 0; T < 2; ++T) SpawnHomeBase(*this, T);
    // PLAYABILITY CHECK. footprint, mine_clearance, the mine rows and the HQ position are all tunable
    // and they compete for the same ground — push two of them far enough and the opening band has no
    // legal spot left at all. That is not an AI bug (a human cannot place either), but it used to be
    // SILENT: the AI just sat there having built nothing, which reads as a broken AI rather than an
    // impossible map. Costs one coarse sweep per match, dev builds only.
    for (uint8_t T = 0; T < 2; ++T) {
        bool Any = false;
        for (int32_t X = 0; X <= WorldWidth.ToInt() && !Any; X += 2)
            for (int32_t Y = 0; Y <= WorldHeight.ToInt() && !Any; Y += 2)
                if (CanPlaceBuilding(T, UnitMiner, F(X), F(Y))) Any = true;
        if (!Any)
            Lur::Log::Info("RPS map: team %u has NO legal opening placement — footprint %d / "
                           "mine_clearance %d / mine rows %d,%d leave no buildable ground. Neither "
                           "the AI nor a player can open; loosen one of them.",
                           static_cast<unsigned>(T), Cv.BuildingFootprint.ToInt(),
                           Cv.MineClearance.ToInt(), Cv.MineRowHome.ToInt(), Cv.MineRowSafe.ToInt());
    }
    // #135/§9: the match starts with ONLY gold — no start-miners, no start buildings. Each team
    // must place its mining camp (the forced first building, ApplyPlace) to begin producing;
    // gold flows once that camp's miners are out and mining. The pre-tick-0 opening is the
    // placement, not a seeded economy.
    for (uint8_t T = 0; T < 2; ++T) Teams[T].Gold = Cv.StartingGold;  // #138/§12.6 opening gold
}

void Sim::StepEvents(const InputEvent* Events, int32_t Count) {
    if (Result != ResultOngoing) return;
    LUR_TRACE_SCOPE("sim.step");
    PreTick(*this);
    // Phase 0: apply the tick's events in array order — deterministic (both peers hold the
    // identical ordered batch for this tick, so the mutations land in the same sequence).
    for (int32_t I = 0; I < Count; ++I) {
        const InputEvent& E = Events[I];
        if (E.Kind == EventPlaceBuilding) ApplyPlace(*this, E.Team, E.Type, Fixed{E.X}, Fixed{E.Y});
        else if (E.Kind == EventQueueUnits) ApplyQueue(*this, E.Team, E.X, E.Y);
    }
    RunTick(*this);
}

#if LUR_INTERNAL
void Sim::StressFill(int32_t PerTeam, uint8_t FillType) {
    Lur::Sim::SplitMix64 R(Seed ^ 0x57A9E55ull);
    const int32_t Wi = WorldWidth.ToInt(), Hi = WorldHeight.ToInt();
    for (uint8_t T = 0; T < 2; ++T) {
        // Team 0 fills the lower half, team 1 the upper — soldiers only (types 1..3).
        const int32_t Y0 = T == 0 ? 4 : Hi / 2;
        for (int32_t K = 0; K < PerTeam; ++K) {
            const int32_t I = AllocSlot(*this);
            if (I < 0) return;  // hit the cap
            // #162: a named type fills with that one; UnitNone keeps the original soldier mix.
            const uint8_t Ty = FillType != UnitNone ? FillType
                                                    : static_cast<uint8_t>(1 + R.NextBounded(3));
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

bool Sim::HasMinerCamp(uint8_t TeamId) const {
    for (int32_t I = 0; I < Count; ++I)
        if (IsAlive(I) && IsBuilding(I) && !IsHomeBase(I) && Team[I] == TeamId && Type[I] == UnitMiner)
            return true;
    return false;
}

int32_t Sim::AliveCount(uint8_t TeamId) const {
    // #136: alive MOBILE units only — buildings are separate entities and never count as army
    // (the win rule, the HUD army count, and the AI's strength read all want units, not camps).
    int32_t C = 0;
    for (int32_t I = 0; I < Count; ++I)
        if (IsAlive(I) && !IsBuilding(I) && Team[I] == TeamId) ++C;
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
    for (int T = 0; T < 2; ++T) Mix(&Teams[T].Gold, sizeof(int32_t));  // #145: per-team state is Gold only now
    Mix(MineGold, sizeof(int32_t) * NumMines);  // mutable reserves (#84) — MineX/Y stay excluded (static)
    Mix(&FrontierT0, sizeof(Fixed));            // #131/§5.3 frontier high-water (gates placement)
    Mix(&FrontierT1, sizeof(Fixed));
    Mix(&Cv, sizeof(Cv));  // #112: latched gameplay-CVar snapshot — a mis-latch surfaces as a desync
    Mix(&Tick, sizeof(uint32_t));
    Mix(&Result, sizeof(uint8_t));
    return H;
}

}  // namespace Rps
