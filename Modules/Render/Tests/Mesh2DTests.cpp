// Tests for Lur::Render::Mesh2D — the unit-rect mesh builders promoted out of Rps::GameView (#201).
//
// The geometry is separated from CreateMesh precisely so these are testable with no GPU. What they
// check is the set of things whose breakage looks like an ART bug rather than a code bug: winding,
// index bounds, ramp interpolation — and, most importantly, that the disc is a triangle LIST, because
// a fan renders correctly on Android and is outside the portability subset MoltenVK runs on iOS.
#include <cmath>
#include <cstdio>

#include "Lur/Render/Mesh2D.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Render::BuildDisc;
using Lur::Render::BuildGradientStripH;
using Lur::Render::BuildGradientStripV;
using Lur::Render::Color;
using Lur::Render::GradSample;
using Lur::Render::GradStop;
using Lur::Render::MaxDiscSegments;
using Lur::Render::MaxGradStops;
using Lur::Render::Vertex;

// ---- GradSample: clamps at the ends, lerps between ----
static void TestGradSample() {
    const GradStop S[3] = {{0.0f, {0, 0, 0, 1}}, {0.5f, {1, 0, 0, 1}}, {1.0f, {1, 1, 1, 1}}};
    CHECK(GradSample(S, 3, -1.0f).R == 0.0f);          // clamp low, never extrapolate
    CHECK(GradSample(S, 3, 2.0f).R == 1.0f);           // clamp high
    CHECK(GradSample(S, 3, 2.0f).G == 1.0f);
    const Color Mid = GradSample(S, 3, 0.25f);         // halfway into the first span
    CHECK(std::fabs(Mid.R - 0.5f) < 1e-5f);
    CHECK(std::fabs(Mid.G - 0.0f) < 1e-5f);
    // Duplicate stop positions must not divide by zero.
    const GradStop D[3] = {{0.0f, {0, 0, 0, 1}}, {0.5f, {1, 0, 0, 1}}, {0.5f, {0, 1, 0, 1}}};
    const Color At = GradSample(D, 3, 0.5f);
    CHECK(At.R == At.R);                               // not NaN
    // No stops: returns the default Color (opaque white) rather than dereferencing the pointer.
    const Color None = GradSample(nullptr, 0, 0.5f);
    CHECK(None.R == 1.0f && None.A == 1.0f);
}

// ---- Strip topology: 2 verts per stop, 2 triangles per span, indices in range ----
static void TestGradientStripTopology() {
    const GradStop S[4] = {{0.0f, {1, 0, 0, 1}}, {0.3f, {0, 1, 0, 1}},
                           {0.7f, {0, 0, 1, 1}}, {1.0f, {1, 1, 1, 1}}};
    Vertex V[2 * MaxGradStops];
    uint32_t I[6 * (MaxGradStops - 1)];
    uint32_t VC = 0, IC = 0;
    CHECK(BuildGradientStripV(S, 4, 1.0f, V, I, VC, IC) == 4);
    CHECK(VC == 8);                                    // 2 per stop
    CHECK(IC == 18);                                   // 6 per span x 3 spans
    CHECK(IC % 3 == 0);                                // a triangle LIST
    for (uint32_t K = 0; K < IC; ++K) CHECK(I[K] < VC);
    // Vertical: the stop drives Y, and the row spans the full width.
    for (int K = 0; K < 4; ++K) {
        CHECK(std::fabs(V[2 * K + 0].Position.Y - S[K].P) < 1e-6f);
        CHECK(V[2 * K + 0].Position.X == 0.0f);
        CHECK(V[2 * K + 1].Position.X == 1.0f);
    }
}

// ---- Horizontal is the same topology with the axes swapped ----
static void TestHorizontalStripSwapsAxes() {
    const GradStop S[2] = {{0.0f, {1, 0, 0, 1}}, {1.0f, {0, 0, 1, 1}}};
    Vertex V[2 * MaxGradStops];
    uint32_t I[6 * (MaxGradStops - 1)];
    uint32_t VC = 0, IC = 0;
    BuildGradientStripH(S, 2, 1.0f, V, I, VC, IC);
    CHECK(VC == 4 && IC == 6);
    for (int K = 0; K < 2; ++K) {
        CHECK(std::fabs(V[2 * K + 0].Position.X - S[K].P) < 1e-6f);   // stop drives X now
        CHECK(V[2 * K + 0].Position.Y == 0.0f);
        CHECK(V[2 * K + 1].Position.Y == 1.0f);
    }
}

