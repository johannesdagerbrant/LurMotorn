#pragma once
// Nearest-point projection into a rectangle minus a set of circular exclusions.
//
// Promoted out of Rps::Snapshot::SnapToValidPlace (#201). The *algorithm* is generic constraint
// projection; the entity walk that finds the exclusions is not, so this takes a callback rather than
// an array — the caller keeps iterating its own buildings/mines/bodies and nothing has to be
// materialised or allocated.
//
// ---- Why iterated projection and not a sample grid ----
// The obvious implementation is to sample a lattice around the input point and pick the nearest valid
// sample. That was tried and rejected on feedback (2026-08-03): a point-anchored lattice moves WITH
// the input, so the discrete "nearest valid sample" hops between frames as the input scrubs, and the
// preview jittered even when the true nearest feasible point (a corner, say) was stationary.
//
// Projection is a CONTINUOUS function of the input: a small move slides the result a little instead of
// teleporting it, and a corner is a hard, stable rest. That property is the reason this code exists,
// and it is the thing to preserve if anyone reimplements it.
//
// ---- Two subtleties worth keeping ----
//  * ONE disc per pass, worst-first. Pushing out of every overlapping disc in a single pass makes them
//    fight and oscillate; resolving the deepest penetration and re-clamping converges.
//  * A point exactly at a disc centre has no outward direction. Picking a fixed axis there is
//    arbitrary but stable — and stable matters more than "correct", because the alternative is a
//    direction that flips with floating-point noise while the user's finger sits still.
//
// Convergence is not guaranteed in a cramped multi-disc pocket, which is why this reports whether it
// settled: the caller must re-validate before trusting the result (RPS does, and falls back to
// rejecting the placement).
#include <cmath>

namespace Lur::Math {

// Axis-aligned bounds. Callers usually inset these by an epsilon so a boundary point cannot round
// back inside a constraint.
struct Rect2 {
    float Xlo = 0.0f, Ylo = 0.0f, Xhi = 0.0f, Yhi = 0.0f;
};

// The most-penetrated exclusion at the current point, as reported by the caller's callback.
struct WorstDisc {
    float Cx = 0.0f, Cy = 0.0f;   // centre
    float R = 0.0f;               // radius the point must be pushed outside of
    float Penetration = 0.0f;     // R - distance(point, centre); <= 0 means "clear"
};

// Project (Px, Py) to (approximately) the nearest point inside Bounds and outside every exclusion.
//
// Worst is called once per pass as `WorstDisc Worst(float X, float Y)` and must report the deepest
// penetrating exclusion at that point, or any struct with Penetration <= 0 when none penetrate.
// Eps pushes a hair past each boundary so a later fixed-point round-trip cannot land back inside.
//
// Returns true if the point settled (clear of every exclusion within MaxIters), false if it ran out —
// in which case Px/Py hold the best effort and the caller must re-validate.
template <class WorstFn>
bool ProjectIntoRectMinusDiscs(float& Px, float& Py, const Rect2& Bounds, WorstFn&& Worst,
                               int MaxIters = 12, float Eps = 0.02f) {
    for (int It = 0; It < MaxIters; ++It) {
        // Nearest point of the rectangle. Alone, this already wedges a corner correctly.
        Px = Px < Bounds.Xlo ? Bounds.Xlo : (Px > Bounds.Xhi ? Bounds.Xhi : Px);
        Py = Py < Bounds.Ylo ? Bounds.Ylo : (Py > Bounds.Yhi ? Bounds.Yhi : Py);

        const WorstDisc W = Worst(Px, Py);
        if (W.Penetration <= 0.0f) return true;   // inside the rect, outside every disc

        float Ux = Px - W.Cx, Uy = Py - W.Cy;
        float Ul = std::sqrt(Ux * Ux + Uy * Uy);
        if (Ul < 1e-4f) { Ux = 0.0f; Uy = 1.0f; Ul = 1.0f; }   // dead centre: fixed axis, see above
        const float Target = W.R + Eps;
        Px = W.Cx + Ux / Ul * Target;
        Py = W.Cy + Uy / Ul * Target;
    }
    // One last clamp so the reported point is at least inside the rectangle.
    Px = Px < Bounds.Xlo ? Bounds.Xlo : (Px > Bounds.Xhi ? Bounds.Xhi : Px);
    Py = Py < Bounds.Ylo ? Bounds.Ylo : (Py > Bounds.Yhi ? Bounds.Yhi : Py);
    return false;
}

}  // namespace Lur::Math
