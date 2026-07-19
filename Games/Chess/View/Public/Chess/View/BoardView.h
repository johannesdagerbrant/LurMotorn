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
#include "Lur/Save/SyncManager.h"
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

#if LUR_INTERNAL
    // --- Developer-only scripted/bot play (issue #57/#58) — compiled out of a
    // SHIPPING build (LUR_INTERNAL=0). Both go through the SAME wire path a real tap
    // uses (build the legal list, encode the index, SendMove, then apply locally), so
    // a bot exercises the exact protocol a human does.

    // Play a move by squares. A promotion defaults to a queen. Returns true iff a
    // legal From->To move existed for the side to move on our turn and was played.
    bool PlayMove(Chess::Square From, Chess::Square To);

    // Play a uniformly-random legal move for the side to move, on our turn. RngState
    // is an in/out LCG seed (each device keeps its own). Returns true iff a move was
    // played. The dev-rig autoplayer calls this every frame it's our turn, so a reply
    // ships the same frame the opponent's move was received.
    bool AutoPlayRandomLegalMove(uint32_t& RngState);
#endif

    // Attach a networked session: peer moves arrive as EMsgType::Move and are applied
    // to the state; our own moves are sent on it; the link-state bar reads it. Colour
    // comes from ChessMatchState (GUID order + match parity), NOT the session. Without
    // a session (and before identity is set) the view is a local hot-seat.
    void AttachSession(Lur::Net::Session* Session);

    // Give the view the save store + sync manager + this device's GUID so it can
    // populate the opponent selector and switch the active match between opponents.
    // Without it, the selector is hidden (a store-less hot-seat).
    void AttachPersistence(Lur::Save::Store* Store, Lur::Save::SyncManager* Sync,
                           std::string LocalGuid);

    // Optional log sink (the platform wires it to the "OnlyChess" tag). Used for the
    // selector's diagnostic line; no-op if unset.
    void SetLogger(std::function<void(const char*)> Logger) { Log = std::move(Logger); }

    // Optional hook fired right after a move is applied to the board (any path: local tap,
    // peer move, or dev autoplay). The app wires it to the SFX mixer so a move clicks the
    // instant it lands — kept as a bare callback so the view has no audio dependency. The
    // trigger is wait-free on the app side (Mixer::Play just enqueues). No-op if unset.
    void SetMovePlayed(std::function<void()> Hook) { MovePlayed = std::move(Hook); }

    // Optional draw hook invoked at the very end of the GUI layer, inside the frame
    // (after all game GUI, before EndFrame) — the seam an app uses to composite an
    // engine overlay (e.g. Hud::DebugOverlay, issue #54) on top. No-op if unset.
    void SetPostGuiHook(std::function<void()> Hook) { PostGuiHook = std::move(Hook); }

    // The opponent the active match is against: a GUID, or empty for "same device"
    // (local both-sides play).
    const std::string& ActiveOpponentGuid() const { return ActiveOpponent; }

    // A peer link came up. Applies the hijack rule (#38) and returns whether we
    // adopted this peer as the active game (the app sends its record iff true):
    //   - active is "same device" (empty) -> adopt the peer (the only auto-switch);
    //   - active == this peer            -> adopt (the selected game just went live);
    //   - active is a DIFFERENT opponent -> do NOT hijack (stay on the selected game).
    bool OnPeerLinked(const std::string& PeerGuid);

private:
    // Decode a peer move (its index in our regenerated legal list) and apply it.
    void ApplyRemoteMove(const uint8_t* Data, std::size_t Size);

    // Rebuild the selector's item list from the store + live link state.
    void RebuildItems();
    // Record the last move's wall-clock time + persist the record for the active
    // opponent (so offline moves survive and sync on the next link).
    void StampMove();
    // Switch the active match to an opponent GUID (hard-load its record) or, for an
    // empty GUID, to a fresh "same device" local game. Persists the outgoing game.
    void SwitchActive(const std::string& Guid);

    // True when the local player views from Black's side (board rotated 180°).
    bool FlipBoard() const;
    // True when a tap may make a move now (our turn on a live link, or hot-seat).
    bool CanMoveNow() const;

    ChessMatchState* State = nullptr;   // authoritative game state (app-owned)
    Square Selected = NoSquare;

    Lur::Net::Session* Net = nullptr;   // null => local hot-seat

    // Opponent selector (replaces the old link-status bar). Populated from the store.
    Lur::Save::Store*         Persist = nullptr;   // null => selector hidden
    Lur::Save::SyncManager*   Sync = nullptr;      // rekeyed as the active opponent switches
    std::string               DeviceId;            // this device's GUID
    std::string               ActiveOpponent;      // "" => same device (local)
    std::vector<std::string>  ItemGuid;            // GUID per selector row ("" = header/same-device)
    bool                      ItemsDirty = true;   // rebuild the list on next Render
    int                       LastLink = -1;       // last ELinkState, to detect changes
    std::function<void(const char*)> Log;          // optional "OnlyChess" log sink
    std::function<void()>            PostGuiHook;  // optional overlay draw (issue #54)
    std::function<void()>            MovePlayed;   // optional SFX trigger on move (audio)

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
