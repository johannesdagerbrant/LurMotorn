#pragma once
// Lur::Math::Spring — critically-damped springs for VISUAL smoothing, after Daniel Holden's
// "Spring-It-On / Spring Roll Call" (https://theorangeduck.com/page/spring-roll-call#doublespring).
//
// These live in Modules/Math, which is the FLOAT/render half of the engine by charter: a spring
// here must never touch simulation state. Gameplay stays on `Modules/Sim`'s Fixed + fixed timestep,
// so a spring cannot desync two peers — each device is free to smooth its own picture differently,
// and the sim never learns the spring exists.
//
// Why the EXACT solutions rather than a per-frame lerp toward a target:
//   * Frame-rate independent by construction (closed form in dt), so a hitch does not change the
//     motion, and the same code behaves identically at 30, 60 or 120 fps.
//   * Parameterised by HALFLIFE (seconds to cover half the remaining distance) instead of an
//     opaque stiffness/damping pair — a designer-legible number.
//   * Critically damped, so there is never overshoot or ringing.
//
// The DOUBLE spring chains two critically-damped springs, each at half the halflife: the first
// carries an intermediate value toward the goal, the second follows the first. The result is a
// smooth-in AND smooth-out S-curve — motion that starts gently instead of snapping away on frame
// one, which is exactly what a single spring cannot do.
#include <cstddef>

namespace Lur::Math {

// exp(-X) for X >= 0, to a few decimals, without libm. The springs evaluate this every frame per
// tracked value; the polynomial is ~an order of magnitude cheaper than std::exp and the error
// (<1e-3 over the range springs actually use) is invisible in motion.
inline float FastNegExp(float X) {
    return 1.0f / (1.0f + X + 0.48f * X * X + 0.235f * X * X * X);
}

// Halflife (seconds to close half the gap) -> the damping term the exact solution wants.
// 4*ln(2) is the critical-damping constant from the reference; Eps keeps halflife 0 finite
// (halflife 0 then means "snap", which is the useful degenerate case, not a divide by zero).
inline float HalflifeToDamping(float Halflife, float Eps = 1.0e-5f) {
    return (4.0f * 0.69314718056f) / (Halflife + Eps);
}

// One critically-damped spring step, exact for this dt: X converges on Goal, V is its velocity and
// is carried between frames. Both are in/out.
inline void SpringDamperExact(float& X, float& V, float Goal, float Halflife, float DtSec) {
    const float Y = HalflifeToDamping(Halflife) * 0.5f;
    const float J0 = X - Goal;
    const float J1 = V + J0 * Y;
    const float Eydt = FastNegExp(Y * DtSec);
    X = Eydt * (J0 + J1 * DtSec) + Goal;
    V = Eydt * (V - J1 * Y * DtSec);
}

// A double spring's whole state: the visible value (X,V) chasing an intermediate one (Xi,Vi) that
// chases the goal. POD, four floats — cheap enough to keep one per tracked quantity.
struct DoubleSpring {
    float X = 0.0f, V = 0.0f;    // the value to DRAW, and its velocity
    float Xi = 0.0f, Vi = 0.0f;  // intermediate stage (the reason this eases IN as well as out)

    // Park the whole spring at Value with no motion — for the first frame, or any time the visual
    // should jump rather than travel (a new match, a re-anchored camera, a gameplay-exact move).
    void Snap(float Value) {
        X = Xi = Value;
        V = Vi = 0.0f;
    }

    // Advance toward Goal. Halflife is the FULL halflife; each internal stage gets half of it, as
    // the reference prescribes, so the pair still settles in about the time the number implies.
    void Update(float Goal, float Halflife, float DtSec) {
        const float Stage = 0.5f * Halflife;
        SpringDamperExact(Xi, Vi, Goal, Stage, DtSec);
        SpringDamperExact(X, V, Xi, Stage, DtSec);
    }
};

}  // namespace Lur::Math
