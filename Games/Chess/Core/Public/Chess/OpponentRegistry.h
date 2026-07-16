#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "Lur/Save/Store.h"

namespace Chess {

// One historical opponent, derived from its persisted ChessRecord (issue #35).
// Player-oriented: Wins/Losses are from THIS device's perspective (the record is
// stored lower/higher-GUID-agnostic; we orient it via the GUID order).
struct OpponentInfo {
    std::string Guid;                 // the opponent's 32-hex device GUID
    bool        MyTurn   = false;     // is it this device's move against them?
    std::uint8_t Wins    = 0;
    std::uint8_t Losses  = 0;
    std::uint8_t Draws   = 0;
    std::uint32_t MoveCount = 0;      // plies in the in-progress match
};

// Enumerate every historical opponent from the save store. Reads each opponent's
// ChessRecord, replays it under this device's identity, and reports whose turn it is
// plus the all-time tally — WITHOUT needing a live link (this is what lets an offline
// opponent show "your turn"). Order is unspecified; the caller sorts/groups for the
// UI. LocalGuid is this device's id (from Lur::Save::LoadOrCreateDeviceId).
//
// Store keys are filtered to 32-hex GUIDs, so control keys ("device-id", "peer-id")
// are skipped, and LocalGuid itself is excluded (you are not your own opponent).
std::vector<OpponentInfo> EnumerateOpponents(const Lur::Save::Store& Store,
                                             const std::string& LocalGuid);

} // namespace Chess
