#pragma once
#include <mutex>

#include "Rps/Sim.h"  // InputEvent, MaxEventsPerTick

namespace Rps {

// Thread-safe hand-off of the solo human's place/queue EVENTS from the input/render thread to the
// SimRunner's sim thread — the solo counterpart of LockstepPeer's mutex-guarded input inbox (#91).
// A solo match has no LockstepPeer (no peer), so its InputFn drains this instead. A human issues at
// most a few taps per 100 ms tick, so a short mutex-guarded buffer with essentially no contention
// is exactly right (same reasoning as the Lp event inbox). Shared verbatim by the desktop RunSolo
// and the Android solo-vs-AI path so the two can't drift.
class SoloInputInbox {
public:
    // Producer (input/render thread): queue one event for the next sim tick to drain.
    void Push(const InputEvent& E) {
        std::lock_guard<std::mutex> L(M_);
        if (Count_ < Cap) Buf_[Count_++] = E;  // full = drop (never reached at human tap rate)
    }

    // Consumer (sim thread): copy up to Max events into Out, remove them, return the count. Any
    // events beyond Max are kept for the next tick (order preserved).
    int Drain(InputEvent* Out, int Max) {
        std::lock_guard<std::mutex> L(M_);
        const int N = Count_ < Max ? Count_ : Max;
        for (int I = 0; I < N; ++I) Out[I] = Buf_[I];
        const int Rem = Count_ - N;
        for (int I = 0; I < Rem; ++I) Buf_[I] = Buf_[N + I];
        Count_ = Rem;
        return N;
    }

private:
    static constexpr int Cap = MaxEventsPerTick * 4;
    std::mutex M_;
    InputEvent Buf_[Cap];
    int Count_ = 0;
};

}  // namespace Rps
