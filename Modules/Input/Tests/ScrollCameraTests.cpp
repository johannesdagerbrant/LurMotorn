// Tests for Lur::Input::ScrollCamera — the content-drag scroll promoted out of RPS (#201).
//
// Two of these assert FEEL rather than arithmetic, deliberately: drag direction (content-drag, not
// scrollbar-drag) is the single most common way a scroll view is wrong, and it is the kind of thing a
// reviewer nods past. The other one worth having is frame-rate independence of the flick coast, since
// the whole point of an exponential in real time is that a 30 Hz device glides the same distance.
#include <cmath>
#include <cstdio>

#include "Lur/Input/ScrollCamera.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Input::ScrollCamera;

// ---- CONTENT-DRAG: the ground follows the finger ----
static void TestDragFollowsFinger() {
    ScrollCamera C;
    C.Begin(100.0f);
    C.Move(150.0f, /*Ppu*/ 10.0f);      // finger moved +50 px at 10 px per world unit
    CHECK(std::fabs(C.Y - 5.0f) < 1e-5f);
    C.Move(100.0f, 10.0f);              // and back
    CHECK(std::fabs(C.Y - 0.0f) < 1e-5f);
}

// ---- Move() before Begin(), or with a degenerate scale, must do nothing ----
static void TestMoveIsInertWithoutDrag() {
    ScrollCamera C;
    C.Move(500.0f, 10.0f);              // never Begin()'d
    CHECK(C.Y == 0.0f);
    C.Begin(0.0f);
    C.Move(500.0f, 0.0f);               // Ppu 0 would divide by zero
    CHECK(C.Y == 0.0f);
    C.Move(500.0f, -4.0f);              // and a negative scale is nonsense, not a flip
    CHECK(C.Y == 0.0f);
}

// ---- A flick keeps coasting after release, then comes to rest ----
static void TestFlickCoastsThenStops() {
    ScrollCamera C;
    C.Begin(0.0f);
    // Drag upward over a few frames so Update() measures a velocity.
    for (int I = 1; I <= 5; ++I) {
        C.Move(static_cast<float>(I) * 20.0f, 10.0f);
        C.Update(1.0f / 60.0f, /*MaxY*/ 1000.0f);
    }
    const float AtRelease = C.Y;
    CHECK(C.Vel > 0.0f);                // moving
    C.End();
    for (int I = 0; I < 5; ++I) C.Update(1.0f / 60.0f, 1000.0f);
    CHECK(C.Y > AtRelease);             // it kept going — that is the momentum
    for (int I = 0; I < 300; ++I) C.Update(1.0f / 60.0f, 1000.0f);
    CHECK(C.Vel == 0.0f);               // and it came fully to rest, not creeping
}

// ---- Clamping stops at the edges AND kills the velocity ----
// Without zeroing Vel the coast keeps pushing into the clamp, which reads as a stuck view that
// suddenly releases when you drag the other way.
static void TestClampKillsVelocity() {
    ScrollCamera C;
    C.Y = 9.0f;
    C.Vel = 500.0f;
    C.End();
    C.Update(1.0f / 60.0f, /*MaxY*/ 10.0f);
    CHECK(C.Y == 10.0f);
    CHECK(C.Vel == 0.0f);
    // A negative floor is legitimate (scrolling below world zero to clear a bottom HUD block).
    ScrollCamera D;
    D.Y = -1.0f;
    D.Vel = -500.0f;
    D.End();
    D.Update(1.0f / 60.0f, 10.0f, /*MinY*/ -5.0f);
    CHECK(D.Y == -5.0f);   // -1 + (-500/60) overshoots the floor, so it clamps there
    CHECK(D.Vel == 0.0f);
}

// ---- Frame-rate independence of the coast ----
static void TestCoastIsFrameRateIndependent() {
    ScrollCamera A, B;
    A.Vel = B.Vel = 100.0f;
    A.End(); B.End();
    for (int I = 0; I < 30; ++I) A.Update(1.0f / 60.0f, 1e9f, -1e9f);   // 0.5 s at 60 Hz
    for (int I = 0; I < 15; ++I) B.Update(1.0f / 30.0f, 1e9f, -1e9f);   // 0.5 s at 30 Hz
    // Same elapsed time -> same distance travelled, within INTEGRATION error.
    //
    // The velocity decay is exactly rate-independent (composing exp(-Dt/tau) n times is
    // exp(-total/tau)); the POSITION is a first-order left-Riemann sum of it, so a coarser step
    // overshoots slightly. Measured here: 25.02 at 60 Hz vs 25.71 at 30 Hz over 0.5 s — about 3%,
    // against an exact answer of 24.33. That is imperceptible, so the tolerance is set to admit it
    // rather than "fixed" by switching to the closed form Y += Vel*tau*(1-r), which WOULD be exactly
    // rate-independent but would also change the shipped feel by ~3% during what is meant to be a
    // pure relocation. Worth doing deliberately later if anyone cares; not as a side effect of a move.
    CHECK(std::fabs(A.Y - B.Y) < 1.0f);
    CHECK(A.Y > 20.0f);                 // and it actually travelled
}

// ---- DampingTau is a real knob ----
static void TestLongerTauGlidesFurther() {
    ScrollCamera Slow, Fast;
    Slow.DampingTau = 1.0f;
    Fast.DampingTau = 0.1f;
    Slow.Vel = Fast.Vel = 100.0f;
    Slow.End(); Fast.End();
    for (int I = 0; I < 60; ++I) { Slow.Update(1.0f / 60.0f, 1e9f, -1e9f); Fast.Update(1.0f / 60.0f, 1e9f, -1e9f); }
    CHECK(Slow.Y > Fast.Y);
}

int main() {
    TestDragFollowsFinger();
    TestMoveIsInertWithoutDrag();
    TestFlickCoastsThenStops();
    TestClampKillsVelocity();
    TestCoastIsFrameRateIndependent();
    TestLongerTauGlidesFurther();
    if (GFailures == 0) std::printf("scroll_camera_tests: ALL PASS\n");
    else std::printf("scroll_camera_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
