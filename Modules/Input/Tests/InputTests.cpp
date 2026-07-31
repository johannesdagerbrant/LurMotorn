// Host tests for the shared dev-console gesture (#151).
//
// This exists because the gesture was written per platform and the copies DRIFTED: the desktop
// scrolled the console and Android didn't, Android could open it and iOS could not open it at all
// ("the two-finger triple-tap does nothing on the iPhone" — playtest 2026-07-25, which made the
// iPhone half of a two-phone playtest un-tunable). A third hand-written copy was the obvious next
// step and the wrong one, so the recognizer moved here, where it can actually be tested: timing
// windows and slop are exactly the kind of thing that silently differs between three copies.
#include <cstdio>

#include "Lur/Input/ConsoleGesture.h"

using Lur::Input::ConsoleGesture;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

static constexpr uint64_t Ms = 1'000'000ull;

// One two-finger tap: a second finger lands, both lift, HoldMs later.
static bool TwoFingerTap(ConsoleGesture& G, uint64_t& T, uint64_t HoldMs) {
    G.PointersDown(1, T);
    G.PointersDown(2, T);
    T += HoldMs * Ms;
    const bool Open = G.LiftAndShouldOpen(T);
    return Open;
}

// ---- The open gesture: three quick two-finger taps, and nothing else ----
static void TestThreeTwoFingerTapsOpenTheConsole() {
    ConsoleGesture G;
    uint64_t T = 1'000'000'000ull;
    CHECK(!TwoFingerTap(G, T, 50));   // first
    T += 100 * Ms;
    CHECK(!TwoFingerTap(G, T, 50));   // second
    T += 100 * Ms;
    CHECK(TwoFingerTap(G, T, 50));    // third opens

    // The chain resets on opening: the NEXT tap must not re-open immediately, or a player tapping
    // around after closing the panel would keep re-summoning it.
    T += 100 * Ms;
    CHECK(!TwoFingerTap(G, T, 50));
    T += 100 * Ms;
    CHECK(!TwoFingerTap(G, T, 50));
    T += 100 * Ms;
    CHECK(TwoFingerTap(G, T, 50));
}

// ---- A HOLD is not a tap ----
// The window is what keeps the gesture out of the way of normal play: two fingers resting on a
// phone happens, and it must never open a debug panel over a live match.
static void TestTwoFingerHoldIsNotATap() {
    ConsoleGesture G;
    uint64_t T = 5'000'000'000ull;
    for (int I = 0; I < 6; ++I) {
        CHECK(!TwoFingerTap(G, T, ConsoleGesture::TapHoldMaxNs / Ms + 10));  // each held too long
        T += 100 * Ms;
    }
}

// ---- Taps must be a CHAIN, not three taps in a session ----
static void TestSlowTapsDoNotChain() {
    ConsoleGesture G;
    uint64_t T = 9'000'000'000ull;
    for (int I = 0; I < 5; ++I) {
        CHECK(!TwoFingerTap(G, T, 40));
        T += ConsoleGesture::TapChainMaxNs + 50 * Ms;   // too long a gap: the chain restarts each time
    }
}

// ---- One finger never opens it ----
// The gesture shares the pointer stream with camera panning, plate drags and the opponent selector,
// all of which are one-finger. If a single finger could reach it the console would appear during play.
static void TestOneFingerNeverOpens() {
    ConsoleGesture G;
    uint64_t T = 2'000'000'000ull;
    for (int I = 0; I < 8; ++I) {
        G.PointersDown(1, T);
        T += 40 * Ms;
        CHECK(!G.LiftAndShouldOpen(T));
        CHECK(!G.TwoFingerActive());
        T += 50 * Ms;
    }
}

// ---- Another gesture claiming the input voids the candidate ----
// A two-finger touch that turns into a building placement or a pinch is not a console tap, and the
// shims need a way to say so — otherwise releasing a placement could open the panel.
static void TestCancelVoidsTheCandidate() {
    ConsoleGesture G;
    uint64_t T = 3'000'000'000ull;
    for (int I = 0; I < 4; ++I) {
        G.PointersDown(1, T);
        G.PointersDown(2, T);
        CHECK(G.TwoFingerActive());
        G.Cancel();                       // a placement drag took over
        CHECK(!G.TwoFingerActive());
        T += 40 * Ms;
        CHECK(!G.LiftAndShouldOpen(T));   // ...so the lift is not a tap in the chain
        T += 50 * Ms;
    }
}

// ---- TwoFingerActive is what suppresses the normal one-finger tap ----
static void TestTwoFingerActiveSuppressesTheOrdinaryTap() {
    ConsoleGesture G;
    uint64_t T = 4'000'000'000ull;
    G.PointersDown(1, T);
    CHECK(!G.TwoFingerActive());     // one finger: the ordinary tap path owns it
    G.PointersDown(2, T);
    CHECK(G.TwoFingerActive());      // two: the shims must NOT also fire OnTap on release
    T += 40 * Ms;
    G.LiftAndShouldOpen(T);
    CHECK(!G.TwoFingerActive());     // cleared by the lift, ready for the next candidate
}

// ---- Routing while the console is OPEN: drag scrolls, a still release is a tap ----
// The console sits on top of a LIVE match, so the shim swallows the whole gesture; this decides
// which of the two it was. Android had no scrolling at all and the desktop did — the drift that
// made a long cvar list unreachable on the phone (#150).
static void TestDragScrollsAndAStillReleaseIsATap() {
    ConsoleGesture G;
    G.DragBegin(500.0f);
    CHECK(G.DragEndIsTap());                       // nothing moved yet: a release here is a tap

    // A real drag: the deltas handed to DevScroll are the finger's movement, sign included (dragging
    // UP scrolls the list DOWN, which is what "the content follows the finger" means).
    CHECK(G.DragMove(480.0f) == 20.0f);
    CHECK(G.DragMove(460.0f) == 20.0f);
    CHECK(G.DragMove(470.0f) == -10.0f);
    CHECK(!G.DragEndIsTap());                      // it moved: a scroll, not a tap

    // A tiny wiggle is still a tap — a finger jitters where a mouse doesn't, which is exactly why
    // this slop is a shared constant rather than three different numbers.
    ConsoleGesture H;
    H.DragBegin(300.0f);
    H.DragMove(300.0f + ConsoleGesture::DragSlopPx * 0.25f);
    H.DragMove(300.0f);
    CHECK(H.DragEndIsTap());

    // Accumulated movement counts, not net displacement: a slow circle back to the start is a drag,
    // not a tap. Net-displacement logic would fire a spurious hit-test at the end of a long scrub.
    ConsoleGesture K;
    K.DragBegin(300.0f);
    K.DragMove(300.0f + ConsoleGesture::DragSlopPx);
    K.DragMove(300.0f);
    CHECK(!K.DragEndIsTap());
}

int main() {
    TestThreeTwoFingerTapsOpenTheConsole();
    TestTwoFingerHoldIsNotATap();
    TestSlowTapsDoNotChain();
    TestOneFingerNeverOpens();
    TestCancelVoidsTheCandidate();
    TestTwoFingerActiveSuppressesTheOrdinaryTap();
    TestDragScrollsAndAStillReleaseIsATap();

    if (GFailures == 0) std::printf("input_tests: ALL PASS\n");
    else std::printf("input_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
