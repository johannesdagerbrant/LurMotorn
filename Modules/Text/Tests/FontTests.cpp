// Dependency-free unit tests for Lur::Text (#24 cooked asset, #25 runtime + layout):
//  - the generated FontAtlas_Inter.h is well-formed (ASCII coverage, monotonic
//    codepoints, normalised UVs, y-down metrics, whitespace invariant);
//  - the runtime Font (lookup, advances, '?' fallback);
//  - greedy word-wrap layout (single line, wrap, hard newline, char-break, alignment,
//    overflow flags) and MeasureText;
//  - the FontRegistry (register / find / idempotence / validity).
// No framework: each CHECK records a failure; the process exits non-zero if any failed.
// Mirrors save_tests / net_tests.
#include <cstdio>
#include <cstring>

#include "Lur/Text/BuiltinFonts.h"
#include "Lur/Text/Font.h"
#include "Lur/Text/FontRegistry.h"
#include "Lur/Text/TextLayout.h"

using namespace Lur::Text;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

static bool Approx(float A, float B, float Eps = 0.05f) {
    const float D = A - B;
    return (D < 0 ? -D : D) <= Eps;
}

static void CheckCookedAsset() {
    const CookedFont& F = InterFont();

    CHECK(F.AtlasWidth > 0);
    CHECK(F.AtlasHeight > 0);
    CHECK(F.AtlasChannels == 3);        // MSDF = RGB
    CHECK(F.Atlas != nullptr);
    CHECK(F.Glyphs != nullptr);
    CHECK(F.EmSize == 1.0f);
    CHECK(F.LineHeight > 0.0f);
    CHECK(F.Ascender < 0.0f);           // y-down: above baseline is negative
    CHECK(F.Descender > 0.0f);
    CHECK(F.DistanceRange > 0.0f);

    CHECK(F.GlyphCount == 95);          // 0x20..0x7E
    if (F.GlyphCount != 95) return;
    CHECK(F.Glyphs[0].Codepoint == 0x20u);
    CHECK(F.Glyphs[94].Codepoint == 0x7Eu);
    for (int i = 0; i < F.GlyphCount; ++i) {
        const CookedGlyph& G = F.Glyphs[i];
        CHECK(G.Codepoint == static_cast<unsigned>(0x20 + i));   // contiguous + sorted
        CHECK(G.Advance > 0.0f);
        const bool HasQuad = G.UvRight > G.UvLeft;
        if (G.Codepoint == 0x20u) {
            CHECK(!HasQuad);            // space: no rendered quad
        } else {
            CHECK(HasQuad);
            CHECK(G.UvLeft >= 0.0f && G.UvRight <= 1.0f);
            CHECK(G.UvTop  >= 0.0f && G.UvBottom <= 1.0f);
            CHECK(G.UvBottom > G.UvTop);       // y-down atlas
            CHECK(G.PlaneRight  > G.PlaneLeft);
            CHECK(G.PlaneBottom > G.PlaneTop); // y-down plane
            CHECK(G.PlaneTop < 0.0f);          // caps sit above the baseline
        }
    }
}

static void CheckFontRuntime() {
    Font F(InterFont());
    CHECK(F.IsValid());
    CHECK(F.Find('A') != nullptr);
    CHECK(F.Find('~') != nullptr);
    CHECK(F.Find(0x2603u) == nullptr);         // snowman: absent
    CHECK(F.Advance('A') > 0.0f);
    CHECK(F.Advance(0x2603u) == 0.0f);
    CHECK(F.Ascender() < 0.0f);
    CHECK(F.Atlas() == 0);                      // not uploaded (no renderer)
    // 'W' is wider than 'i' in a proportional font.
    CHECK(F.Advance('W') > F.Advance('i'));
}

static LayoutResult Lay(const Font& F, const char* S, const LayoutSpec& Spec) {
    LayoutResult R;
    LayoutText(F, S, static_cast<int>(std::strlen(S)), Spec, R);
    return R;
}

