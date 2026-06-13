#pragma once
#include <cstdint>

namespace Lur::Sim {

// Q16.16 fixed-point number.
//
// Why fixed-point lives in the engine core: when the reflex games arrive, their
// netcode will use rollback — each device predicts the peer's input and silently
// re-simulates when corrected. That ONLY works if both devices compute bit-for-bit
// identical results from identical inputs. Floating-point can diverge across CPUs,
// compilers, and optimization levels; fixed-point integer math is deterministic
// everywhere. Chess doesn't need this yet, but the platform is built on it.
struct Fixed {
    int32_t Raw = 0;
    static constexpr int FracBits = 16;
    static constexpr int32_t One = 1 << FracBits;

    constexpr Fixed() = default;
    constexpr explicit Fixed(int32_t Raw) : Raw(Raw) {}

    static constexpr Fixed FromInt(int32_t Value) { return Fixed{Value << FracBits}; }
    constexpr int32_t ToInt() const { return Raw >> FracBits; }

    constexpr Fixed operator+(Fixed O) const { return Fixed{Raw + O.Raw}; }
    constexpr Fixed operator-(Fixed O) const { return Fixed{Raw - O.Raw}; }
    constexpr Fixed operator*(Fixed O) const {
        return Fixed{static_cast<int32_t>((static_cast<int64_t>(Raw) * O.Raw) >> FracBits)};
    }
    constexpr Fixed operator/(Fixed O) const {
        return Fixed{static_cast<int32_t>((static_cast<int64_t>(Raw) << FracBits) / O.Raw)};
    }
    constexpr bool operator==(Fixed O) const { return Raw == O.Raw; }
    constexpr bool operator<(Fixed O) const { return Raw < O.Raw; }
};

} // namespace Lur::Sim
