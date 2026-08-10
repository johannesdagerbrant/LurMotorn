#include "Lur/Transport/BleStartRetry.h"

namespace Lur::Transport {

uint64_t BleStartRetry::DelayNs() const {
    // Doubling, capped. Attempt_ counts retries already scheduled, so the delay in force is the
    // one for the most recent schedule (or the first delay when none has happened yet).
    const int Shift = Attempt_ > 0 ? Attempt_ - 1 : 0;
    const uint64_t D = BaseDelayNs << (Shift < 3 ? Shift : 3);
    return D < MaxDelayNs ? D : MaxDelayNs;
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
