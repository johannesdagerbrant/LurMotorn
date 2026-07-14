#pragma once
#include <cstdint>

// Lur::Text cooked-font asset format.
//
// A font is baked OFFLINE by scripts/gen-font.ps1 (which drives the sanctioned
// build-time tool msdf-atlas-gen — see CLAUDE.md) into a generated header, e.g.
// Modules/Text/Private/Cooked/FontAtlas_Inter.h. That header includes this file and
// exposes exactly one `CookedFont` describing an MSDF atlas + per-glyph metrics. NO
// third-party code ships in the app: the runtime only samples the embedded atlas.
//
// All geometry is em-normalised (msdf-atlas-gen "em" units): multiply by a runtime
// pixel size to get screen units. That is what makes the text resolution-independent.
namespace Lur::Text {

// One glyph's layout + atlas placement. Bounds are the msdf-atlas-gen convention:
//   PlaneBounds  — baseline-relative quad in EM units, +x right, +y up.
//   Uv*          — normalised [0,1] atlas rect, matching the atlas yOrigin (bottom).
// Whitespace glyphs (e.g. space) carry a zero quad (all Plane*/Uv* == 0) and only a
// meaningful Advance.
struct CookedGlyph {
    uint32_t Codepoint;
    float    Advance;                                   // em
    float    PlaneLeft, PlaneBottom, PlaneRight, PlaneTop;   // em, baseline-relative
    float    UvLeft, UvBottom, UvRight, UvTop;               // normalised [0,1]
};

// A whole cooked font: the MSDF atlas pixels + global metrics + the glyph table.
// `Atlas` is AtlasWidth*AtlasHeight*AtlasChannels bytes, row-major. MSDF atlases are
// 3-channel (RGB); the runtime reconstructs coverage as median(r,g,b) in the shader,
// so channel order is irrelevant. `DistanceRange` is the msdfgen spread in ATLAS
// pixels — the render layer feeds it (with the texture size + fwidth) to compute the
// screen-space smoothing width.
struct CookedFont {
    int   AtlasWidth  = 0;
    int   AtlasHeight = 0;
    int   AtlasChannels = 3;

    float EmSize        = 1.0f;   // reference em (metrics are in these units)
    float LineHeight    = 0.0f;   // em, baseline-to-baseline
    float Ascender      = 0.0f;   // em, +up
    float Descender     = 0.0f;   // em, -down
    float DistanceRange = 0.0f;   // atlas px (msdfgen -pxrange)

    const CookedGlyph*   Glyphs     = nullptr;
    int                  GlyphCount = 0;
    const unsigned char* Atlas      = nullptr;
};

} // namespace Lur::Text
