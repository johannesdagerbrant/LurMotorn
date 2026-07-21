#pragma once
// Lur::Sim::FromString / ToString for Fixed — the Q16.16 half of the CVar/console
// value codec (dev-console-cvar-tech-spec.md §1.2). It lives in Modules/Sim, NOT
// Core, because Fixed is a Sim type and Sim depends on Core (never the reverse). A
// CVar<Fixed> is a Core template; its SetFromString/ValueString call FromString/
// ToString unqualified, so ADL finds THESE overloads for a Fixed argument while the
// generic bool/int/float/enum overloads come from Lur::Core.
//
// Integer-only on both sides (no float in the path) so parsing is exact and
// deterministic — the same reason the sim uses Fixed at all.
#include <cstdint>
#include <cstdio>
#include <string>

#include "Lur/Sim/Fixed.h"

namespace Lur::Sim {

// Parse an optional-sign decimal EXACTLY into raw Q16.16 with round-to-nearest.
// "0.7" -> round(0.7 * 65536) = 45875, matching Rps::F(7,10)'s truncating build.
inline bool FromString(const char* S, Fixed& Out) {
    if (!S) return false;
    // Trim ASCII whitespace both ends.
    const char* B = S;
    while (*B == ' ' || *B == '\t' || *B == '\r' || *B == '\n') ++B;
    const char* E = B;
    while (*E) ++E;
    while (E > B && (E[-1] == ' ' || E[-1] == '\t' || E[-1] == '\r' || E[-1] == '\n')) --E;
    if (E == B) return false;

    const char* P = B;
    bool Neg = false;
    if (*P == '+' || *P == '-') { Neg = (*P == '-'); ++P; }

    int64_t IntPart = 0;
    bool AnyDigit = false;
    for (; P < E && *P >= '0' && *P <= '9'; ++P) {
        IntPart = IntPart * 10 + (*P - '0');
        AnyDigit = true;
        if (IntPart > 32768) return false;  // Q16.16 integer range guard
    }
    int64_t FracNum = 0, FracDen = 1;
    if (P < E && *P == '.') {
        ++P;
        for (; P < E && *P >= '0' && *P <= '9'; ++P) {
            if (FracDen <= 1000000000LL) {  // cap precision; extra digits ignored
                FracNum = FracNum * 10 + (*P - '0');
                FracDen *= 10;
            }
            AnyDigit = true;
        }
    }
    if (P != E || !AnyDigit) return false;  // trailing junk / bare sign or dot

    const int64_t FracRaw = (FracNum * Fixed::One + FracDen / 2) / FracDen;
    int64_t Raw = (IntPart << Fixed::FracBits) + FracRaw;
    if (Neg) Raw = -Raw;
    if (Raw < INT32_MIN || Raw > INT32_MAX) return false;
    Out = Fixed{static_cast<int32_t>(Raw)};
    return true;
}

// Fixed -> the SHORTEST decimal that FromString reads back to the same raw. Q16.16 has
// no exact decimal for most values (raw 45875 is 0.69999...), but it IS the value that
// "0.7" parses to under round-to-nearest — so we emit "0.7", not "0.699997". This keeps
// a hand-edited cvars.cfg stable (what you type is what persists) and readable. We try
// increasing fractional precision and stop at the first that round-trips exactly;
// 5 digits (>65536) always suffices, 6 is the safety cap. Integer-only formatting.
inline std::string ToString(Fixed V) {
    const bool Neg = V.Raw < 0;
    const int64_t A = Neg ? -static_cast<int64_t>(V.Raw) : static_cast<int64_t>(V.Raw);
    const int64_t Ip = A >> Fixed::FracBits;
    const int64_t Fp = A & (Fixed::One - 1);

    for (int P = 0; P <= 6; ++P) {
        int64_t Pow = 1;
        for (int k = 0; k < P; ++k) Pow *= 10;
        // digits = round(Fp / 65536 * 10^P), then reconstruct raw exactly as FromString
        // would from (Ip, digits, 10^P) and accept the first precision that matches.
        const int64_t Digits = (Fp * Pow + Fixed::One / 2) / Fixed::One;
        const int64_t FracRaw = (Digits * Fixed::One + Pow / 2) / Pow;
        if ((Ip << Fixed::FracBits) + FracRaw != A) continue;  // this precision loses info
        std::string Out;
        if (Neg) Out += '-';
        Out += std::to_string(Ip);
        if (P > 0 && Digits != 0) {
            char FracBuf[8];
            std::snprintf(FracBuf, sizeof(FracBuf), "%0*lld", P, static_cast<long long>(Digits));
            Out += '.';
            Out += FracBuf;
        }
        return Out;
    }
    // Unreachable for Q16.16 (6 digits always round-trip); keep the compiler happy.
    return std::to_string(static_cast<long long>(V.Raw)) + "/65536";
}

}  // namespace Lur::Sim
