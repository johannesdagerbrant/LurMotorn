#include "Chess/MoveCodec.h"
#include "Lur/Serialization/Varint.h"

namespace Chess {

using Lur::Serialization::BitsForIndex;

void EncodeMove(const Move& MoveToSend, const MoveList& Legal,
                Lur::Serialization::BitWriter& W) {
    const int Bits = BitsForIndex(static_cast<uint32_t>(Legal.Count));

    // Linear scan is fine: Legal.Count is tiny (<= ~40 in practice) and this runs
    // once per move, not in any tight loop.
    uint32_t Index = 0;
    for (int I = 0; I < Legal.Count; ++I) {
        if (Legal.Moves[I] == MoveToSend) {
            Index = static_cast<uint32_t>(I);
            break;
        }
    }

    // When Bits == 0 (a forced move) this is a no-op: nothing crosses the wire.
    W.WriteBits(Index, Bits);
}

Move DecodeMove(Lur::Serialization::BitReader& R, const MoveList& Legal) {
    const int Bits = BitsForIndex(static_cast<uint32_t>(Legal.Count));
    const uint32_t Index = R.ReadBits(Bits);
    if (static_cast<int>(Index) < Legal.Count) {
        return Legal.Moves[Index];
    }
    return Move{};  // out-of-range: invalid move, session treats as protocol error
}

void EncodeGame(const std::vector<Move>& History, Lur::Serialization::BitWriter& W) {
    W.WriteBits(static_cast<uint32_t>(History.size()), 16);  // ply count
    Board B = Board::StartPosition();
    for (const Move& M : History) {
        MoveList Legal;
        GenerateLegalMoves(B, Legal);
        EncodeMove(M, Legal, W);   // index into the legal list at this ply
        B.MakeMove(M);
    }
}

bool DecodeGame(Lur::Serialization::BitReader& R, Board& OutBoard,
                std::vector<Move>& OutHistory) {
    const uint32_t Count = R.ReadBits(16);
    if (!R.IsOk()) return false;
    Board B = Board::StartPosition();
    OutHistory.clear();
    for (uint32_t i = 0; i < Count; ++i) {
        MoveList Legal;
        GenerateLegalMoves(B, Legal);
        const Move M = DecodeMove(R, Legal);
        if (!R.IsOk() || M == Move{}) return false;  // corrupt / illegal index
        B.MakeMove(M);
        OutHistory.push_back(M);
    }
    OutBoard = B;
    return true;
}

} // namespace Chess
