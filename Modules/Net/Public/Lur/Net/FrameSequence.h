#pragma once
// Detects LOST and DUPLICATED per-tick input frames from a one-byte sequence stamp.
//
// Promoted out of Rps::LockstepPeer (#163, #201). Ten lines of modular arithmetic, and every one of
// them is there because of a specific hardware failure.
//
// ---- Why a frame carries a sequence at all ----
// A lockstep/rollback peer sends one input frame per tick. If one goes missing, every later frame
// lands one exec tick EARLY for the rest of the match, and the sims skew. That skew can stay
// INVISIBLE for a long time: re-placing a building onto an occupied square is a sim no-op, so the
// hashes kept matching while the two input streams were a tick apart. It took diffing two phones'
// recordings to find (#159/#160). A sequence stamp turns that into a detection at the moment it
// happens, and — as importantly — LOCATES it: "the peer's tick 4528 never arrived".
//
// ---- Why the comparison is SIGNED ----
// The stamp is one byte, so it wraps every 256 frames, and the only meaningful reading of "how far is
// this from what we expect" is the SHORTER way round the circle. Unsigned subtraction alone cannot
// tell one frame MISSING from one frame arriving TWICE: the duplicate yields 255, which reads as a
// 25-second hole. That is not hypothetical — on 2026-08-01 a duplicate GATT subscription delivered
// every notification twice and produced a "255 frames missing" report on every tick of a 20-minute
// match, which sent the reader hunting a link that was working fine.
//
// ---- Why a duplicate is DROPPED and a gap is RE-BASED ----
// A frame BEHIND the expectation is a duplicate or a reorder, never a loss: its tick already has a
// batch. Appending a second one skews every later index by a frame — invisible while batches are
// empty, a divergence the moment one is not. And counting it as a gap would spend a repair attempt on
// a frame already held.
//
// A gap re-bases the expectation PAST the hole, so ONE lost frame costs ONE report instead of one per
// frame for the rest of the match. Without that, "how many did we lose" — the number that actually
// matters — is unanswerable.
//
// ---- Why the expectation is a full tick, not just the wire byte ----
// So the reported tick stays exact after an earlier gap has already shifted the buffer. The byte is
// only what fits on the wire; the truth is 32 bits wide and kept here.
#include <cstdint>

namespace Lur::Net {

enum class EFrameSeq : uint8_t {
    OnTime,     // exactly the frame we expected
    Missing,    // ahead of the expectation: Count frames were lost
    Duplicate,  // behind the expectation: this tick already has a batch
};

struct FrameSeqVerdict {
    EFrameSeq Kind = EFrameSeq::OnTime;
    uint32_t Count = 0;        // Missing: frames lost. Duplicate: how far behind.
    uint32_t GapAtTick = 0;    // Missing: the first tick that never arrived.
};

// Pure classification of a received one-byte stamp against the full tick expected next. Separated from
// the tracker so the signed-circle arithmetic is testable with no state at all — it is the part that
// was wrong on hardware.
inline FrameSeqVerdict ClassifyFrameSeq(uint32_t ReceivedByte, uint32_t ExpectedTick) {
    const uint32_t Fwd = (ReceivedByte - (ExpectedTick & 0xFFu)) & 0xFFu;
    const int32_t Ahead = Fwd >= 128 ? static_cast<int32_t>(Fwd) - 256 : static_cast<int32_t>(Fwd);
    FrameSeqVerdict V;
    if (Ahead < 0) {
        V.Kind = EFrameSeq::Duplicate;
        V.Count = static_cast<uint32_t>(-Ahead);
    } else if (Ahead > 0) {
        V.Kind = EFrameSeq::Missing;
        V.Count = static_cast<uint32_t>(Ahead);
        V.GapAtTick = ExpectedTick;
    }
    return V;
}

class FrameSequenceTracker {
public:
    // Re-base the expectation. Call on a match start, and on a resync that re-bases both timelines to
    // the same frontier — without that, every frame after a resync reads as a gap, so the detector
    // would cry wolf on the ONE path where frames are legitimately dropped.
    void Reset(uint32_t NextTick) { Expected_ = NextTick; }

    uint32_t ExpectedTick() const { return Expected_; }

    // Classify an arriving frame, count it, and RE-BASE past a gap. Does NOT consume the frame: the
    // caller decides whether the frame is buffered (a duplicate never is; a gap may trigger recovery
    // that discards it), and signals acceptance with AdvanceExpected().
    //
    // HAZARD: forgetting AdvanceExpected() makes every subsequent frame read as a gap. That is a
    // caller obligation this class cannot check, which is why acceptance is one named call.
    FrameSeqVerdict Observe(uint32_t ReceivedByte) {
        const FrameSeqVerdict V = ClassifyFrameSeq(ReceivedByte, Expected_);
        if (V.Kind == EFrameSeq::Duplicate) {
            ++Duplicates_;
        } else if (V.Kind == EFrameSeq::Missing) {
            ++Gaps_;
            LastGapTick_ = V.GapAtTick;
            Expected_ += V.Count;   // count the gap ONCE, not forever
        }
        return V;
    }

    // The frame was accepted for the expected tick; expect the next one.
    void AdvanceExpected() { ++Expected_; }

    void ResetCounters() {
        Gaps_ = 0;
        Duplicates_ = 0;
        LastGapTick_ = 0;
    }

    uint32_t Gaps() const { return Gaps_; }
    uint32_t Duplicates() const { return Duplicates_; }
    uint32_t LastGapTick() const { return LastGapTick_; }

private:
    uint32_t Expected_ = 0;
    uint32_t Gaps_ = 0;
    uint32_t Duplicates_ = 0;
    uint32_t LastGapTick_ = 0;
};

}  // namespace Lur::Net
