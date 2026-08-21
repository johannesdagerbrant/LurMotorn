#pragma once
// Double-buffered snapshot hand-off from the tick thread to the render thread.
// Single-producer, single-consumer, one lock, no allocation.
//
// Promoted out of Rps::SnapshotMailbox (#201) as a template over the snapshot type: there is zero
// game content in it, and the seam it implements — "the sim thread owns state, the render thread gets
// copies" — is the shape every game on a dedicated tick thread needs.
//
// The producer fills the back buffer UNLOCKED, because that copy is the expensive part (~90 KB for
// RPS) and must not block the render thread. Publish() then flips the indices under a short lock, and
// the consumer copies the front buffer out under the same lock. Once anything has been published the
// two indices differ, so the lock only ever guards the index swap and the consumer's copy — the same
// "copy under lock, heavy work outside" shape as Transport::EventInbox. (They start EQUAL, which is
// safe precisely because Consume() refuses until the first Publish().)
//
// A lock-free triple buffer is a drop-in upgrade if a publish rate ever contends the render thread.
// At RPS's 10 Hz it does not, and an uncontended mutex is cheaper to reason about than three indices.
//
// T must be trivially copyable — the whole design rests on the snapshot being a memcpy, which is the
// same POD-state precondition the rollback snapshot ring depends on.
#include <mutex>
#include <type_traits>

namespace Lur::Sim {

template <class T>
class SnapshotMailbox {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SnapshotMailbox copies under a lock; T must be a POD snapshot");

public:
    // Producer (tick thread): write here, then Publish().
    T& Back() { return Buffers[BackIdx]; }

    void Publish() {
        std::lock_guard<std::mutex> Lock(Mutex);
        FrontIdx = BackIdx;
        BackIdx = 1 - BackIdx;
        HasPublished = true;
    }

    // Consumer (render thread): copies the latest published snapshot into Out. False until the first
    // Publish(). The copy is under the lock so Front cannot flip mid-copy, which is what makes two
    // buffers sufficient.
    bool Consume(T& Out) const {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (!HasPublished) return false;
        Out = Buffers[FrontIdx];
        return true;
    }

    bool Published() const {
        std::lock_guard<std::mutex> Lock(Mutex);
        return HasPublished;
    }

private:
    mutable std::mutex Mutex;
    T Buffers[2]{};
    int FrontIdx = 0;
    int BackIdx = 0;   // starts equal to Front; the first Publish() moves Front onto it and flips

    bool HasPublished = false;
};

}  // namespace Lur::Sim
