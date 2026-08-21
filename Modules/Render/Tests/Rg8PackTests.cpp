// Tests for Lur::Render::PackRg8 — the shade+coverage interleave both games upload through.
//
// Worth testing despite being ten lines: the destination index is
// `2 * ((DstY + Y) * DstWidthPx + DstX + X)`, and the two call sites it replaced had that arithmetic
// written out by hand in two different shapes. An off-by-one in the stride produces a sheared atlas
// that still uploads, still renders, and looks like an art bug rather than a code bug.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Render/Rg8Pack.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Render::PackRg8;

// ---- The whole-image case (chess): DstWidthPx == W, origin 0,0 ----
static void TestWholeImageInterleave() {
    constexpr int W = 3, H = 2;
    const uint8_t Shade[W * H]    = {1, 2, 3, 4, 5, 6};
    const uint8_t Coverage[W * H] = {10, 20, 30, 40, 50, 60};
    std::vector<uint8_t> Dst(W * H * 2, 0xEE);
    PackRg8(Dst.data(), W, 0, 0, Shade, Coverage, W, H);
    for (int I = 0; I < W * H; ++I) {
        CHECK(Dst[I * 2 + 0] == Shade[I]);     // R = shade
        CHECK(Dst[I * 2 + 1] == Coverage[I]);  // G = coverage
    }
}

// ---- The atlas case (RPS): tiles placed side by side in a wider destination ----
// This is the one that catches a stride bug. Two 2x2 tiles into a 4-wide atlas must interleave
// column-wise, NOT append — appending produces the same bytes in the wrong places, so a test that
// only checked "every source byte appears somewhere" would pass against a sheared atlas.
static void TestAtlasTilesSideBySide() {
    constexpr int S = 2, Tiles = 2, AtlasW = S * Tiles;
    const uint8_t ShadeA[S * S]    = {1, 2, 3, 4};
    const uint8_t CoverA[S * S]    = {11, 12, 13, 14};
    const uint8_t ShadeB[S * S]    = {5, 6, 7, 8};
    const uint8_t CoverB[S * S]    = {15, 16, 17, 18};
    std::vector<uint8_t> Dst(static_cast<std::size_t>(AtlasW) * S * 2, 0);
    PackRg8(Dst.data(), AtlasW, 0 * S, 0, ShadeA, CoverA, S, S);
    PackRg8(Dst.data(), AtlasW, 1 * S, 0, ShadeB, CoverB, S, S);

    // Row 0 of the atlas: [A(0,0) A(1,0) B(0,0) B(1,0)]
    const uint8_t WantR0[AtlasW] = {1, 2, 5, 6};
    const uint8_t WantG0[AtlasW] = {11, 12, 15, 16};
    // Row 1: [A(0,1) A(1,1) B(0,1) B(1,1)]
    const uint8_t WantR1[AtlasW] = {3, 4, 7, 8};
    const uint8_t WantG1[AtlasW] = {13, 14, 17, 18};
    for (int X = 0; X < AtlasW; ++X) {
        CHECK(Dst[2 * (0 * AtlasW + X) + 0] == WantR0[X]);
        CHECK(Dst[2 * (0 * AtlasW + X) + 1] == WantG0[X]);
        CHECK(Dst[2 * (1 * AtlasW + X) + 0] == WantR1[X]);
        CHECK(Dst[2 * (1 * AtlasW + X) + 1] == WantG1[X]);
    }
}

// ---- A tile at a non-zero row, so DstY is exercised too ----
static void TestVerticalOffset() {
    constexpr int S = 2, AtlasW = 2, AtlasH = 4;
    const uint8_t Shade[S * S] = {9, 8, 7, 6};
    const uint8_t Cover[S * S] = {1, 2, 3, 4};
    std::vector<uint8_t> Dst(static_cast<std::size_t>(AtlasW) * AtlasH * 2, 0);
    PackRg8(Dst.data(), AtlasW, 0, 2, Shade, Cover, S, S);
    // Rows 0-1 untouched, rows 2-3 written.
    for (int I = 0; I < AtlasW * 2 * 2; ++I) CHECK(Dst[I] == 0);
    CHECK(Dst[2 * (2 * AtlasW + 0) + 0] == 9);
    CHECK(Dst[2 * (3 * AtlasW + 1) + 0] == 6);
    CHECK(Dst[2 * (3 * AtlasW + 1) + 1] == 4);
}

// ---- It must not write outside the tile it was given ----
// The atlas call sites rely on this: each tile is packed independently and a bleed would silently
// corrupt the neighbour that was packed before it.
static void TestDoesNotWriteOutsideTile() {
    constexpr int S = 2, AtlasW = 4;
    const uint8_t Shade[S * S] = {1, 1, 1, 1};
    const uint8_t Cover[S * S] = {2, 2, 2, 2};
    std::vector<uint8_t> Dst(static_cast<std::size_t>(AtlasW) * S * 2, 0x77);
    PackRg8(Dst.data(), AtlasW, 0, 0, Shade, Cover, S, S);
    // Columns 2..3 of both rows must still hold the sentinel.
    for (int Y = 0; Y < S; ++Y)
        for (int X = S; X < AtlasW; ++X) {
            CHECK(Dst[2 * (Y * AtlasW + X) + 0] == 0x77);
            CHECK(Dst[2 * (Y * AtlasW + X) + 1] == 0x77);
        }
}

static void TestNullAndEmptyAreNoOps() {
    std::vector<uint8_t> Dst(8, 0x5A);
    const uint8_t Src[4] = {1, 2, 3, 4};
    PackRg8(nullptr, 2, 0, 0, Src, Src, 2, 2);      // must not crash
    PackRg8(Dst.data(), 2, 0, 0, nullptr, Src, 2, 2);
    PackRg8(Dst.data(), 2, 0, 0, Src, nullptr, 2, 2);
    PackRg8(Dst.data(), 2, 0, 0, Src, Src, 0, 0);   // empty tile writes nothing
    for (uint8_t B : Dst) CHECK(B == 0x5A);
}

int main() {
    TestWholeImageInterleave();
    TestAtlasTilesSideBySide();
    TestVerticalOffset();
    TestDoesNotWriteOutsideTile();
    TestNullAndEmptyAreNoOps();
    if (GFailures == 0) std::printf("render_rg8_tests: ALL PASS\n");
    else std::printf("render_rg8_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
