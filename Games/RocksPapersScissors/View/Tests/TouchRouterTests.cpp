// Host unit tests for Rps::TouchRouter (issue #43, section D).
//
// The dispatch these cover existed FOUR times — the Android main, the iOS main, and both halves of
// DesktopMain.cpp — and had drifted in six separate ways. None of those differences was visible to
// any test, because all four copies lived in platform mains the host build never compiles. That is
// the defect; the drift was the symptom.
//
// WHAT IS AND IS NOT COVERED HERE, stated because the first draft of this file was worse than
// useless. Three of its nine tests passed against a deliberately sabotaged router: they asserted
// "nothing was emitted" in situations where nothing could have been emitted anyway. Every test below
// has since been checked to FAIL against a targeted sabotage, and the ones that could not be made to
// fail were deleted rather than left in to pad the count.
//
// What that leaves uncovered, and why:
//   * the 24 px TAP SLOP and the two-finger CHAIN SUPPRESSION both act by feeding (or not feeding)
//     GameView::OnTap. Their only observable is a hit-test against laid-out HUD rects, and the rects
//     are refreshed in Draw() — which needs an IRenderer. So a host test cannot see the difference
//     between "tapped and hit nothing" and "never tapped". Those two rules go to the device pass.
#include <cstdio>

#include "Rps/Snapshot.h"
#include "Rps/TouchRouter.h"

static int Failures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #Cond); \
            ++Failures;                                                 \
        }                                                               \
    } while (false)

namespace {

using Lur::Input::ETouchPhase;
using Lur::Input::TouchEvent;

// One router plus everything it points at, wired the way a main wires it. The real GameView is used
// with no GPU resources: hit-testing and the camera work fine without them, and placement declines
// gracefully because an unlaid-out view has no plates — which is why the placement tests below start
// their drag through BeginPlaceDrag directly.
struct Rig {
    Rps::GameView              View;
    Lur::Input::ScrollCamera          Cam;
    Lur::Input::ConsoleGesture Gesture;
    Rps::Snapshot              Snap;
    Rps::TouchRouter           Router;

    int             Emitted = 0;
    Rps::InputEvent LastEvent{};

    Rig() {
        Rps::TouchRouterHooks Hooks;
        Hooks.Emit = [this](const Rps::InputEvent& E) { ++Emitted; LastEvent = E; };
        Router.Init(&View, &Cam, &Gesture, std::move(Hooks));
    }

    void Send(ETouchPhase Phase, float X, float Y, int Pointers = 1, uint64_t Ns = 0) {
        TouchEvent T;
        T.Phase = Phase;
        T.XPx = X;
        T.YPx = Y;
        T.TimeNs = Ns;
        T.PointerCount = Pointers;
        Rps::TouchFrame F;
        F.ViewW = 1080.0f;
        F.ViewH = 1920.0f;
        F.Team  = 0;
        F.Live  = true;
        Router.Route(T, Snap, F);
    }
};

}  // namespace

// A one-finger drag pans; a two-finger drag must not.
//
// iOS lacked this gate and Android had it — so on an iPhone the world scrolled under the console
// panel while you performed the two-finger chain that opens it.
static void TestPanIsOneFingerOnly() {
    {
        Rig R;
        R.Send(ETouchPhase::Began, 500.0f, 900.0f, /*Pointers*/ 1);
        const float Before = R.Cam.Y;
        R.Send(ETouchPhase::Moved, 500.0f, 800.0f, /*Pointers*/ 1);
        CHECK(R.Cam.Y != Before);   // one finger: the ground follows
    }
    {
        Rig R;
        R.Send(ETouchPhase::Began, 500.0f, 900.0f, /*Pointers*/ 1);
        const float Before = R.Cam.Y;
        R.Send(ETouchPhase::Moved, 500.0f, 800.0f, /*Pointers*/ 2);
        CHECK(R.Cam.Y == Before);   // two fingers: a gesture, not a scroll
    }
}

// An open console owns the pointer, so a press under it must not even ARM the camera. Asserting on
// Cam.Dragging rather than Cam.Y is the point: an un-armed camera makes Move a no-op, which is what
// made the first version of this test unable to fail.
static void TestOpenConsoleSwallowsThePress() {
    Rig R;
    R.View.SetDevOverlayOpen(true);
    R.Send(ETouchPhase::Began, 500.0f, 900.0f);
    CHECK(!R.Cam.Dragging);
    CHECK(R.Emitted == 0);
}

// ... and a console opened MID-DRAG must stop the pan dead, with the camera still armed. This is the
// case that catches a Moved handler which scrolls the CVar list and then falls through to the world.
static void TestConsoleOpenedMidDragStopsThePan() {
    Rig R;
    R.Send(ETouchPhase::Began, 500.0f, 900.0f);
    CHECK(R.Cam.Dragging);            // armed by a press taken while the console was closed
    R.View.SetDevOverlayOpen(true);
    const float Before = R.Cam.Y;
    R.Send(ETouchPhase::Moved, 500.0f, 700.0f);
    CHECK(R.Cam.Y == Before);         // the world did not move under the panel
}