// ---- Alpha scales the stop alpha through ----
static void TestAlphaIsApplied() {
    const GradStop S[2] = {{0.0f, {1, 1, 1, 1.0f}}, {1.0f, {1, 1, 1, 0.5f}}};
    Vertex V[2 * MaxGradStops];
    uint32_t I[6 * (MaxGradStops - 1)];
    uint32_t VC = 0, IC = 0;
    BuildGradientStripV(S, 2, 0.5f, V, I, VC, IC);
    CHECK(std::fabs(V[0].Color.W - 0.5f) < 1e-6f);
    CHECK(std::fabs(V[2].Color.W - 0.25f) < 1e-6f);
}

// ---- OVERFLOW GUARD: the original wrote into Vertex[16] with no bounds check ----
// An eight-stop limit was enforced only by nobody having tried a ninth. Now it clamps.
static void TestTooManyStopsClamps() {
    GradStop S[MaxGradStops + 4];
    for (int K = 0; K < MaxGradStops + 4; ++K)
        S[K] = {static_cast<float>(K) / (MaxGradStops + 3), {1, 1, 1, 1}};
    Vertex V[2 * MaxGradStops];
    uint32_t I[6 * (MaxGradStops - 1)];
    uint32_t VC = 0, IC = 0;
    const int Used = BuildGradientStripV(S, MaxGradStops + 4, 1.0f, V, I, VC, IC);
    CHECK(Used == MaxGradStops);
    CHECK(VC == static_cast<uint32_t>(2 * MaxGradStops));
    CHECK(IC == static_cast<uint32_t>(6 * (MaxGradStops - 1)));
    for (uint32_t K = 0; K < IC; ++K) CHECK(I[K] < VC);
    // Fewer than two stops is not a strip.
    CHECK(BuildGradientStripV(S, 1, 1.0f, V, I, VC, IC) == 0);
    CHECK(VC == 0 && IC == 0);
}

// ---- THE DISC IS A TRIANGLE LIST, NOT A FAN ----
// A fan would be indices {0,1,2,3,4,...}: N+2 indices for N triangles. A list is 3N, and every
// triangle must carry the centre explicitly. Getting this wrong renders fine on Android and is
// outside the subset MoltenVK runs, so it fails only on the iPhone.
static void TestDiscIsTriangleList() {
    Vertex V[MaxDiscSegments + 2];
    uint32_t I[3 * MaxDiscSegments];
    uint32_t VC = 0, IC = 0;
    const int Seg = 12;
    BuildDisc(Seg, V, I, VC, IC);
    CHECK(IC == static_cast<uint32_t>(3 * Seg));    // 3 indices per triangle, not N+2
    CHECK(VC == static_cast<uint32_t>(Seg + 2));
    for (uint32_t T = 0; T < IC; T += 3) {
        CHECK(I[T] == 0);                           // every triangle carries the centre
        CHECK(I[T + 1] < VC && I[T + 2] < VC);
        CHECK(I[T + 2] == I[T + 1] + 1);            // and two consecutive rim vertices
    }
}

// ---- Disc geometry: rim on the circle, inscribed in the unit rect ----
static void TestDiscGeometry() {
    Vertex V[MaxDiscSegments + 2];
    uint32_t I[3 * MaxDiscSegments];
    uint32_t VC = 0, IC = 0;
    BuildDisc(20, V, I, VC, IC);
    CHECK(V[0].Position.X == 0.5f && V[0].Position.Y == 0.5f);
    for (uint32_t K = 1; K < VC; ++K) {
        const float Dx = V[K].Position.X - 0.5f, Dy = V[K].Position.Y - 0.5f;
        CHECK(std::fabs(std::sqrt(Dx * Dx + Dy * Dy) - 0.5f) < 1e-5f);
        CHECK(V[K].Position.X >= -1e-6f && V[K].Position.X <= 1.0f + 1e-6f);
        CHECK(V[K].Color.X == 1.0f);                // WHITE, so a material tint colours it
    }
}

// ---- Segment count is clamped both ways ----
static void TestDiscSegmentClamp() {
    Vertex V[MaxDiscSegments + 2];
    uint32_t I[3 * MaxDiscSegments];
    uint32_t VC = 0, IC = 0;
    BuildDisc(1000, V, I, VC, IC);                  // must not overrun
    CHECK(VC == static_cast<uint32_t>(MaxDiscSegments + 2));
    CHECK(IC == static_cast<uint32_t>(3 * MaxDiscSegments));
    BuildDisc(0, V, I, VC, IC);                     // degenerate -> minimum viable triangle count
    CHECK(IC == 9);
}

int main() {
    TestGradSample();
    TestGradientStripTopology();
    TestHorizontalStripSwapsAxes();
    TestAlphaIsApplied();
    TestTooManyStopsClamps();
    TestDiscIsTriangleList();
    TestDiscGeometry();
    TestDiscSegmentClamp();
    if (GFailures == 0) std::printf("render_mesh2d_tests: ALL PASS\n");
    else std::printf("render_mesh2d_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
