#pragma once
#include <cstddef>
#include <cstdint>

namespace Lur::Core {

// FNV-1a, 64-bit. Cheap, dependency-free, good enough for state fingerprints and
// desync detection (Review #2 §4.2/§7.5) — NOT cryptographic. Deterministic across
// platforms (pure integer ops), which is the property the desync check relies on.
inline uint64_t Fnv1a64(const uint8_t* Data, std::size_t Size, uint64_t Seed = 1469598103934665603ull) {
    uint64_t Hash = Seed;
    for (std::size_t i = 0; i < Size; ++i) {
        Hash ^= Data[i];
        Hash *= 1099511628211ull;
    }
    return Hash;
}

}  // namespace Lur::Core
