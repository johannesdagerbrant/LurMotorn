#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "Lur/Render/Renderer.h"
#include "Lur/Hud/Dropdown.h"
#include "Lur/Hud/TextField.h"
#include "Lur/Text/Font.h"
#include "Lur/Save/Store.h"
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

    // Give the view the save store + this device's GUID so it can populate the
    // opponent selector (list historical opponents, per-opponent last-move times).
    // Without it, the selector is hidden (a store-less hot-seat).
    void AttachPersistence(Lur::Save::Store* Store, std::string LocalGuid);

    // Optional log sink (the platform wires it to the "OnlyChess" tag). Used for the
    // selector's diagnostic line; no-op if unset.
    void SetLogger(std::function<void(const char*)> Logger) { Log = std::move(Logger); }

    // The opponent the selector currently targets: a GUID, or empty for "same device"
    // (local both-sides play). Display state in #37; drives the active match in #38.
    const std::string& ActiveOpponentGuid() const { return ActiveOpponent; }

private:
    // Decode a peer move (its index in our regenerated legal list) and apply it.
    void ApplyRemoteMove(const uint8_t* Data, std::size_t Size);

    // Rebuild the selector's item list from the store + live link state.
    void RebuildItems();
    // Record the last move's wall-clock time against the currently-linked opponent.
    void StampMove();

    // True when the local player views from Black's side (board rotated 180°).
    bool FlipBoard() const;
    // True when a tap may make a move now (our turn on a live link, or hot-seat).
    bool CanMoveNow() const;

    ChessMatchState* State = nullptr;   // authoritative game state (app-owned)
    Square Selected = NoSquare;

    Lur::Net::Session* Net = nullptr;   // null => local hot-seat

    // Opponent selector (replaces the old link-status bar). Populated from the store.
    Lur::Save::Store*         Persist = nullptr;   // null => selector hidden
    std::string               DeviceId;            // this device's GUID
    std::string               ActiveOpponent;      // "" => same device (local)
    std::vector<std::string>  ItemGuid;            // GUID per selector row ("" = header/same-device)
    bool                      ItemsDirty = true;   // rebuild the list on next Render
    int                       LastLink = -1;       // last ELinkState, to detect changes
    std::function<void(const char*)> Log;          // optional "OnlyChess" log sink

    Lur::Render::MeshHandle     QuadMesh = 0;
    Lur::Render::MaterialHandle LightSquare = 0;
    Lur::Render::MaterialHandle DarkSquare = 0;
    Lur::Render::MaterialHandle Highlight = 0;
    Lur::Hud::Dropdown          Selector;   // engine widget: the opponent dropdown
    // MSDF text: the all-time W/L/D score line + the between-match result banner (#22).
    Lur::Text::Font             UiFont;
    Lur::Hud::TextField         Text;
    // Per piece TYPE (Chess::EPieceType order), light- and dark-tinted materials
    // over that type's single silhouette texture (the "tint trick").
    Lur::Render::MaterialHandle PieceLight[6] = {};
    Lur::Render::MaterialHandle PieceDark[6] = {};
};

} // namespace Chess
