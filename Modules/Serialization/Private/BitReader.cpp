#include "Lur/Serialization/BitReader.h"

namespace Lur::Serialization {

bool BitReader::ReadBit() {
    if (Pos >= SizeBits) {
        Ok = false;
        return false;
    }
    const std::size_t Byte = Pos >> 3;
    const int BitIndex = 7 - static_cast<int>(Pos & 7);  // MSB-first
    ++Pos;
    return (Data[Byte] >> BitIndex) & 1u;
}

uint32_t BitReader::ReadBits(int Count) {
    uint32_t Value = 0;
    for (int I = 0; I < Count; ++I) {
        Value = (Value << 1) | (ReadBit() ? 1u : 0u);
    }
    return Value;
}

} // namespace Lur::Serialization
