#pragma once
// Rps::PlacementAccepts — the ONE spatial building-placement predicate (#157).
//
// Why this exists: the rule had two implementations. Sim::CanPlaceBuilding is authoritative (it
// gates ApplyPlace), but the drag-place ghost's valid/invalid blink is evaluated on the RENDER
// thread, which cannot touch the live Sim while the SimRunner is ticking it — so Snapshot carried a
// hand-copied mirror over the published fields, with a comment ordering future editors to "change
// both together".
//
// That order was duly broken the first time the rule changed: widening the mine clearance updated
// the Sim and not the mirror, so the ghost turned green 3-6 units from a deposit and the drop was
// then refused. From the player's side the game simply looks broken — the preview is a promise, and
// a preview that lies is worse than no preview.
//
// So the predicate now lives here once, as a template over anything that exposes the board fields.
// Sim and Snapshot both satisfy it because they already name those members identically (Count,
// IsAlive, IsBuilding, PosX/PosY, MineGold, MineX/MineY, FrontierT0/T1); the only genuine difference
// is where the two tunables come from (Sim::Cv vs the snapshot's captured copies), so those are
// passed in. There is no runtime cost — it inlines into each caller exactly as the hand-written
// versions did.
//
// It is SPATIAL ONLY, matching the old split: the opening gate (is this type unlocked) and
// affordability stay with the callers, because the snapshot answers those from captured Gold while
// the sim reads its own.
#include <cstdint>

#include "Lur/Sim/Fixed.h"
#include "Rps/Tunables.h"

namespace Rps {

// int64 squared distance on Fixed raws — overflow-safe, and identical on every caller so the
// comparison can never differ between the preview and the sim.
inline int64_t PlacementDist2(Fixed Ax, Fixed Ay, Fixed Bx, Fixed By) {
    const int64_t Dx = static_cast<int64_t>(Ax.Raw) - Bx.Raw;
    const int64_t Dy = static_cast<int64_t>(Ay.Raw) - By.Raw;
    return Dx * Dx + Dy * Dy;
}

// Fp          = Cv.BuildingFootprint  — drives BOTH the building/building spacing (2x) and the
//               map-edge margin (1.5x, so the whole ICON stays on-map, not just the footprint).
// MineClear   = Cv.MineClearance      — building-centre distance from a LIVE deposit. Deliberately
//               NOT the footprint: the icons draw much larger, so a footprint-sized rule let a camp
//               cover a mine and hide the carts working it.
template <class Board>
bool PlacementAccepts(const Board& B, Fixed Fp, Fixed MineClear, uint8_t Team, Fixed X, Fixed Y) {
    // In-bounds with a margin covering the building's VISUAL extent (~1.5x footprint), so a placed
    // building — and the x1/x5 buttons drawn inside its icon — can never poke off the map edge.
    const Fixed Edge = Fp * F(3, 2);
    if (X.Raw - Edge.Raw < 0 || X + Edge > WorldWidth) return false;
    if (Y.Raw - Edge.Raw < 0 || Y + Edge > WorldHeight) return false;
    // Frontier gate: you cannot build past your own high-water line.
    if (Team == 0) { if (Y > B.FrontierT0) return false; }
    else           { if (Y < B.FrontierT1) return false; }
    // No overlap with another building: shared footprint -> centres must be >= 2*Fp apart.
    const int64_t TwoFp = static_cast<int64_t>(Fp.Raw) + Fp.Raw;
    const int64_t MinBB = TwoFp * TwoFp;
    for (int32_t J = 0; J < B.Count; ++J) {
        if (!B.IsAlive(J) || !B.IsBuilding(J)) continue;
        if (PlacementDist2(X, Y, B.PosX[J], B.PosY[J]) < MinBB) return false;
    }
    // Clearance from a LIVE mine (a depleted deposit is gone, so building over it is allowed).
    const int64_t MinBM = static_cast<int64_t>(MineClear.Raw) * MineClear.Raw;
    for (int32_t M = 0; M < NumMines; ++M) {
        if (B.MineGold[M] <= 0) continue;
        if (PlacementDist2(X, Y, B.MineX[M], B.MineY[M]) < MinBM) return false;
    }
    return true;
}

}  // namespace Rps
