#include "Chess/Board.h"

#include "Bitboard.h"

namespace Chess {

Square KingSquare(const Board& B, EColor Side) {
    return static_cast<Square>(LsbIndex(B.Pieces[static_cast<int>(Side)][static_cast<int>(EPieceType::King)]));
}

bool IsSquareAttacked(const Board& B, Square S, EColor ByColor) {
    const int By = static_cast<int>(ByColor);
    const uint64_t Occ = AllOccupied(B);

    if (KnightAttacks(S) & B.Pieces[By][static_cast<int>(EPieceType::Knight)]) return true;
    if (KingAttacks(S)   & B.Pieces[By][static_cast<int>(EPieceType::King)])   return true;

    const uint64_t BishopsQueens = B.Pieces[By][static_cast<int>(EPieceType::Bishop)]
                                 | B.Pieces[By][static_cast<int>(EPieceType::Queen)];
    if (BishopAttacks(S, Occ) & BishopsQueens) return true;

    const uint64_t RooksQueens = B.Pieces[By][static_cast<int>(EPieceType::Rook)]
                               | B.Pieces[By][static_cast<int>(EPieceType::Queen)];
    if (RookAttacks(S, Occ) & RooksQueens) return true;

    // A `ByColor` pawn attacks S iff it stands on a square from which it could
    // capture onto S — which is exactly the attack pattern of the OPPOSITE color
    // pawn placed on S.
    if (PawnAttacks(S, Opposite(ByColor)) & B.Pieces[By][static_cast<int>(EPieceType::Pawn)]) return true;

    return false;
}

bool IsInCheck(const Board& B, EColor Side) {
    return IsSquareAttacked(B, KingSquare(B, Side), Opposite(Side));
}

// Append castling moves for the side to move (after the king's normal moves, so
// the ordering stays deterministic). Verifies rights, empty squares, and that the
// king is not in check and does not pass through or land on an attacked square.
static void AddCastling(const Board& B, EColor Us, MoveList& Out) {
    const EColor Them = Opposite(Us);
    const uint64_t Occ = AllOccupied(B);

    if (Us == EColor::White) {
        if ((B.Castling & CastleWhiteKing) && !(Occ & (Bit(5) | Bit(6)))
            && !IsSquareAttacked(B, 4, Them) && !IsSquareAttacked(B, 5, Them) && !IsSquareAttacked(B, 6, Them))
            Out.Add({4, 6, EPieceType::None, MoveFlagCastleKing});
        if ((B.Castling & CastleWhiteQueen) && !(Occ & (Bit(1) | Bit(2) | Bit(3)))
            && !IsSquareAttacked(B, 4, Them) && !IsSquareAttacked(B, 3, Them) && !IsSquareAttacked(B, 2, Them))
            Out.Add({4, 2, EPieceType::None, MoveFlagCastleQueen});
    } else {
        if ((B.Castling & CastleBlackKing) && !(Occ & (Bit(61) | Bit(62)))
            && !IsSquareAttacked(B, 60, Them) && !IsSquareAttacked(B, 61, Them) && !IsSquareAttacked(B, 62, Them))
            Out.Add({60, 62, EPieceType::None, MoveFlagCastleKing});
        if ((B.Castling & CastleBlackQueen) && !(Occ & (Bit(57) | Bit(58) | Bit(59)))
            && !IsSquareAttacked(B, 60, Them) && !IsSquareAttacked(B, 59, Them) && !IsSquareAttacked(B, 58, Them))
            Out.Add({60, 58, EPieceType::None, MoveFlagCastleQueen});
    }
}

// Promotion piece order — part of the wire ordering. Q, R, B, N.
static constexpr EPieceType PromoOrder[4] = {
    EPieceType::Queen, EPieceType::Rook, EPieceType::Bishop, EPieceType::Knight
};

void GenerateLegalMoves(const Board& B, MoveList& Out) {
    Out.Count = 0;

    const EColor Us = B.SideToMove;
    const EColor Them = Opposite(Us);
    const uint64_t Own = B.Occupied[static_cast<int>(Us)];
    const uint64_t Opp = B.Occupied[static_cast<int>(Them)];
    const uint64_t Occ = Own | Opp;

    const int Dir       = (Us == EColor::White) ? 8 : -8;
    const int StartRank = (Us == EColor::White) ? 1 : 6;
    const int PromoRank = (Us == EColor::White) ? 7 : 0;

    // Deterministic ORDER (this IS the wire protocol): iterate `From` squares
    // 0..63 ascending; for each own piece, emit targets in ascending square order
    // (PopLsb yields lowest first); promotions expand Q,R,B,N; the king's castles
    // follow its normal moves. The same code on both phones => the same indices.
    MoveList Pseudo;

    for (Square From = 0; From < 64; ++From) {
        if (!(Own & Bit(From))) continue;
        const EPieceType Pt = PieceTypeAt(B, Us, From);

        switch (Pt) {
            case EPieceType::Pawn: {
                const Square One = static_cast<Square>(From + Dir);
                if (!(Occ & Bit(One))) {
                    if (RankOf(One) == PromoRank) {
                        for (EPieceType Pp : PromoOrder) Pseudo.Add({From, One, Pp, MoveFlagPromotion});
                    } else {
                        Pseudo.Add({From, One, EPieceType::None, MoveFlagNone});
                        if (RankOf(From) == StartRank) {
                            const Square Two = static_cast<Square>(From + 2 * Dir);
                            if (!(Occ & Bit(Two))) Pseudo.Add({From, Two, EPieceType::None, MoveFlagDoublePush});
                        }
                    }
                }
                uint64_t Caps = PawnAttacks(From, Us) & Opp;
                while (Caps) {
                    const Square To = PopLsb(Caps);
                    if (RankOf(To) == PromoRank) {
                        for (EPieceType Pp : PromoOrder) Pseudo.Add({From, To, Pp, MoveFlagPromotion});
                    } else {
                        Pseudo.Add({From, To, EPieceType::None, MoveFlagNone});
                    }
                }
                if (B.EnPassant != NoSquare && (PawnAttacks(From, Us) & Bit(B.EnPassant))) {
                    Pseudo.Add({From, B.EnPassant, EPieceType::None, MoveFlagEnPassant});
                }
            } break;

            case EPieceType::Knight: {
                uint64_t T = KnightAttacks(From) & ~Own;
                while (T) Pseudo.Add({From, PopLsb(T), EPieceType::None, MoveFlagNone});
            } break;
            case EPieceType::Bishop: {
                uint64_t T = BishopAttacks(From, Occ) & ~Own;
                while (T) Pseudo.Add({From, PopLsb(T), EPieceType::None, MoveFlagNone});
            } break;
            case EPieceType::Rook: {
                uint64_t T = RookAttacks(From, Occ) & ~Own;
                while (T) Pseudo.Add({From, PopLsb(T), EPieceType::None, MoveFlagNone});
            } break;
            case EPieceType::Queen: {
                uint64_t T = QueenAttacks(From, Occ) & ~Own;
                while (T) Pseudo.Add({From, PopLsb(T), EPieceType::None, MoveFlagNone});
            } break;
            case EPieceType::King: {
                uint64_t T = KingAttacks(From) & ~Own;
                while (T) Pseudo.Add({From, PopLsb(T), EPieceType::None, MoveFlagNone});
                AddCastling(B, Us, Pseudo);
            } break;

            default: break;
        }
    }

    // Legality filter: a pseudo-legal move is legal iff it does not leave our own
    // king in check. Copy-make keeps this simple and bug-resistant (no unmake).
    for (int I = 0; I < Pseudo.Count; ++I) {
        Board C = B;
        C.MakeMove(Pseudo.Moves[I]);
        if (!IsSquareAttacked(C, KingSquare(C, Us), Them)) Out.Add(Pseudo.Moves[I]);
    }
}

EGameResult Result(const Board& B) {
    MoveList Moves;
    GenerateLegalMoves(B, Moves);
    if (Moves.Count == 0) {
        return IsInCheck(B, B.SideToMove) ? EGameResult::Checkmate : EGameResult::Stalemate;
    }
    if (B.HalfmoveClock >= 100) return EGameResult::DrawFiftyMove;  // 100 plies = 50 full moves

    // Conservative insufficient-material: no pawns/rooks/queens and at most one
    // minor piece in total (K vs K, K+N vs K, K+B vs K). Anything richer is left
    // Ongoing rather than risk a wrong draw.
    uint64_t Heavy = 0, Minors = 0;
    for (int C = 0; C < 2; ++C) {
        Heavy |= B.Pieces[C][static_cast<int>(EPieceType::Pawn)];
        Heavy |= B.Pieces[C][static_cast<int>(EPieceType::Rook)];
        Heavy |= B.Pieces[C][static_cast<int>(EPieceType::Queen)];
        Minors |= B.Pieces[C][static_cast<int>(EPieceType::Bishop)];
        Minors |= B.Pieces[C][static_cast<int>(EPieceType::Knight)];
    }
    if (Heavy == 0 && PopCount(Minors) <= 1) return EGameResult::DrawInsufficientMaterial;

    return EGameResult::Ongoing;
}

} // namespace Chess
