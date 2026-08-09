#include "Lur/Transport/BleSendQueue.h"

#include <cstring>

namespace Lur::Transport {

bool BleSendQueue::Enqueue(const uint8_t* Data, std::size_t Size) {
    if (Data == nullptr || Size == 0 || Size > MaxDatagram) return false;
    if (Count_ >= MaxQueued) { ++Dropped_; return false; }

    Slot& S = Ring_[(Head_ + Count_) % MaxQueued];
    std::memcpy(S.Bytes, Data, Size);
    S.Size = Size;
    ++Count_;

    Pump();
    return true;
}

void BleSendQueue::OnSendComplete() {
    // Platform completion callbacks carry no identity, so the only way to tell whose it is, is
    // to consume them oldest-first. Anything the watchdog abandoned is still owed a callback;
    // that one arrives first and must be swallowed. Acting on it would release the next datagram
    // while the watchdog's replacement is still outstanding — two in flight, which a BLE stack
    // answers by silently dropping one of them.
    if (Abandoned_ > 0) { --Abandoned_; return; }
    if (!InFlight_) return;          // nothing outstanding at all: a spurious callback
    InFlight_ = false;
    InFlightNs_ = 0;
    Pump();
}

void BleSendQueue::Tick(uint64_t ElapsedNs) {
    if (InFlight_) {
        InFlightNs_ += ElapsedNs;
        if (InFlightNs_ >= SendTimeoutNs) {
            // The completion is not coming *in time*. Stop waiting and let the queue move; the
            // datagram we gave up on may or may not have reached the peer, which is precisely
            // what the session's own resync exists to reconcile. It may yet be acknowledged, so
            // remember that one completion is owed and must be swallowed when it lands.
            InFlight_ = false;
            InFlightNs_ = 0;
            ++Abandoned_;
        }
    }
    Pump();
}

void BleSendQueue::OnLinkLost() {
    Head_ = 0;
    Count_ = 0;
    InFlight_ = false;
    InFlightNs_ = 0;
    // A dead link owes us nothing: any pending callback dies with it, so a stale "swallow the
    // next completion" debt carried into the NEW link would eat a real datagram's completion
    // and stall the fresh queue for a full watchdog period.
    Abandoned_ = 0;
}

void BleSendQueue::Pump() {
    if (InFlight_ || Count_ == 0 || Radio_ == nullptr) return;

    const Slot& S = Ring_[Head_];
    if (!Radio_->Write(S.Bytes, S.Size)) return;   // busy: keep it, retry on the next tick

    Head_ = (Head_ + 1) % MaxQueued;
    --Count_;
    InFlight_ = true;
    InFlightNs_ = 0;
}

}  // namespace Lur::Transport
