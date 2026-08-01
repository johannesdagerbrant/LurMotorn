#pragma once
// Lur::DevGui::ColorPicker — the console's colour editor (#117 v1, #174 v2), a popover opened
// for a CVar<Color> row where the numpad would go. A colour is four numbers, and typing them one
// at a time through a numeric pad means never seeing what you are making.
//
// Layout:  +-----------------------+  <- swatch (the CVar's live value)
//          |                       |
//          |     S V   s q u a r e |  <- saturation across, brightness/value down
//          |                       |
//          +-----------------------+
//          [######## hue ##########]  <- hue strip
//           R 0.88  G 0.31  B 0.22  A 1.00   <- readouts, updated live by either drag
//
// HOW THE SV SQUARE IS BUILT (correctness-critical, and the reason v2 was once deferred):
// three layers, which is what makes it exactly S(horizontal) x V(vertical).
//   1. a white quad;
//   2. a quad whose vertex ALPHA ramps 0 -> 1 left to right, material-tinted with the CURRENT
//      HUE — so dragging the hue strip re-tints it via SetMaterialTint with no mesh rebuild;
//   3. a black quad whose vertex alpha ramps 0 -> 1 top to bottom.
// Compositing 1+2 gives white -> hue across (saturation); 3 darkens downward (value).
//
// Spec §7 asked whether this needed a renderer extension (DrawGlyphs untextured, or a new
// DrawVerts / 1px-white texture). It does not: Lur::Render::Vertex already carries a Vec4 colour
// and meshes with per-vertex colour are already built for the field backdrop.
//
// WORKING-STATE RULE: the host keeps H,S,V as the picker's live state while it is open and writes
// RGBA out to the CVar. It must only re-derive HSV from the CVar when the bound value changes
// EXTERNALLY. RGB->HSV is lossy at the grey/black/white corners (hue is undefined when S or V is
// 0), so round-tripping every frame snaps the hue handle to red the instant you drag into one.
//
// PURE geometry + hit-testing, like Numpad: one function produces the rect the renderer draws and
// the tap tests, so a visible control is always pressable. Holds no state.
#include "Lur/DevGui/Widgets.h"

namespace Lur::DevGui {

struct ColorPicker {
    static constexpr int Channels = 4;

    // ASCII single letters — the MSDF atlas is cooked from the glyphs we ship.
    static const char* ChannelLabel(int I) {
        static const char* L[Channels] = {"R", "G", "B", "A"};
        return (I >= 0 && I < Channels) ? L[I] : "";
    }

    // What a press landed on. The host needs to distinguish these because they write different
    // parts of the working state: the square sets S and V, the strip sets H, and alpha is its own
    // channel that neither touches.
    enum class EHit { None, SvSquare, HueStrip, AlphaStrip };

    // Total height, so the caller can place the popover before anything is laid out
    // (PlaceBelowOrAbove needs the height up front).
    static float PanelH(float SwatchH, float SquareH, float StripH, float ReadoutH, float Gap) {
        return SwatchH + Gap + SquareH + Gap + StripH + Gap + StripH + Gap + ReadoutH;
    }

    static void SwatchRect(float X, float Y, float W, float SwatchH,
                           float& Rx, float& Ry, float& Rw, float& Rh) {
        Rx = X; Ry = Y; Rw = W; Rh = SwatchH;
    }
    static void SquareRect(float X, float Y, float W, float SwatchH, float SquareH, float Gap,
                           float& Rx, float& Ry, float& Rw, float& Rh) {
        Rx = X; Ry = Y + SwatchH + Gap; Rw = W; Rh = SquareH;
    }
    static void HueRect(float X, float Y, float W, float SwatchH, float SquareH, float StripH,
                        float Gap, float& Rx, float& Ry, float& Rw, float& Rh) {
        Rx = X; Ry = Y + SwatchH + Gap + SquareH + Gap; Rw = W; Rh = StripH;
    }
    static void AlphaRect(float X, float Y, float W, float SwatchH, float SquareH, float StripH,
                          float Gap, float& Rx, float& Ry, float& Rw, float& Rh) {
        Rx = X; Ry = Y + SwatchH + Gap + SquareH + Gap + StripH + Gap; Rw = W; Rh = StripH;
    }
    static void ReadoutRect(float X, float Y, float W, float SwatchH, float SquareH, float StripH,
                            float ReadoutH, float Gap,
                            float& Rx, float& Ry, float& Rw, float& Rh) {
        Rx = X;
        Ry = Y + SwatchH + Gap + SquareH + Gap + StripH + Gap + StripH + Gap;
        Rw = W; Rh = ReadoutH;
    }

    // Saturation/value at a point in the square. Saturation runs left(0) -> right(1); VALUE runs
    // top(1) -> bottom(0), the conventional orientation — bright at the top, black along the
    // bottom edge. Clamped, so a drag that leaves the square pins to the edge instead of jumping.
    static void SvAt(float Sx, float Sy, float Sw, float Sh, float Px, float Py,
                     float& OutS, float& OutV) {
        float S = (Sw > 0.0f) ? (Px - Sx) / Sw : 0.0f;
        float V = (Sh > 0.0f) ? 1.0f - (Py - Sy) / Sh : 0.0f;
        OutS = S < 0.0f ? 0.0f : (S > 1.0f ? 1.0f : S);
        OutV = V < 0.0f ? 0.0f : (V > 1.0f ? 1.0f : V);
    }
    // Inverse: where the reticle is drawn for the current S,V.
    static void SvPoint(float Sx, float Sy, float Sw, float Sh, float S, float V,
                        float& OutX, float& OutY) {
        OutX = Sx + S * Sw;
        OutY = Sy + (1.0f - V) * Sh;
    }

    // Hit-test a press against the interactive parts, in draw order. Returns what was hit and
    // writes the value(s) it selects. A press ANYWHERE in a region acts — a knob a few pixels
    // wide is unhittable on a phone, and point-anywhere is the standard behaviour regardless.
    static EHit Hit(float X, float Y, float W, float SwatchH, float SquareH, float StripH,
                    float Gap, float KnobW, float Px, float Py,
                    float& OutA, float& OutB) {
        float Rx, Ry, Rw, Rh;
        SquareRect(X, Y, W, SwatchH, SquareH, Gap, Rx, Ry, Rw, Rh);
        if (HitRect(Rx, Ry, Rw, Rh, Px, Py)) {
            SvAt(Rx, Ry, Rw, Rh, Px, Py, OutA, OutB);   // OutA = S, OutB = V
            return EHit::SvSquare;
        }
        HueRect(X, Y, W, SwatchH, SquareH, StripH, Gap, Rx, Ry, Rw, Rh);
        if (HitRect(Rx, Ry, Rw, Rh, Px, Py)) {
            OutA = Slider::ValueAt(Rx, Rw, KnobW, Px, 0.0f, 1.0f);  // OutA = H
            return EHit::HueStrip;
        }
        AlphaRect(X, Y, W, SwatchH, SquareH, StripH, Gap, Rx, Ry, Rw, Rh);
        if (HitRect(Rx, Ry, Rw, Rh, Px, Py)) {
            OutA = Slider::ValueAt(Rx, Rw, KnobW, Px, 0.0f, 1.0f);  // OutA = alpha
            return EHit::AlphaStrip;
        }
        return EHit::None;
    }
};

}  // namespace Lur::DevGui
