#include "Chess/Captures.h"

#include "Chess/Board.h"

namespace Chess {

void CollectCaptures(const std::vector<Move>& Moves, CaptureList& Out) {
    Out.Count = 0;
    Board B = Board::StartPosition();
    for (const Move& M : Moves) {
        // Read the victim BEFORE the move lands — afterwards the square holds the
        // capturing piece and the evidence is gone.
        const EColor Victim = Opposite(B.SideToMove);
        if (M.Flags & MoveFlagEnPassant) {
            // The taken pawn is not on M.To: it stands on M.To's file at M.From's rank,
            // i.e. the square the capturing pawn passed beside.
            Out.Add({Victim, EPieceType::Pawn});
        } else {
            const EPieceType T = PieceTypeAt(B, Victim, M.To);
            if (T != EPieceType::None) Out.Add({Victim, T});
        }
        B.MakeMove(M);
    }
}

} // namespace Chess
