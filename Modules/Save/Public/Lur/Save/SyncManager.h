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

    // A peer link is up: remember its id and load our stored record for it (absent
    // -> the ISaveState restores fresh defaults).
    void OnLink(std::string_view PeerId) {
        Peer = std::string(PeerId);
        const std::vector<uint8_t> Blob = Disk.Load(Peer);
        State.Read(Blob.data(), Blob.size());
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

    const std::string& PeerId() const { return Peer; }

private:
    Store&      Disk;
    ISaveState& State;
    std::string Peer;
};

} // namespace Lur::Save
