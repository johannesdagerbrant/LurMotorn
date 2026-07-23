#pragma once
#include <cstdint>
#include <cstring>
#include <mutex>

#include "Lur/Sim/Fixed.h"
#include "Rps/Sim.h"
#include "Rps/Tunables.h"

namespace Rps {

using Lur::Sim::Fixed;

// The render-facing view of one sim tick — the ONLY data that crosses the
// tick-thread -> render-thread boundary (the symmetric counterpart of #40's
// EventInbox, which crosses radio -> engine). It carries just what the instanced
// draw and the HUD need: the two interpolation endpoints (Prev, Pos) plus per-unit
// draw attributes, and a few scalar counters. Interpolation happens in the vertex
// shader (mix(prev, curr, alpha)); the CPU never touches a float per unit — the
// only float is the single alpha, computed on the read side (AlphaAt), so floats
// stay quarantined to the view.
struct Snapshot {
    int32_t  Count = 0;
    Fixed    PrevX[MaxUnits];
    Fixed    PrevY[MaxUnits];
    Fixed    PosX[MaxUnits];
    Fixed    PosY[MaxUnits];
    uint8_t  Type[MaxUnits];
    uint8_t  Team[MaxUnits];
    uint8_t  Kind[MaxUnits];    // #139 EKind — KindBuilding renders as a placed building, not a unit
    int32_t  Hp[MaxUnits];
    int32_t  Carry[MaxUnits];   // miner gold in hand — the view's deposit-flash edge
    uint64_t AliveBits[(MaxUnits + 63) / 64];

    // Mine positions (constant after Init, carried here so the view needs nothing
    // else) + live reserves (#84: a mine with MineGold <= 0 is gone — don't draw it).
    Fixed   MineX[NumMines];
    Fixed   MineY[NumMines];
    int32_t MineGold[NumMines];

    // #139/#141: per-team frontier high-water (the build line) + the shared building footprint
    // radius, carried so the view can draw the frontier lines and size the placement ghost.
    Fixed    FrontierT0{};
    Fixed    FrontierT1{};
    Fixed    BuildingFootprint{};

    // HUD / overlay counters (read via this same hand-off, never from the live Sim).
    uint32_t Tick = 0;
    uint8_t  Result = 0;              // EResult
    int32_t  Gold[2] = {};
    // Per-team, per-produced-type queue totals for the HUD — now AGGREGATED over that team's
    // buildings (#132/#145: production is per-building; there are no per-team camp queues). Sum
    // of Queue across a team's buildings of each type; BuildProgress is the max in-flight bar.
    int32_t  QueueCount[2][UnitCount] = {};
    int32_t  BuildProgress[2][UnitCount] = {};
    int32_t  AliveCount[2] = {};

    // Live per-type stats (#122): the sim's Cv-derived Units[] (cost/hp/speed/damage/build),
    // so the HUD's cost label, affordability, build bar, and health-bar scale track a tuned
    // CVar instead of the compile-time UnitTable default. Defaulted to UnitTable (see the ctor)
    // so a PRE-MATCH / unpublished snapshot still shows real costs, not zeros — a live match
    // overwrites it from the latched (and synced) Sim::Units in CaptureFrom.
    UnitStats Units[UnitCount];

    Snapshot() { std::memcpy(Units, UnitTable, sizeof(Units)); }

    // For interpolation: when this tick was published (steady clock ns) and the sim
    // step duration. The render thread times alpha itself from these — the tick
    // thread never exposes its accumulator.
    uint64_t PublishNs = 0;
    uint64_t StepNs = 0;

