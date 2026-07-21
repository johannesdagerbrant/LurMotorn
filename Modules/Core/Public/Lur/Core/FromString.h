#pragma once
// Lur::Core::FromString / ToString — the console/CVar/persistence value codec
// (dev-console-cvar-tech-spec.md §1.2, §5). Text <-> the primitive CVar value types:
// bool, integral (int32 and the small ints enums decay to), float, and enums
// (generically, as their underlying int).
//
// It lives in Modules/Core because BOTH the engine and games register CVars, and the
// same codec backs the console field, the desktop panel, and the human-readable
// cvars.cfg. Core is the dependency-free base layer, so the Lur::Sim::Fixed overloads
// do NOT live here (Sim depends on Core, never the reverse) — they are in
// Lur/Sim/FixedString.h and found by ADL at a CVar<Fixed> call site.
//
// It is only ever *called* from dev-only surfaces, but the header itself is unguarded
// (pure, header-only, no cost if unreferenced) so it can be unit-tested plainly.
//
// Contract: FromString returns false on malformed input (the caller decides whether
// that is assert-loud or a warned skip — house policy differs for our-invariant vs
// hand-edited-file). ToString(FromString(x)) round-trips for every representable value.
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace Lur::Core {

// ---- Parse -----------------------------------------------------------------

// Trim ASCII whitespace both ends. Empty after trim => malformed (returns false).
inline bool ParseTrim(const char* S, std::string& Out) {
    if (!S) return false;
    const char* B = S;
    while (*B == ' ' || *B == '\t' || *B == '\r' || *B == '\n') ++B;
    const char* E = B + std::strlen(B);
    while (E > B && (E[-1] == ' ' || E[-1] == '\t' || E[-1] == '\r' || E[-1] == '\n')) --E;
    Out.assign(B, static_cast<size_t>(E - B));
    return !Out.empty();
}

inline bool FromString(const char* S, bool& Out) {
    std::string T;
    if (!ParseTrim(S, T)) return false;
    if (T == "true" || T == "1" || T == "on" || T == "yes")  { Out = true;  return true; }
    if (T == "false" || T == "0" || T == "off" || T == "no") { Out = false; return true; }
    return false;
}

// Every integral CVar type (int32, and the u8/u16 that enums decay to). strtoll with a
// strict full-consume check + a range check for the target width.
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
bool FromString(const char* S, T& Out) {
    std::string Tr;
    if (!ParseTrim(S, Tr)) return false;
    char* End = nullptr;
    errno = 0;
    const long long V = std::strtoll(Tr.c_str(), &End, 10);
    if (End != Tr.c_str() + Tr.size() || errno != 0) return false;  // trailing junk / overflow
    if (V < static_cast<long long>(std::numeric_limits<T>::min()) ||
        V > static_cast<long long>(std::numeric_limits<T>::max()))
        return false;
    Out = static_cast<T>(V);
    return true;
}

inline bool FromString(const char* S, float& Out) {
    std::string Tr;
    if (!ParseTrim(S, Tr)) return false;
    char* End = nullptr;
    errno = 0;
    const double V = std::strtod(Tr.c_str(), &End);
    if (End != Tr.c_str() + Tr.size() || errno != 0) return false;
    Out = static_cast<float>(V);
    return true;
}

// Enums: parse as the underlying integer (by ordinal). Name-based enum parsing is a
// per-enum concern added where a CVar<EnumT> is registered; the codec handles ordinals.
template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
bool FromString(const char* S, E& Out) {
    std::underlying_type_t<E> V{};
    if (!FromString(S, V)) return false;
    Out = static_cast<E>(V);
    return true;
}

// ---- Format ----------------------------------------------------------------

inline std::string ToString(bool V) { return V ? "true" : "false"; }

template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, int> = 0>
std::string ToString(T V) { return std::to_string(static_cast<long long>(V)); }

inline std::string ToString(float V) {
    char Buf[32];
    std::snprintf(Buf, sizeof(Buf), "%g", static_cast<double>(V));
    return Buf;
}

template <class E, std::enable_if_t<std::is_enum_v<E>, int> = 0>
std::string ToString(E V) { return ToString(static_cast<std::underlying_type_t<E>>(V)); }

}  // namespace Lur::Core
