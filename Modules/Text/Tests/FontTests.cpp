// Dependency-free unit tests for Lur::Text's cooked MSDF font asset (#24): the
// generated FontAtlas_Inter.h is well-formed and self-consistent — full printable
// ASCII coverage, monotonic unique codepoints, normalised UVs, sane global metrics,
// and the whitespace/visible-glyph invariants. No framework: each CHECK records a
// failure and the process exits non-zero if any failed, which CTest reports. Mirrors
// save_tests / net_tests.
#include <cstdio>

#include "Lur/Text/BuiltinFonts.h"

using Lur::Text::CookedFont;
using Lur::Text::CookedGlyph;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

int main() {
    const CookedFont& F = Lur::Text::InterFont();

    // Atlas + global metrics.
    CHECK(F.AtlasWidth > 0);
    CHECK(F.AtlasHeight > 0);
    CHECK(F.AtlasChannels == 3);          // MSDF = RGB
    CHECK(F.Atlas != nullptr);
    CHECK(F.Glyphs != nullptr);
    CHECK(F.EmSize == 1.0f);
    CHECK(F.LineHeight > 0.0f);
    CHECK(F.Ascender > 0.0f);
    CHECK(F.Descender < 0.0f);
    CHECK(F.DistanceRange > 0.0f);

    // Full printable-ASCII charset: 0x20..0x7E == 95 glyphs, sorted, unique, complete.
    CHECK(F.GlyphCount == 95);
    if (F.GlyphCount == 95) {
        CHECK(F.Glyphs[0].Codepoint == 0x20u);
        CHECK(F.Glyphs[94].Codepoint == 0x7Eu);
        for (int i = 0; i < F.GlyphCount; ++i) {
            const CookedGlyph& G = F.Glyphs[i];

            // Monotonic + contiguous (so it's exactly 0x20..0x7E).
            CHECK(G.Codepoint == static_cast<unsigned>(0x20 + i));

            // Every glyph advances the pen.
            CHECK(G.Advance > 0.0f);

            const bool HasQuad = (G.UvRight > G.UvLeft) || (G.UvTop > G.UvBottom);
            if (G.Codepoint == 0x20u) {
                // Space: no rendered quad.
                CHECK(!HasQuad);
            } else {
                // Visible glyph: a real, in-bounds atlas rect and a non-empty plane box.
                CHECK(HasQuad);
                CHECK(G.UvLeft >= 0.0f && G.UvRight <= 1.0f);
                CHECK(G.UvBottom >= 0.0f && G.UvTop <= 1.0f);
                CHECK(G.UvRight > G.UvLeft);
                CHECK(G.UvTop > G.UvBottom);
                CHECK(G.PlaneRight > G.PlaneLeft);
                CHECK(G.PlaneTop > G.PlaneBottom);
            }
        }
    }

    if (GFailures == 0) {
        std::printf("text_tests: all checks passed\n");
        return 0;
    }
    std::printf("text_tests: %d failure(s)\n", GFailures);
    return 1;
}
