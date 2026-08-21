// Tests for Lur::Render::CorrectionSmoother — rollback correction smoothing, promoted out of
// Rps::GameView (#201).
//
// The headline test is the one that would have caught the shipped bug: a slot whose OCCUPANT changed
// must snap, not ease in from its predecessor's position. The bug's signature was a "swinging arc" on
// everything freshly built, and it was invisible in single-player because nothing rolls back there.
#include <cmath>
#include <cstdio>

#include "Lur/Render/CorrectionSmoother.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Smoother = Lur::Render::CorrectionSmoother<16>;

// ---- Normal play: the endpoints chain, so there is nothing to absorb ----
// snapshot N's Pos == snapshot N+1's Prev. This must be an exact no-op, because it is what happens on
// every tick of every match that never rolls back.
static void TestChainedSnapshotsAreANoOp() {
    Smoother S;
    // Tick 1: entity 7 moves 0 -> 1.
    CHECK(S.Observe(0, true, /*Serial*/ 7, 0.0f, 0.0f, 1.0f, 0.0f));   // first sighting = new occupant
    CHECK(S.ErrX(0) == 0.0f && S.ErrY(0) == 0.0f);
    // Tick 2: Prev == last Pos (1.0) -> chained.
    CHECK(!S.Observe(0, true, 7, 1.0f, 0.0f, 2.0f, 0.0f));
    CHECK(S.ErrX(0) == 0.0f && S.ErrY(0) == 0.0f);
    CHECK(!S.Observe(0, true, 7, 2.0f, 0.0f, 3.0f, 0.0f));
    CHECK(S.ErrX(0) == 0.0f && S.ErrY(0) == 0.0f);
}

// ---- A real correction is absorbed, then decays to zero ----
static void TestCorrectionIsAbsorbedAndDecays() {
    Smoother S;
    S.Observe(0, true, 7, 0.0f, 0.0f, 5.0f, 0.0f);          // last known Pos = 5
    // The rollback says the segment begins at 4, not 5: a 1-unit discontinuity.
    CHECK(!S.Observe(0, true, 7, 4.0f, 0.0f, 4.5f, 0.0f));
    CHECK(std::fabs(S.ErrX(0) - 1.0f) < 1e-5f);             // absorbed, so the DRAWN position holds
    CHECK(S.ErrY(0) == 0.0f);
    // Decay over a few halflives -> back to zero.
    for (int I = 0; I < 60; ++I) S.Decay(1.0f / 60.0f);
    CHECK(S.ErrX(0) == 0.0f);
}

// ---- THE BUG: a new occupant must SNAP, and must be reported ----
// A rollback re-runs allocations, so one extra entity slides every later spawn down a slot. Slot 0
// then holds a DIFFERENT entity about a unit away — indistinguishable from a correction by position,
// team or type. Only the serial can tell.
static void TestNewOccupantSnapsAndIsReported() {
    Smoother S;
    S.Observe(0, true, /*Serial*/ 7, 0.0f, 0.0f, 5.0f, 0.0f);
    // Same slot, same distance a real correction would have, but serial 8 — the neighbour that slid
    // down one slot. Must snap and must return true so the caller resets facing/latches too.
    const bool NewOccupant = S.Observe(0, true, /*Serial*/ 8, 4.0f, 0.0f, 4.5f, 0.0f);
    CHECK(NewOccupant);
    CHECK(S.ErrX(0) == 0.0f && S.ErrY(0) == 0.0f);
}

// ---- A dead slot coming back to life is also a new occupant ----
static void TestRespawnSnaps() {
    Smoother S;
    S.Observe(0, true, 7, 0.0f, 0.0f, 5.0f, 0.0f);
    S.Observe(0, false, 0, 0.0f, 0.0f, 0.0f, 0.0f);          // died
    const bool NewOccupant = S.Observe(0, true, 7, 4.0f, 0.0f, 4.5f, 0.0f);  // same serial, but was dead
    CHECK(NewOccupant);
    CHECK(S.ErrX(0) == 0.0f);
}

