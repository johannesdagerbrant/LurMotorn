#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "Lur/Render/Renderer.h"
#include "Lur/Hud/LinkStatusBar.h"
#include "Chess/Board.h"

namespace Lur::Net { class Session; }

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

    // Attach a networked session: our colour is derived from the session's seat
    // when the handshake completes (seat 0 -> White), the peer's moves are applied
    // as they arrive, and OnTap only acts on our own turn. Without a session the
    // view stays a local hot-seat (both sides tapped on one device), unchanged.
    void AttachSession(Lur::Net::Session* Session);

private:
    // Decode a peer move (its index in our regenerated legal list) and apply it.
    void ApplyRemoteMove(const uint8_t* Data, std::size_t Size);
    // Apply a move to the board AND record it in History (the resync source).
    void Apply(const Move& M);
    // Reconnect resync: send our full history / adopt the peer's if it is longer.
    void SendResync();
    void OnResync(const uint8_t* Data, std::size_t Size);

    Board  Position = Board::StartPosition();
    Square Selected = NoSquare;
    std::vector<Move> History;                 // moves applied, in order (for resync)

    Lur::Net::Session* Net = nullptr;          // null => local hot-seat
    EColor MyColor = EColor::White;            // meaningful once Net && Net->IsReady()
    bool   ShouldFlipBoard = false;                  // true when local player is Black

    Lur::Render::MeshHandle     QuadMesh = 0;
    Lur::Render::MaterialHandle LightSquare = 0;
    Lur::Render::MaterialHandle DarkSquare = 0;
    Lur::Render::MaterialHandle Highlight = 0;
    Lur::Hud::LinkStatusBar     StatusBar;  // engine widget: draws the link-state bar
    // Per piece TYPE (Chess::EPieceType order), light- and dark-tinted materials
    // over that type's single silhouette texture (the "tint trick").
    Lur::Render::MaterialHandle PieceLight[6] = {};
    Lur::Render::MaterialHandle PieceDark[6] = {};
};

} // namespace Chess
