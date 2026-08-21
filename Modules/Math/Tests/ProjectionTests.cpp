// Tests for Lur::Math::ProjectIntoRectMinusDiscs — the nearest-feasible-point projection promoted
// out of RPS's magnetic placement (#201).
//
// The property that matters is CONTINUITY, not just "lands somewhere legal": the whole reason this is
// iterated projection instead of a sample lattice is that a small input move must produce a small
// output move. A test that only checked legality would pass against the jittering grid version this
// replaced, so there is an explicit continuity test below.
#include <cmath>
#include <cstdio>
#include <vector>

#include "Lur/Math/Projection.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Math::ProjectIntoRectMinusDiscs;
using Lur::Math::Rect2;
using Lur::Math::WorstDisc;

namespace {

struct Disc { float X, Y, R; };

// The callback shape a caller supplies: walk your own entities, report the deepest penetration.
struct Field {
    std::vector<Disc> Discs;
    WorstDisc operator()(float Px, float Py) const {
        WorstDisc W;
        for (const Disc& D : Discs) {
            const float Dx = Px - D.X, Dy = Py - D.Y;
            const float Pen = D.R - std::sqrt(Dx * Dx + Dy * Dy);
            if (Pen > W.Penetration) W = WorstDisc{D.X, D.Y, D.R, Pen};
        }
        return W;
    }
};

bool Clear(const Field& F, float X, float Y, float Slack = 1e-3f) {
    return F(X, Y).Penetration <= Slack;
}

}  // namespace

// ---- Already feasible: the point must not move ----
static void TestFeasiblePointIsUntouched() {
    const Rect2 B{0.0f, 0.0f, 10.0f, 10.0f};
    Field F{{{2.0f, 2.0f, 1.0f}}};
    float X = 8.0f, Y = 8.0f;
    CHECK(ProjectIntoRectMinusDiscs(X, Y, B, F));
    CHECK(X == 8.0f && Y == 8.0f);
}

// ---- Outside the rectangle: clamp to its nearest point, including the corner case ----
static void TestRectangleClamp() {
    const Rect2 B{0.0f, 0.0f, 10.0f, 10.0f};
    Field F{};
    float X = -5.0f, Y = 4.0f;
    CHECK(ProjectIntoRectMinusDiscs(X, Y, B, F));
    CHECK(X == 0.0f && Y == 4.0f);          // nearest edge point
    X = -5.0f; Y = 20.0f;
    CHECK(ProjectIntoRectMinusDiscs(X, Y, B, F));
    CHECK(X == 0.0f && Y == 10.0f);         // the corner is the nearest point, and it is a stable rest
}

// ---- Inside a disc: pushed radially out to just past its boundary ----
static void TestPushOutOfSingleDisc() {
    const Rect2 B{0.0f, 0.0f, 20.0f, 20.0f};
    Field F{{{10.0f, 10.0f, 3.0f}}};
    float X = 11.0f, Y = 10.0f;   // 1 unit right of centre, well inside R=3
    CHECK(ProjectIntoRectMinusDiscs(X, Y, B, F));
    CHECK(Clear(F, X, Y));
    CHECK(Y > 9.99f && Y < 10.01f);         // pushed along +X, so Y is unchanged
    CHECK(X > 13.0f && X < 13.1f);          // to R + Eps
}

// ---- Dead centre: no outward direction exists, so it must pick one and be STABLE ----
static void TestDeadCentreIsStable() {
    const Rect2 B{0.0f, 0.0f, 20.0f, 20.0f};
    Field F{{{10.0f, 10.0f, 2.0f}}};
    float X1 = 10.0f, Y1 = 10.0f, X2 = 10.0f, Y2 = 10.0f;
    CHECK(ProjectIntoRectMinusDiscs(X1, Y1, B, F));
    CHECK(ProjectIntoRectMinusDiscs(X2, Y2, B, F));
    CHECK(Clear(F, X1, Y1));
    CHECK(X1 == X2 && Y1 == Y2);            // same input -> same output, not a noise-dependent flip
}

// ---- Overlapping discs: one per pass, worst-first, must still converge ----
static void TestOverlappingDiscsConverge() {
    const Rect2 B{0.0f, 0.0f, 40.0f, 40.0f};
    Field F{{{18.0f, 20.0f, 4.0f}, {22.0f, 20.0f, 4.0f}, {20.0f, 23.0f, 4.0f}}};
    float X = 20.0f, Y = 20.0f;             // in the middle of all three
    const bool Settled = ProjectIntoRectMinusDiscs(X, Y, B, F, 32);
    CHECK(Settled);
    CHECK(Clear(F, X, Y));
    CHECK(X >= B.Xlo && X <= B.Xhi && Y >= B.Ylo && Y <= B.Yhi);
}

// ---- CONTINUITY: the reason this is projection and not a sample grid ----
// Sweep the input across a disc in small steps; the output must never jump much more than the input
// did. A lattice-sampling implementation fails this — that is exactly the jitter it was replaced for.
static void TestOutputIsContinuousInInput() {
    const Rect2 B{0.0f, 0.0f, 40.0f, 40.0f};
    Field F{{{20.0f, 20.0f, 5.0f}}};
    const float Step = 0.05f;
    float PrevX = 0.0f, PrevY = 0.0f;
    bool Have = false;
    float WorstJump = 0.0f;
    for (float In = 12.0f; In <= 28.0f; In += Step) {
        float X = In, Y = 20.4f;            // just off the centre line, so it never hits dead centre
        ProjectIntoRectMinusDiscs(X, Y, B, F);
        if (Have) {
            const float J = std::sqrt((X - PrevX) * (X - PrevX) + (Y - PrevY) * (Y - PrevY));
            if (J > WorstJump) WorstJump = J;
        }
        PrevX = X; PrevY = Y; Have = true;
    }
    // Crossing the disc, the projected point slides around the rim. It travels faster than the input
    // near the tangent points, but it must not TELEPORT: a hop of several units would mean the output
    // jumped from one side of the disc to the other, which is the lattice failure mode.
    CHECK(WorstJump < 2.0f);
    if (WorstJump >= 2.0f) std::printf("    worst jump was %.3f for a %.3f input step\n", WorstJump, Step);
}

// ---- An impossible pocket must report failure rather than a plausible lie ----
static void TestUnsatisfiableReportsFalse() {
    // A 1x1 box entirely swallowed by a big disc: nothing inside the rect is outside the disc.
    const Rect2 B{9.5f, 9.5f, 10.5f, 10.5f};
    Field F{{{10.0f, 10.0f, 8.0f}}};
    float X = 10.0f, Y = 10.0f;
    CHECK(!ProjectIntoRectMinusDiscs(X, Y, B, F));
    // Best effort is still inside the rectangle, so the caller's re-validation gets a sane point.
    CHECK(X >= B.Xlo && X <= B.Xhi && Y >= B.Ylo && Y <= B.Yhi);
}

int main() {
    TestFeasiblePointIsUntouched();
    TestRectangleClamp();
    TestPushOutOfSingleDisc();
    TestDeadCentreIsStable();
    TestOverlappingDiscsConverge();
    TestOutputIsContinuousInInput();
    TestUnsatisfiableReportsFalse();
    if (GFailures == 0) std::printf("math_projection_tests: ALL PASS\n");
    else std::printf("math_projection_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
