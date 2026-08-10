#include "Lur/Transport/BleStartRetry.h"

namespace Lur::Transport {

uint64_t BleStartRetry::DelayNs() const {
    // Doubling, capped by clamping the SHIFT — which is the whole cap. A second `min(D, MaxDelayNs)`
    // on the result was here initially and is unreachable by construction (MaxDelayNs is defined as
    // BaseDelayNs << MaxShift); a mutation test proved it dead, so it is gone rather than left as a
    // reassuring no-op that hides where the bound actually lives.
    const int Shift = Attempt_ > 0 ? Attempt_ - 1 : 0;
    return BaseDelayNs << (Shift < MaxShift ? Shift : MaxShift);
}

bool BleStartRetry::OnStartFailed() {
    const bool Loud = !Logged_;
    Logged_ = true;

    if (Attempt_ >= MaxFastRetries) {
        // Out of fast attempts. Stop scheduling and let the caller's slow discovery watchdog keep
        // trying — a slower cadence, not a give-up.
        Pending_ = false;
        HandedOff_ = true;
        return Loud;
    }

    ++Attempt_;
    Pending_ = true;
    WaitedNs_ = 0;
    return Loud;
}

void BleStartRetry::OnStarted() {
    Attempt_ = 0;
    Pending_ = false;
    HandedOff_ = false;
    Logged_ = false;
    WaitedNs_ = 0;
}

void BleStartRetry::Cancel() {
    Pending_ = false;
    WaitedNs_ = 0;
}

bool BleStartRetry::Tick(uint64_t ElapsedNs) {
    if (!Pending_) return false;      // nothing scheduled: time is not banked
    WaitedNs_ += ElapsedNs;
    if (WaitedNs_ < DelayNs()) return false;
    // Fire once. The caller retries the start; a further failure re-schedules with a longer delay.
    Pending_ = false;
    WaitedNs_ = 0;
    return true;
}

}  // namespace Lur::Transport
