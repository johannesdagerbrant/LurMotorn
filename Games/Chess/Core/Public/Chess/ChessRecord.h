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
// On-disk / on-wire layout:
//   [WinsLower : u8][WinsHigher : u8][Draws : u8][moves: u16 ply + packed indices]
// The move bytes (only the in-progress match) reuse the slim move-index codec
// (EncodeGame/DecodeGame) — ~1 byte per ply. Completed matches keep only their
// tally; their moves are discarded.
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

    // Restore. Size == 0 yields fresh defaults (returns true). Returns false on a
    // corrupt/illegal stream, leaving *this unchanged.
    bool Read(const uint8_t* Data, std::size_t Size);
};

} // namespace Chess
