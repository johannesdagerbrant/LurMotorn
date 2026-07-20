<!--
  Each block delimited by "===== ISSUE =====" is one standalone GitHub issue (no epic).
  First line of each block is "Title:"; everything after is the issue body (Markdown).
  Source: Review #2 (Handmade lens) items that the Review #1 extraction epic never drafted but the
  master roadmap places in Phase 0 (floor-raises). Filed standalone; tracked from #12.
-->

===== ISSUE =====
Title: P0-floor — Compiler flags (-Wall -Wextra -Werror, exceptions/RTTI off) + LUR_ASSERT (crash loudly in dev)

Roadmap Phase 0. Review #2 §3.5 and §5. **Do this first in Phase 0** — every subsequent fix (the P0
bugs, the core-hardening batch, the de-chess `SendBare` refusal) should be written under loud asserts
and full warnings.

## Problem

There is **no** `-Wall`/`-Wextra`/`-Werror`, no exceptions/RTTI decision, and no assertion facility
**anywhere in the tree** (verified: zero matches for the flags and for `LUR_ASSERT`/`assert`/`<cassert>`
across all CMake and source). For a from-scratch engine that owns its whole stack, warnings are the
cheapest bug-finder there is, and the codebase currently *absorbs* contradictions silently instead of
announcing them (the >61-ply `Send` drop, `EncodeMove` index-0, `MoveList` overflow, `OnHello`
self-collision are all silent guards). Review #2's inversion: **in dev builds, wrongness should be
deafening; release builds keep the quiet guards.**

## Fix

- **`EngineFlags.cmake`** (shared include applied to all targets): `-Wall -Wextra -Werror`, exceptions
  off (`-fno-exceptions`) and RTTI off (`-fno-rtti`) — nothing is thrown and nothing `dynamic_cast`ed
  today, so the machinery is pure cost (and on iOS it entangles with the `std::filesystem` floor). A
  defined Debug/Release optimization story. Expect ~a day of warning cleanup.
- **`LUR_ASSERT(cond)`** (a small `Lur/Core/Assert.h`): in dev builds, log the expression + `file:line`
  + relevant values, then trap into the debugger; in release builds, compile to the existing quiet
  guard (or nothing). Use it aggressively at every "can't happen": encode-of-illegal-move, `MoveList`
  overflow, oversized bare send, unknown message index, GUID self-collision.
- Coordinate with the core-hardening batch and the de-chess `SendBare` issue — those provide the *call
  sites*; this issue provides the *macro + flags*.

## Acceptance criteria

- [ ] Shared flags include applied to every target; host build is `-Werror`-green.
- [ ] `-fno-exceptions -fno-rtti` decision recorded in the CMake + a short comment (or a deliberate,
      documented exception if something needs them).
- [ ] `LUR_ASSERT` exists, traps in dev, is a quiet guard in release, and is used at ≥3 of the named
      "can't happen" sites.
- [ ] `build.ps1` still green.


===== ISSUE =====
Title: P0-floor — Add a format-version field to the on-disk ChessRecord (Review #2 §6)

Roadmap Phase 0 (floor-raise). Review #2 §6.

## Problem

The **wire** has `Lur::Net::ProtocolVersion`; the **on-disk** record format has **no version field**
(verified). The first time the serialized `ChessRecord` layout changes, every existing save silently
misparses. One reserved byte today; a migration story that would be miserable to retrofit later.

## Fix

- Prepend a small version byte (or reserved header) to the serialized record; current layout = v1.
- Reader validates the version: known → parse; unknown/absent → handle deliberately (reject cleanly, or
  a best-effort migration). Since saves are currently disposable test data, a clean reject + wipe is an
  acceptable v1 policy — but the *seam* is the point, so the next layout change is a version bump, not a
  silent break.
- This is an **on-disk** format change, not a wire change — no `Lur::Net::ProtocolVersion` bump; but
  document the on-disk version constant next to where records are (de)serialized.

## Acceptance criteria

- [ ] Serialized `ChessRecord` carries a version; the reader branches on it.
- [ ] An unknown/old version is handled without misparsing (rejected or migrated, not read as garbage).
- [ ] Host test covers round-trip at v1 and rejection of a bogus version.
