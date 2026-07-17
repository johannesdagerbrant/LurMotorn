Title: Render captured pieces in capture order above/below the board (derived from move history)

---

## Summary

Show captured pieces as small piece icons in the margins around the board:

- Pieces the **local player has captured** (opponent's colour) lined up **below** the board.
- The **local player's pieces the opponent has captured** lined up **above** the board.

Both rows in the order the captures happened, left → right. Capture order is **not stored** — it is derived by replaying `ChessRecord::Moves`, exactly the way the board itself is already reconstructed (`ChessMatchState::RebuildBoard`). This keeps with the engine's "derive minimal state" philosophy: nothing new to persist or sync.

## Behaviour

- **Below** the board (near the local player): opponent-colour pieces the player has captured.
- **Above** the board (far side): player-colour pieces captured by the opponent.
- Ordered oldest-capture-first, left → right.
- "Above/below" are **screen-space** and must follow `BoardView::FlipBoard()`, so they stay correct when the Black player's view is rotated 180°.

## Deriving capture order (no new stored state)

Add a pure-C++ helper in **Chess Core** (host-testable, no render/transport deps) that replays the move list and collects captures in order:

- Source of truth: `ChessMatchState::Record().Moves` — `std::vector<Chess::Move>`, the in-progress match from the start position.
- Replay from `Board::StartPosition()`. For each `Move M`, **before** calling `Board::MakeMove(M)`:
  - **Normal capture:** `PieceTypeAt(B, Opposite(B.SideToMove), M.To)` — if it is not `EPieceType::None`, a piece of colour `Opposite(B.SideToMove)` and that type was captured.
  - **En passant** (`M.Flags & MoveFlagEnPassant`): a pawn of `Opposite(B.SideToMove)` is captured on the square *behind* `M.To` — same file as `M.To`, rank of `M.From`.
- The order pieces appear during replay **is** the capture order. No separate list, no persistence, no wire changes.

Suggested shape (Core):

```cpp
struct CapturedPiece { EColor Color; EPieceType Type; };

// Captured pieces in the order they were taken, oldest first.
// Replays Moves from the start position; detects normal + en-passant captures.
std::vector<CapturedPiece> CapturedPieces(const std::vector<Move>& Moves);
```

`BoardView` then splits the result by colour into the two rows.

## Rendering

Reuse what `BoardView` already owns — no new art or materials:

- `ComputeLayout(W, H)` gives `OriginX / OriginY / Square`. The board is a centred square at `0.95 *` the shorter side, so there is a margin band above `OriginY` and below `OriginY + 8*Square` to hold the rows.
- Draw each captured piece as a `QuadMesh` with the existing per-type materials: a captured **White** piece uses `PieceLight[type]`, a captured **Black** piece uses `PieceDark[type]` (the tint-trick silhouettes).
- Scale to fit the margin (≈ `Square * 0.5`), lay out along X from `OriginX` in capture order; clamp or wrap if one side has captured many pieces.

## Module placement

- **Derivation** → `Games/Chess/Core` (pure logic; add a test in `Games/Chess/Core/Tests`).
- **Presentation only** → `BoardView::Render` in `Games/Chess/View`.

Keeps the `Games/* → Modules/*` (never the reverse) rule intact and the logic host-testable.

## Protocol

Purely presentational, derived from already-synced state (the move list). Does **not** change move ordering or the wire format — **no `Lur::Net::ProtocolVersion` bump.**

## Open questions

- **Hot-seat / no identity** (`ClearIdentity`, see #38): there is no single "local player" colour. Fall back to a White-at-bottom perspective (White's losses above, Black's below), or hide the trays in that mode?
- Optional: show a small material-advantage number (+N) beside a tray.

## Acceptance criteria

- [ ] Core helper returns captured pieces in capture order; unit-tested, including an en-passant capture and a promotion-with-capture.
- [ ] Captured pieces render in the correct margin, correct colours, in capture order.
- [ ] Rows respect `FlipBoard()` orientation.
- [ ] Nothing new persisted or sent on the wire; no protocol version bump.
