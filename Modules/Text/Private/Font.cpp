#include "Lur/Text/Font.h"

#include <vector>

namespace Lur::Text {

void Font::Init(const CookedFont& Cooked) {
    Source   = &Cooked;
    AtlasTex = 0;
}

const CookedGlyph* Font::Find(uint32_t Codepoint) const {
    if (Source == nullptr || Source->GlyphCount <= 0) return nullptr;
    // The cooked table is sorted by codepoint — binary search.
    int Lo = 0, Hi = Source->GlyphCount - 1;
    while (Lo <= Hi) {
        const int Mid = (Lo + Hi) / 2;
        const uint32_t C = Source->Glyphs[Mid].Codepoint;
        if (C == Codepoint) return &Source->Glyphs[Mid];
        if (C < Codepoint) Lo = Mid + 1;
        else Hi = Mid - 1;
    }
    return nullptr;
}

float Font::Advance(uint32_t Codepoint) const {
    const CookedGlyph* G = Find(Codepoint);
    return G ? G->Advance : 0.0f;
}

void Font::UploadAtlas(Render::IRenderer& Renderer) {
    if (Source == nullptr || Source->Atlas == nullptr) return;
    const int W = Source->AtlasWidth;
    const int H = Source->AtlasHeight;
    const int C = Source->AtlasChannels;   // MSDF = 3
    // LoadTexture wants RGBA; expand the cooked RGB, opaque alpha (coverage is the
    // median(r,g,b) of the MSDF, computed in the shader — alpha is unused for MSDF).
    std::vector<uint8_t> Rgba(static_cast<size_t>(W) * H * 4);
    for (int i = 0; i < W * H; ++i) {
        Rgba[i * 4 + 0] = Source->Atlas[i * C + 0];
        Rgba[i * 4 + 1] = (C > 1) ? Source->Atlas[i * C + 1] : Source->Atlas[i * C + 0];
        Rgba[i * 4 + 2] = (C > 2) ? Source->Atlas[i * C + 2] : Source->Atlas[i * C + 0];
        Rgba[i * 4 + 3] = 255;
    }
    AtlasTex = Renderer.LoadTexture(Rgba.data(), W, H);
}

} // namespace Lur::Text
