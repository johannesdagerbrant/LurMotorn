#pragma once
#include <cstdint>

namespace Lur::Input {

// The dev-console gesture, in ONE place (#151).
//
// It was written per platform and the three copies drifted, each in a different direction: the
// desktop scrolled the console and Android did not (#150, which made a long cvar list unreachable on
// the phone), Android could open it and iOS could not open it AT ALL — the two-finger triple-tap was
// simply never wired there, so on-device tuning was Android-only and the iPhone half of a two-phone
// playtest was un-tunable (playtest 2026-07-25). Writing a third hand-rolled copy was the obvious
// next step and the wrong one.
//
// What lives here is only the DECISION — timing windows, tap chaining, drag-vs-tap slop — because
// that is the part that silently differs between copies and the part a host test can pin. Each shim
// still owns its own native event plumbing (Android's AMotionEvent, iOS's UITouch set, the desktop's
// normalized TouchEvent stream); it feeds this the pointer count and a timestamp, and acts on the
// answer.
//
// Deliberately NOT a `#if !LUR_SHIPPING` type: it is a header-only state machine with no console
// dependency, so the gate stays where it already is — at the CALL SITES, which is how the rest of
// the console plumbing is gated.
class ConsoleGesture {
public:
    // A two-finger touch must be a TAP, not a rest. Two fingers left on a phone happen, and must
    // never open a debug panel over a live match.
    static constexpr uint64_t TapHoldMaxNs = 350'000'000ull;
    // ...and the taps must be a CHAIN. Three taps spread over a minute of play are an accident;
    // three inside this window each are deliberate.
    //
    // 1000 ms, MEASURED rather than guessed (2026-08-16, both phones instrumented). The lift-to-lift
    // gaps a human actually produces fall into two clusters with a wide empty band between them:
    //
    //   chained fine   148 150 150 152 166 167 222 251 350 384 ms
    //   reset          634 685 935 | 1503 1553 2083 2605 ms
    //
    // At the old 600 ms the first three failures were NEAR MISSES — 634 ms against a 600 ms window is
    // a 34 ms miss, and it reads to the player as the gesture being broken rather than as being 34 ms
    // slow. 1000 ms sits in the empty band: it rescues every near miss without reaching the >1.5 s
    // gaps, which are genuinely separate taps rather than a chain. No successful gap is anywhere near
    // the boundary, so widening cannot make an accidental chain more likely — and it still takes
    // THREE two-finger taps inside ~2 s, in a game played with one finger.
    static constexpr uint64_t TapChainMaxNs = 1'000'000'000ull;
    static constexpr int      TapsToOpen = 3;
    // How far a finger may travel while the console is open and still count as a tap rather than a
    // scroll. Sized for a finger, not a mouse: this was 12 px on Android and 6 on the desktop, and a
    // single shared number is the point — a slop that differs per platform makes the same console
    // behave differently on each.
    static constexpr float DragSlopPx = 12.0f;

    // ---- The OPEN gesture (used while the console is closed) ----

    // The active pointer count became Count (a finger landed). Two fingers arm a candidate; a first
    // finger clears any stale one, so a fresh gesture always starts from a known state.
    void PointersDown(int Count, uint64_t NowNs) {
        if (Count >= 2) {
            if (!TwoFingerActive_) { TwoFingerActive_ = true; DownNs_ = NowNs; }
        } else {
            TwoFingerActive_ = false;
        }
    }

    // Another gesture claimed the input — a building placement, a pinch, a plate press. This
    // candidate is void, so releasing that gesture must not be read as a console tap.
    void Cancel() { TwoFingerActive_ = false; }

    // Why a lift did or did not advance the chain. Populated on every LiftAndShouldOpen so a
    // "the gesture feels picky" report can become two numbers instead of a theory — feel is not
    // debuggable, and the two windows below fail in ways that look identical from the outside.
    struct LiftDiag {
        bool     WasCandidate = false;  // were two fingers down when this lift happened?
        uint64_t HoldNs = 0;            // second finger down -> last finger up (vs TapHoldMaxNs)
        uint64_t SinceLastTapNs = 0;    // previous chain tap -> this one (vs TapChainMaxNs)
        int      TapCount = 0;          // chain length after this lift
        bool     Opened = false;
    };
    const LiftDiag& LastLift() const { return LastLift_; }

    // The last finger lifted. Returns true when the console should OPEN. Resets the chain on opening,
    // so tapping around after closing the panel does not immediately re-summon it.
    bool LiftAndShouldOpen(uint64_t NowNs) {
        const bool WasCandidate = TwoFingerActive_;
        TwoFingerActive_ = false;
        LastLift_ = LiftDiag{};
        LastLift_.WasCandidate = WasCandidate;
        LastLift_.HoldNs = WasCandidate ? NowNs - DownNs_ : 0;
        LastLift_.SinceLastTapNs = NowNs - LastTapNs_;
        if (!WasCandidate || NowNs - DownNs_ >= TapHoldMaxNs) return false;  // a hold, not a tap
        TapCount_ = (NowNs - LastTapNs_ < TapChainMaxNs) ? TapCount_ + 1 : 1;
        LastTapNs_ = NowNs;
        LastLift_.TapCount = TapCount_;
        if (TapCount_ < TapsToOpen) return false;
        TapCount_ = 0;
        LastLift_.Opened = true;
        return true;
    }

    // Is a two-finger candidate live? The shims read this to SUPPRESS the ordinary one-finger tap on
    // release — otherwise the second tap of the chain would also hit the HUD underneath.
    bool TwoFingerActive() const { return TwoFingerActive_; }

    // ---- Routing while the console is OPEN ----
    // The console sits on top of a live match, so the shim swallows the whole gesture (a scroll must
    // not leak through and pan the camera or start a building drag). These decide which it was.

    void DragBegin(float YPx) { DragY_ = YPx; DragMoved_ = 0.0f; }

    // Returns the scroll delta to hand to the console. The content follows the finger: dragging up
    // (a smaller Y) yields a positive delta.
    float DragMove(float YPx) {
        const float Delta = DragY_ - YPx;
        DragMoved_ += Delta < 0.0f ? -Delta : Delta;   // ACCUMULATED travel, not net displacement:
        DragY_ = YPx;                                  // a scrub back to the start is still a drag
        return Delta;
    }

    bool DragEndIsTap() const { return DragMoved_ < DragSlopPx; }

private:
    bool     TwoFingerActive_ = false;
    uint64_t DownNs_ = 0;      // when the two-finger candidate began
    uint64_t LastTapNs_ = 0;   // when the previous tap in the chain landed
    int      TapCount_ = 0;
    float    DragY_ = 0.0f;
    float    DragMoved_ = 0.0f;
    LiftDiag LastLift_{};
};

} // namespace Lur::Input
