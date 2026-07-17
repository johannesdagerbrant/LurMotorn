#pragma once
#include <cstdint>
#include <string_view>
#include "Chess/Types.h"
#include "Lur/Core/Assert.h"

namespace Chess {

constexpr int MaxMoves = 256;  // safe upper bound on legal moves in any position (true max 218)

// Fixed-capacity move list — no heap allocation in the hot path.
struct MoveList {
    Move Moves[MaxMoves];
    int  Count = 0;
    // 256 bounds legal chess (max 218 moves), so this can't overflow from real move
    // generation — but a future off-by-one would silently smash the stack. Assert loudly
    // in dev; the write is unchanged in release.
    void Add(const Move& M) {
        LUR_ASSERT_MSG(Count < MaxMoves, "MoveList overflow (Count=%d)", Count);
        Moves[Count++] = M;
    }
};

enum ECastleRight : uint8_t {
    CastleWhiteKing  = 1 << 0, CastleWhiteQueen = 1 << 1,
    CastleBlackKing  = 1 << 2, CastleBlackQueen = 1 << 3,
    CastleAll = CastleWhiteKing | CastleWhiteQueen | CastleBlackKing | CastleBlackQueen,
};

enum class EGameResult : uint8_t {
    Ongoing, Checkmate, Stalemate, DrawFiftyMove, DrawInsufficientMaterial,
};

// Bitboard board state. One uint64_t per (color, piece type): bit S set means a
// piece stands on square S. Bitboards make move generation fast via bitwise ops.
struct Board {
    uint64_t Pieces[2][6] = {};   // [color][piece type]
    uint64_t Occupied[2]  = {};   // per-color union, kept in sync with Pieces
    EColor   SideToMove   = EColor::White;
    uint8_t  Castling     = CastleAll;
    Square   EnPassant    = NoSquare;  // capture-target square behind a double push
    uint16_t HalfmoveClock = 0;        // plies since last capture/pawn move (50-move rule)
    uint16_t Fullmove      = 1;

    static Board StartPosition();
    static Board FromFen(std::string_view Fen);

    // Apply a legal move in place: moves the piece (handling capture, promotion,
    // en passant, castling), updates castling rights, the en-passant target, the
    // clocks, side to move, and occupancy.
    void MakeMove(const Move& M);
};

inline EColor   Opposite(EColor C)        { return C == EColor::White ? EColor::Black : EColor::White; }
inline uint64_t AllOccupied(const Board& B) { return B.Occupied[0] | B.Occupied[1]; }

// Piece type of `Side` standing on `S`, or EPieceType::None.
EPieceType PieceTypeAt(const Board& B, EColor Side, Square S);

// Square of `Side`'s king.
Square KingSquare(const Board& B, EColor Side);

// Is `S` attacked by any piece of `ByColor` (used for check, castling, legality)?
bool IsSquareAttacked(const Board& B, Square S, EColor ByColor);

// Is `Side`'s king currently in check?
bool IsInCheck(const Board& B, EColor Side);

// Generate all LEGAL moves into `Out`.
//
// CRITICAL: the ORDER of moves is part of the wire protocol. Both phones run this
// identical function on the identical position, so a move encoded as "index k" by
// one device decodes to the same move on the other. Any change to ordering is a
// protocol-breaking change (bump Lur::Net::ProtocolVersion). See MoveGen.cpp for
// the exact, documented ordering, and MoveCodec.h for how it is used.
void GenerateLegalMoves(const Board& B, MoveList& Out);

// Result for the side to move. Note: threefold repetition is NOT detected here —
// it needs game history and is the session layer's concern.
EGameResult Result(const Board& B);

} // namespace Chess