// The two-finger triple-tap opens the console through the router, using the shared recognizer's
// windows. Three separate call sites used to feed this.
static void TestTwoFingerTripleTapOpensTheConsole() {
    Rig R;
    CHECK(!R.View.DevOverlayOpen());

    uint64_t T = 1'000'000'000ull;
    for (int Tap = 0; Tap < 3; ++Tap) {
        R.Send(ETouchPhase::Began, 400.0f, 400.0f, /*Pointers*/ 2, T);
        R.Send(ETouchPhase::Ended, 400.0f, 400.0f, /*Pointers*/ 2, T + 40'000'000ull);
        T += 200'000'000ull;
    }
    CHECK(R.View.DevOverlayOpen());
    CHECK(R.Emitted == 0);   // opening the console never produces a game event
}

// One-finger taps must NOT open the console, however many of them there are. The recognizer is
// shared, but the router is what feeds it the pointer count — passing a constant 1 or a constant 2
// would each break exactly one of this test and the one above.
static void TestOneFingerTapsNeverOpenTheConsole() {
    Rig R;
    uint64_t T = 1'000'000'000ull;
    for (int Tap = 0; Tap < 5; ++Tap) {
        R.Send(ETouchPhase::Began, 400.0f, 400.0f, /*Pointers*/ 1, T);
        R.Send(ETouchPhase::Ended, 400.0f, 400.0f, /*Pointers*/ 1, T + 40'000'000ull);
        T += 200'000'000ull;
    }
    CHECK(!R.View.DevOverlayOpen());
}

// A second finger landing must not act as a press: it feeds the recognizer and stops. Resting a
// finger mid-pan must not re-seed the tap origin, restart the camera, or hit-test a build plate.
//
// Android had a dedicated ACTION_POINTER_DOWN case doing exactly this; iOS sent every
// -touchesBegan: through the full press path, so the two phones disagreed about what a second
// finger does. Android's is the careful one and is what the router does now.
static void TestSecondFingerIsNotAPress() {
    Rig R;
    R.Send(ETouchPhase::Began, 500.0f, 900.0f, /*Pointers*/ 1);
    CHECK(R.Cam.Dragging);
    R.Cam.End();                                         // pretend the pan already ended
    R.Send(ETouchPhase::Began, 200.0f, 300.0f, /*Pointers*/ 2);
    CHECK(!R.Cam.Dragging);                              // the second finger did not re-arm it
    CHECK(R.Emitted == 0);
}

// A release ENDS a placement and commits it; the drag is over either way.
//
// The drag is started through BeginPlaceDrag rather than by pressing a plate, because plate rects
// only exist after a Draw and Draw needs a renderer. What is under test is the router's release
// handling given a placement in progress, which is exactly that.
static void TestReleaseEndsThePlacement() {
    Rig R;
    R.View.BeginPlaceDrag(0, 500.0f, 900.0f);
    CHECK(R.View.IsPlacing());
    R.Send(ETouchPhase::Ended, 500.0f, 900.0f);
    CHECK(!R.View.IsPlacing());   // committed or slid back — but not left dangling
}

// Cancelled is the platform saying "this gesture did not happen". It must end the placement WITHOUT
// committing: no Place event, ever. That difference is the entire meaning of the phase, and routing
// Cancelled to the Ended handler — the obvious shortcut — would place a building the player never
// dropped.
static void TestCancelEndsThePlacementWithoutEmitting() {
    Rig R;
    R.View.BeginPlaceDrag(0, 500.0f, 900.0f);
    CHECK(R.View.IsPlacing());
    R.Send(ETouchPhase::Cancelled, 500.0f, 900.0f);
    CHECK(!R.View.IsPlacing());
    CHECK(R.Emitted == 0);
    CHECK(!R.Cam.Dragging);
}

// A cancel with nothing in flight is harmless, and still disarms the camera — otherwise a pan
// interrupted by a system alert leaves Dragging latched and the next Move teleports the world.
static void TestCancelDuringAPanDisarmsTheCamera() {
    Rig R;
    R.Send(ETouchPhase::Began, 500.0f, 900.0f);
    CHECK(R.Cam.Dragging);
    R.Send(ETouchPhase::Cancelled, 500.0f, 900.0f);
    CHECK(!R.Cam.Dragging);
    CHECK(R.Emitted == 0);
}

// Routing before Init must be inert rather than a null dereference — the alternative is a crash on
// the first touch of a launch that raced the wiring.
static void TestUninitialisedRouterIsInert() {
    Rps::TouchRouter Router;
    Rps::Snapshot Snap;
    Rps::TouchFrame F;
    TouchEvent T{};
    T.Phase = ETouchPhase::Began;
    CHECK(!Router.Route(T, Snap, F));
}

int main() {
    TestPanIsOneFingerOnly();
    TestOpenConsoleSwallowsThePress();
    TestConsoleOpenedMidDragStopsThePan();
    TestTwoFingerTripleTapOpensTheConsole();
    TestOneFingerTapsNeverOpenTheConsole();
    TestSecondFingerIsNotAPress();
    TestReleaseEndsThePlacement();
    TestCancelEndsThePlacementWithoutEmitting();
    TestCancelDuringAPanDisarmsTheCamera();
    TestUninitialisedRouterIsInert();
    if (Failures == 0) std::printf("rps_touch_router_tests: all passed\n");
    return Failures == 0 ? 0 : 1;
}
