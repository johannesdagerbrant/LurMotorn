#pragma once
#include <cstdint>

namespace Lur::Net {

// Estimates the clock offset between the two phones so timestamps (move times,
// game-clock deductions, and later the tick alignment for rollback) can be
// compared on a shared timeline.
//
// Method (NTP-style): one side sends ClockPing with its local send time T0; the
// peer replies ClockPong echoing T0 plus its own receive/transmit time; the
// originator notes arrival T3. Round-trip = (T3 - T0) - peer_processing; offset
// is estimated from the midpoint. Repeated a few times, keeping the sample with
// the smallest round-trip (least jitter), this pins the offset to well under the
// per-move stakes for a turn-based game — and is the seed of the tighter sync a
// reflex game's rollback will need.
class ClockSync {
public:
    bool    IsReady() const { return Ready; }
    int64_t GetOffsetNs() const { return OffsetNs; }   // PeerClock ~= LocalClock + OffsetNs

    // TODO(net): OnPing / OnPong handlers, best-of-N sample selection, and a
    // small drift correction. Wired up in the clock-sync + end-to-end task.

private:
    bool    Ready = false;
    int64_t OffsetNs = 0;
};

} // namespace Lur::Net
