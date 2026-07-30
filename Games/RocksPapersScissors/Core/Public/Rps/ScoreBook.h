#pragma once
// Rps::ScoreBook — the persistent W-L-D record, one per device: a tally per AI tier and a tally
// per human opponent (keyed by their device GUID). This is what makes the numbers on the opponent
// dropdown mean something: they were session-scoped, so every relaunch reset the ladder to 0-0-0
// and "have I ever beaten Hard" had no answer. Part of the #15-20 persistence family, on the same
// Modules/Save primitive as the device id.
//
// ONE BLOB, not one file per opponent, and that differs from chess on purpose. Chess keys a
// ChessRecord per opponent GUID because that record also carries the IN-PROGRESS match (its moves),
// so a record is a resumable game and belongs on its own. RPS has no resumable match — a score is
// three integers — and the AI tallies have no GUID to be keyed by at all, so they would need a home
// of their own anyway. One blob is one atomic write and one thing to reason about.
//
// THE PEER TALLIES ARE STORED PLAYER-AGNOSTICALLY (WinsLower/WinsHigher, anchored to whichever
// device has the lower GUID) even though nothing syncs them yet. Same reasoning as ChessRecord: two
// phones then hold BYTE-IDENTICAL bytes for the same rivalry, so the day the link exchanges them
// (#15-20) a merge is a comparison and not a reconciliation. Orienting to "my wins" happens at READ
// time, from the GUID order. The AI tallies are the opposite — deliberately LOCAL-ONLY, from this
// device's own perspective, because your record against a bot is nobody else's business and must
// never be merged with a peer's.
//
// A rivalry row therefore carries BOTH GUIDS as an ordered pair (lower, higher), not just "the
// opponent's". The first attempt keyed each row by the opponent alone — the obvious thing, since
// that is the id this device looked up — and it quietly destroyed the byte-identity above: my row
// names YOU and your row names ME, so the same rivalry serialised differently on each phone and
// nothing could be merged. Chess gets away with the opponent-only key because its key lives in the
// FILENAME and never inside the record. Ours is one blob, so the key has to be in the row, and the
// symmetric key is the pair. (A test reads one record from both sides for exactly this reason.)
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Rps/AiController.h"   // AiTierCount — the one source of truth for the per-tier array size

namespace Rps {

// A W-L-D from THIS device's point of view (what the UI shows).
struct Tally {
    uint32_t Wins = 0, Losses = 0, Draws = 0;
    uint32_t Total() const { return Wins + Losses + Draws; }
};

class ScoreBook {
public:
    // The Modules/Save key this record lives under. Not a GUID, so chess's 32-hex opponent filter
    // (and ours) skips it — it is a control key like "device-id".
    static constexpr const char* StoreKey = "rps-scores";
    // Rivalries kept. A phone that plays 32 different people is well past what this feature is for,
    // and a fixed cap keeps the record bounded without an eviction policy anyone has to trust. The
    // 33rd opponent is simply not tallied (logged by the caller if it cares) — never silently
    // evicting an older rivalry, which would be the worse failure.
    static constexpr int MaxPeers = 32;

    // ---- reads ----
    // Out-of-range tiers return a zero tally rather than trapping: a recording or a persisted blob
    // from a build with a different AiTierCount is data, not a programming error.
    Tally Ai(int Tier) const;
    // This device's record against PeerGuid, oriented by comparing the two GUIDs. An unknown
    // opponent is 0-0-0.
    Tally Peer(std::string_view PeerGuid, std::string_view MyGuid) const;

    // ---- writes: one finished match ----
    // Result is a Rps::Result* constant; MyTeam is the side this device played. Both take the raw
    // sim result rather than a pre-computed win/loss so the caller cannot get the orientation
    // subtly wrong in one main and right in another (there are three mains).
    void RecordAi(int Tier, uint8_t Result, uint8_t MyTeam);
    // Ignored (returns false) if either GUID is malformed or the book is full — a bad id must not
    // create a junk rivalry row that then persists forever.
    bool RecordPeer(std::string_view PeerGuid, std::string_view MyGuid, uint8_t Result,
                    uint8_t MyTeam);

    // ---- bytes ----
    // Layout: "RSB" + version, then the AI tallies (count-prefixed, so a build whose AiTierCount
    // changed reads an old blob without sliding the tiers along), then the peer rows.
    void Write(std::vector<uint8_t>& Out) const;
    // Size == 0 (absent record) yields fresh defaults and returns true. Returns false on a corrupt
    // stream, leaving *this in a usable (possibly partially filled) state — a damaged score file
    // must never stop the game from starting.
    bool Read(const uint8_t* Data, std::size_t Size);

    // ---- store ----
    bool Load(const Lur::Save::Store& S);
    bool Save(Lur::Save::Store& S) const;

private:
    struct PeerEntry {
        // The rivalry's identity: the two device ids, ALWAYS lower-first. Symmetric, so both phones
        // write the same row.
        char     Lower[Lur::Save::DeviceIdHexLen] = {};
        char     Higher[Lur::Save::DeviceIdHexLen] = {};
        uint32_t WinsLower = 0, WinsHigher = 0, Draws = 0;
    };
    // Index of the row for this pair of ids in either order, or -1. Never mutates — the const read
    // path uses it too.
    int FindPeer(std::string_view A, std::string_view B) const;

    Tally     Ai_[AiTierCount] = {};
    PeerEntry Peers_[MaxPeers] = {};
    int       PeerCount_ = 0;
};

}  // namespace Rps
