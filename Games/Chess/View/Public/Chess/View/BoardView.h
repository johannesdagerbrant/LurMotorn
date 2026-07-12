#pragma once
#include <cstddef>
#include <cstdint>
#include "Lur/Render/Renderer.h"
#include "Lur/Hud/LinkStatusBar.h"
#include "Chess/ChessMatchState.h"
#include "Chess/Types.h"

namespace Lur::Net { class Session; }

namespace Chess {

// The chess board's PRESENTATION + touch (issue #18): it renders the board and the
// selection and turns taps into moves, but it owns NO game state. The authoritative
// state — position, move history, colour, stats — lives in a ChessMatchState the app
// owns; BoardView reads it to draw and calls into it to apply moves. It talks only to
// the abstract Lur::Render::IRenderer, so it has no platform or Vulkan dependency.
//
// The platform app drives it: SetState + AttachSession once, CreateResources when the
// renderer is up, Render each frame, and OnTap on a touch.
class BoardView {
public:
    // The authoritative match state to render + mutate (owned by the app).
    void SetState(ChessMatchState* MatchState) { State = MatchState; }

    void CreateResources(Lur::Render::IRenderer* Renderer);
    void Render(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx);
    void OnTap(float XPx, float YPx, float WidthPx, float HeightPx);

    // Attach a networked session: peer moves arrive as EMsgType::Move and are applied
    // to the state; our own moves are sent on it; the link-state bar reads it. Colour
    // comes from ChessMatchState (GUID order + match parity), NOT the session. Without
    // a session (and before identity is set) the view is a local hot-seat.
    void AttachSession(Lur::Net::Session* Session);

private:
    // Decode a peer move (its index in our regenerated legal list) and apply it.
    void ApplyRemoteMove(const uint8_t* Data, std::size_t Size);

    // True when the local player views from Black's side (board rotated 180°).
    bool FlipBoard() const;
    // True when a tap may make a move now (our turn on a live link, or hot-seat).
    bool CanMoveNow() const;

    ChessMatchState* State = nullptr;   // authoritative game state (app-owned)
    Square Selected = NoSquare;

    Lur::Net::Session* Net = nullptr;   // null => local hot-seat

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
