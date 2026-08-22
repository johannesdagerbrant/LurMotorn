#pragma once
// Chess::ChessGame — chess's answers to Lur::App::IGame, in ONE place (#45).
//
// This is the block that used to be written out in AndroidMain.cpp, AppMain.mm and DesktopMain.cpp:
// the record-sync trio, the view attachment, the match-end hook and the state hash. Identical logic
// three times, differing only in capture style — see IGame.h for the measurement.
//
// It owns the game's state (the match and the board view) as well as the wiring, because splitting
// those was what made the wiring platform-shaped in the first place: each main declared its own
// AppState holding Match + View, so each main had to do the connecting. With both here, a platform
// shell holds a ChessGame and supplies only what is genuinely platform — a save directory, a
// transport, a log sink, a renderer, an audio device.
//
// Deliberately NOT in chess::view. A view draws; this decides. Keeping it in its own chess::app
// library is also what lets it depend on lur::app without dragging GameHost into everything that
// merely wants to render a board.
#include <functional>
#include <string>

#include "Chess/ChessMatchState.h"
#include "Chess/View/BoardView.h"
#include "Lur/App/IGame.h"

namespace Chess {

class ChessGame final : public Lur::App::IGame {
public:
    // ---- Lur::App::IGame ----
    void Configure(Lur::App::GameHost& Host, Lur::App::GameHost::Hooks& Hooks) override;

    // The game's state, for the platform shell to render and route input into. Handed out by
    // reference rather than copied: the view holds pointers to the match, and the host holds
    // callbacks into both.
    BoardView&       View()  { return View_; }
    ChessMatchState& Match() { return Match_; }

private:
    ChessMatchState Match_;   // authoritative game state (record + board + colour)
    BoardView       View_;    // board presentation + touch, shared by every platform
};

}  // namespace Chess
