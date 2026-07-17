#pragma once
#include <cstdint>
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"

namespace Lur::Serialization {

// Variable-length unsigned integer over the bit stream.
//
// Encodes `Value` as groups of `GroupBits` data bits, each preceded by a
// continuation bit (1 = more groups follow, 0 = last group). Small values cost
// little: with the default 4-bit groups, values < 16 take 5 bits, < 256 take 10,
// etc. Useful for counters/lengths whose magnitude varies (clock ticks, move
// numbers) without committing to a fixed width.
inline void WriteVarUint(BitWriter& W, uint64_t Value, int GroupBits = 4) {
    const uint32_t Mask = (GroupBits >= 32) ? 0xFFFFFFFFu : ((1u << GroupBits) - 1u);
    for (;;) {
        const uint32_t Group = static_cast<uint32_t>(Value) & Mask;
        Value >>= GroupBits;
        const bool More = (Value != 0);
        W.WriteBit(More);
        W.WriteBits(Group, GroupBits);
        if (!More) break;
    }
}

inline uint64_t ReadVarUint(BitReader& R, int GroupBits = 4) {
    uint64_t Value = 0;
    int Shift = 0;
    for (;;) {
        const bool More = R.ReadBit();
        const uint64_t Group = R.ReadBits(GroupBits);
        // Ignore any bits past bit 63: `Group << Shift` is undefined once Shift >= 64,
        // and a corrupt/overlong stream (continuation bit stuck on) would drive it
        // there. This decodes peer-supplied bytes, so guard quietly; the loop still
        // terminates on the last group or when the reader runs dry (!R.IsOk()).
        if (Shift < 64) Value |= Group << Shift;
        Shift += GroupBits;
        if (!More || !R.IsOk()) break;
    }
    return Value;
}

// Smallest number of bits needed to index `Count` distinct items (0..Count-1).
// Returns 0 when Count <= 1 — a forced choice costs nothing on the wire, which
// is exactly why "1 legal move" encodes to zero bits.
inline int BitsForIndex(uint32_t Count) {
    if (Count <= 1) return 0;
    int Bits = 0;
    uint32_t N = Count - 1;
    while (N > 0) { ++Bits; N >>= 1; }
    return Bits;
}

} // namespace Lur::Serialization
