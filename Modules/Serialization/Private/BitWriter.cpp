#include "Lur/Serialization/BitWriter.h"

namespace Lur::Serialization {

void BitWriter::WriteBit(bool Bit) {
    Cur = static_cast<uint8_t>((Cur << 1) | (Bit ? 1u : 0u));
    ++CurBits;
    ++BitCount;
    if (CurBits == 8) {
        Bytes.push_back(Cur);
        Cur = 0;
        CurBits = 0;
    }
}

void BitWriter::WriteBits(uint32_t Value, int Count) {
    // MSB-first: emit the highest of the `Count` bits first so the reader,
    // shifting left as it consumes, reconstructs the same value.
    for (int I = Count - 1; I >= 0; --I) {
        WriteBit((Value >> I) & 1u);
    }
}

const std::vector<uint8_t>& BitWriter::Finish() {
    if (CurBits > 0) {
        // Left-align the leftover bits into the high end of the final byte and
        // pad the low bits with zeros, so byte 0..n-1 read back MSB-first.
        Cur = static_cast<uint8_t>(Cur << (8 - CurBits));
        Bytes.push_back(Cur);
        Cur = 0;
        CurBits = 0;
    }
    return Bytes;
}

void BitWriter::Reset() {
    Bytes.clear();
    Cur = 0;
    CurBits = 0;
    BitCount = 0;
}

} // namespace Lur::Serialization
