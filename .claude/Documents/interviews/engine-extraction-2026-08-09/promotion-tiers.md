# Promotion decisions — what moves into the engine

## Key Points

- **All of Tier 1 (~2,200 LOC) is IN** — relocation, not abstraction; no plausible conflict.
- **Four Tier 2 items are IN**, one of them substantially reframed by the user.
- **Four items explicitly EXCLUDED**, with reasons recorded so they aren't re-proposed.

---

## Tier 2 — ACCEPTED

### 1. Dev console assembly (~690 LOC, RPS `GameView.cpp:1931-2620`)
Completes a half-done job: `Modules/DevGui` already exists but holds **only the leaf widgets**
(479 LOC) — the assembly/layout/interaction layer never came back. Conflict is disallowed by the
existing devtools doctrine (*"no per-game hook; if a tool behaviour needs to change, it changes in
the engine, for all games at once"*). **Chess has no console today and gets one free.**
Watch: RPS's numpad-first + colour-picker layout is phone-RTS-shaped; may need a second layout mode
for a desktop-heavy physics workflow.

### 2. Selector plumbing — NOT row semantics
Promote a **rows-as-data** selector model plus the semantic-reselection lesson both games learned
independently (re-point by identity, never by row index — RPS uses `SelPeer_`/`SelAiTier_`, chess
re-scans `ItemGuid`; same lesson, two implementations).
**Stays in the game:** what a row *means* — chess's persistent async opponents sorted your-turn-first
with Online/Offline headers vs RPS's one live peer + AI tiers. These differ **essentially**.
Kills ~80 lines of duplicated widget-driving per game.

### 3. Audio — the generic layer (REVISED, user rationale)

**User (2026-08-09):** *"Both RPS and the physics game need audio eventually, and the majority of
audio features will not be unique to any of the games. That is why SFX is the exception."*

**This reclassifies the item.** It is not a one-sample promotion — **the second and third consumers
are known, they simply haven't been built yet.** RPS has no audio only because it hasn't got round
to it. That is precisely the case the promote-on-second-consumer rule exists to serve, and it means
audio should be scoped *wider* than the narrow "one-shot variation picker" I originally proposed.

Aligns with **#82**'s already-written per-piece disposition:

| Piece | Owner |
|---|---|
| `Mixer` (wait-free SPSC, 16 voices, Q32.32 pitch) | **Engine** — already is |
| Sound bank / clip registry | **Engine** |
| Variation policy — group table, no-immediate-repeat picker, independent pitch/gain jitter | **Engine** |
| Trigger path | **Engine** |
| Per-platform `IAudioDevice` backend | **Engine** — and note only chess has one today, so RPS/iOS + RPS/Android would otherwise each copy the seam a 3rd and 4th time (disease A waiting to happen) |
| Event → sound **classification** (chess's `EMoveSound`, `ClassifyMove`) | **Game** |
| The clips themselves | **Game** (content) |

**Still excluded:** continuous/parameterized audio (machinery hum, impact-energy-scaled sound) —
that shape is genuinely unknown until the physics game exists.

### 4. Per-opponent persistence with a GAME-DEFINED SCHEMA  ← reframed by the user

**The user's correction, and it is a better boundary than the "W-L-D tally" I proposed.**

This is not a tally primitive. It is *per-opponent persistent storage where the engine owns the
mechanism and the game declares the fields*:

| Layer | Owner | Content |
|---|---|---|
| Mechanism | **Engine** | Key by opponent GUID; player-agnostic ordering (lower/higher GUID); orient at read time; `MergeIfNewer` over the link; store-key enumeration; sidecar timestamps |
| Schema | **Game** | Which fields persist |

Declared schemas:
- **Chess** — wins / losses / draws
- **RPS** — wins / losses
- **Co-op physics puzzle** — levels completed / gold stars found

**Why this is the model case for the whole extraction:** the merge-and-sync machinery is *identical*
across all three; only the payload schema differs. It is exactly the user's stated test — *"one
engine feature that satisfies both via API arguments, not two."* It also means **the co-op game gets
per-opponent persistence for free despite having no win/loss concept at all**, which the narrower
"tally" framing would have failed to serve.

Supersedes: chess's `ChessRecord` + `MatchMeta` + `OpponentRegistry` and RPS's `ScoreBook` (276 LOC)
each re-implementing the same principle. `Modules/Save` already has `ISaveState`/`SyncManager` — but
they are **chess-only** today; RPS reimplemented persistence itself rather than use them, which is
evidence the current seam is chess-shaped and needs this reframing.

---

## EXCLUDED — with reasons, so they are not re-proposed

| Excluded | Reason |
|---|---|
| **Merging the two flight recorders** | They record **different layers**, not drifted copies. `Modules/Core/FlightRecorder` records *datagrams* (transport-level). `Rps::MatchRecord` records *sim input + hashes* (replayable). Physics wants the second. Merging conflates layers. **Instead:** keep both, name the layer split explicitly, promote RPS's as the sim-layer recorder |
| **Alpha-step material LUT** | It is a **workaround** for immutable renderer materials, hand-rolled at **5 sites in one file**. Promoting cements the wart. **Instead:** fix `Modules/Render` to allow mutable material tints, which retires the workaround |
| **Localization** | Neither game has any; all strings are inline English literals. Zero data points — the exact greenfield speculation review §1 warns against |
| **Continuous / parameterized audio** | The physics game will likely want machinery hum and impact-energy-scaled sound, but designing that API before it exists is a one-sample guess. Wait for game #3 to ask |

---

## Open Questions

- Ratchet cure (Decision 3) — unanswered.
- Netcode coordinator shape: template parameters vs runtime interface.
