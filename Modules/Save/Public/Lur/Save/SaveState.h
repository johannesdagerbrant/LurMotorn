#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Lur::Save {

// The contract a game implements so the engine can persist + sync its state without
// knowing anything about it (issue #18). The engine (Store + SyncManager) only ever
// moves opaque bytes; all meaning — what "newer" means, how to merge — lives in the
// game's implementation.
//
// The three operations mirror the three things the engine does with a record:
//   Write        -> serialise (for disk save AND for the link-time Sync message)
//   Read         -> restore from bytes (an empty buffer means "fresh defaults")
//   MergeIfNewer -> adopt the peer's record iff it is strictly newer than ours
//
// Keeping this game-agnostic is what lets a future game reuse the same persistence +
// link-time-sync plumbing by supplying its own ISaveState.
class ISaveState {
public:
    virtual ~ISaveState() = default;

    // Serialise our current state into Out (appended).
    virtual void Write(std::vector<uint8_t>& Out) const = 0;

    // Restore from Data/Size. Size == 0 (absent record) must yield fresh defaults.
    virtual void Read(const uint8_t* Data, std::size_t Size) = 0;

    // Compare the peer's serialised record against ours and adopt it iff it is
    // strictly newer. Returns whether we adopted it (the caller then re-persists).
    virtual bool MergeIfNewer(const uint8_t* Data, std::size_t Size) = 0;
};

} // namespace Lur::Save