    // Copy the render-relevant subset out of the live sim (producer side, unlocked).
    void CaptureFrom(const Sim& S, uint64_t InPublishNs, uint64_t InStepNs) {
        Count = S.Count;
        const size_t N = static_cast<size_t>(S.Count);
        std::memcpy(PrevX, S.PrevX, sizeof(Fixed) * N);
        std::memcpy(PrevY, S.PrevY, sizeof(Fixed) * N);
        std::memcpy(PosX, S.PosX, sizeof(Fixed) * N);
        std::memcpy(PosY, S.PosY, sizeof(Fixed) * N);
        std::memcpy(Type, S.Type, N);
        std::memcpy(Team, S.Team, N);
        std::memcpy(Kind, S.Kind, N);
        std::memcpy(Hp, S.Hp, sizeof(int32_t) * N);
        std::memcpy(Carry, S.Carry, sizeof(int32_t) * N);
        std::memcpy(AliveBits, S.AliveBits, sizeof(uint64_t) * ((N + 63) / 64));
        std::memcpy(MineX, S.MineX, sizeof(Fixed) * NumMines);
        std::memcpy(MineY, S.MineY, sizeof(Fixed) * NumMines);
        std::memcpy(MineGold, S.MineGold, sizeof(int32_t) * NumMines);
        FrontierT0 = S.FrontierT0;
        FrontierT1 = S.FrontierT1;
        BuildingFootprint = S.Cv.BuildingFootprint;
        Tick = S.Tick;
        Result = S.Result;
        for (int T = 0; T < 2; ++T) {
            Gold[T] = S.Teams[T].Gold;
            for (int K = 0; K < UnitCount; ++K) { QueueCount[T][K] = 0; BuildProgress[T][K] = 0; }
            AliveCount[T] = S.AliveCount(static_cast<uint8_t>(T));
        }
        // Aggregate per-building queues into the per-type HUD totals (#145): sum queued units,
        // keep the furthest-along build bar per type. Buildings share the [0,Count) slot space.
        for (int32_t I = 0; I < S.Count; ++I) {
            if (!S.IsAlive(I) || !S.IsBuilding(I)) continue;
            const int T = S.Team[I], K = S.Type[I];
            if (T < 0 || T >= 2 || K < 0 || K >= UnitCount) continue;
            QueueCount[T][K] += S.Queue[I];
            if (S.BuildProgress[I] > BuildProgress[T][K]) BuildProgress[T][K] = S.BuildProgress[I];
        }
        std::memcpy(Units, S.Units, sizeof(Units));  // #122: live per-type stats for the HUD
        PublishNs = InPublishNs;
        StepNs = InStepNs;
    }

    bool IsAlive(int32_t I) const { return (AliveBits[I >> 6] >> (I & 63)) & 1ull; }
    bool IsBuilding(int32_t I) const { return Kind[I] == KindBuilding; }  // #139

    // Fixed-timestep interpolation factor at render time NowNs. Clamps to [0,1] — no
    // extrapolation: if the next tick is late (sim stalled), it holds at Pos, which is
    // exactly the "freeze gracefully" behaviour the netcode wants at the ceiling.
    float AlphaAt(uint64_t NowNs) const {
        if (StepNs == 0 || NowNs <= PublishNs) return 0.0f;
        const uint64_t D = NowNs - PublishNs;
        if (D >= StepNs) return 1.0f;
        return static_cast<float>(D) / static_cast<float>(StepNs);
    }
};

// Double-buffered hand-off, single-producer (tick thread) / single-consumer (render
// thread). The producer fills the back buffer UNLOCKED (the ~90 KB CaptureFrom must
// not block the render thread), then Publish() flips the front/back indices under a
// short lock. The consumer copies the front buffer out under the same lock. Front and
// back are always different, so the only thing the lock guards is the index swap and
// the consumer's copy — the same "copy under lock, heavy work outside" shape as
// EventInbox. A lock-free triple buffer is a drop-in upgrade if the 10 Hz publish ever
// contends the render thread (it won't).
class SnapshotMailbox {
public:
    // Producer: write here, then Publish().
    Snapshot& Back() { return Buffers[BackIdx]; }

    void Publish() {
        std::lock_guard<std::mutex> Lock(Mutex);
        FrontIdx = BackIdx;
        BackIdx = 1 - BackIdx;
        HasPublished = true;
    }

    // Consumer: copies the latest published snapshot into Out. False until the first
    // Publish(). Copy is under the lock so Front can't flip mid-copy (2-buffer safe).
    bool Consume(Snapshot& Out) const {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (!HasPublished) return false;
        Out = Buffers[FrontIdx];
        return true;
    }

private:
    mutable std::mutex Mutex;
    Snapshot Buffers[2];
    int FrontIdx = 0;
    int BackIdx = 0;   // starts equal to Front; first Publish() moves Front onto it and flips
    bool HasPublished = false;
};

} // namespace Rps
