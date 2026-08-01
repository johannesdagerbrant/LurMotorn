#pragma once
// Lur::DevGui::ColorPicker — the v1 RGBA picker (#117), a popover the console opens for a
// CVar<Color> row in place of the numpad. A colour is four numbers, and typing them one at a
// time through a numeric pad is not editing a colour: you cannot see what you are making.
//
// Layout:  [ swatch                          ]
//          R  [=========o===============]
//          G  [==o======================]
//          B  [==============o==========]
//          A  [========================o]
//
// v2 (the HSV square + hue strip) is deliberately NOT here. It needs per-vertex-coloured
// gradient triangles, and whether DrawGlyphs can draw untextured — or whether we owe the
// renderer a DrawVerts / 1px-white-texture path — is an open question (spec §7). Four sliders
// and a live swatch make CVar<Color> fully usable today without answering it.
//
// PURE geometry + hit-testing, like Numpad: one function produces the rect the renderer draws
// and the tap tests, so a visible control is always pressable. The host owns the colour; this
// type holds no state at all, which is what makes a drag trivially correct (there is nothing
// to keep in sync with the CVar).
#include "Lur/DevGui/Widgets.h"

namespace Lur::DevGui {

struct ColorPicker {
    static constexpr int Channels = 4;

    // ASCII single letters — the MSDF atlas is cooked from the glyphs we ship.
    static const char* ChannelLabel(int I) {
        static const char* L[Channels] = {"R", "G", "B", "A"};
        return (I >= 0 && I < Channels) ? L[I] : "";
    }

    // Total height for a panel with the given row metrics, so the caller can place the popover
    // (PlaceBelowOrAbove needs the height BEFORE anything is laid out).
    static float PanelH(float RowH, float SwatchH, float Gap) {
        return SwatchH + Gap + static_cast<float>(Channels) * (RowH + Gap);
    }

    static void SwatchRect(float X, float Y, float W, float SwatchH,
                           float& Rx, float& Ry, float& Rw, float& Rh) {
        Rx = X; Ry = Y; Rw = W; Rh = SwatchH;
    }

    // Row I's full-width band — the hit target. Deliberately the WHOLE row, not just the track:
    // on a phone a 4 px knob is unhittable, and point-anywhere is the standard behaviour anyway.
    static void RowRect(float X, float Y, float W, float RowH, float SwatchH, float Gap, int I,
                        float& Rx, float& Ry, float& Rw, float& Rh) {
        Rx = X;
        Ry = Y + SwatchH + Gap + static_cast<float>(I) * (RowH + Gap);
        Rw = W;
        Rh = RowH;
    }

    // The slider track inside a row, after the channel letter and before the numeric readout.
    static void TrackRect(float X, float Y, float W, float RowH, float SwatchH, float Gap, int I,
                          float LabelW, float ValueW,
                          float& Tx, float& Ty, float& Tw, float& Th) {
        float Rx, Ry, Rw, Rh;
        RowRect(X, Y, W, RowH, SwatchH, Gap, I, Rx, Ry, Rw, Rh);
        Tx = Rx + LabelW;
        Ty = Ry;
        Tw = Rw - LabelW - ValueW;
        if (Tw < 1.0f) Tw = 1.0f;
        Th = Rh;
    }

    // Hit-test a press. Returns the channel index touched (0..3) and writes the value it
    // selects, or -1 for a press that missed every row. Channels are 0..1 — the picker is the
    // one place a colour IS clamped, because a swatch cannot show you 2.0 and a slider has
    // nowhere to put it. (Typing an out-of-range channel in the console still works; see
    // ColorString.h.)
    static int Tap(float X, float Y, float W, float RowH, float SwatchH, float Gap,
                   float LabelW, float ValueW, float KnobW, float Px, float Py,
                   float& OutValue) {
        for (int I = 0; I < Channels; ++I) {
            float Rx, Ry, Rw, Rh;
            RowRect(X, Y, W, RowH, SwatchH, Gap, I, Rx, Ry, Rw, Rh);
            if (!HitRect(Rx, Ry, Rw, Rh, Px, Py)) continue;
            float Tx, Ty, Tw, Th;
            TrackRect(X, Y, W, RowH, SwatchH, Gap, I, LabelW, ValueW, Tx, Ty, Tw, Th);
            OutValue = Slider::ValueAt(Tx, Tw, KnobW, Px, 0.0f, 1.0f);
            return I;
        }
        return -1;
    }
};

}  // namespace Lur::DevGui
