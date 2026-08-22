#include "Chess/ChessRecord.h"

#include "Chess/Board.h"
#include "Chess/MoveCodec.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"

namespace Chess {

void ChessRecord::Write(std::vector<uint8_t>& Out) const {
    Out.push_back(Version1);
    Out.push_back(WinsLower);
    Out.push_back(WinsHigher);
    Out.push_back(Draws);
    // Moves: reuse the slim game codec ([u16 ply][packed indices]).
    Lur::Serialization::BitWriter W;
    EncodeGame(Moves, W);
    const std::vector<uint8_t>& MoveBytes = W.Finish();
    Out.insert(Out.end(), MoveBytes.begin(), MoveBytes.end());
}

// Parse the tallies-then-moves body that both versions share, starting at Data.
// Leaves *this untouched and returns false on a corrupt move stream.
bool ChessRecord::ReadBody(const uint8_t* Data, std::size_t Size) {
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

bool ChessRecord::Read(const uint8_t* Data, std::size_t Size) {
    if (Size == 0) { *this = ChessRecord{}; return true; }  // absent -> fresh defaults

    const uint8_t Lead = Data[0];
    if (Lead == Version1) return ReadBody(Data + 1, Size - 1);

    // The high byte range is RESERVED for version bytes, so an unrecognised one is refused rather than
    // guessed at. Without this a future v2 record would fall into the v0 path below and be read as
    // tallies-then-moves — which is precisely the silent misparse #66 exists to prevent, just moved one
    // version along.
    if (Lead >= VersionSpaceBegin) return false;

    // Below the reserved range the lead byte is a WinsLower, so this is a v0 (pre-#66, unversioned)
    // record. Migrate it: if the move stream decodes, the tallies are real and worth keeping, and the
    // next Write re-stamps it as v1. A tally is cheap to keep and annoying to lose.
    //
    // Reachable only from DISK. A v0 peer cannot link (ProtocolVersion 12), so it can never deliver one
    // over the wire.
    return ReadBody(Data, Size);
}

} // namespace Chess
