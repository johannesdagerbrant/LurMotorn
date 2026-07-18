#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "Chess/Board.h"
#include "Chess/ChessRecord.h"
#include "Chess/Types.h"
#include "Lur/Save/SaveState.h"

namespace Chess {

// The authoritative game state for a networked chess match (issue #18): the
// player-agnostic record + the board derived from it + this device's colour. It is
// an ISaveState, so the engine persists and link-syncs it without knowing chess.
//
// Colour is DETERMINISTIC and identical on both phones, independent of the BLE
// radio role: it comes from the two device GUIDs' order plus the match-count parity
// (even total => the lower-GUID device is White; odd => Black). Because both phones
// share the same synced record, both compute the same, opposite colours — and it is
// stable across restarts (the flip that broke reconnect in Phase A is gone).
//
// Pure logic, no rendering or transport: BoardView renders it, SyncManager persists
// and syncs it. Host-testable.
class ChessMatchState : public Lur::Save::ISaveState {
public:
    // Establish identity once the peer's GUID is known (link established). Both are
    // the hex device ids from Lur::Save. Colour + the lower/higher anchor derive
    // from their lexicographic order.
    void SetIdentity(std::string_view LocalGuid, std::string_view PeerGuid);
    bool HasIdentity() const { return Identified; }
    bool IsLocalLower() const { return LocalLower; }

    // Drop identity: the match becomes a local hot-seat (either side may move, no
    // colour lock, board not flipped) — the "same device / both sides" mode (#38).
    void ClearIdentity() { Identified = false; }

    // This device's colour for the current match (valid once identity is set).
    EColor MyColor() const;
    EColor SideToMove() const { return Position.SideToMove; }
    bool   IsMyTurn() const { return Identified && Position.SideToMove == MyColor(); }

    const Board&       CurrentBoard() const { return Position; }
    const ChessRecord& Record() const { return Rec; }

    // A hash of the position that must match on both peers for move indices to align
    // (pieces, side, castling, en passant, halfmove clock). Rides the Net keepalive so
    // a mid-game desync is detected and healed via resync (issue #72). Deterministic.
    std::uint64_t PositionHash() const;

    // The result of the most recently concluded match (Ongoing until the first ends).
    // Set when a terminal move auto-concludes a match; useful for a UI "you won" note.
    EGameResult LastResult() const { return Last; }

    // Called once whenever a move concludes a match (the tally is already bumped and
    // the board reset). The app wires this to persist the updated all-time stats to
    // disk. LOCAL only — no network sync, since the terminal move already concluded
    // the match deterministically on both peers.
    void SetOnMatchEnd(std::function<void()> H) { OnMatchEnd = std::move(H); }

    // Apply a legal move: advance the board and append it to the record. If the move
    // ends the game (checkmate / stalemate / 75-move auto-draw), auto-conclude the
    // match: bump the agnostic W/L/D tally and start the next match (board reset,
    // colour recomputed from the new match-count parity). This is DETERMINISTIC from
    // the shared move sequence, so both peers conclude identically and stay in sync.
    void ApplyMove(const Move& M);

    // Replay the record's move list from the start position into the board. Call
    // after Read / a merge that replaced the record.
    void RebuildBoard();

    // --- Lur::Save::ISaveState ---
    void Write(std::vector<uint8_t>& Out) const override { Rec.Write(Out); }
    void Read(const uint8_t* Data, std::size_t Size) override;
    bool MergeIfNewer(const uint8_t* Data, std::size_t Size) override;

private:
    // Newness key: (totalMatches, moveCount). Matches dominate (a finished match
    // resets moves to 0), so they compare first.
    static bool StrictlyNewer(const ChessRecord& A, const ChessRecord& B);  // A newer than B?

    // Terminal state of the current position, or Ongoing.
    EGameResult DetectResult() const;
    // Bump the agnostic tally for a terminal result and start the next match.
    void ConcludeMatch(EGameResult R);

    ChessRecord           Rec;
    Board                 Position = Board::StartPosition();
    EGameResult           Last = EGameResult::Ongoing;
    std::function<void()> OnMatchEnd;
    std::string           LocalGuid;
    std::string           PeerGuid;
    bool                  Identified = false;
    bool                  LocalLower = false;  // our GUID sorts before the peer's
};

} // namespace Chess
