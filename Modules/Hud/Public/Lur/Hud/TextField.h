#pragma once
#include "Lur/Render/Renderer.h"
#include "Lur/Text/Font.h"
#include "Lur/Text/TextLayout.h"

namespace Lur::Hud {

// A reusable heads-up text field: lays out a string with a Lur::Text::Font inside a
// screen rect (greedy word-wrap + alignment) and draws it as crisp, scalable MSDF glyph
// quads via IRenderer::DrawGlyphs. Engine-level and game-agnostic — the game supplies
// the font, rect, size, colour, and alignment.
//
// Draw() must be called inside an IRenderer BeginFrame..EndFrame using the 2D/ortho
// camera (pixel-space X/Y, Y down) — the same space Render/Sprite2D sets up.
//
// Optional dev overflow warning: when enabled (SetDebugOverflow(true)), a field whose
// text does not fit is outlined in red — a build-time aid, off by default.
class TextField {
public:
    // Bind to a font (its MSDF atlas must already be uploaded, i.e. Font::Atlas() != 0)
    // and build the atlas material + debug resources. Safe to call once.
    void CreateResources(Lur::Render::IRenderer* Renderer, const Lur::Text::Font* Font);

    // Lay out + draw `Text` within (X,Y,W,H) at PixelSize (em->px), tinted `Color`,
    // aligned per HAlign/VAlign, wrapping to W when Wrap. Returns the layout result
    // (line count, measured size, overflow flags) for callers that care.
    Lur::Text::LayoutResult Draw(Lur::Render::IRenderer* Renderer, const char* Text,
                                 float X, float Y, float W, float H, float PixelSize,
                                 Lur::Render::Color Color,
                                 Lur::Text::EHAlign HAlign = Lur::Text::EHAlign::Left,
                                 Lur::Text::EVAlign VAlign = Lur::Text::EVAlign::Top,
                                 bool Wrap = true) const;

    void SetDebugOverflow(bool On) { DebugOverflow = On; }

private:
    const Lur::Text::Font*      Font = nullptr;
    Lur::Render::MaterialHandle AtlasMaterial = 0;   // font atlas, white tint
    Lur::Render::MaterialHandle DebugMaterial = 0;   // red, for the overflow outline
    Lur::Render::MeshHandle     UnitQuad = 0;        // for the overflow outline
    bool                        DebugOverflow = false;
};

} // namespace Lur::Hud
