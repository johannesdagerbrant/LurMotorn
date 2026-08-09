# Wire versioning, input pipeline, dead-code sweep

## Key Points

- **Split the wire version**: engine `ProtocolVersion` + per-game `GameProtocolVersion`.
- **Engine owns the full input pipeline**, gesture recognizers opt-in; multi-touch lands with it.
- **Delete ALL dead code** — including anything that merely *looks* useful.

---

## Wire versioning — split

```cpp
// Modules/Net (engine-owned)
constexpr uint8_t ProtocolVersion = 3;  // reset: framing/handshake/keepalive/resync only

// Games/Chess/ChessWire.h
constexpr uint8_t GameProtocolVersion = 1;

// Games/RocksPapersScissors/RpsWire.h
constexpr uint8_t GameProtocolVersion = 4;  // buildings, camp, seq byte, rollback

// Hello frame: [engineVer][gameVer][guid...]
// mismatch on either => refuse, with WHICH one named in the log
```

**Fixes:** today `ProtocolVersion = 9` and **v6/v7/v8/v9 are all RPS gameplay-wire changes**, so a
chess-only wire change cannot reuse a number and game #3 would bump it for everyone again.
After the split each game bumps independently and the engine's changelog stops accumulating one
game's gameplay history.

**Distinct from** RPS's build-fingerprint gate (which catches *build* mismatch, not wire
incompatibility) — that is separately promoted to the engine as a Tier 1 item.

**Note:** the engine version RESETS to a low number, since most of the current 9 bumps were game
concerns. The reset must be called out loudly — a peer on an old build must not read the new low
number as "older". The `[engineVer][gameVer]` pair in Hello plus a hard refuse handles this, but
the first release after the split is a hard break for every existing install.

---

## Input pipeline — engine owns it, recognizers opt-in

Engine normalizes native events into a **multi-touch** `TouchEvent` on all three platforms, **and**
provides gesture recognizers (tap, drag, pan, pinch, long-press) as a **library the game composes**.

The division: *"this was a drag, not a tap"* is generic; *"what the drag means"* stays in the game.

**Evidence this generalizes:** `Modules/Input/ConsoleGesture` is already an engine module unifying
the two-finger triple-tap across all three platforms, and it exists precisely because that logic had
drifted across three copies.

**What it fixes:**
- `Lur::Input::TouchEvent` is a 5-field struct **bypassed entirely on both phone platforms** — each
  main translates native events inline, which is why the pipeline is invisible to the engine.
- RPS's input-dispatch skeleton is copy-pasted **three times inside RPS alone**
  (`RouteLocalEvent` Android, `placeLocal:` iOS, desktop path).
- **Multi-touch has never been implemented.** The physics game is a **two-hand direct-manipulation
  game**, so this is a hard requirement for game #3, not a nicety.

---

## Dead-code sweep — DELETE ALL

Approved for deletion:
- **`Modules/Pairing`** (39 LOC) — zero consumers repo-wide; `IPairing` referenced only by its own
  header; superseded by `Transport/BleProtocol.h`'s `DecideBleRole` + per-platform transports.
  Still in the top-level `add_subdirectory`. (#47)
- **`Hud/LinkStatusBar`** (60 LOC) — only self-references. Deleting also helps unpick the
  **`Hud → Net`** dependency (presentation coupled to the net session type).
- **`Text/FontRegistry`** — only consumer is its own test; both games hold a bare `Font`.
- **`Math`: `Quat`, `Mat4::Perspective`, `Mat4::LookAt`** — the entire 3D half of `Modules/Math` is
  unexercised. Deleted **despite** the renderer being "3D-capable by design" and a glTF loader being
  a future issue (#9).
- **`Net/ClockSync`** stub — 30 lines returning hardcoded false/0; `EMsgType::ClockPing/ClockPong`
  reserved but never sent; "lockstep is self-clocking" is already on record. **Frees two wire slots.**

### The governing rule (user, verbatim)

> *"Get rid of all dead code. If anything looks useful get rid of it anyway. If some game needs it
> later we build it for that game first. If more games need it we consider if it deserves to get
> promoted to an engine feature."*

This is coherent with rejecting the engine-first ratchet cure: **build in the game, promote on the
second consumer, delete anything speculative.**

**Consequence:** the rule deliberately creates game-first facilities, which makes the **promotion
pass** load-bearing — it is the only mechanism that moves a second-consumer facility back into the
engine. Without it the rule degrades into the one-way ratchet that stranded RPS's dev console.

### Also sweep (not in the original four)
- `Core/Hash.h`'s `Fnv1a64` — dead *in production*, only tests. But RPS **hand-rolls its own
  `StateHash`**. Resolution: either the promoted hashing path uses `Fnv1a64`, or it goes.
  Decide during the netcode lift; do not leave it dead.
- Audit for others during the extraction — the rule is a sweep, not a list.
