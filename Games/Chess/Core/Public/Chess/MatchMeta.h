#pragma once
#include <cstdint>
#include <string>

#include "Lur/Save/Store.h"

namespace Chess {

// Per-opponent LOCAL metadata (issue #36). Deliberately separate from ChessRecord:
// the record is byte-identical on both phones and merged via MergeIfNewer, so a
// per-device wall-clock would corrupt the merge. This sidecar is never synced and
// never on the wire (no ProtocolVersion change). Stored under key "meta-"+guid,
// which is not 32-hex, so EnumerateOpponents' GUID filter ignores it.
struct MatchMeta {
    // Wall-clock time (Unix epoch milliseconds) of the last move applied locally
    // against this opponent — mine or received. 0 = no move yet. Drives the
    // "moved 2m ago" / "you moved 40s ago" hint (wording keys off whose turn it is).
    std::uint64_t LastMoveMs = 0;
};

// Load the sidecar for an opponent GUID (absent -> default {0}). Save returns false
// on I/O failure. The store key is derived as "meta-"+Guid.
MatchMeta LoadMatchMeta(const Lur::Save::Store& Store, const std::string& Guid);
bool      SaveMatchMeta(Lur::Save::Store& Store, const std::string& Guid, const MatchMeta& Meta);

// Current wall-clock time in Unix epoch milliseconds (std::chrono::system_clock).
// Display-only (not simulation), so it is exempt from the fixed-point rule — the
// caller stamps MatchMeta::LastMoveMs with this whenever a move is applied.
std::uint64_t NowMillisUtc();

} // namespace Chess
