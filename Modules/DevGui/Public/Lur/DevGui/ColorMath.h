#pragma once
// Lur::DevGui::ColorMath — HSV <-> RGB for the colour picker (#174). Self-contained: Modules/Math
// is vectors and matrices for rendering and has no colour space code, and this is the only caller.
//
// Float, and that is fine: colours are RENDER state, never simulation state. The determinism rule
// (no floats in the sim) is about Modules/Sim; a CVar<Color> is barred from AffectsGameplay
// anyway, since CVar's compile-time guard rejects float gameplay knobs.
#include <cmath>

namespace Lur::DevGui::ColorMath {

inline float Clamp01(float V) { return V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V); }

// H in [0,1) (wrapping), S and V in [0,1]. Standard six-sector conversion.
inline void HsvToRgb(float H, float S, float V, float& R, float& G, float& B) {
    H = H - std::floor(H);            // wrap, so a hue slider dragged past the end continues
    S = Clamp01(S);
    V = Clamp01(V);
    const float Hx = H * 6.0f;
    const int   Sector = static_cast<int>(Hx) % 6;
    const float F = Hx - std::floor(Hx);
    const float P = V * (1.0f - S);
    const float Q = V * (1.0f - S * F);
    const float T = V * (1.0f - S * (1.0f - F));
    switch (Sector) {
        case 0:  R = V; G = T; B = P; break;
        case 1:  R = Q; G = V; B = P; break;
        case 2:  R = P; G = V; B = T; break;
        case 3:  R = P; G = Q; B = V; break;
        case 4:  R = T; G = P; B = V; break;
        default: R = V; G = P; B = Q; break;
    }
}

// The inverse. LOSSY at the grey/black/white corners: when S or V is 0 the hue carries no
// information and this returns 0. That is exactly why the picker keeps H,S,V as its own live
// working state while open and only re-derives from the CVar on an EXTERNAL change — round-
// tripping every frame would snap the hue handle to red the moment you drag into a corner.
inline void RgbToHsv(float R, float G, float B, float& H, float& S, float& V) {
    const float Mx = (R > G ? (R > B ? R : B) : (G > B ? G : B));
    const float Mn = (R < G ? (R < B ? R : B) : (G < B ? G : B));
    const float D = Mx - Mn;
    V = Mx;
    S = (Mx <= 0.0f) ? 0.0f : (D / Mx);
    if (D <= 0.0f) { H = 0.0f; return; }
    if (Mx == R)      H = (G - B) / D + (G < B ? 6.0f : 0.0f);
    else if (Mx == G) H = (B - R) / D + 2.0f;
    else              H = (R - G) / D + 4.0f;
    H /= 6.0f;
}

// The fully-saturated, fully-bright colour of a hue — what the SV square is tinted with, and
// what each stop of the hue strip is.
inline void HueColor(float H, float& R, float& G, float& B) { HsvToRgb(H, 1.0f, 1.0f, R, G, B); }

}  // namespace Lur::DevGui::ColorMath
