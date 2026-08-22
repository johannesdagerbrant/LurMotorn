#pragma once
// The set of gameplay-CVar overrides two peers agree on, merged last-write-wins with a deterministic
// tie-break.
//
// Promoted out of Rps::LockstepPeer (#147, #169, #201). Both peers keep tunable knobs the player can
// edit from the dev console, both can edit the SAME knob, and the two sims must end up bit-identical
// from tick 0 or they diverge with nothing on the wire to point at. That is what this resolves.
//
// ---- The merge rule, which is the whole file ----
//   * A LATER wall-clock edit wins. That is the ordinary case and it needs no coordination.
//   * A TIE at the same millisecond with DIFFERENT values ERASES the entry, dropping back to the
//     compiled default.
//
// The erase is the interesting half. Two peers editing the same knob in the same millisecond have no
// ordering between them, and there is no referee to ask (LurMotorn has no host — see CLAUDE.md's
// distributed-authority rule). Picking one peer's value would require agreeing WHICH, so instead both
// peers discard: the compiled default is a value they provably already share. This is the
// "consistency, not fairness" rule applied to tunables — nobody gets what they typed, but both
// screens agree, which is the property that matters.
//
// ---- Ids are one byte, so the ledger is a flat array ----
// 256 slots, allocation-free, and — unlike the std::unordered_map this replaces — iterated in ASCENDING
// ID ORDER. That matters because the sync message is built by iterating this: an unordered_map emitted
// the entries in an implementation-defined order, so the same override set produced different byte
// sequences on the two phones. Harmless today (see the KNOWN LIMIT below) but not a property worth
// relying on by accident.
//
// ---- KNOWN LIMIT: the tie-break is not TOTAL ----
// With THREE values at the identical millisecond for one id, the result depends on merge order:
// merging (1@t, 2@t) erases and then 4@t lands as 4, while merging (1@t, 4@t) erases and then 2@t
// lands as 2. Reaching it needs three edits of one knob inside one millisecond, which needs one peer
// to write the same knob twice in a frame — so it is narrow, and it is pinned by a test rather than
// left to be discovered. Making the tie-break total (e.g. "on a tie keep the lower raw value") would
// close it, but that changes what a tie resolves TO, which is a decision rather than a side effect of
// a relocation.
#include <cstdint>

namespace Lur::Net {

// One override: the raw value, and the wall-clock millisecond of the edit that set it.
struct CvarEdit {
    int32_t Raw = 0;
    uint64_t WallMs = 0;
};

class CvarLedger {
public:
    static constexpr int MaxIds = 256;

    // Merge one edit. See the rule above: later wins; a same-millisecond disagreement erases.
    void Merge(uint8_t Id, int32_t Raw, uint64_t WallMs) {
        Merged_ = true;
        CvarEdit& E = Slot_[Id];
        if (!Present_[Id]) {
            E = {Raw, WallMs};
            Present_[Id] = true;
            ++Count_;
            return;
        }
        if (WallMs > E.WallMs) {
            E = {Raw, WallMs};
        } else if (WallMs == E.WallMs && Raw != E.Raw) {
            Present_[Id] = false;   // both peers discard; the compiled default is common ground
            --Count_;
        }
    }

    bool Has(uint8_t Id) const { return Present_[Id]; }

    bool Get(uint8_t Id, int32_t& OutRaw) const {
        if (!Present_[Id]) return false;
        OutRaw = Slot_[Id].Raw;
        return true;
    }

    bool Get(uint8_t Id, CvarEdit& Out) const {
        if (!Present_[Id]) return false;
        Out = Slot_[Id];
        return true;
    }

    int Count() const { return Count_; }

    // Whether anything has EVER been merged, even if a tie has since erased it. The caller uses this
    // to decide whether the sim must be rebuilt from an overridden set at all — and it must stay true
    // after an erase, because "an override was discussed and resolved to the default" is still a
    // different starting point from "no override was ever mentioned" as far as the rebuild path is
    // concerned.
    bool AnyMerged() const { return Merged_; }

    void Clear() {
        for (int I = 0; I < MaxIds; ++I) Present_[I] = false;
        Count_ = 0;
        Merged_ = false;
    }

    // Visit every present override in ASCENDING ID ORDER. F(uint8_t Id, const CvarEdit&).
    template <class Fn>
    void ForEach(Fn&& F) const {
        for (int I = 0; I < MaxIds; ++I)
            if (Present_[I]) F(static_cast<uint8_t>(I), Slot_[I]);
    }

private:
    CvarEdit Slot_[MaxIds] = {};
    bool Present_[MaxIds] = {};
    int Count_ = 0;
    bool Merged_ = false;
};

}  // namespace Lur::Net
