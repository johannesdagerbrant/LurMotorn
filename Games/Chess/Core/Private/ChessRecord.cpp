#include "Chess/ChessRecord.h"

#include "Chess/Board.h"
#include "Chess/MoveCodec.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"

namespace Chess {

void ChessRecord::Write(std::vector<uint8_t>& Out) const {
    Out.push_back(WinsLower);
    Out.push_back(WinsHigher);
    Out.push_back(Draws);
    // Moves: reuse the slim game codec ([u16 ply][packed indices]).
    Lur::Serialization::BitWriter W;
    EncodeGame(Moves, W);
    const std::vector<uint8_t>& MoveBytes = W.Finish();
    Out.insert(Out.end(), MoveBytes.begin(), MoveBytes.end());
}

bool ChessRecord::Read(const uint8_t* Data, std::size_t Size) {
    if (Size == 0) { *this = ChessRecord{}; return true; }  // absent -> fresh defaults
    if (Size < 3) return false;                             // need at least the 3 tallies

    Board Replayed;
    std::vector<Move> DecodedMoves;
    Lur::Serialization::BitReader R(Data + 3, Size - 3);
    if (!DecodeGame(R, Replayed, DecodedMoves)) return false;  // corrupt move stream

    WinsLower  = Data[0];
    WinsHigher = Data[1];
    Draws      = Data[2];
    Moves      = std::move(DecodedMoves);
    return true;
}

} // namespace Chess
