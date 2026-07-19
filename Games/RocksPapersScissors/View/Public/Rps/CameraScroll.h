#pragma once
#include <cmath>

namespace Rps {

// The vertical scroll camera — one home for the feel the mains all share (Android, iOS,
// desktop): content-drag (the ground sticks to the finger), flick momentum that dampens
// after release, and clamping to the field. Pure view state (per-device); it never
// touches the sim. `Y` is the world-Y at the bottom of the screen, in the (possibly
// flipped) render space GameView draws in — the per-player flip lives in GameView, so
// this stays orientation-agnostic.
struct CameraScroll {
    float Y = 0.0f;            // world Y at screen bottom (the scroll position)
    float Vel = 0.0f;         // Y velocity, world-units/second (for momentum)
    bool  Dragging = false;
    float PrevTouchYpx = 0.0f;
    float PrevFrameY = 0.0f;

    void Begin(float TouchYpx) {
        Dragging = true;
        Vel = 0.0f;
        PrevTouchYpx = TouchYpx;
    }
    // Content-drag: moving the finger down moves the ground down with it (grab the world,
    // like spinning a globe), so Y follows the finger delta.
    void Move(float TouchYpx, float Ppu) {
        if (!Dragging || Ppu <= 0.0f) return;
        Y += (TouchYpx - PrevTouchYpx) / Ppu;
        PrevTouchYpx = TouchYpx;
    }
    void End() { Dragging = false; }

    // Once per frame: while dragging, measure velocity from the frame's motion; after
    // release, coast on that velocity and dampen it exponentially (~0.3 s time constant),
    // stopping cleanly at the field edges. MinCam is normally NEGATIVE (#85): the view
    // may scroll below world-0 by the height of the bottom HUD block, so the home camp
    // clears the production plates when fully scrolled down.
    void Update(float DtSec, float MaxCam, float MinCam = 0.0f) {
        if (Dragging) {
            const float Inst = DtSec > 0.0f ? (Y - PrevFrameY) / DtSec : 0.0f;
            Vel = 0.5f * Vel + 0.5f * Inst;  // light smoothing so a flick reads clean
        } else if (DtSec > 0.0f) {
            Y += Vel * DtSec;
            Vel *= std::exp(-DtSec / 0.3f);
            if (std::fabs(Vel) < 0.01f) Vel = 0.0f;
        }
        if (Y < MinCam) { Y = MinCam; Vel = 0.0f; }
        if (Y > MaxCam) { Y = MaxCam; Vel = 0.0f; }
        PrevFrameY = Y;
    }
};

}  // namespace Rps
