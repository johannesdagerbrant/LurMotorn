#pragma once
// Lur::DevGui — small render-agnostic widget helpers (#113), same discipline as Numpad: they
// compute GEOMETRY and answer HIT-TESTS, and the host does the drawing through IRenderer. The
// point is that one function produces the rect both the renderer and the hit-test use, so a
// control can never be drawn somewhere you can't press.
//
// Deliberately not an immediate-mode framework with retained hot/active ids. The console owns
// exactly one interaction at a time (a tap, resolved on the render thread where the rects are
// known), so an id stack would be machinery with no second caller.
#include <cstdint>

namespace Lur::DevGui {

// Is (Px,Py) inside the rect? Inclusive on all edges, matching Numpad::Tap — adjacent controls
// share an edge and a press exactly on it should hit the first one tested rather than nothing.
inline bool HitRect(float X, float Y, float W, float H, float Px, float Py) {
    return Px >= X && Px <= X + W && Py >= Y && Py <= Y + H;
}

// ---- Horizontal slider (#116 min/max rows, #117's RGBA channels) ----
// Value <-> pixel mapping for a track of width W at X. The knob is drawn centred on the value;
// the track is inset by half a knob at each end so the knob's CENTRE spans exactly [Min,Max]
// and the knob itself never hangs off the track.
struct Slider {
    // Centre-x of the knob for Value in [Min,Max]. Clamps, so an out-of-range CVar (the console
    // warns-but-allows) parks the knob at an end instead of drawing outside the widget.
    static float KnobX(float X, float W, float KnobW, float Value, float Min, float Max) {
        const float Span = (Max > Min) ? (Max - Min) : 1.0f;
        float T = (Value - Min) / Span;
        if (T < 0.0f) T = 0.0f;
        if (T > 1.0f) T = 1.0f;
        const float Inset = KnobW * 0.5f;
        return X + Inset + T * (W - KnobW);
    }

    // The value a press/drag at Px selects — the exact inverse of KnobX, so grabbing a knob does
    // not make it jump. Clamped to [Min,Max].
    static float ValueAt(float X, float W, float KnobW, float Px, float Min, float Max) {
        const float Inset = KnobW * 0.5f;
        const float Usable = (W - KnobW) > 1.0f ? (W - KnobW) : 1.0f;
        float T = (Px - X - Inset) / Usable;
        if (T < 0.0f) T = 0.0f;
        if (T > 1.0f) T = 1.0f;
        return Min + T * (Max - Min);
    }

    // Point-anywhere: a press on the TRACK jumps the knob there, rather than requiring the user
    // to catch a knob a few pixels wide. Tall hit band (the row height), not the knob's own
    // height — the whole row is the target on a phone.
    static bool Hit(float X, float Y, float W, float H, float Px, float Py) {
        return HitRect(X, Y, W, H, Px, Py);
    }
};

}  // namespace Lur::DevGui
