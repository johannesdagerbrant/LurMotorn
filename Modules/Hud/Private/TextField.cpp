#include "Lur/Hud/TextField.h"

#include <cstring>
#include <vector>

#include "Lur/Render/Sprite2D.h"

namespace Lur::Hud {

using Lur::Render::Color;
using Lur::Render::MaterialDesc;
using Lur::Render::Vertex;
using namespace Lur::Text;

void TextField::CreateResources(Lur::Render::IRenderer* Renderer, const Lur::Text::Font* F) {
    Font = F;
    // The glyph atlas material: white tint so the per-vertex colour is the text colour.
    if (F != nullptr && F->Atlas() != 0)
        AtlasMaterial = Renderer->CreateMaterial(MaterialDesc{F->Atlas(), Color{1, 1, 1, 1}, false});

    // Debug overflow outline resources (a unit quad + a red material).
    const Lur::Render::Quad Q = Lur::Render::MakeQuad();
    UnitQuad = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);
    DebugMaterial = Renderer->CreateMaterial(MaterialDesc{0, Color{0.90f, 0.15f, 0.15f, 0.9f}, false});
}

Lur::Text::LayoutResult TextField::Draw(Lur::Render::IRenderer* Renderer, const char* Text,
                                        float X, float Y, float W, float H, float PixelSize,
                                        Color Col, EHAlign HAlign, EVAlign VAlign,
                                        bool Wrap) const {
    LayoutResult R;
    if (Font == nullptr || AtlasMaterial == 0 || Text == nullptr) return R;

    LayoutSpec Spec;
    Spec.X = X; Spec.Y = Y; Spec.W = W; Spec.H = H;
    Spec.PixelSize = PixelSize;
    Spec.HAlign = HAlign; Spec.VAlign = VAlign; Spec.Wrap = Wrap;
    LayoutText(*Font, Text, static_cast<int>(std::strlen(Text)), Spec, R);

    if (R.Count > 0) {
        // Expand each placed glyph into a quad (4 verts / 6 indices), UVs + colour baked.
        std::vector<Vertex>   Verts(static_cast<size_t>(R.Count) * 4);
        std::vector<uint32_t> Indices(static_cast<size_t>(R.Count) * 6);
        for (int i = 0; i < R.Count; ++i) {
            const PlacedGlyph& G = R.Glyphs[i];
            Vertex* V = &Verts[i * 4];
            // (left,top)-(right,bottom) → 4 corners, matching MakeQuad winding (0,1,2, 0,2,3).
            V[0].Position = {G.X0, G.Y0, 0.0f}; V[0].Uv = {G.U0, G.V0};
            V[1].Position = {G.X0, G.Y1, 0.0f}; V[1].Uv = {G.U0, G.V1};
            V[2].Position = {G.X1, G.Y1, 0.0f}; V[2].Uv = {G.U1, G.V1};
            V[3].Position = {G.X1, G.Y0, 0.0f}; V[3].Uv = {G.U1, G.V0};
            for (int k = 0; k < 4; ++k) {
                V[k].Normal = {0.0f, 0.0f, 0.0f};
                V[k].Color  = {Col.R, Col.G, Col.B, Col.A};
            }
            const uint32_t Base = static_cast<uint32_t>(i * 4);
            uint32_t* Idx = &Indices[i * 6];
            Idx[0] = Base + 0; Idx[1] = Base + 1; Idx[2] = Base + 2;
            Idx[3] = Base + 0; Idx[4] = Base + 2; Idx[5] = Base + 3;
        }
        Renderer->DrawGlyphs(Verts.data(), static_cast<uint32_t>(Verts.size()),
                             Indices.data(), static_cast<uint32_t>(Indices.size()),
                             AtlasMaterial, Font->DistanceRange());
    }

    // Dev aid: outline the field in red when the text doesn't fit.
    if (DebugOverflow && (R.OverflowX || R.OverflowY) && UnitQuad != 0) {
        using Lur::Math::Mat4;
        const float T = 2.0f;   // outline thickness (px)
        auto Bar = [&](float Bx, float By, float Bw, float Bh) {
            Renderer->DrawMesh(UnitQuad, DebugMaterial,
                Mat4::Translation({Bx, By, 0.0f}) * Mat4::Scale({Bw, Bh, 1.0f}));
        };
        Bar(X, Y, W, T);              // top
        Bar(X, Y + H - T, W, T);      // bottom
        Bar(X, Y, T, H);              // left
        Bar(X + W - T, Y, T, H);      // right
    }
    return R;
}

} // namespace Lur::Hud
