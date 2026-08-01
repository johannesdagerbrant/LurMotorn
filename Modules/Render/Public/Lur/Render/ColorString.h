#pragma once
// FromString / ToString / operator== for Render::Color, so CVar<Color> works (#116).
//
// Lives in Lur::Render and is found by ADL, exactly like Lur::Sim's Fixed overloads — that is
// what lets Modules/Core stay free of any dependency on Render or Sim while CVar<T> still
// parses both. Core's CVar.h calls FromString/ToString UNQUALIFIED for precisely this reason.
//
// Syntax (spec Addendum E.3): four floats 0..1, "r g b a". Alpha is optional and defaults to 1
// — most tuned colours are opaque and typing a trailing 1 every time is friction. Values are
// NOT clamped on parse: the console warns-but-allows out of range everywhere else, and a
// >1 channel is meaningful when a tint is used as a multiplier.
#include <cstdio>
#include <string>

#include "Lur/Core/FromString.h"
#include "Lur/Render/Renderer.h"

namespace Lur::Render {

// Needed by CVar<T>::Overridden(), which asks whether the value still equals the default.
// Exact float comparison is right here: the question is "has anyone edited this", and an edit
// that lands on a bit-identical value genuinely is not an edit.
inline bool operator==(const Color& A, const Color& B) {
    return A.R == B.R && A.G == B.G && A.B == B.B && A.A == B.A;
}
inline bool operator!=(const Color& A, const Color& B) { return !(A == B); }

// "r g b [a]" — whitespace-separated floats. Returns false and leaves Out untouched on any
// malformed component, so a typo cannot half-apply a colour (three channels set, one stale).
inline bool FromString(const char* S, Color& Out) {
    if (!S) return false;
    float V[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    int Count = 0;
    const char* P = S;
    while (Count < 4) {
        while (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n') ++P;
        if (*P == '\0') break;
        char* End = nullptr;
        const float F = std::strtof(P, &End);
        if (End == P) return false;   // junk where a number was expected
        V[Count++] = F;
        P = End;
    }
    while (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n') ++P;
    if (*P != '\0') return false;     // trailing junk (a 5th component, a stray word)
    if (Count < 3) return false;      // r g b are mandatory; a is not
    Out = Color{V[0], V[1], V[2], V[3]};
    return true;
}

inline std::string ToString(const Color& C) {
    char Buf[80];
    std::snprintf(Buf, sizeof(Buf), "%g %g %g %g", static_cast<double>(C.R),
                  static_cast<double>(C.G), static_cast<double>(C.B), static_cast<double>(C.A));
    return Buf;
}

// Index of the channel a ".r"/".g"/".b"/".a" suffix names, or -1. Used by the console's
// per-channel edit (`theme.accent.g 0.5`) and by the picker's four sliders, so both agree on
// which letter is which slot.
inline int ColorChannelIndex(char Suffix) {
    switch (Suffix) {
        case 'r': case 'R': return 0;
        case 'g': case 'G': return 1;
        case 'b': case 'B': return 2;
        case 'a': case 'A': return 3;
        default:            return -1;
    }
}
inline float GetColorChannel(const Color& C, int Index) {
    switch (Index) {
        case 0:  return C.R;
        case 1:  return C.G;
        case 2:  return C.B;
        case 3:  return C.A;
        default: return 0.0f;
    }
}
inline void SetColorChannel(Color& C, int Index, float V) {
    switch (Index) {
        case 0: C.R = V; break;
        case 1: C.G = V; break;
        case 2: C.B = V; break;
        case 3: C.A = V; break;
        default: break;
    }
}

}  // namespace Lur::Render
