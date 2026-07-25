// Host tests for the visual springs (Lur::Math::Spring). These smooth what the player SEES, so the
// properties that matter are the ones a wrong solver breaks quietly: it must settle, it must never
// overshoot (a build line that springs past its target reads as a bug), and — because it is a
// closed-form solution in dt — the same motion must arrive at the same place whatever the frame
// rate. A per-frame lerp would fail that last one, which is the reason this module exists.
#include <cmath>
#include <cstdio>

#include "Lur/Math/Spring.h"

using Lur::Math::DoubleSpring;
using Lur::Math::FastNegExp;
using Lur::Math::SpringDamperExact;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// The polynomial stand-in for exp(-x) is accurate where springs actually evaluate it — y*dt, which
// for a 0.2-0.5 s halflife at any sane frame rate is well under 1 — and deliberately loosens
// further out (measured: ~0.019 worst case near x=3.3, i.e. ~2%). That matters only for enormous
// steps, where the spring is already collapsing onto its goal and 2% of a nearly-zero factor is
// invisible. Asserted in two bands so the tight region stays tight if anyone touches the constants.
static void TestFastNegExp() {
    for (float X = 0.0f; X <= 1.0f; X += 0.02f) CHECK(std::fabs(FastNegExp(X) - std::exp(-X)) < 1.0e-3f);
    for (float X = 0.0f; X <= 4.0f; X += 0.05f) CHECK(std::fabs(FastNegExp(X) - std::exp(-X)) < 2.0e-2f);
    CHECK(FastNegExp(0.0f) == 1.0f);
    CHECK(FastNegExp(50.0f) > 0.0f);      // never negative, however large the step
    float Prev = FastNegExp(0.0f);        // and monotonically decreasing, so it can't un-decay
    for (float X = 0.05f; X <= 8.0f; X += 0.05f) {
        const float Cur = FastNegExp(X);
        CHECK(Cur < Prev);
        Prev = Cur;
    }
}

// Halflife means what it claims: after one halflife, about half the distance is gone. Checked on the
// SINGLE spring, where the definition applies directly.
static void TestHalflifeIsHalfTheDistance() {
    float X = 0.0f, V = 0.0f;
    const float Halflife = 0.2f, Dt = 1.0f / 240.0f;
    for (float T = 0.0f; T < Halflife; T += Dt) SpringDamperExact(X, V, 1.0f, Halflife, Dt);
    CHECK(X > 0.4f && X < 0.6f);
}

// The double spring must EASE IN: unlike a single spring, its first frames move only a little, which
// is the whole reason it was chosen for the retracting build line.
static void TestDoubleSpringEasesIn() {
    DoubleSpring D;
    D.Snap(0.0f);
    float Single = 0.0f, SingleV = 0.0f;
    const float Halflife = 0.3f, Dt = 1.0f / 60.0f;
    D.Update(1.0f, Halflife, Dt);
    SpringDamperExact(Single, SingleV, 1.0f, Halflife, Dt);
    CHECK(D.X < Single);        // gentler start than a single spring
    CHECK(D.X >= 0.0f);        // and it does start moving the right way
}

// It settles on the goal, and never passes it (critically damped, both stages).
static void TestDoubleSpringSettlesWithoutOvershoot() {
    DoubleSpring D;
    D.Snap(0.0f);
    const float Dt = 1.0f / 60.0f;
    float Peak = 0.0f;
    for (int I = 0; I < 600; ++I) {   // 10 s at 60 fps
        D.Update(1.0f, 0.25f, Dt);
        if (D.X > Peak) Peak = D.X;
    }
    CHECK(std::fabs(D.X - 1.0f) < 1.0e-3f);   // arrived
    CHECK(Peak <= 1.0f + 1.0e-4f);            // never overshot
    CHECK(std::fabs(D.V) < 1.0e-2f);          // and came to rest
}

// Frame-rate independence — the property a naive lerp lacks. The same elapsed time at 30, 60 and
// 240 fps must land in the same place.
static void TestFrameRateIndependence() {
    auto RunAt = [](float Dt) {
        DoubleSpring D;
        D.Snap(0.0f);
        const int Steps = static_cast<int>(1.0f / Dt);   // one second, whatever the step
        for (int I = 0; I < Steps; ++I) D.Update(1.0f, 0.3f, Dt);
        return D.X;
    };
    const float A = RunAt(1.0f / 30.0f), B = RunAt(1.0f / 60.0f), C = RunAt(1.0f / 240.0f);
    CHECK(std::fabs(A - B) < 5.0e-3f);
    CHECK(std::fabs(B - C) < 5.0e-3f);
}

// Snap is a hard cut: no residual velocity to drag the value away on the next frame. The forward
// (gameplay-exact) direction of the build line depends on this.
static void TestSnapIsInstant() {
    DoubleSpring D;
    D.Snap(0.0f);
    for (int I = 0; I < 10; ++I) D.Update(1.0f, 0.3f, 1.0f / 60.0f);   // build up some velocity
    CHECK(D.V != 0.0f);
    D.Snap(5.0f);
    CHECK(D.X == 5.0f && D.V == 0.0f && D.Xi == 5.0f && D.Vi == 0.0f);
    D.Update(5.0f, 0.3f, 1.0f / 60.0f);
    CHECK(std::fabs(D.X - 5.0f) < 1.0e-6f);   // parked on the goal, it stays put
}

int main() {
    TestFastNegExp();
    TestHalflifeIsHalfTheDistance();
    TestDoubleSpringEasesIn();
    TestDoubleSpringSettlesWithoutOvershoot();
    TestFrameRateIndependence();
    TestSnapIsInstant();
    if (GFailures == 0) { std::printf("spring_tests: ALL PASS\n"); return 0; }
    std::printf("spring_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