static void CheckLayout() {
    Font F(InterFont());

    // --- Single line, no wrap: left-to-right, one line, in-bounds. ---
    {
        LayoutSpec S; S.X = 0; S.Y = 0; S.W = 1000; S.H = 200; S.PixelSize = 32;
        LayoutResult R = Lay(F, "Hi", S);
        CHECK(R.LineCount == 1);
        CHECK(R.Count == 2);                    // 'H' and 'i' both visible
        CHECK(!R.OverflowX && !R.OverflowY);
        CHECK(R.WidthPx > 0.0f);
        if (R.Count == 2) {
            CHECK(R.Glyphs[1].X0 > R.Glyphs[0].X0);   // advances rightward
            CHECK(R.Glyphs[0].X0 >= -1.0f);           // near the field origin
            CHECK(R.Glyphs[0].Y1 > R.Glyphs[0].Y0);   // top above bottom (y-down)
        }
        // MeasureText agrees with the laid-out size.
        float MW, MH; int ML;
        MeasureText(F, "Hi", 2, S.W, S.PixelSize, true, MW, MH, ML);
        CHECK(ML == 1);
        CHECK(Approx(MW, R.WidthPx, 0.5f));
        CHECK(Approx(MH, R.HeightPx, 0.5f));
    }

    // --- Word wrap: multiple words into a narrow field -> multiple lines, no overflow. ---
    {
        LayoutSpec S; S.W = 120; S.H = 400; S.PixelSize = 32; S.Wrap = true;
        LayoutResult R = Lay(F, "alpha beta gamma delta", S);
        CHECK(R.LineCount > 1);
        CHECK(!R.OverflowX);                    // every wrapped line fits
        CHECK(R.WidthPx <= S.W + 0.5f);
    }

    // --- Hard newline forces a break regardless of width. ---
    {
        LayoutSpec S; S.W = 1000; S.H = 400; S.PixelSize = 32;
        LayoutResult R = Lay(F, "A\nB", S);
        CHECK(R.LineCount == 2);
    }

    // --- Character-break fallback: one unbreakable word longer than the field. ---
    {
        LayoutSpec S; S.W = 120; S.H = 400; S.PixelSize = 32; S.Wrap = true;
        LayoutResult R = Lay(F, "MMMMMMMMMMMM", S);
        CHECK(R.LineCount > 1);                 // split across lines by character
        CHECK(!R.OverflowX);                    // a single 'M' still fits the width
    }

    // --- Alignment: center sits right of left; right sits right of center. ---
    {
        LayoutSpec L; L.W = 300; L.H = 100; L.PixelSize = 32; L.HAlign = EHAlign::Left;
        LayoutSpec C = L; C.HAlign = EHAlign::Center;
        LayoutSpec Rt = L; Rt.HAlign = EHAlign::Right;
        LayoutResult RL = Lay(F, "Hi", L);
        LayoutResult RC = Lay(F, "Hi", C);
        LayoutResult RR = Lay(F, "Hi", Rt);
        CHECK(RL.Count == 2 && RC.Count == 2 && RR.Count == 2);
        if (RL.Count == 2 && RC.Count == 2 && RR.Count == 2) {
            CHECK(RC.Glyphs[0].X0 > RL.Glyphs[0].X0);
            CHECK(RR.Glyphs[0].X0 > RC.Glyphs[0].X0);
        }
    }

    // --- Vertical overflow: block taller than a tiny field. ---
    {
        LayoutSpec S; S.W = 1000; S.H = 4; S.PixelSize = 32;   // one line >> 4px tall
        LayoutResult R = Lay(F, "Hi", S);
        CHECK(R.OverflowY);
    }
    {
        LayoutSpec S; S.W = 1000; S.H = 400; S.PixelSize = 32;
        LayoutResult R = Lay(F, "Hi", S);
        CHECK(!R.OverflowY);
    }

    // --- Horizontal overflow: a single glyph wider than a 1px field. ---
    {
        LayoutSpec S; S.W = 1; S.H = 200; S.PixelSize = 32; S.Wrap = true;
        LayoutResult R = Lay(F, "A", S);
        CHECK(R.LineCount == 1);
        CHECK(R.OverflowX);
    }

    // --- Missing glyph falls back to '?' (still produces a quad). ---
    {
        LayoutSpec S; S.W = 1000; S.H = 200; S.PixelSize = 32;
        const char Snowman[] = {char(0xE2), char(0x98), char(0x83), 0};   // stray bytes
        LayoutResult R = Lay(F, Snowman, S);
        CHECK(R.Count >= 1);                    // rendered as '?'(s), not dropped
    }
}

static void CheckRegistry() {
    FontRegistry Reg;
    CHECK(Reg.Count() == 0);
    CHECK(Reg.Find("Inter") == 0);

    const FontHandle H = Reg.Register("Inter", InterFont());
    CHECK(H != 0);
    CHECK(Reg.Count() == 1);
    CHECK(Reg.IsValid(H));
    CHECK(!Reg.IsValid(0));
    CHECK(!Reg.IsValid(H + 1));
    CHECK(Reg.Find("Inter") == H);
    CHECK(Reg.Get(H).IsValid());
    CHECK(Reg.Get(H).Advance('A') > 0.0f);

    // Idempotent: re-registering the same name returns the same handle, no new slot.
    const FontHandle H2 = Reg.Register("Inter", InterFont());
    CHECK(H2 == H);
    CHECK(Reg.Count() == 1);
}

// Multi-font seam (#28): the DSEG7 clock font cooks + registers alongside the UI font.
static void CheckMultiFont() {
    // DSEG7 is a 7-segment display face: digits present, not full ASCII.
    Font Dseg(Dseg7Font());
    CHECK(Dseg.IsValid());
    CHECK(Dseg.Cooked().GlyphCount > 10);
    CHECK(Dseg.Cooked().GlyphCount < 95);        // segmented font ≠ full ASCII
    for (uint32_t C = '0'; C <= '9'; ++C) CHECK(Dseg.Find(C) != nullptr);
    CHECK(Dseg.Advance('8') > 0.0f);

    // Two distinct fonts coexist in one registry (distinct handles), the intended
    // multi-font model for e.g. a UI font + a clock font.
    FontRegistry Reg;
    const FontHandle Ui   = Reg.Register("Inter", InterFont());
    const FontHandle Clock = Reg.Register("Dseg7", Dseg7Font());
    CHECK(Ui != 0 && Clock != 0);
    CHECK(Ui != Clock);
    CHECK(Reg.Count() == 2);
    CHECK(Reg.Find("Inter") == Ui);
    CHECK(Reg.Find("Dseg7") == Clock);
    CHECK(&Reg.Get(Ui).Cooked()   == &InterFont());
    CHECK(&Reg.Get(Clock).Cooked() == &Dseg7Font());
}

int main() {
    CheckCookedAsset();
    CheckFontRuntime();
    CheckLayout();
    CheckRegistry();
    CheckMultiFont();

    if (GFailures == 0) {
        std::printf("text_tests: all checks passed\n");
        return 0;
    }
    std::printf("text_tests: %d failure(s)\n", GFailures);
    return 1;
}
