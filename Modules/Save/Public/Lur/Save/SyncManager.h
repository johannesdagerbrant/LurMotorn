#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Lur/Save/SaveState.h"
#include "Lur/Save/Store.h"

namespace Lur::Save {

// Ties a Store (disk) to an ISaveState (the game's record) for the per-opponent
// persist + link-time sync flow (issue #18). It is DELIBERATELY decoupled from
// Lur::Net: the app wires it to the Session's hooks (ready -> OnLink + send
// Snapshot as a Sync message; Sync received -> OnSync; background/disconnect ->
// Persist). Keeping Net out avoids a Save<->Net dependency cycle and lets this be
// host-tested with no transport.
//
// Network sync happens ONLY at link establishment (initial + reconnect); disk
// Persist() is the separate local concern (call it on background/disconnect/
// match-end).
class SyncManager {
public:
    SyncManager(Store& StoreRef, ISaveState& StateRef) : Disk(StoreRef), State(StateRef) {}

    // A peer link is up: remember its id and fold our stored record into the current
    // state. Uses MergeIfNewer (NOT a hard Read) so this can't downgrade state that a
    // peer's Sync already delivered — link-time reconciliation is order-independent
    // and monotonic across {current state, disk, peer}, so nothing ever resets to an
    // older record regardless of whether the disk load or the Sync arrives first.
    void OnLink(std::string_view PeerId) {
        Peer = std::string(PeerId);
        const std::vector<uint8_t> Blob = Disk.Load(Peer);
        if (!Blob.empty()) State.MergeIfNewer(Blob.data(), Blob.size());
    }

    // Serialise the current state — the payload for a link-time Sync message.
    std::vector<uint8_t> Snapshot() const {
        std::vector<uint8_t> Out;
        State.Write(Out);
        return Out;
    }

    // Write the current state to disk under the current peer. No-op before OnLink.
    void Persist() const {
        if (Peer.empty()) return;
        std::vector<uint8_t> Out;
        State.Write(Out);
        Disk.Save(Peer, Out.data(), Out.size());
    }

    // A peer's Sync payload arrived: adopt it iff strictly newer, and persist if so.
    // Returns whether we adopted it.
    bool OnSync(const uint8_t* Data, std::size_t Size) {
        const bool Adopted = State.MergeIfNewer(Data, Size);
        if (Adopted) Persist();
        return Adopted;
    }

    // Re-key to a different peer WITHOUT loading/merging its record — for when the
    // caller has already hard-loaded the target opponent's game (a deliberate switch,
    // #38), as opposed to OnLink's monotonic reconnect merge. Subsequent Persist()/
    // OnSync() then target this peer. Pass "" to unbind (local hot-seat).
    void Rebind(std::string_view PeerId) { Peer = std::string(PeerId); }

    const std::string& PeerId() const { return Peer; }

private:
    Store&      Disk;
    ISaveState& State;
    std::string Peer;
};

} // namespace Lur::Save
