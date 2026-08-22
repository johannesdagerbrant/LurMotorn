// Tests for Lur::Net::FrameSequence — the lost/duplicated input-frame detector promoted out of
// Rps::LockstepPeer (#163, #201).
//
// The headline test is TestDuplicateIsNotAHugeGap. On 2026-08-01 a duplicate GATT subscription
// delivered every notification twice; unsigned subtraction reported "255 frames missing" on every tick
// of a 20-minute match, and the reader went hunting a link that was working fine. The signed
// shorter-way-round-the-circle reading is the fix, and it is exactly what a hand-written unsigned
// comparison gets wrong.
#include <cstdio>
#include <cstdint>

#include "Lur/Net/FrameSequence.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::Net::ClassifyFrameSeq;
using Lur::Net::EFrameSeq;
using Lur::Net::FrameSeqVerdict;
using Lur::Net::FrameSequenceTracker;

// ---- The ordinary case: every frame arrives, in order ----
static void TestPerfectStreamIsAllOnTime() {
    FrameSequenceTracker T;
    T.Reset(0);
    for (uint32_t Tick = 0; Tick < 1000; ++Tick) {
        const FrameSeqVerdict V = T.Observe(Tick & 0xFFu);
        CHECK(V.Kind == EFrameSeq::OnTime);
        T.AdvanceExpected();
    }
    CHECK(T.Gaps() == 0);
    CHECK(T.Duplicates() == 0);
    CHECK(T.ExpectedTick() == 1000);   // and it survived four byte wraps
}

// ---- THE 2026-08-01 BUG: one frame arriving twice is a DUPLICATE, not 255 missing ----
static void TestDuplicateIsNotAHugeGap() {
    FrameSequenceTracker T;
    T.Reset(500);
    // Frame 500 arrives and is accepted; then it arrives AGAIN.
    CHECK(T.Observe(500 & 0xFFu).Kind == EFrameSeq::OnTime);
    T.AdvanceExpected();
    const FrameSeqVerdict V = T.Observe(500 & 0xFFu);
    CHECK(V.Kind == EFrameSeq::Duplicate);
    CHECK(V.Count == 1);                 // one behind — NOT 255 ahead
    CHECK(T.Duplicates() == 1);
    CHECK(T.Gaps() == 0);                // and it must not spend a repair attempt
    CHECK(T.ExpectedTick() == 501);      // a duplicate does not move the expectation
    // The real frame then arrives normally.
    CHECK(T.Observe(501 & 0xFFu).Kind == EFrameSeq::OnTime);
}

// ---- A duplicated STREAM (every frame twice) must report duplicates every time, never gaps ----
// This is the actual shape of the hardware failure: not one stray frame, but a doubled link.
static void TestEveryFrameTwice() {
    FrameSequenceTracker T;
    T.Reset(0);
    for (uint32_t Tick = 0; Tick < 600; ++Tick) {
        CHECK(T.Observe(Tick & 0xFFu).Kind == EFrameSeq::OnTime);
        T.AdvanceExpected();
        CHECK(T.Observe(Tick & 0xFFu).Kind == EFrameSeq::Duplicate);   // the echo
    }
    CHECK(T.Duplicates() == 600);
    CHECK(T.Gaps() == 0);   // the whole point: a doubled link must not read as a lossy one
}

// ---- A gap is counted ONCE and located, then the expectation re-bases past the hole ----
// Without the re-base, one lost frame reports on every frame for the rest of the match and "how many
// did we lose" becomes unanswerable.
static void TestGapIsCountedOnceAndLocated() {
    FrameSequenceTracker T;
    T.Reset(100);
    CHECK(T.Observe(100 & 0xFFu).Kind == EFrameSeq::OnTime);
    T.AdvanceExpected();                        // expecting 101
    // 101 is lost; 102 arrives.
    const FrameSeqVerdict V = T.Observe(102 & 0xFFu);
    CHECK(V.Kind == EFrameSeq::Missing);
    CHECK(V.Count == 1);
    CHECK(V.GapAtTick == 101);                  // LOCATED, which is the thing #163 could not do
    CHECK(T.LastGapTick() == 101);
    CHECK(T.Gaps() == 1);
    CHECK(T.ExpectedTick() == 102);             // re-based past the hole
    T.AdvanceExpected();
    // Every later frame is on time — ONE report, not one per frame forever.
    for (uint32_t Tick = 103; Tick < 200; ++Tick) {
        CHECK(T.Observe(Tick & 0xFFu).Kind == EFrameSeq::OnTime);
        T.AdvanceExpected();
    }
    CHECK(T.Gaps() == 1);
}

