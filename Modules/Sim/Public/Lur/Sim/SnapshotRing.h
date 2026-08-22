#pragma once
// A fixed-capacity ring of sim snapshots keyed by tick: the rollback RESTORE buffer.
//
// Promoted out of Rps::SnapshotRing (#201) as a template over the state type. Save(T, S) stores the
// state AT tick T; Get(T) hands it back, or null once that tick has been evicted. On a mispredicted
// peer frame for tick T, the netcode does Get(T), re-applies the corrected input, and re-simulates
// forward to the speculative head.
//
// ---- Two properties make the memcpy legitimate and cheap ----
//   * T must be trivially copyable, so a snapshot is a flat byte copy — no per-entity walk, no
//     pointers to fix up. This is the same POD-state precondition SnapshotMailbox rests on, and it is
//     why "no pointers in sim state" is a rule rather than a preference.
//   * The states live on the HEAP, allocated ONCE at construction. A sim state is hundreds of KB and
//     the netcode object is stack-allocated in host tests (`LockstepPeer A, B;`), so a ring of states
//     BY VALUE inside it overflows the stack — and a per-Save allocation would touch the heap inside
//     the tick. One up-front allocation, then Save is a memcpy into a reused slot.
//
// ---- Eviction is IMPLICIT in the modular indexing ----
// slot = Tick % Capacity, and each slot remembers WHICH tick it holds. A Save at T reuses (and so
// evicts) the slot last written by T - Capacity, and Get checks the stored tick tag, so a query for an
// evicted tick returns null instead of a stale state — silently answering with the wrong tick's state
// is the one failure this class exists to make impossible.
//
// Capacity is the depth of history the ring can answer for. Size it RollbackHorizon + 2 so the
// confirmed tick's snapshot survives alongside a full horizon of speculative ones.
#include <cstdint>
#include <memory>
#include <type_traits>

namespace Lur::Sim {

template <class T>
class SnapshotRing {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SnapshotRing stores states by memcpy; T must be POD sim state");

public:
    explicit SnapshotRing(uint32_t Capacity)
        : Cap_(Capacity == 0 ? 1u : Capacity),
          Store_(new T[Cap_]),
          Tag_(new uint32_t[Cap_]),
          Used_(new bool[Cap_]()) {}   // () value-inits Used_ to all-false; Store_/Tag_ are set on Save

    uint32_t Capacity() const { return Cap_; }

    // Store the state at tick T. Overwrites — and so evicts — whatever tick previously occupied
    // Tick % Cap_.
    void Save(uint32_t Tick, const T& S) {
        const uint32_t I = Tick % Cap_;
        Store_[I] = S;
        Tag_[I] = Tick;
        Used_[I] = true;
    }

    // The snapshot saved for tick T, or null if that tick is not currently held (never saved, or
    // evicted by a later save that reused its slot). Read-only: the caller copies it into a live state.
    const T* Get(uint32_t Tick) const {
        const uint32_t I = Tick % Cap_;
        return (Used_[I] && Tag_[I] == Tick) ? &Store_[I] : nullptr;
    }

    // Forget every snapshot. A match restart or a resync rebuild re-bases the timeline, so old
    // snapshots key to ticks that no longer mean anything. Keeps the allocation.
    void Clear() {
        for (uint32_t I = 0; I < Cap_; ++I) Used_[I] = false;
    }

private:
    uint32_t Cap_;
    std::unique_ptr<T[]> Store_;           // the snapshots (heap, one allocation)
    std::unique_ptr<uint32_t[]> Tag_;      // slot -> the tick it holds (guards Get against a stale slot)
    std::unique_ptr<bool[]> Used_;         // slot -> ever written (so tick 0 isn't confused with empty)
};

}  // namespace Lur::Sim
