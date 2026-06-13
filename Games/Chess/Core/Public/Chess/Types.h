#pragma once
#include <cstdint>

namespace Chess {

enum class EColor : uint8_t { White = 0, Black = 1 };

enum class EPieceType : uint8_t {
    Pawn = 0, Knight = 1, Bishop = 2, Rook = 3, Queen = 4, King = 5, None = 6
};

// Square index 0..63, rank-major: a1 = 0, b1 = 1, ... h1 = 7, a2 = 8, ... h8 = 63.
using Square = uint8_t;
constexpr Square NoSquare = 64;

// Per-move classification bits (set by move generation; consumed by make/unmake).
// Unscoped enum so flags combine with |. Values carry the MoveFlag prefix so they
// never collide with members like Board::EnPassant.
enum EMoveFlag : uint8_t {
    MoveFlagNone        = 0,
    MoveFlagDoublePush  = 1 << 0,  // pawn moved two squares (sets en-passant target)
    MoveFlagEnPassant   = 1 << 1,  // pawn captured en passant
    MoveFlagCastleKing  = 1 << 2,
    MoveFlagCastleQueen = 1 << 3,
    MoveFlagPromotion   = 1 << 4,  // `Promo` field is meaningful
};

// A chess move. Note this is the INTERNAL representation; it is never sent on the
// wire — the codec transmits only the move's index within the legal-move list.
struct Move {
    Square     From  = NoSquare;
    Square     To    = NoSquare;
    EPieceType Promo = EPieceType::None;  // valid only when MoveFlagPromotion is set
    uint8_t    Flags = MoveFlagNone;

    constexpr bool operator==(const Move& O) const {
        return From == O.From && To == O.To && Promo == O.Promo && Flags == O.Flags;
    }
};

} // namespace Chess
