#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Lur::Serialization {

// MSB-first bit-stream writer.
//
// Bits are packed into bytes from the most-significant bit downward, giving a
// deterministic wire layout that BitReader consumes symmetrically. This is the
// foundation of LurMotorn's "smallest possible payload" goal: an encoder writes
// exactly as many bits as an event carries — e.g. a chess move as a ~4-6 bit
// index into the legal-move list — with no per-field byte padding.
//
// Usage:
//   BitWriter W;
//   W.WriteBits(MoveIndex, BitsNeeded);          // e.g. 5 bits
//   const std::vector<uint8_t>& Bytes = W.Finish();  // pads final byte, returns packed bytes
class BitWriter {
public:
    // Append the low `Count` bits of `Value` (0 <= Count <= 32), MSB-first.
    // Writing Count == 0 is a no-op (used when a choice is forced: 1 legal move).
    void WriteBits(uint32_t Value, int Count);

    // Append a single bit.
    void WriteBit(bool Bit);

    // Bits written so far, excluding the zero padding added by Finish().
    std::size_t GetBitCount() const { return BitCount; }

    // Pad the trailing partial byte with zero bits and return the packed bytes.
    // Call exactly once when encoding is complete. Do not write afterwards
    // without Reset(), or byte alignment will be corrupted.
    const std::vector<uint8_t>& Finish();

    // Clear all state so the writer can be reused.
    void Reset();

private:
    std::vector<uint8_t> Bytes;
    uint8_t     Cur = 0;       // bits accumulated for the current (incomplete) byte
    int         CurBits = 0;   // valid bits in Cur (0..7)
    std::size_t BitCount = 0;
};

} // namespace Lur::Serialization
