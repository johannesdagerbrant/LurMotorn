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

} // namespace Chess
