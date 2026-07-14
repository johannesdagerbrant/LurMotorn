#pragma once
#include <cstdint>

#include "Lur/Text/Font.h"

namespace Lur::Text {

enum class EHAlign : uint8_t { Left, Center, Right };
enum class EVAlign : uint8_t { Top, Middle, Bottom };

// One placed glyph quad, in PIXELS (screen-fixed, y-down — matches the pixel-space ortho
// camera), plus its atlas UV rect. The render/HUD layer emits it as two triangles.
struct PlacedGlyph {
    float X0, Y0, X1, Y1;   // pixel rect: (left,top)-(right,bottom)
    float U0, V0, U1, V1;   // atlas UV rect: (left,top)-(right,bottom)
};

// Field rect + styling for one layout pass. PixelSize is the em->pixel scale (the
// nominal glyph size in px); all wrap/measure math is done in em then scaled, so wrap
// decisions are resolution-independent for a given (W / PixelSize).
struct LayoutSpec {
    float   X = 0.0f, Y = 0.0f, W = 0.0f, H = 0.0f;   // field rect, pixels
    float   PixelSize = 16.0f;
    EHAlign HAlign = EHAlign::Left;
    EVAlign VAlign = EVAlign::Top;
    bool    Wrap   = true;   // wrap to W; if false, only '\n' breaks lines
};

struct LayoutResult {
    static constexpr int MaxGlyphs = 512;

    PlacedGlyph Glyphs[MaxGlyphs];
    int   Count = 0;

    int   LineCount = 0;
    float WidthPx   = 0.0f;   // widest laid-out line
    float HeightPx  = 0.0f;   // block height = LineCount * LineHeight * PixelSize
    bool  OverflowX = false;  // a line is wider than the field W
    bool  OverflowY = false;  // the block is taller than the field H
    bool  Truncated = false;  // hit MaxGlyphs / max lines (some content dropped)
};

// Lay out `Text` (Len bytes; ASCII, '\n' = hard break, '\t' treated as space) with
// `Font` into `Out`, wrapping/aligning within Spec's field rect. Missing glyphs fall
// back to '?'. Fills Out.Glyphs (visible glyphs only) + measured size + overflow flags.
void LayoutText(const Font& Font, const char* Text, int Len,
                const LayoutSpec& Spec, LayoutResult& Out);

// Measure only (no glyph placement): the wrapped block's pixel size + line count.
void MeasureText(const Font& Font, const char* Text, int Len,
                 float MaxWidthPx, float PixelSize, bool Wrap,
                 float& OutWidthPx, float& OutHeightPx, int& OutLineCount);

} // namespace Lur::Text
