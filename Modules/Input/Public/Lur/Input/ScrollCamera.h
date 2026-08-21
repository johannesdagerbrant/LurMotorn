#pragma once
// Vertical content-drag scroll with flick momentum — one home for the feel every main shares.
//
// Promoted out of Rps::CameraScroll (#201). Zero game content: it takes touch pixels and a
// pixels-per-unit scale, and produces a scroll position. That makes it a gesture recogniser, which is
// what Modules/Input is for, sitting beside ConsoleGesture rather than in a renderer.
//
// CONTENT-DRAG, not scrollbar-drag: moving the finger down moves the ground down with it, like
// spinning a globe. Getting this backwards is the single most common way a scroll view feels wrong,
// so the direction is asserted in the tests rather than left to a reviewer's intuition.
//
// Pure view state, per device — it never touches simulation state, and two peers may sit at totally
// different scroll positions. `Y` is the world-Y at the bottom of the screen in whatever render space
// the caller draws in; any per-player flip belongs to the caller, so this stays orientation-agnostic.
#include <cmath>

namespace Lur::Input {

struct ScrollCamera {
    float Y = 0.0f;      // world Y at screen bottom (the scroll position)
    float Vel = 0.0f;    // world units/second, for post-release momentum
    bool  Dragging = false;

    // Exponential time constant for the flick coast, in seconds. Higher = longer glide.
    float DampingTau = 0.3f;
    // Below this speed the coast is stopped outright, so it comes to rest instead of creeping.
    float RestSpeed = 0.01f;

    void Begin(float TouchYpx) {
        Dragging = true;
        Vel = 0.0f;
        PrevTouchYpx = TouchYpx;
    }

    // Content-drag: Y follows the finger delta, scaled out of pixels.
    void Move(float TouchYpx, float Ppu) {
        if (!Dragging || Ppu <= 0.0f) return;
        Y += (TouchYpx - PrevTouchYpx) / Ppu;
        PrevTouchYpx = TouchYpx;
    }

    void End() { Dragging = false; }

    // Once per frame. While dragging, measure velocity from the frame's motion (lightly smoothed so a
    // flick reads clean); after release, coast and dampen exponentially in REAL TIME, so the glide is
    // the same on a 30 Hz and a 60 Hz device. Clamps to [MinY, MaxY] and kills the velocity at an edge
    // so it stops cleanly instead of fighting the clamp.
    //
    // MinY is often NEGATIVE: a caller may want to scroll below world zero by the height of a bottom
    // HUD block, so the home area clears the overlay when fully scrolled down.
    void Update(float DtSec, float MaxY, float MinY = 0.0f) {
        if (Dragging) {
            const float Inst = DtSec > 0.0f ? (Y - PrevFrameY) / DtSec : 0.0f;
            Vel = 0.5f * Vel + 0.5f * Inst;
        } else if (DtSec > 0.0f) {
            Y += Vel * DtSec;
            Vel *= std::exp(-DtSec / DampingTau);
            if (std::fabs(Vel) < RestSpeed) Vel = 0.0f;
        }
        if (Y < MinY) { Y = MinY; Vel = 0.0f; }
        if (Y > MaxY) { Y = MaxY; Vel = 0.0f; }
        PrevFrameY = Y;
    }

private:
    float PrevTouchYpx = 0.0f;
    float PrevFrameY = 0.0f;
};

}  // namespace Lur::Input
