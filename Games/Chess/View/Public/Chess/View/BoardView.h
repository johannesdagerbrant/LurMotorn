#pragma once
#include "Lur/Render/Renderer.h"
#include "Chess/Board.h"

namespace Chess {

// The chess board's presentation + interaction, shared verbatim by the Android
// and iOS apps. Owns the render resources (one shared quad; square, piece, and
// highlight materials; the six piece-silhouette textures) plus the current
// position and selection. It talks only to the abstract Lur::Render::IRenderer,
// so it has no platform or Vulkan dependency.
//
// The platform app drives it: CreateResources once the renderer is up, Render
// each frame (it owns the whole BeginFrame..EndFrame), and OnTap on a touch.
class BoardView {
public:
    void CreateResources(Lur::Render::IRenderer* Renderer);
    void Render(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx);
    void OnTap(float XPx, float YPx, float WidthPx, float HeightPx);

private:
    Board  Position = Board::StartPosition();
    Square Selected = NoSquare;

    Lur::Render::MeshHandle     QuadMesh = 0;
    Lur::Render::MaterialHandle LightSquare = 0;
    Lur::Render::MaterialHandle DarkSquare = 0;
    Lur::Render::MaterialHandle Highlight = 0;
    // Per piece TYPE (Chess::EPieceType order), light- and dark-tinted materials
    // over that type's single silhouette texture (the "tint trick").
    Lur::Render::MaterialHandle PieceLight[6] = {};
    Lur::Render::MaterialHandle PieceDark[6] = {};
};

} // namespace Chess
