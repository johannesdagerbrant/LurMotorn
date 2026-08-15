#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "Lur/Sim/Fixed.h"
#include "Rps/Placement.h"   // #157: the ONE shared placement predicate (no more sim/preview mirror)
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
    int32_t  Queue[MaxUnits];   // #140 per-building units queued (0 for units) — the N in "N/max"
    int32_t  BuildProgress[MaxUnits];  // #140 per-building next-unit progress (ticks) — the progress bar
    uint64_t AliveBits[(MaxUnits + 63) / 64];
    // Per-slot entity identity (Sim::Serial): unique per creation, never reused. The view's
    // rollback smoothing needs it to tell "this slot's occupant moved" from "this slot has a new
    // occupant" — a slot index alone cannot, because a rollback reallocates every spawn made
    // inside the resim window. Without it a fresh unit eases in from its predecessor's position.
    uint32_t Serial[MaxUnits] = {};

    // Mine positions (constant after Init, carried here so the view needs nothing
    // else) + live reserves (#84: a mine with MineGold <= 0 is gone — don't draw it).
    Fixed   MineX[NumMines];
    Fixed   MineY[NumMines];
    int32_t MineGold[NumMines];

    // #139/#141: per-team frontier high-water (the build line) + the shared building footprint
    // radius, carried so the view can draw the frontier lines and size the placement ghost.
    Fixed    FrontierT0{};
    Fixed    FrontierT1{};
    // #157: the WHOLE latched gameplay CVar block, copied in one assignment, rather than a handful
    // of cherry-picked fields. The cherry-picking was the actual defect behind the ghost/sim
    // disagreement: adding a placement tunable to the sim left a second place that had to be
    // remembered, and it wasn't. With the block carried wholesale, any knob the shared predicate
    // (Rps/Placement.h) reads is present here automatically and cannot fall out of step. It is a
    // POD of ints/Fixed — a few hundred bytes against this struct's ~90 KB, so the copy is noise.
    CvSnapshot Cv{};

    // HUD / overlay counters (read via this same hand-off, never from the live Sim).
    uint32_t Tick = 0;
    uint8_t  Result = 0;              // EResult
    int32_t  Gold[2] = {};
    int32_t  AliveCount[2] = {};
    int32_t  BuildingQueueMax = 0;   // #140 shared per-building queue cap (the "max" in "N/max")

    // Live per-type stats (#122): the sim's Cv-derived Units[] (cost/hp/speed/damage/build),
    // so the HUD's cost label, affordability, build bar, and health-bar scale track a tuned
    // CVar instead of the compile-time UnitTable default. Defaulted to UnitTable (see the ctor)
    // so a PRE-MATCH / unpublished snapshot still shows real costs, not zeros — a live match
    // overwrites it from the latched (and synced) Sim::Units in CaptureFrom.
    UnitStats Units[UnitCount];
    // #139/#140: gold to PLACE a building of each type + the building's max HP (distinct from the
    // unit cost/HP in Units[]), so the plates show the placement price and buildings get a health
    // bar scaled to building HP.
    int32_t BuildingCost[UnitCount] = {};
    int32_t BuildingMaxHp[UnitCount] = {};
    int32_t HomeBaseMaxHp = 0;   // #146: the HQ's max HP (Type is UnitNone, so not in BuildingMaxHp[])

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
        std::memcpy(Queue, S.Queue, sizeof(int32_t) * N);                  // #140 per-building queue
        std::memcpy(BuildProgress, S.BuildProgress, sizeof(int32_t) * N);  // #140 per-building progress
        std::memcpy(AliveBits, S.AliveBits, sizeof(uint64_t) * ((N + 63) / 64));
        std::memcpy(Serial, S.Serial, sizeof(uint32_t) * N);
        std::memcpy(MineX, S.MineX, sizeof(Fixed) * NumMines);
        std::memcpy(MineY, S.MineY, sizeof(Fixed) * NumMines);
        std::memcpy(MineGold, S.MineGold, sizeof(int32_t) * NumMines);
        FrontierT0 = S.FrontierT0;
        FrontierT1 = S.FrontierT1;
        Cv = S.Cv;   // #157: one line, so no future placement knob can be forgotten here
        Tick = S.Tick;
        Result = S.Result;
        for (int T = 0; T < 2; ++T) {
            Gold[T] = S.Teams[T].Gold;
            AliveCount[T] = S.AliveCount(static_cast<uint8_t>(T));
        }
        BuildingQueueMax = S.Cv.BuildingQueueMax;
        std::memcpy(Units, S.Units, sizeof(Units));  // #122: live per-type stats for the HUD
        for (int K = 0; K < UnitCount; ++K) {
            BuildingCost[K] = BuildingCostFor(S.Cv, static_cast<uint8_t>(K));   // #139/#140 placement price
            BuildingMaxHp[K] = BuildingHpFor(S.Cv, static_cast<uint8_t>(K));    // #140 building health-bar scale
        }
        HomeBaseMaxHp = S.Cv.HomeBaseHp;   // #146 HQ health-bar scale
        PublishNs = InPublishNs;
        StepNs = InStepNs;
    }

    bool IsAlive(int32_t I) const { return (AliveBits[I >> 6] >> (I & 63)) & 1ull; }
    // #139/#146: any static structure (producing building OR the home base) — matches Sim::IsBuilding.
    bool IsBuilding(int32_t I) const { return Kind[I] != KindUnit; }
    bool IsHomeBase(int32_t I) const { return Kind[I] == KindHomeBase; }  // #146 the HQ
    // Has the match started? Mirrors Sim::IsPreMatch — the clock is held until the opening camp, so
    // the first tick IS the start. Use this, not HasMinerCamp, for anything meaning "before the match
    // begins": a player who loses every camp mid-match still owns none, and keying the camera lock on
    // that yanked the view back to the baseline the moment a raid took their last one. Same mistake
    // froze the sim outright (see Sim::IsPreMatch).
    bool IsPreMatch() const { return Tick == 0; }
    // Has this team placed its first mining CAMP (not the HQ)? The mains lock the camera at the
    // baseline until the local team commits its first camp (mirrors Sim::HasMinerCamp).
    bool HasMinerCamp(uint8_t T) const {
        for (int32_t I = 0; I < Count; ++I)
            if (IsAlive(I) && IsBuilding(I) && !IsHomeBase(I) && Team[I] == T && Type[I] == UnitMiner)
                return true;
        return false;
    }

    // #148 magnetic placement: snap a desired drop (Dx,Dy world) to the NEAREST valid build spot
    // within Radius world units (≈ the building icon size), so placement isn't pixel-perfect
    // tedious. Returns true + the snapped (Ox,Oy) — the desired point itself when it's already
    // valid; false (and Ox/Oy = desired) when nothing valid is within the radius, so the caller
    // blinks red there. View-side only: the resulting Fixed travels on the wire and ApplyPlace
    // re-validates, so a snapped point the sim later rejects is a safe no-op.
    bool SnapToValidPlace(uint8_t Team, uint8_t Type, float Dx, float Dy, float Radius,
                          float& Ox, float& Oy) const {
        auto Fx = [](float V) {
            return Fixed{static_cast<int32_t>(V * static_cast<float>(Fixed::One) + (V < 0 ? -0.5f : 0.5f))};
        };
        if (WouldAcceptPlace(Team, Type, Fx(Dx), Fx(Dy))) { Ox = Dx; Oy = Dy; return true; }
        // Nothing valid under the finger: snap to the NEAREST valid spot within Radius.
        //
        // This is done ANALYTICALLY, not by sampling a grid around the finger, and that is the whole
        // point (feedback 2026-08-03). A finger-anchored sample lattice moves WITH the thumb, so the
        // discrete "nearest valid sample" hops between frames as you scrub — the ghost jittered even
        // though the true nearest feasible point (e.g. a screen corner) was stationary. Projecting the
        // finger onto the feasible region is a CONTINUOUS function of the finger: a small thumb move
        // slides the ghost a little instead of teleporting it, and a corner is a hard, stable rest.
        //
        // The feasible region is the legal rectangle (map margin + your frontier half-plane) minus a
        // disc around every building (centres must be ≥ 2*Fp apart) and every live mine (≥ MineClear).
        // Iterated projection converges to the nearest point of that region: clamp into the rectangle
        // (its exact nearest point — this alone wedges a corner), then push out of the single most-
        // penetrated disc, and repeat. One disc per pass, worst-first, so overlapping exclusions
        // resolve instead of fighting. Bounds are inset by a hair so the Fixed round-trip below can't
        // land a boundary point back inside a constraint.
        auto ToF = [](Fixed V) { return static_cast<float>(V.Raw) / static_cast<float>(Fixed::One); };
        constexpr float Eps = 0.02f;                       // > one Fixed quantum, << every feature size
        const float Fpf = ToF(Cv.BuildingFootprint);
        const float Edge = Fpf * 1.5f;                     // matches PlacementAccepts' visual-extent margin
        const float Xlo = Edge + Eps, Xhi = ToF(WorldWidth) - Edge - Eps;
        float Ylo = Edge + Eps, Yhi = ToF(WorldHeight) - Edge - Eps;
        if (Team == 0) Yhi = std::min(Yhi, ToF(FrontierT0) - Eps);   // build no further than your line
        else           Ylo = std::max(Ylo, ToF(FrontierT1) + Eps);
        const float BR = 2.0f * Fpf, MR = ToF(Cv.MineClearance);     // building / mine exclusion radii
        float Px = Dx, Py = Dy;
        for (int It = 0; It < 12; ++It) {
            Px = std::min(std::max(Px, Xlo), Xhi);         // nearest point of the legal rectangle
            Py = std::min(std::max(Py, Ylo), Yhi);
            float WorstPen = 0.0f, Wcx = 0.0f, Wcy = 0.0f, Wr = 0.0f;
            auto Consider = [&](float Cx, float Cy, float R) {
                const float Dx2 = Px - Cx, Dy2 = Py - Cy;
                const float Pen = R - std::sqrt(Dx2 * Dx2 + Dy2 * Dy2);
                if (Pen > WorstPen) { WorstPen = Pen; Wcx = Cx; Wcy = Cy; Wr = R; }
            };
            for (int32_t J = 0; J < Count; ++J)
                if (IsAlive(J) && IsBuilding(J)) Consider(ToF(PosX[J]), ToF(PosY[J]), BR);
            for (int M = 0; M < NumMines; ++M)
                if (MineGold[M] > 0) Consider(ToF(MineX[M]), ToF(MineY[M]), MR);
            if (WorstPen <= 0.0f) break;                   // inside the rect, outside every disc → done
            float Ux = Px - Wcx, Uy = Py - Wcy, Ul = std::sqrt(Ux * Ux + Uy * Uy);
            if (Ul < 1e-4f) { Ux = 0.0f; Uy = 1.0f; Ul = 1.0f; }   // dead-centre: pick a fixed axis
            const float Target = Wr + Eps;
            Px = Wcx + Ux / Ul * Target;                   // push radially out to the disc boundary
            Py = Wcy + Uy / Ul * Target;
        }
        // Accept the projected point only if it is genuinely valid (a cramped multi-disc pocket can
        // leave the iteration short of feasible) AND within the magnetic radius. Otherwise fall back to
        // the desired point and blink red — same contract as before.
        const float PD2 = (Px - Dx) * (Px - Dx) + (Py - Dy) * (Py - Dy);
        if (PD2 <= Radius * Radius && WouldAcceptPlace(Team, Type, Fx(Px), Fx(Py))) {
            Ox = Px; Oy = Py; return true;
        }
        Ox = Dx; Oy = Dy; return false;
    }

    // int64 squared distance on Fixed raws (matches Sim.cpp's Dist2) — overflow-safe.
    static int64_t Dist2Raw(Fixed Ax, Fixed Ay, Fixed Bx, Fixed By) {
        const int64_t Dx = static_cast<int64_t>(Ax.Raw) - Bx.Raw;
        const int64_t Dy = static_cast<int64_t>(Ay.Raw) - By.Raw;
        return Dx * Dx + Dy * Dy;
    }

    // #139 RENDER-THREAD placement preview. The SimRunner ticks the sim on its own thread, so the
    // drag-place ghost's valid/invalid blink (evaluated on the render thread) can't call the live
    // Sim — it answers over the published snapshot instead.
    //
    // #157: this is no longer a hand-written MIRROR of the sim's predicate. It calls the SAME
    // function (Rps::PlacementAccepts) over the same captured CVar block, so "change both together"
    // is no longer a rule anyone can forget — there is only one implementation to change. The data
    // is still a snapshot (it must be: the render thread cannot read a ticking Sim), so the preview
    // can be one tick stale; it can no longer be WRONG.
    // §9 opening gate as a LOCATION-INDEPENDENT question: is this building type unlocked for the
    // team at all? A miner CAMP always is; SOLDIER buildings only once the team's first miner UNIT
    // has spawned (a placed camp isn't enough). Split out so the HUD can grey out and un-arm a
    // locked plate — a locked building used to look identical to an available one and just silently
    // refuse every drop, which reads as the game being broken (feedback 2026-07-25).
    bool IsBuildingUnlocked(uint8_t PlaceTeam, uint8_t PlaceType) const {
        if (PlaceType >= UnitCount || PlaceTeam > 1) return false;
        if (PlaceType == UnitMiner) return true;
        for (int32_t J = 0; J < Count; ++J)
            if (IsAlive(J) && !IsBuilding(J) && Team[J] == PlaceTeam && Type[J] == UnitMiner) return true;
        return false;
    }

    bool WouldAcceptPlace(uint8_t PlaceTeam, uint8_t PlaceType, Fixed X, Fixed Y) const {
        if (PlaceType >= UnitCount || PlaceTeam > 1) return false;
        if (!IsBuildingUnlocked(PlaceTeam, PlaceType)) return false;      // §9 opening gate
        if (!CanPlaceBuilding(PlaceTeam, PlaceType, X, Y)) return false;  // spatial validity (§5.1)
        return Gold[PlaceTeam] >= BuildingCostFor(Cv, PlaceType);         // affordable
    }

    // Spatial-only placement validity — the SHARED predicate (§5.1/§5.3), not a copy of it.
    bool CanPlaceBuilding(uint8_t PlaceTeam, uint8_t PlaceType, Fixed X, Fixed Y) const {
        (void)PlaceType;  // one shared footprint for all building types (§12.2)
        return PlacementAccepts(*this, Cv.BuildingFootprint, Cv.MineClearance, PlaceTeam, X, Y);
    }

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
