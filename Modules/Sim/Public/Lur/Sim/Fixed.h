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
// everywhere. A turn-based game may never need it; a reflex game cannot ship without it.
struct Fixed {
    int32_t Raw = 0;
    static constexpr int FracBits = 16;
    static constexpr int32_t One = 1 << FracBits;

    constexpr Fixed() = default;
    constexpr explicit Fixed(int32_t Raw) : Raw(Raw) {}

    // Q16.16 represents integers in [-32768, 32767]. Outside that range the shift
    // overflows; we do it in unsigned to keep the behaviour DEFINED (wrap) instead of
    // signed-overflow UB — but callers must stay in range (documented precondition).
    static constexpr Fixed FromInt(int32_t Value) {
        return Fixed{static_cast<int32_t>(static_cast<uint32_t>(Value) << FracBits)};
    }
    constexpr int32_t ToInt() const { return Raw >> FracBits; }

    constexpr Fixed operator+(Fixed O) const { return Fixed{Raw + O.Raw}; }
    constexpr Fixed operator-(Fixed O) const { return Fixed{Raw - O.Raw}; }
    constexpr Fixed operator*(Fixed O) const {
        return Fixed{static_cast<int32_t>((static_cast<int64_t>(Raw) * O.Raw) >> FracBits)};
    }
    constexpr Fixed operator/(Fixed O) const {
        // Divide-by-zero is a programmer error: in a constant expression it hard-errors
        // at compile time (loud, as intended); at runtime it saturates to 0 rather than
        // hardware-trapping the process.
        return O.Raw == 0
                   ? Fixed{0}
                   : Fixed{static_cast<int32_t>((static_cast<int64_t>(Raw) << FracBits) / O.Raw)};
    }
    constexpr bool operator==(Fixed O) const { return Raw == O.Raw; }
    constexpr bool operator!=(Fixed O) const { return Raw != O.Raw; }
    constexpr bool operator<(Fixed O) const { return Raw < O.Raw; }
    constexpr bool operator<=(Fixed O) const { return Raw <= O.Raw; }
    constexpr bool operator>(Fixed O) const { return Raw > O.Raw; }
    constexpr bool operator>=(Fixed O) const { return Raw >= O.Raw; }
    constexpr Fixed operator-() const { return Fixed{-Raw}; }
};

// Free helpers — grown on demand as real sim call sites need them (issue
// #75), not speculatively. All pure integer ops on Raw, so determinism-safe.
constexpr Fixed Abs(Fixed A) { return A.Raw < 0 ? Fixed{-A.Raw} : A; }
constexpr Fixed Min(Fixed A, Fixed B) { return A.Raw < B.Raw ? A : B; }
constexpr Fixed Max(Fixed A, Fixed B) { return A.Raw > B.Raw ? A : B; }

// ---- Compile-time Fixed builders ----
// F(n) = whole number; F(num, den) = a rational, for sub-integer values like a 0.45-units/tick
// speed, evaluated through an int64 intermediate so it is exact and constexpr.
//
// Promoted out of Rps/Tunables.h (#201). They are builders for THIS type and belong beside it; every
// game that types a Fixed literal wants them, and the FRound rationale below is a property of the
// decimal codec in Modules/Core, not of any game.
constexpr Fixed F(int32_t V) { return Fixed::FromInt(V); }
constexpr Fixed F(int32_t Num, int32_t Den) {
    return Fixed{static_cast<int32_t>((static_cast<int64_t>(Num) << Fixed::FracBits) / Den)};
}
// Rounding counterpart to F(Num, Den) — use it for any value you would naturally WRITE as a decimal,
// so the compile-time constant and the typed/persisted one are the same raw.
//
// F truncates; the console + cvars.cfg decimal codec (FixedString.h) rounds to nearest. They agree
// whenever the division is exact and disagree when it is not: F(1,10) is raw 6553, but "0.1" parses
// to 6554. A truncated default therefore can NEVER round-trip through its own console — it persists
// as "0.09999" — and a hand-edited cvars.cfg saying "0.1" silently differs from it. Not theoretical:
// it cost one raw unit of w_predator_flee and showed up as a StateHash difference between the two
// phones (#155), visible only because the pre-match LOCKSTEP readout prints the hash.
constexpr Fixed FRound(int32_t Num, int32_t Den) {
    return Fixed{static_cast<int32_t>(((static_cast<int64_t>(Num) << Fixed::FracBits) + Den / 2) / Den)};
}

} // namespace Lur::Sim
