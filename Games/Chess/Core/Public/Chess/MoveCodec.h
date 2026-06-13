#pragma once
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

} // namespace Chess
