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
    S.WorkerState[I] = WorkToTree;
    S.Carry[I] = 0;
    S.WorkerTimer[I] = 0;
    SetAlive(S, I);
    if (I + 1 > S.Count) S.Count = I + 1;
}

// --- map: v1 is fixed + mirrored; the seed is derived and stored so later
//     variation is free, exactly like chess derives colours from GUIDs. ---
void BuildMap(Sim& S) {
    // Four trees per grove, spread across the 34-wide field.
    const Fixed Xs[TreesPerGrove] = {F(8), F(14), F(20), F(26)};
    // Grove centre lines (portrait, §9): team0 safe/contested near the bottom camp,
    // team1 mirrored near the top. Contested groves sit nearer mid-field (y=30).
    const Fixed GroveY[4] = {F(12), F(24), F(48), F(36)};  // t0-safe, t0-contested, t1-safe, t1-contested
    int32_t Idx = 0;
    for (int G = 0; G < 4; ++G)
        for (int K = 0; K < TreesPerGrove; ++K) {
            S.TreeX[Idx] = Xs[K];
            S.TreeY[Idx] = GroveY[G];
            ++Idx;
        }
}

// ---- Phase 0: apply this tick's inputs (caller passes P0 then P1) ----
void TryEnqueue(Sim& S, uint8_t Team, uint8_t Type) {
    TeamState& T = S.Teams[Team];
    const int32_t Cost = UnitTable[Type].Cost;
    // Queue full or too poor -> the press is DETERMINISTICALLY ignored (a silent
    // no-op is correct here, distinct from an assert-worthy error). No partial
    // reservation: wood is spent only on a successful enqueue.
    if (T.QueueLen >= QueueDepth) return;
    if (T.Wood < Cost) return;
    T.Wood -= Cost;
    T.Queue[T.QueueLen++] = Type;
    if (T.QueueLen == 1) T.BuildTimer = UnitTable[Type].BuildTicks;  // head starts building
}
void ApplyInput(Sim& S, uint8_t Team, uint8_t Mask) {
    // Bit ty of the mask = a press of unit type ty this tick. Same-type presses
    // collapse to one bit (a built-in one-per-button-per-tick rate limit).
    for (uint8_t Ty = 0; Ty < UnitCount; ++Ty)
        if (Mask & (1u << Ty)) TryEnqueue(S, Team, Ty);
}

// ---- Phase 1: production timers; completed units spawn ----
void Production(Sim& S) {
    for (uint8_t T = 0; T < 2; ++T) {
        TeamState& Q = S.Teams[T];
        if (Q.QueueLen == 0) continue;
        if (Q.BuildTimer > 0) --Q.BuildTimer;
        if (Q.BuildTimer <= 0) {
            SpawnUnit(S, T, Q.Queue[0]);
            for (int K = 1; K < Q.QueueLen; ++K) Q.Queue[K - 1] = Q.Queue[K];
            --Q.QueueLen;
            Q.BuildTimer = Q.QueueLen > 0 ? UnitTable[Q.Queue[0]].BuildTicks : 0;
        }
    }
}

