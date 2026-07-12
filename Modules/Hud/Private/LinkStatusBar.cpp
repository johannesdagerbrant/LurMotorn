#include "Lur/Hud/LinkStatusBar.h"
#include "Lur/Render/Sprite2D.h"

namespace Lur::Hud {

using Lur::Render::Color;
using Lur::Render::MaterialDesc;
using ELinkState = Lur::Net::ELinkState;

void LinkStatusBar::CreateResources(Lur::Render::IRenderer* Renderer) {
    const Lur::Render::Quad Q = Lur::Render::MakeQuad();
    Quad = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);

    // Canonical status palette, indexed by ELinkState (untextured, tinted quads).
    auto Set = [&](ELinkState S, Color C) {
        Colors[static_cast<int>(S)] = Renderer->CreateMaterial(MaterialDesc{0, C, false});
    };
    Set(ELinkState::Searching,       Color{0.95f, 0.65f, 0.15f, 1.0f});  // amber
    Set(ELinkState::Handshaking,     Color{0.25f, 0.55f, 0.95f, 1.0f});  // blue
    Set(ELinkState::Linked,          Color{0.25f, 0.80f, 0.35f, 1.0f});  // green
    Set(ELinkState::Disconnected,    Color{0.85f, 0.20f, 0.20f, 1.0f});  // red
    Set(ELinkState::VersionMismatch, Color{0.85f, 0.20f, 0.75f, 1.0f});  // magenta
}

void LinkStatusBar::Draw(Lur::Render::IRenderer* Renderer, ELinkState State,
                         float X, float Y, float Width, float Height) const {
    using Lur::Math::Mat4;
    const int Idx = static_cast<int>(State);
    if (Idx < 0 || Idx >= 5 || Colors[Idx] == 0 || Width <= 0.0f || Height <= 0.0f) return;
    const Mat4 Model = Mat4::Translation({X, Y, 0.0f}) * Mat4::Scale({Width, Height, 1.0f});
    Renderer->DrawMesh(Quad, Colors[Idx], Model);
}

} // namespace Lur::Hud
