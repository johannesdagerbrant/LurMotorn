#pragma once
#include <vector>

#include "Chess/Types.h"
#include "Lur/Core/Assert.h"

namespace Chess {

// Which pieces have been taken, in the order they were taken (issue #67) — the data
// behind the two capture trays drawn in the board's margins.
//
// Capture order is NOT stored anywhere. It is DERIVED by replaying the record's move
// list from the start position, exactly the way ChessMatchState::RebuildBoard already
// reconstructs the board. That keeps with the engine's "derive minimal state" rule:
// nothing new to persist, nothing new to sync, and no way for the tray to disagree
// with the board it sits beside.

// One taken piece. Color is the colour of the piece that WAS CAPTURED (so a white
// entry is a loss for White), not of the player who took it.
struct CapturedPiece {
    EColor     Color;
    EPieceType Type;
};

// 30 = 32 men minus the two kings, which can never be captured — so this bound is the
// rules', not a guess, and the list can never overflow from a legal game.
constexpr int MaxCaptures = 30;

// Fixed-capacity, like Chess::MoveList: the view rebuilds this whenever the board
// changes, and no heap allocation belongs on that path.
struct CaptureList {
    CapturedPiece Items[MaxCaptures];
    int           Count = 0;

    void Add(const CapturedPiece& C) {
        LUR_ASSERT_MSG(Count < MaxCaptures, "CaptureList overflow (Count=%d)", Count);
        if (Count < MaxCaptures) Items[Count++] = C;
    }
};

// Replay `Moves` from the start position and collect every capture, oldest first.
// Handles en passant (the pawn taken sits behind the destination square, not on it).
// `Moves` must be a legal sequence from Board::StartPosition() — i.e. a ChessRecord's
// move list; anything else is undefined the same way RebuildBoard is.
void CollectCaptures(const std::vector<Move>& Moves, CaptureList& Out);

} // namespace Chess