// ---- A multi-frame burst loss reports its true size ----
static void TestMultiFrameGap() {
    FrameSequenceTracker T;
    T.Reset(10);
    const FrameSeqVerdict V = T.Observe(19 & 0xFFu);
    CHECK(V.Kind == EFrameSeq::Missing);
    CHECK(V.Count == 9);
    CHECK(V.GapAtTick == 10);
    CHECK(T.ExpectedTick() == 19);
}

// ---- Gaps and duplicates are classified correctly ACROSS the byte wrap ----
// The wrap is where an unsigned comparison stops being merely imprecise and starts being backwards.
static void TestBehaviourAcrossTheByteWrap() {
    // Expect tick 256 (byte 0). Frame 255 (byte 255) is a duplicate one behind, not 255 ahead.
    FrameSeqVerdict V = ClassifyFrameSeq(255, 256);
    CHECK(V.Kind == EFrameSeq::Duplicate);
    CHECK(V.Count == 1);
    // Expect tick 255 (byte 255). Frame 256 (byte 0) is one AHEAD... which means tick 255 was lost.
    V = ClassifyFrameSeq(0, 255);
    CHECK(V.Kind == EFrameSeq::Missing);
    CHECK(V.Count == 1);
    CHECK(V.GapAtTick == 255);
    // And the tracker keeps a full 32-bit tick across the wrap, so the located tick stays exact.
    FrameSequenceTracker T;
    T.Reset(255);
    CHECK(T.Observe(1).GapAtTick == 255);   // byte 1 == tick 257, so 255 and 256 are missing
    CHECK(T.Gaps() == 1);
    CHECK(T.LastGapTick() == 255);
    CHECK(T.ExpectedTick() == 257);
}

// ---- The 128 boundary: exactly where "ahead" flips to "behind" ----
// 127 ahead is a (large, implausible) loss; 128 ahead is read as 128 BEHIND. Pinned because it is the
// one place the choice of split point is visible, and picking the other side would turn a huge loss
// into a huge duplicate.
static void TestSignedSplitAtHalfTheCircle() {
    FrameSeqVerdict V = ClassifyFrameSeq(127, 0);
    CHECK(V.Kind == EFrameSeq::Missing);
    CHECK(V.Count == 127);
    V = ClassifyFrameSeq(128, 0);
    CHECK(V.Kind == EFrameSeq::Duplicate);
    CHECK(V.Count == 128);
    V = ClassifyFrameSeq(129, 0);
    CHECK(V.Kind == EFrameSeq::Duplicate);
    CHECK(V.Count == 127);
    // Only the byte matters, so a stamp equal to the expectation's byte is always on time — even a
    // whole wrap away, which is the honest limit of a one-byte sequence.
    CHECK(ClassifyFrameSeq(0, 256).Kind == EFrameSeq::OnTime);
    CHECK(ClassifyFrameSeq(44, 300).Kind == EFrameSeq::OnTime);
}

// ---- Reset re-bases: a resync must not make every later frame read as a gap ----
// Both peers re-base to the same frontier on a resync, so the expectation has to move with them.
// Without this the detector cries wolf on the one path where frames ARE legitimately dropped.
static void TestResetRebasesAfterAResync() {
    FrameSequenceTracker T;
    T.Reset(0);
    for (uint32_t Tick = 0; Tick < 40; ++Tick) { T.Observe(Tick & 0xFFu); T.AdvanceExpected(); }
    CHECK(T.Gaps() == 0);
    T.Reset(4000);   // resync re-based both timelines to tick 4000
    CHECK(T.ExpectedTick() == 4000);
    CHECK(T.Observe(4000 & 0xFFu).Kind == EFrameSeq::OnTime);
    CHECK(T.Gaps() == 0);
    // Counters survive a Reset (they are per-session diagnostics); ResetCounters clears them.
    T.ResetCounters();
    CHECK(T.Gaps() == 0 && T.Duplicates() == 0 && T.LastGapTick() == 0);
    CHECK(T.ExpectedTick() == 4000);   // ...without disturbing the expectation
}

int main() {
    TestPerfectStreamIsAllOnTime();
    TestDuplicateIsNotAHugeGap();
    TestEveryFrameTwice();
    TestGapIsCountedOnceAndLocated();
    TestMultiFrameGap();
    TestBehaviourAcrossTheByteWrap();
    TestSignedSplitAtHalfTheCircle();
    TestResetRebasesAfterAResync();
    if (GFailures == 0) std::printf("frame_sequence_tests: ALL PASS\n");
    else std::printf("frame_sequence_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
