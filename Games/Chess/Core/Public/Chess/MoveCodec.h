#pragma once
#include <vector>
#include "Chess/Board.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"

namespace Chess {

// The slim-bytes payoff, made concrete.
//
// A naive move needs 12+ bits (From:6 + To:6). Instead we send only the move's
// INDEX within the deterministic legal-move list, costing BitsForIndex(N) bits:
//   - 1 legal move    -> 0 bits   (forced; nothing crosses the wire)
//   - 2 legal moves   -> 1 bit
//   - ~20 (typical)   -> 5 bits
//   - 218 (max ever)  -> 8 bits
// Because both phones regenerate the same legal list from the same position, the
// receiver maps the index straight back to a full move. Squares, flags, promotion
// — none of it travels; it's all reconstructed locally.
void EncodeMove(const Move& MoveToSend, const MoveList& Legal,
                Lur::Serialization::BitWriter& W);

// Inverse of EncodeMove. Returns the indexed legal move, or a default-constructed
// (invalid) Move if the index is out of range — which the session must treat as a
// protocol error rather than apply.
Move DecodeMove(Lur::Serialization::BitReader& R, const MoveList& Legal);

// Encode a whole game as its move sequence from the start position: a 16-bit ply
// count, then each move as its index (EncodeMove) replayed from the start. This is
// the reconnect-resync payload — because chess is turn-based, two boards can only
// diverge by one side being ahead, so exchanging full histories lets the shorter
// side replay the longer and catch up. Slim: ~1 byte per move.
void EncodeGame(const std::vector<Move>& History, Lur::Serialization::BitWriter& W);

// Inverse of EncodeGame: replay Count moves from the start position. Fills OutBoard
// and OutHistory and returns true; returns false (leaving outputs unspecified) on a
// corrupt/illegal stream, which the caller must treat as a failed resync.
bool DecodeGame(Lur::Serialization::BitReader& R, Board& OutBoard,
                std::vector<Move>& OutHistory);

} // namespace Chess
