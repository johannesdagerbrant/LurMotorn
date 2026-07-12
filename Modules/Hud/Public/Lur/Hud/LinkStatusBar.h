#pragma once
#include "Lur/Render/Renderer.h"
#include "Lur/Net/Session.h"

namespace Lur::Hud {

// A reusable heads-up link-state indicator: a filled bar whose colour reflects a
// net session's connection state (searching / handshaking / linked / lost /
// version-mismatch). Engine-level and game-agnostic — a game supplies only the
// screen rect to fill (where the bar goes depends on that game's own layout), and
// the canonical status palette lives here so every game reads the same.
//
// Draw() must be called inside an IRenderer BeginFrame..EndFrame using a 2D/ortho
// camera (pixel-space X/Y, Y down), the same space Render/Sprite2D sets up.
class LinkStatusBar {
public:
    void CreateResources(Lur::Render::IRenderer* Renderer);
    void Draw(Lur::Render::IRenderer* Renderer, Lur::Net::ELinkState State,
              float X, float Y, float Width, float Height) const;

private:
    Lur::Render::MeshHandle     Quad = 0;
    Lur::Render::MaterialHandle Colors[5] = {};  // indexed by ELinkState
};

} // namespace Lur::Hud
