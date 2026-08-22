#pragma once
// Bounded, mutex-guarded hand-off of local input events from the input/render thread to the sim
// thread. Single lock, fixed capacity, no allocation.
//
// Promoted out of Rps::SoloInputInbox and Rps::LockstepPeer::PendingLocalEvents (#201). Those two were
// NOT copies of each other, which is why this file is worth reading before using it: the solo inbox was
// a fixed array that dropped the NEWEST event when full, and the lockstep one was an unbounded
// std::vector. Unifying them means the lockstep path becomes bounded and allocation-free — and that
// changes a failure mode, so the drop is COUNTED rather than silent (see Dropped()).
//
// ---- Why a mutex is the right answer here ----
// A human issues at most a few taps per sim tick, and the sim thread drains at ~1 kHz, so contention is
// effectively nil. A lock-free queue would buy nothing measurable and cost a great deal of reasoning.
// The same judgement as SnapshotMailbox, for the same reason.
//
// ---- Why the capacity is not "big enough to never matter" ----
// Because an unbounded queue does not remove the failure, it relocates it: if the sim thread stalls,
// an unbounded inbox grows until something else breaks, and the input path allocates on a phone. A cap
// makes the limit explicit and, with Dropped(), makes hitting it OBSERVABLE. A dropped local input is
// a lost player action, so a caller that can drop should say so out loud.
//
// ---- Drain semantics ----
// Drain copies up to Max events in FIFO order and keeps the remainder, in order, for the next call.
// That ordering is load-bearing for a deterministic sim: two events on the same tick (place, then
// queue into what was just placed) must arrive in the order the player made them.
//
// T must be a trivially copyable event POD.
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace Lur::Sim {

template <class T, int Cap>
class InputInbox {
    static_assert(std::is_trivially_copyable_v<T>, "InputInbox copies under a lock; T must be POD");
    static_assert(Cap > 0, "InputInbox needs room for at least one event");

public:
    // Producer (input/render thread). Returns FALSE if the event was dropped because the inbox is
    // full — a dropped local input is a lost player action, so callers should not ignore this.
    bool Push(const T& E) {
        std::lock_guard<std::mutex> L(Mutex);
        if (Count >= Cap) {
            ++DroppedCount;
            return false;
        }
        Buf[Count++] = E;
        return true;
    }

    // Consumer (sim thread). Copies up to Max events into Out, removes them, returns the count.
    // Anything beyond Max is kept for the next call with its order preserved.
    int Drain(T* Out, int Max) {
        std::lock_guard<std::mutex> L(Mutex);
        const int N = Count < Max ? Count : Max;
        for (int I = 0; I < N; ++I) Out[I] = Buf[I];
        const int Rem = Count - N;
        for (int I = 0; I < Rem; ++I) Buf[I] = Buf[N + I];
        Count = Rem;
        return N;
    }

    // Read every queued event, in order, WITHOUT removing any — the callback runs under the lock.
    // This exists for the pre-match case where the consumer is looking for one particular event among
    // the queued ones and must decide, from the sim state, whether to accept it; a Drain-then-inspect
    // would have already destroyed the queue by the time it discovered nothing qualified.
    template <class Fn>
    void Visit(Fn&& F) const {
        std::lock_guard<std::mutex> L(Mutex);
        for (int I = 0; I < Count; ++I) F(static_cast<const T&>(Buf[I]));
    }

    void Clear() {
        std::lock_guard<std::mutex> L(Mutex);
        Count = 0;
    }

    int Size() const {
        std::lock_guard<std::mutex> L(Mutex);
        return Count;
    }

    // Total events ever dropped for lack of room. Never reset, so a caller can log the first one and
    // then report the running total.
    uint32_t Dropped() const {
        std::lock_guard<std::mutex> L(Mutex);
        return DroppedCount;
    }

    static constexpr int Capacity = Cap;

private:
    mutable std::mutex Mutex;
    T Buf[Cap] = {};
    int Count = 0;
    uint32_t DroppedCount = 0;
};

}  // namespace Lur::Sim
