#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Chess/Types.h"

namespace Chess {

// The per-opponent save record (issue #15), PLAYER-AGNOSTIC so both phones store
// byte-identical bytes: win counts are anchored to the lower-GUID device, not to
// "me". A phone shows its own W/L by checking whether it is the lower GUID.
//
// On-disk / on-wire layout (#66), VERSIONED as of v1:
//   [Version : u8 = 0xC1][WinsLower : u8][WinsHigher : u8][Draws : u8][moves: u16 ply + packed]
// The move bytes (only the in-progress match) reuse the slim move-index codec
// (EncodeGame/DecodeGame) — ~1 byte per ply. Completed matches keep only their
// tally; their moves are discarded.
//
// ---- WHY A VERSION BYTE, AND WHY 0xC1 ----
// Before #66 this format had no version, so the FIRST layout change would have made every existing
// record misparse silently — the tallies would simply be wrong numbers, with nothing to notice.
//
// 0xC1 is chosen to be implausible as a WinsLower value, because that is the byte it displaces: an
// unversioned (v0) record whose first byte is 0xC1 would need 193 wins on one device to be confused
// for a versioned one. Reader::Read uses that to accept v0 records as a MIGRATION rather than wiping
// them — a tally is cheap to keep and annoying to lose.
//
// ---- THESE BYTES ARE ALSO THE WIRE ----
// ChessRecord is the game's Lur::Save::ISaveState, and GameHost sends exactly these bytes as
// Lur::Net::EMsgType::Sync on every link (per-opponent sync, #15-#20). So a layout change here IS a
// wire change, and it bumped Lur::Net::ProtocolVersion to 12. Issue #66 asserted the opposite ("an
// on-disk format change, not a wire change"); it was wrong, and the bump is what stops a mixed pair
// from exchanging records it cannot parse — Session refuses the link on a version mismatch before any
// Sync payload is sent.
//
// Note the asymmetry that follows: the v0 fallback below is reachable only from DISK. A v0 peer can no
// longer link at all, so it can never deliver a v0 payload over the wire.
struct ChessRecord {
    uint8_t WinsLower  = 0;   // matches won by the lower-GUID device
    uint8_t WinsHigher = 0;   // won by the higher-GUID device
    uint8_t Draws      = 0;
    std::vector<Move> Moves;  // the in-progress match's moves, from the start position

    unsigned TotalMatches() const {
        return static_cast<unsigned>(WinsLower) + WinsHigher + Draws;
    }

    // Serialise (appended to Out).
    void Write(std::vector<uint8_t>& Out) const;

    // The on-disk/on-wire format version this build writes. See the layout note above.
    static constexpr uint8_t Version1 = 0xC1;
    // Bytes >= this are RESERVED for format versions and never appear as a v0 WinsLower. That is what
    // lets Read tell a versioned record from an unversioned one, and lets it REFUSE a version it does
    // not know instead of guessing. A per-opponent tally at or above 192 wins is not a thing; if one
    // ever were, the cost is a rejected save (fresh defaults), never a corrupted one.
    static constexpr uint8_t VersionSpaceBegin = 0xC0;

    // Restore. Size == 0 yields fresh defaults (returns true). Accepts a v1 record, and a v0
    // (pre-#66, unversioned) record as a migration. Returns false on a corrupt/illegal stream or an
    // UNKNOWN version, leaving *this unchanged.
    bool Read(const uint8_t* Data, std::size_t Size);

private:
    // The tallies-then-moves body, shared by v0 and v1.
    bool ReadBody(const uint8_t* Data, std::size_t Size);
};

} // namespace Chess
