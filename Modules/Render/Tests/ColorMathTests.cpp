// Tests for Lur::Render::ColorMath, moved out of Modules/DevGui and merged with RPS's own HSV (#201).
//
// The important test is EQUIVALENCE. RPS had written its own HSV in a different form — hue in degrees,
// chroma/X/M rather than six-sector — and the two are mathematically the same function but nothing
// proved it. Consolidating them without checking would shift the team palette by an invisible amount
// on a change whose whole claim is "pure relocation", so the old formula is reproduced here and the
// two are compared across a sweep.
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "Lur/Render/ColorMath.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

namespace CM = Lur::Render::ColorMath;
using Lur::Render::Color;

// RPS's original, verbatim, as the reference. Deliberately kept in the test rather than the header:
// it is the thing being replaced, and its only remaining job is to prove the replacement matches.
static Color LegacyRpsHsv(float H, float S, float V) {
    const float C = V * S;
    const float X = C * (1.0f - std::fabs(std::fmod(H / 60.0f, 2.0f) - 1.0f));
    const float M = V - C;
    float R = 0, G = 0, B = 0;
    if (H < 60)       { R = C; G = X; }
    else if (H < 120) { R = X; G = C; }
    else if (H < 180) { G = C; B = X; }
    else if (H < 240) { G = X; B = C; }
    else if (H < 300) { R = X; B = C; }
    else              { R = C; B = X; }
    return {R + M, G + M, B + M, 1.0f};
}

// ---- The consolidation is exact across the whole hue circle ----
static void TestMatchesLegacyRpsFormula() {
    float Worst = 0.0f;
    for (int Hi = 0; Hi < 360; ++Hi)
        for (float S : {0.25f, 0.6f, 1.0f})
            for (float V : {0.3f, 0.75f, 1.0f}) {
                const float H = static_cast<float>(Hi);
                const Color A = LegacyRpsHsv(H, S, V);
                const Color B = CM::FromHsvDeg(H, S, V);
                Worst = std::fmax(Worst, std::fabs(A.R - B.R));
                Worst = std::fmax(Worst, std::fabs(A.G - B.G));
                Worst = std::fmax(Worst, std::fabs(A.B - B.B));
            }
    CHECK(Worst < 1e-5f);
    if (Worst >= 1e-5f) std::printf("    worst channel difference was %.6f\n", Worst);
}

// ---- The actual RPS team hues, pinned ----
// #142: cyan 180 / magenta 320 for the two teams, and 200/290 for the type interpolation ends. If a
// future refactor shifts these the teams stop being readable at a glance, which is the whole point.
static void TestTeamHuesUnchanged() {
    const Color Cyan = CM::FromHsvDeg(180.0f, 1.0f, 1.0f);
    CHECK(std::fabs(Cyan.R - 0.0f) < 1e-5f);
    CHECK(std::fabs(Cyan.G - 1.0f) < 1e-5f);
    CHECK(std::fabs(Cyan.B - 1.0f) < 1e-5f);
    const Color Magenta = CM::FromHsvDeg(320.0f, 1.0f, 1.0f);
    CHECK(std::fabs(Magenta.R - 1.0f) < 1e-5f);
    CHECK(std::fabs(Magenta.G - 0.0f) < 1e-5f);
    CHECK(std::fabs(Magenta.B - 2.0f / 3.0f) < 1e-3f);
}

// ---- Hue wraps; S and V clamp ----
// The wrap is why a hue slider dragged past the end keeps going instead of sticking.
static void TestWrapAndClamp() {
    float R1, G1, B1, R2, G2, B2;
    CM::HsvToRgb(0.25f, 1.0f, 1.0f, R1, G1, B1);
    CM::HsvToRgb(1.25f, 1.0f, 1.0f, R2, G2, B2);
    CHECK(R1 == R2 && G1 == G2 && B1 == B2);
    CM::HsvToRgb(-0.75f, 1.0f, 1.0f, R2, G2, B2);
    CHECK(R1 == R2 && G1 == G2 && B1 == B2);
    CM::HsvToRgb(0.5f, 5.0f, 5.0f, R1, G1, B1);      // out-of-range S/V clamp, not wrap
    CHECK(R1 >= 0.0f && R1 <= 1.0f && G1 <= 1.0f && B1 <= 1.0f);
}

// ---- Round trip, and the documented lossy corners ----
static void TestRoundTripAndItsLimits() {
    for (float H : {0.1f, 0.4f, 0.9f}) {
        float R, G, B, H2, S2, V2;
        CM::HsvToRgb(H, 0.8f, 0.7f, R, G, B);
        CM::RgbToHsv(R, G, B, H2, S2, V2);
        CHECK(std::fabs(H2 - H) < 1e-3f);
        CHECK(std::fabs(S2 - 0.8f) < 1e-3f);
        CHECK(std::fabs(V2 - 0.7f) < 1e-3f);
    }
    // Grey carries no hue: the inverse returns 0 by design (the picker keeps its own live H,S,V for
    // exactly this reason, so dragging into a corner doesn't snap the handle to red).
    float H3, S3, V3;
    CM::RgbToHsv(0.5f, 0.5f, 0.5f, H3, S3, V3);
    CHECK(S3 == 0.0f);
    CHECK(H3 == 0.0f);
}

// ---- Srgb8 is constexpr, because palettes are constexpr tables ----
static void TestSrgb8() {
    CHECK(std::fabs(CM::Srgb8(255) - 1.0f) < 1e-6f);
    CHECK(CM::Srgb8(0) == 0.0f);
    static_assert(CM::Srgb8(255) == 1.0f, "Srgb8 must be constexpr");
    static_assert(CM::Srgb8(0x80) > 0.5f, "0x80 is just over half");
}

int main() {
    TestMatchesLegacyRpsFormula();
    TestTeamHuesUnchanged();
    TestWrapAndClamp();
    TestRoundTripAndItsLimits();
    TestSrgb8();
    if (GFailures == 0) std::printf("render_colormath_tests: ALL PASS\n");
    else std::printf("render_colormath_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
