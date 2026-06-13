#pragma once
#include <cstddef>
#include <cstdint>

namespace Lur::Serialization {

// MSB-first bit-stream reader — the exact inverse of BitWriter.
//
// Reads consume bits in the same most-significant-first order they were written,
// so a decoder recovers values written by the peer's BitWriter bit-for-bit.
// Reading past the end sets a sticky not-ok flag (see IsOk()) and yields zero
// bits, rather than reading out of bounds — important when decoding untrusted
// wire data.
class BitReader {
public:
    BitReader(const uint8_t* Data, std::size_t SizeBytes)
        : Data(Data), SizeBits(SizeBytes * 8) {}

    // Read `Count` bits (0 <= Count <= 32), MSB-first, returned in the low bits.
    uint32_t ReadBits(int Count);

    // Read a single bit.
    bool ReadBit();

    std::size_t GetBitsConsumed() const { return Pos; }
    std::size_t GetBitsRemaining() const { return Pos <= SizeBits ? SizeBits - Pos : 0; }

    // False once any read has gone past the end of the buffer.
    bool IsOk() const { return Ok; }

private:
    const uint8_t* Data;
    std::size_t SizeBits;
    std::size_t Pos = 0;
    bool Ok = true;
};

} // namespace Lur::Serialization