// ---- The cap: a same-entity jump too large to be a correction snaps ----
static void TestOversizedJumpSnaps() {
    Smoother S{Smoother::Config{0.07f, /*MaxJump*/ 8.0f, 0.001f}};
    S.Observe(0, true, 7, 0.0f, 0.0f, 0.0f, 0.0f);
    // 20 units on the same serial: not a correction, so snap rather than fly in across the map.
    CHECK(!S.Observe(0, true, 7, 20.0f, 0.0f, 20.0f, 0.0f));
    CHECK(S.ErrX(0) == 0.0f);
    // Just inside the cap is still smoothed.
    Smoother T{Smoother::Config{0.07f, 8.0f, 0.001f}};
    T.Observe(0, true, 7, 0.0f, 0.0f, 0.0f, 0.0f);
    T.Observe(0, true, 7, -7.0f, 0.0f, -7.0f, 0.0f);
    CHECK(std::fabs(T.ErrX(0) - 7.0f) < 1e-5f);
}

// ---- Successive corrections accumulate rather than replacing ----
// Two rollbacks in quick succession must not lose the first offset, or the second correction visibly
// snaps the part the first was still easing.
static void TestCorrectionsAccumulate() {
    Smoother S;
    S.Observe(0, true, 7, 0.0f, 0.0f, 0.0f, 0.0f);
    S.Observe(0, true, 7, -1.0f, 0.0f, -1.0f, 0.0f);   // +1 error
    CHECK(std::fabs(S.ErrX(0) - 1.0f) < 1e-5f);
    S.Observe(0, true, 7, -3.0f, 0.0f, -3.0f, 0.0f);   // last Pos was -1, now begins at -3 -> +2 more
    CHECK(std::fabs(S.ErrX(0) - 3.0f) < 1e-5f);
}

// ---- Frame-rate independence: same elapsed time, same result ----
// The decay is exponential in real seconds precisely so a 30 Hz device and a 60 Hz device show the
// same correction curve. A per-frame multiplier would fail this.
static void TestDecayIsFrameRateIndependent() {
    Smoother A, B;
    A.Observe(0, true, 7, 0.0f, 0.0f, 0.0f, 0.0f);
    A.Observe(0, true, 7, -4.0f, 0.0f, -4.0f, 0.0f);
    B.Observe(0, true, 7, 0.0f, 0.0f, 0.0f, 0.0f);
    B.Observe(0, true, 7, -4.0f, 0.0f, -4.0f, 0.0f);
    for (int I = 0; I < 12; ++I) A.Decay(1.0f / 60.0f);   // 0.2 s at 60 Hz
    for (int I = 0; I < 6; ++I)  B.Decay(1.0f / 30.0f);   // 0.2 s at 30 Hz
    CHECK(std::fabs(A.ErrX(0) - B.ErrX(0)) < 1e-4f);
    // And it actually decayed a long way in 0.2 s (~3 halflives).
    CHECK(std::fabs(A.ErrX(0)) < 0.6f);
}

// ---- Slots are independent, and out-of-range calls are inert ----
static void TestSlotIsolationAndBounds() {
    Smoother S;
    S.Observe(0, true, 1, 0.0f, 0.0f, 0.0f, 0.0f);
    S.Observe(1, true, 2, 0.0f, 0.0f, 0.0f, 0.0f);
    S.Observe(0, true, 1, -2.0f, 0.0f, -2.0f, 0.0f);
    CHECK(std::fabs(S.ErrX(0) - 2.0f) < 1e-5f);
    CHECK(S.ErrX(1) == 0.0f);                    // slot 1 untouched
    CHECK(!S.Observe(-1, true, 3, 0, 0, 0, 0));  // must not write out of bounds
    CHECK(!S.Observe(999, true, 3, 0, 0, 0, 0));
    CHECK(S.ErrX(-1) == 0.0f && S.ErrX(999) == 0.0f);
}

int main() {
    TestChainedSnapshotsAreANoOp();
    TestCorrectionIsAbsorbedAndDecays();
    TestNewOccupantSnapsAndIsReported();
    TestRespawnSnaps();
    TestOversizedJumpSnaps();
    TestCorrectionsAccumulate();
    TestDecayIsFrameRateIndependent();
    TestSlotIsolationAndBounds();
    if (GFailures == 0) std::printf("render_smoother_tests: ALL PASS\n");
    else std::printf("render_smoother_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
