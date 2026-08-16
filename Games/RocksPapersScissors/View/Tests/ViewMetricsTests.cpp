// Host unit tests for Rps::ViewMetrics (issue #43, section D).
//
// These four functions had no test while they existed three times each, which is exactly backwards:
// duplicated arithmetic is where a test earns the most, because nothing else notices when one copy
// changes. WorldToFixed in particular feeds the wire — a Place event's coordinates go straight into
// a deterministic fixed-point sim on both peers — so a rounding difference between copies is a
// desync, not a cosmetic drift.
#include <cstdio>

#include "Rps/ViewMetrics.h"

static int Failures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); \
            ++Failures;                                                 \
        }                                                               \
    } while (false)

// WorldToFixed ROUNDS to nearest rather than truncating. The distinction is the whole point: the
// value crosses the wire into the sim, and two peers that disagreed by one raw unit would place a
// building on different squares.
static void TestWorldToFixedRoundsToNearest() {
    const float One = static_cast<float>(Rps::Fixed::One);

    CHECK(Rps::WorldToFixed(0.0f).Raw == 0);
    CHECK(Rps::WorldToFixed(1.0f).Raw == Rps::Fixed::One);
    CHECK(Rps::WorldToFixed(2.5f).Raw == static_cast<int32_t>(2.5f * One));

    // Just under and just over a raw unit: truncation would send both down.
    const float Eps = 0.4f / One;
    CHECK(Rps::WorldToFixed(1.0f + Eps).Raw == Rps::Fixed::One);        // rounds down
    const float Big = 0.6f / One;
    CHECK(Rps::WorldToFixed(1.0f + Big).Raw == Rps::Fixed::One + 1);    // rounds UP — truncation would not
}

// Negative world coordinates clamp to zero rather than producing a negative Fixed. A drag off the
// left edge is the ordinary way to reach this.
static void TestWorldToFixedClampsNegative() {
    CHECK(Rps::WorldToFixed(-0.0f).Raw == 0);
    CHECK(Rps::WorldToFixed(-1.0f).Raw == 0);
    CHECK(Rps::WorldToFixed(-1000.0f).Raw == 0);
}

// Ppu maps the world's WIDTH onto the viewport width, so a full world width of pixels is exactly
// one world unit per unit of world width. Checked as a round trip rather than by restating the
// formula, which would just be the implementation typed twice.
static void TestPpuMapsWorldWidthToViewport() {
    const float WorldW = static_cast<float>(Rps::WorldWidth.Raw) / static_cast<float>(Rps::Fixed::One);
    CHECK(WorldW > 0.0f);

    const float Px = 1080.0f;
    const float P = Rps::Ppu(Px);
    CHECK(P > 0.0f);
    // The whole world width, converted to pixels, is the viewport.
    const float BackToPx = WorldW * P;
    CHECK(BackToPx > Px - 0.01f && BackToPx < Px + 0.01f);

    // Twice the viewport is twice the scale — the property a fullscreen/rotated window relies on.
    CHECK(Rps::Ppu(2.0f * Px) > 2.0f * P - 0.01f);
    CHECK(Rps::Ppu(2.0f * Px) < 2.0f * P + 0.01f);
}

// WorldHeightF is the scroll extent, and the world is TALL — 240 units against a much smaller
// width. A swapped WorldWidth/WorldHeight in one copy would look plausible and clamp the camera to
// the wrong range; this pins the orientation.
static void TestWorldHeightIsTheTallAxis() {
    const float H = Rps::WorldHeightF();
    const float W = static_cast<float>(Rps::WorldWidth.Raw) / static_cast<float>(Rps::Fixed::One);
    CHECK(H > 0.0f);
    CHECK(H > W);   // the RPS map is a tall corridor; both camps sit on the long axis
}

// The ghost lifts by half a footprint, scaled into pixels. Zero footprint means no offset — the
// degenerate case a CVar can actually produce while tuning.
static void TestGhostOffsetIsHalfAFootprintInPixels() {
    const float P = Rps::Ppu(1080.0f);

    CHECK(Rps::GhostOffsetPx(0, P) == 0.0f);

    // One world unit of footprint -> half a world unit of offset, in pixels.
    const float Off = Rps::GhostOffsetPx(Rps::Fixed::One, P);
    CHECK(Off > 0.5f * P - 0.01f && Off < 0.5f * P + 0.01f);

    // Linear in the footprint: double the building, double the lift.
    const float Off2 = Rps::GhostOffsetPx(2 * Rps::Fixed::One, P);
    CHECK(Off2 > 2.0f * Off - 0.01f && Off2 < 2.0f * Off + 0.01f);

    // ... and linear in the scale, which is what makes one number safe to share between the ghost
    // draw, the validity read and the drop on screens of different widths.
    const float OffWide = Rps::GhostOffsetPx(Rps::Fixed::One, Rps::Ppu(2160.0f));
    CHECK(OffWide > 2.0f * Off - 0.01f && OffWide < 2.0f * Off + 0.01f);
}

int main() {
    TestWorldToFixedRoundsToNearest();
    TestWorldToFixedClampsNegative();
    TestPpuMapsWorldWidthToViewport();
    TestWorldHeightIsTheTallAxis();
    TestGhostOffsetIsHalfAFootprintInPixels();
    if (Failures == 0) std::printf("rps_view_metrics_tests: all passed\n");
    return Failures == 0 ? 0 : 1;
}