// ---- Phase 2: target acquisition for units without a valid target ----
int32_t NearestEnemy(const Sim& S, int32_t I) {
    int32_t Best = -1;
    int64_t BestD = INT64_MAX;
    for (int32_t J = 0; J < S.Count; ++J) {
        if (!S.IsAlive(J) || S.Team[J] == S.Team[I]) continue;
        const int64_t D = Dist2(S.PosX[I], S.PosY[I], S.PosX[J], S.PosY[J]);
        if (D < BestD) { BestD = D; Best = J; }  // strict < : lowest id wins ties (ascending J)
    }
    return Best;
}
int32_t TreeOccupancy(const Sim& S, int32_t Tree) {
    int32_t C = 0;
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J) && S.Type[J] == UnitLumberjack && S.Target[J] == Tree) ++C;
    return C;
}
int32_t NearestFreeTree(const Sim& S, int32_t I) {
    int32_t Best = -1;
    int64_t BestD = INT64_MAX;
    for (int32_t Tr = 0; Tr < NumTrees; ++Tr) {
        if (TreeOccupancy(S, Tr) >= WorkersPerTree) continue;
        const int64_t D = Dist2(S.PosX[I], S.PosY[I], S.TreeX[Tr], S.TreeY[Tr]);
        if (D < BestD) { BestD = D; Best = Tr; }
    }
    return Best;
}
void TargetAcquire(Sim& S) {
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        if (S.Type[I] == UnitLumberjack) {
            if (S.Target[I] < 0) S.Target[I] = NearestFreeTree(S, I);  // set in slot order -> occupancy updates deterministically
        } else {
            const int32_t T = S.Target[I];
            const bool Valid = T >= 0 && S.IsAlive(T) && S.Team[T] != S.Team[I];
            if (!Valid) S.Target[I] = NearestEnemy(S, I);  // else keep target until death (hysteresis, no dithering)
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
        case WorkChop:
            if (S.WorkerTimer[I] > 0) --S.WorkerTimer[I];
            if (S.WorkerTimer[I] <= 0) { S.Carry[I] = CarryCapacity; S.WorkerState[I] = WorkToCamp; }
            return;  // stationary while chopping
        case WorkToTree: {
            const int32_t Tr = S.Target[I];
            if (Tr < 0) return;  // no free tree this tick — idle
            const Fixed Tx = S.TreeX[Tr], Ty = S.TreeY[Tr];
            if (Arrived(S, I, Tx, Ty)) {
                S.PosX[I] = Tx; S.PosY[I] = Ty;
                S.WorkerState[I] = WorkChop; S.WorkerTimer[I] = ChopTicks;
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
                S.Carry[I] = 0; S.Target[I] = -1; S.WorkerState[I] = WorkToTree;
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
void Separation(const Sim& S, int32_t I, Fixed& Sx, Fixed& Sy) {
    Sx = Fixed{0}; Sy = Fixed{0};
    const int64_t R2 = RangeSq(SeparationRadius);
    for (int32_t J = 0; J < S.Count; ++J) {
        if (J == I || !S.IsAlive(J) || S.Team[J] != S.Team[I]) continue;
        const int64_t Dx = static_cast<int64_t>(S.PrevX[I].Raw) - S.PrevX[J].Raw;
        const int64_t Dy = static_cast<int64_t>(S.PrevY[I].Raw) - S.PrevY[J].Raw;
        const int64_t D2 = Dx * Dx + Dy * Dy;
        if (D2 == 0 || D2 >= R2) continue;  // exact overlap (skip; no det. direction) or out of range
        // Offset is within radius, so |Dx|,|Dy| < One: reinterpreting as a Fixed Raw is safe.
        Sx = Sx + Fixed{static_cast<int32_t>(Dx)} * SeparationStrength;
        Sy = Sy + Fixed{static_cast<int32_t>(Dy)} * SeparationStrength;
    }
}
void Movement(Sim& S) {
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I)) continue;
        Fixed Sx, Sy;
        Separation(S, I, Sx, Sy);  // from Prev — before we touch Pos
        if (S.Type[I] == UnitLumberjack) WorkerSeek(S, I);
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
        if (!S.IsAlive(I) || S.Type[I] == UnitLumberjack) continue;  // workers ignore combat in v1 (spec §5)
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

// ---- Phase 6: economy — buffered deposits credit wood ----
void Economy(Sim& S) {
    for (int T = 0; T < 2; ++T) { S.Teams[T].Wood += S.DepositBuf[T]; S.DepositBuf[T] = 0; }
}

// ---- Phase 7: win check (spec §6, edge-proof) ----
void WinCheck(Sim& S) {
    bool Lose[2];
    for (uint8_t T = 0; T < 2; ++T)
        Lose[T] = S.AliveCount(T) == 0 && S.Teams[T].QueueLen == 0 && S.Teams[T].Wood < CheapestCost;
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
        Teams[T].Wood = StartWood;
        for (int K = 0; K < StartLumberjacks; ++K) SpawnUnit(*this, T, UnitLumberjack);
    }
}

void Sim::Step(uint8_t Mask0, uint8_t Mask1) {
    if (Result != ResultOngoing) return;  // match decided: freeze (still deterministic on both peers)

    for (int32_t I = 0; I < Count; ++I) { PrevX[I] = PosX[I]; PrevY[I] = PosY[I]; }

    ApplyInput(*this, 0, Mask0);  // phase 0: P0 then P1
    ApplyInput(*this, 1, Mask1);
    Production(*this);            // phase 1
    TargetAcquire(*this);         // phase 2
    Movement(*this);              // phase 3
    Attacks(*this);               // phase 4
    Deaths(*this);                // phase 5
    Economy(*this);               // phase 6
    WinCheck(*this);              // phase 7
    ++Tick;                       // phase 8 (hash) is computed on demand via StateHash()
}

int32_t Sim::AliveCount(uint8_t TeamId) const {
    int32_t C = 0;
    for (int32_t I = 0; I < Count; ++I)
        if (IsAlive(I) && Team[I] == TeamId) ++C;
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
        Mix(&Q.Wood, sizeof(int32_t));
        Mix(Q.Queue, static_cast<size_t>(Q.QueueLen));
        Mix(&Q.QueueLen, sizeof(int32_t));
        Mix(&Q.BuildTimer, sizeof(int32_t));
        Mix(&Q.SpawnCounter, sizeof(int32_t));
    }
    Mix(&Tick, sizeof(uint32_t));
    Mix(&Result, sizeof(uint8_t));
    return H;
}

}  // namespace Rps
