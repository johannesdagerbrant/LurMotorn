#include "Chess/Board.h"

#include <array>
#include <cctype>

#include "Bitboard.h"

namespace Chess {

static void RecomputeOccupied(Board& B) {
    B.Occupied[0] = 0;
    B.Occupied[1] = 0;
    for (int Pt = 0; Pt < 6; ++Pt) {
        B.Occupied[0] |= B.Pieces[0][Pt];
        B.Occupied[1] |= B.Pieces[1][Pt];
    }
}

static void Place(Board& B, EColor C, EPieceType Pt, Square S) {
    B.Pieces[static_cast<int>(C)][static_cast<int>(Pt)] |= Bit(S);
}

EPieceType PieceTypeAt(const Board& B, EColor Side, Square S) {
    const uint64_t M = Bit(S);
    for (int Pt = 0; Pt < 6; ++Pt) {
        if (B.Pieces[static_cast<int>(Side)][Pt] & M) return static_cast<EPieceType>(Pt);
    }
    return EPieceType::None;
}

Board Board::StartPosition() {
    return Board::FromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

Board Board::FromFen(std::string_view Fen) {
    Board B;
    B.Castling = 0;
    B.EnPassant = NoSquare;

    std::size_t I = 0;
    auto SkipSpaces = [&] { while (I < Fen.size() && Fen[I] == ' ') ++I; };

    // 1) Piece placement, from rank 8 down to rank 1.
    int Rank = 7, File = 0;
    for (; I < Fen.size() && Fen[I] != ' '; ++I) {
        const char C = Fen[I];
        if (C == '/') { --Rank; File = 0; continue; }
        if (C >= '1' && C <= '8') { File += C - '0'; continue; }
        const EColor Col = std::isupper(static_cast<unsigned char>(C)) ? EColor::White : EColor::Black;
        EPieceType Pt = EPieceType::None;
        switch (std::tolower(static_cast<unsigned char>(C))) {
            case 'p': Pt = EPieceType::Pawn;   break;
            case 'n': Pt = EPieceType::Knight; break;
            case 'b': Pt = EPieceType::Bishop; break;
            case 'r': Pt = EPieceType::Rook;   break;
            case 'q': Pt = EPieceType::Queen;  break;
            case 'k': Pt = EPieceType::King;   break;
            default:  break;
        }
        Place(B, Col, Pt, static_cast<Square>(Rank * 8 + File));
        ++File;
    }
    SkipSpaces();

    // 2) Side to move.
    if (I < Fen.size()) { B.SideToMove = (Fen[I] == 'w') ? EColor::White : EColor::Black; ++I; }
    SkipSpaces();

    // 3) Castling rights.
    for (; I < Fen.size() && Fen[I] != ' '; ++I) {
        switch (Fen[I]) {
            case 'K': B.Castling |= CastleWhiteKing;  break;
            case 'Q': B.Castling |= CastleWhiteQueen; break;
            case 'k': B.Castling |= CastleBlackKing;  break;
            case 'q': B.Castling |= CastleBlackQueen; break;
            default:  break;  // '-'
        }
    }
    SkipSpaces();

    // 4) En passant target.
    if (I < Fen.size() && Fen[I] != '-') {
        const int F = Fen[I] - 'a';
        const int R = Fen[I + 1] - '1';
        B.EnPassant = static_cast<Square>(R * 8 + F);
        I += 2;
    } else {
        ++I;  // skip '-'
    }
    SkipSpaces();

    // 5) Halfmove clock.
    { int V = 0; bool Any = false;
      while (I < Fen.size() && Fen[I] >= '0' && Fen[I] <= '9') { V = V * 10 + (Fen[I] - '0'); ++I; Any = true; }
      if (Any) B.HalfmoveClock = static_cast<uint16_t>(V); }
    SkipSpaces();

    // 6) Fullmove number.
    { int V = 0; bool Any = false;
      while (I < Fen.size() && Fen[I] >= '0' && Fen[I] <= '9') { V = V * 10 + (Fen[I] - '0'); ++I; Any = true; }
      if (Any) B.Fullmove = static_cast<uint16_t>(V); }

    RecomputeOccupied(B);
    return B;
}

void Board::MakeMove(const Move& M) {
    const EColor Us = SideToMove;
    const EColor Them = Opposite(Us);
    const int us = static_cast<int>(Us);
    const int them = static_cast<int>(Them);
    const Square From = M.From, To = M.To;
    const uint64_t FromBit = Bit(From), ToBit = Bit(To);

    const EPieceType Moving = PieceTypeAt(*this, Us, From);
    const bool Capture = (Occupied[them] & ToBit) != 0 || (M.Flags & MoveFlagEnPassant);

    // 50-move clock: reset on a pawn move or any capture, else advance.
    if (Moving == EPieceType::Pawn || Capture) HalfmoveClock = 0; else ++HalfmoveClock;

    // Remove a normally-captured piece sitting on `To`.
    if (Occupied[them] & ToBit) {
        for (int Pt = 0; Pt < 6; ++Pt) Pieces[them][Pt] &= ~ToBit;
    }
    // En passant: the captured pawn is beside `To`, not on it.
    if (M.Flags & MoveFlagEnPassant) {
        const Square CapSq = (Us == EColor::White) ? static_cast<Square>(To - 8)
                                                   : static_cast<Square>(To + 8);
        Pieces[them][static_cast<int>(EPieceType::Pawn)] &= ~Bit(CapSq);
    }

    // Move the piece off `From`; place it (or its promotion) on `To`.
    Pieces[us][static_cast<int>(Moving)] &= ~FromBit;
    if (M.Flags & MoveFlagPromotion) {
        Pieces[us][static_cast<int>(M.Promo)] |= ToBit;
    } else {
        Pieces[us][static_cast<int>(Moving)] |= ToBit;
    }

    // Castling: the king move is already done above; shift the rook too.
    if (M.Flags & MoveFlagCastleKing) {
        const Square RFrom = (Us == EColor::White) ? static_cast<Square>(7)  : static_cast<Square>(63);
        const Square RTo   = (Us == EColor::White) ? static_cast<Square>(5)  : static_cast<Square>(61);
        Pieces[us][static_cast<int>(EPieceType::Rook)] &= ~Bit(RFrom);
        Pieces[us][static_cast<int>(EPieceType::Rook)] |=  Bit(RTo);
    } else if (M.Flags & MoveFlagCastleQueen) {
        const Square RFrom = (Us == EColor::White) ? static_cast<Square>(0)  : static_cast<Square>(56);
        const Square RTo   = (Us == EColor::White) ? static_cast<Square>(3)  : static_cast<Square>(59);
        Pieces[us][static_cast<int>(EPieceType::Rook)] &= ~Bit(RFrom);
        Pieces[us][static_cast<int>(EPieceType::Rook)] |=  Bit(RTo);
    }

    // En passant target for the opponent's reply (only after a double push).
    if (M.Flags & MoveFlagDoublePush) {
        EnPassant = (Us == EColor::White) ? static_cast<Square>(From + 8) : static_cast<Square>(From - 8);
    } else {
        EnPassant = NoSquare;
    }

    // Castling-rights update via a per-square mask: vacating e1/e8 drops both of
    // that side's rights; vacating or capturing-onto a1/h1/a8/h8 drops that rook's.
    static const std::array<uint8_t, 64> ClearMask = [] {
        std::array<uint8_t, 64> Mask;
        Mask.fill(0x0F);
        Mask[0]  &= static_cast<uint8_t>(~CastleWhiteQueen);
        Mask[7]  &= static_cast<uint8_t>(~CastleWhiteKing);
        Mask[4]  &= static_cast<uint8_t>(~(CastleWhiteKing | CastleWhiteQueen));
        Mask[56] &= static_cast<uint8_t>(~CastleBlackQueen);
        Mask[63] &= static_cast<uint8_t>(~CastleBlackKing);
        Mask[60] &= static_cast<uint8_t>(~(CastleBlackKing | CastleBlackQueen));
        return Mask;
    }();
    Castling &= ClearMask[From] & ClearMask[To];

    if (Us == EColor::Black) ++Fullmove;
    SideToMove = Them;

    RecomputeOccupied(*this);
}

} // namespace Chess
