#pragma once
#include <cstdint>

namespace Lur::Sim {

// Deterministic seeded PRNG — splitmix64 (Steele et al.). ~30 lines, one u64 of
// state, pure integer ops so it produces identical bytes on every target (host
// x86, Android/iOS ARM). That determinism is the whole point: its output can feed
// the sim (map/seed derivation now; unit variation later) without breaking the
// bit-for-bit-identical-on-both-peers contract that Fixed/TickClock exist to hold.
//
// It is NOT a cryptographic RNG and doesn't try to be — anti-cheat is an explicit
// engine non-goal (co-located, trusted players). Statistical quality is plenty.
struct SplitMix64 {
    uint64_t State = 0;

    constexpr explicit SplitMix64(uint64_t Seed) : State(Seed) {}

    constexpr uint64_t Next() {
        uint64_t Z = (State += 0x9E3779B97F4A7C15ull);
        Z = (Z ^ (Z >> 30)) * 0xBF58476D1CE4E5B9ull;
        Z = (Z ^ (Z >> 27)) * 0x94D049BB133111EBull;
        return Z ^ (Z >> 31);
    }

    // Deterministic value in [0, Bound). Plain modulo: the tiny modulo bias is
    // irrelevant for map layout and — unlike rejection sampling — is branch-free
    // and trivially identical across platforms, which is what actually matters here.
    constexpr uint32_t NextBounded(uint32_t Bound) {
        return Bound == 0 ? 0u : static_cast<uint32_t>(Next() % Bound);
    }
};

} // namespace Lur::Sim
