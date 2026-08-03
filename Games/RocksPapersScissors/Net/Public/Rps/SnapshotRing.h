#pragma once
#include <cstdint>
#include <memory>
#include <vector>

#include "Rps/Sim.h"

// Rollback scaffolding (the responsiveness experiment, Docs/Journal/2026-08-03, Phase 1).
//
// This header lands the two REUSABLE pieces rollback needs, on their own so they can be unit-tested
// in isolation before the execution-model surgery in Phase 2 touches LockstepPeer — the game's
// hardest-won file. Nothing here is wired into LockstepPeer yet; Phase 2 adds the ring as a member
// and calls Save/Get from the new speculate/roll-back loop.
namespace Rps {

// A fixed-capacity ring of Sim snapshots keyed by exec tick. Save(T, sim) memcpy-stores the state AT
// tick T; Get(T) hands it back, or null once that tick has been evicted (overwritten by a newer
// save) / was never stored. This is the rollback restore buffer: on a mispredicted peer frame for
// tick T, Phase 2 will Get(T), re-apply the corrected batch, and re-simulate to the speculative head.
//
// Two properties make the memcpy legitimate and cheap:
//   * Sim is trivially copyable (static_assert in Sim.h names exactly this use), so a snapshot is a
//     flat byte copy — no per-unit walk, no pointers to fix up.
//   * The Sims live on the HEAP, allocated ONCE at construction. A Sim is hundreds of KB and
//     LockstepPeer is stack-allocated in the host tests (`LockstepPeer A, B;`), so a ring of Sims by
//     value inside it would overflow the stack; and a per-Save allocation would touch the heap in the
//     tick. One up-front allocation, then Save is a memcpy into a reused slot — nothing allocates
//     during play.
//
// Eviction is IMPLICIT in the modular indexing: slot = Tick % Capacity, and each slot remembers which
// tick it holds. A Save at T reuses (and so evicts) the slot last written by T - Capacity, and Get
// checks the stored tick tag, so a query for an evicted tick returns null instead of a stale Sim.
// Capacity is therefore the depth of history the ring can answer for — size it RollbackHorizon + 1 so
// the confirmed tick's snapshot survives alongside a full horizon of speculative ones.
class SnapshotRing {
public:
    explicit SnapshotRing(uint32_t Capacity)
        : Cap_(Capacity == 0 ? 1u : Capacity),
          Store_(new Sim[Cap_]),
          Tag_(new uint32_t[Cap_]),
          Used_(new bool[Cap_]()) {}  // () value-inits Used_ to all-false; Store_/Tag_ are set on Save

    uint32_t Capacity() const { return Cap_; }

    // memcpy the state at tick T into its slot (trivially-copyable assignment). Overwrites — and so
    // evicts — whatever tick previously occupied Tick % Cap_.
    void Save(uint32_t Tick, const Sim& S) {
        const uint32_t I = Tick % Cap_;
        Store_[I] = S;
        Tag_[I] = Tick;
        Used_[I] = true;
    }

    // The snapshot saved for tick T, or null if that tick is not currently held (never saved, or
    // evicted by a later save that reused its slot). Read-only: the caller copies it into a live Sim.
    const Sim* Get(uint32_t Tick) const {
        const uint32_t I = Tick % Cap_;
        return (Used_[I] && Tag_[I] == Tick) ? &Store_[I] : nullptr;
    }

    // Forget every snapshot (e.g. a match restart / resync rebuild re-bases the timeline, so old
    // snapshots key to ticks that no longer mean anything). Keeps the allocation.
    void Clear() {
        for (uint32_t I = 0; I < Cap_; ++I) Used_[I] = false;
    }

private:
    uint32_t                    Cap_;
    std::unique_ptr<Sim[]>      Store_;  // the snapshots (heap, one allocation)
    std::unique_ptr<uint32_t[]> Tag_;    // slot -> the tick it holds (guards Get against a stale slot)
    std::unique_ptr<bool[]>     Used_;   // slot -> ever written (so tick 0 isn't confused with empty)
};

// The peer-input predictor. Rollback speculates the ticks for which the peer's real frame has not yet
// arrived; the prediction is "the peer produced NO input events this tick". It is right the large
// majority of ticks because human taps are only a few per second against a 10 Hz sim, so most
// speculated ticks match the real frame when it lands and roll back to a no-op (plan §"what we are
// testing"). A named seam, trivial by design: Phase 2 fills a speculative PeerEvents entry with this,
// and the emptiness of the prediction is the property a test pins.
inline void PredictPeerBatch(std::vector<InputEvent>& Out) { Out.clear(); }

}  // namespace Rps
