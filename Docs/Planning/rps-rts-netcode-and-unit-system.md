# RPS-RTS v2 — Netcode & Unit System (as decided)

*2026-07-19, from the Phase-1 kickoff design sessions. This records the **rationale** behind the
decisions now baked into issues #75/#76; where this doc and an issue disagree, **the issue wins**
(CLAUDE.md doctrine). It supersedes the v1 spec (`rps-rts-design-spec.md`) in three places: the §7
wire details, the §2/§3 ~64-units/side assumption, and the entity-storage layout. Everything else
in the v1 spec (game design, tick phases, behavior rules, win rule) still stands.*

*The game lives at `Games/RocksPapersScissors/` (namespace `Rps`); the working title
"Sten Sax Skog" is retired.*

---

## 1. Authority: lockstep is input-stream authority

The engine doctrine says authority follows the interaction whose *feel* must be protected —
instant, never round-tripped. In this game the player never directly manipulates a unit; the only
human interaction is *press button → append to a multi-second production queue*. There is no
aim-feel or steer-feel to protect, so the only feel that matters — button acknowledgement — is a
**view concern**: the button flashes and the queue slot renders "pending" the instant you press,
while the sim applies the input at T+delay.

So lockstep here does not violate the doctrine; it *is* the doctrine applied to this game: each
peer has absolute, un-round-tripped authority over **its own input stream**, and the shared state
is a pure deterministic function of both streams. No host, no referee. The moment a future game
gives the player direct manipulation, this answer flips to rollback (lockstep + prediction) — which
is exactly the substrate this game builds: rollback is lockstep minus the delay plus
snapshot/restore, and the POD unit arrays make snapshots a memcpy.

The engine's one law, now with two witnesses: `State = Replay(Inputs, Seed)`. Live play is replay
at the frontier; hitch recovery is replay from the inbox; resync is replay from chunks; the flight
recorder is replay from a file; the determinism test is replay from a script. One mechanism, five
costumes.

## 2. What rides the wire (and nothing else)

Positions, HP, deaths, wood, the map, colors, the seed: **never sent** — all derived. Wire cost is
**O(0) in unit count**, which is what frees §5 to raise the cap. Five message kinds total:

| Message | Encoding | Wire size (incl. type byte) | Cadence |
|---|---|---|---|
| Input (press **or** empty) | `[delta:4 \| mask:4]`, delta 15 → varint escape | 2 B | **every sim tick** (10/s) |
| Anchor + hash | `[varint absolute tick][u32 StateHash]` | ~7 B | every 10 ticks (1 s) |
| Resync chunk | `[varint start tick][events…]` | ≤512 B | on reconnect |
| Engine handshake/keepalive | existing `Session` machinery | — | existing |

- **Per-tick input-or-empty** (decided over the v1 spec's sparse ~200 ms idle watermarks): ~30 B/s,
  well inside the one-outstanding-write GATT flow control and the ~15–30 ms connection-interval
  cadence. The protocol loses its idle special case entirely; the peer's knowledge of your stream
  is always exactly one latency behind; a stall becomes a pure link-health signal. An empty mask
  (`0000`) *is* the watermark.
- **The tick is implicit on the live wire** — the Nth input message after the baseline is tick N.
  Safe **only** because GATT is reliable and ordered; the Anchor message re-anchors with an
  absolute tick every second, converting any counting bug from silent divergence into a loud alarm.
  The codec keeps the delta nibble anyway so the *same* encoding serves the sparse cases (resync
  history, flight recorder), where deltas genuinely vary.
- **One codec, three uses**: live wire, resync history, recorder/replay files are the same byte
  format. One fuzz target, one byte-budget CI test, one decoder.
- **Mask, not enum**: 4 bits, all 16 combos legal (simultaneity like lumber+paper is one byte, and
  same-button presses within one tick collapse into one bit — a built-in rate limit of one event
  per button per tick).
- **Guards** (the "safely" in tightly-packed): #48's varint shift guard (the roadmap says it exists
  precisely for this format), `LUR_ASSERT` on non-monotonic ticks, fuzz at `Session::OnDatagram`,
  and chess-style byte-budget tests so a size regression fails the build.
- **Desync hash semantics**: mismatch = always a bug (reliable transport + deterministic sim).
  Log both hashes, dump the flight-recorder file, declare a draw. A bug-finding instrument, not a
  recovery system. (Chess's keepalive-hash self-heal in `Session` stays chess-facing; the RTS hash
  is tick-aligned and game-level — don't double-wire.)

## 3. Time is denominated in ticks — wallclock never enters

- No duration, timestamp, or rate inside the sim is ever expressed in seconds — only ticks
  (a Rock thrower takes 50 ticks, not 5.0 s). Seconds exist only outside, where `TickClock`
  converts elapsed real time into "how many ticks to grant."
- **The presser assigns the execution tick** (local tick + delay) and that tick number is what the
  message means. Arrival time is irrelevant. This is the single quantization point where the one
  external nondeterminism — human press timing — enters the sim identically on both phones.
- **Input delay**: default T+3 (300 ms), a tunable 2–4. It exists only to cover transport latency;
  after #69's fast pump, T+2 may hold stall-free — measure, don't debate.
- **The ceiling law**: a peer executing tick T can promise (watermark) through T + delay − 1, so
  the other may never simulate past that. The *fast* phone stalls; nobody sprints to catch a peer.
  Skew stays bounded ≈ the delay window forever; a press can provably never arrive in the peer's
  past. Clock drift is absorbed by the same gate — the match runs at the slower crystal's pace.
- **The sprint law**: a phone runs ticks as fast as silicon allows whenever it already *holds* the
  inputs for them (hitch catch-up: to its own clock; resync: to the frontier). It stalls only at
  the live frontier, where the inputs haven't been created yet.
- **Never discard lockstep ticks**: `TickClock::MaxCatchup`'s drop-the-backlog clamp is for local
  render pacing only. The lockstep sim must eventually execute *every* tick, spread over frames if
  needed — discarding on one phone only is a desync machine.
- **`ClockSync` stays a stub** — lockstep is self-clocking (sims synchronize on data, not time).
  GGPO-style pacing nudges (stretch the tick a hair instead of micro-stalling at the ceiling) are
  a *feel* knob, parked: with ppm crystal drift and a 300 ms window, v1 won't measure a stall.

## 4. Resync: one mechanism for two situations

- **Link blip (the common case)**: both sims intact and — because no watermarks flowed — both
  stalled within a delay-window of each other. Nothing diverged. Resync = fill the in-flight gap:
  a handful of events, single-digit bytes.
- **Cold rejoin (app killed)**: the survivor is frozen at the frontier (stall, not divergence — the
  match is perfectly resumable). The rejoiner needs the full history.
- **Both are the same code path**: the history streams as ≤512-byte framed chunks (GATT ordering
  makes reassembly an append loop); a blip's "history" simply fits in one small chunk. No message
  ever exceeds `MaxFramedPayload`, so chess's #74 marathon-wedge is designed out of game #2.
- Worst case is bounded by the mask-per-tick collapse: ≤1 event/tick → a 15-min game is ≤9 KB ≈
  18 chunks, radio-bound (~1–2 s of chunk delivery), sim-bound in milliseconds. The rejoiner
  free-runs through each chunk (sprint law) while the screen shows "resyncing…", then snaps.
- **Snapshot resync is parked** with a wake condition: it also needs chunking (the SoA block
  exceeds 512 B), costs a second versioned serialization format, and abandons one-codec-three-uses.
  It wakes when measured cold-rejoin replay time at the raised unit cap exceeds ~2 s (measure in
  slice 1's stress soak).

## 5. Unit system: SoA, and scale as a feature

**Decision: raise the engine target now** — hundreds-to-thousands of units per side, not the v1
spec's ~64. The wire is O(0) in unit count, so the walls are all local, and they fall in this
order: O(n²) sim queries → draw calls → O(n) hash/snapshot/replay → fillrate/thermals. Layout and
algorithm buy the 100×; vectorization is the last 4–8×.

- **SoA parallel POD arrays** (`PosX[] PosY[] PrevX[] PrevY[] Hp[] Type[] Team[] Target[]` +
  `AliveBits[]` as a bitmask): still trivially memcpy-able, still one-pass hashable — and the
  struct-padding hash hazard (indeterminate padding bytes across compilers) **vanishes by
  construction**: arrays of naked fixed-width ints have no padding to lie about. `StateHash` is
  pinned as FNV-1a over the arrays in declaration order. No compaction, ever; stable slots, stable
  iteration, id tie-breaks — unchanged from the v1 spec, now in SoA form. Win check = popcount.
- **SIMD via auto-vectorization only.** Integer fixed-point math is exactly associative and
  commutative, so NDK Clang / Apple Clang / host g++ may vectorize scalar `Fixed` loops however
  they like — NEON, SSE, any width — and the bits are identical. The sim's two reductions
  (nearest-enemy = min over exact `(dist², id)`; separation = sum of integer push vectors) are
  order-independent by construction. Floats in the sim remain forbidden (SIMD or not). Hand
  intrinsics are a profiled-need escalation, not v1. Q16.16 multiplies keep `int64` intermediates.
- **Deterministic uniform spatial grid, built now** (the raised cap makes the n² scans a real
  wall): counting-sort rebuild each tick into fixed arrays in slot order (no allocation), fixed
  bin iteration order, id tie-breaks. Cell size ~ the largest interaction radius, a tunable.
- **Stress scene** (`LUR_INTERNAL`): spawn max-slot armies and prove the tick budget and the
  render path at cap, on desktop first, phones in slice 2. The *gameplay* numbers (economy,
  build times, field size — a 60×34 field with thousands of units is soup) start at v1-spec values
  and get pushed toward the raised cap during slice 3's tunables warfare; the engine must simply
  never be the reason the number stays small.

## 6. The snapshot: one object, three problems dead

The tick thread publishes a **double-buffered SoA snapshot** (`Prev/Pos/Type/Team/Hp/AliveBits`)
once per sim tick; the render thread consumes it at frame rate. That one object is:

1. **The thread seam** (#69): sim + transport pump run on a dedicated high-rate thread, decoupled
   from render/vsync — an inbound datagram is serviced in ~1 ms instead of a frame. The snapshot
   swap is the *only* data crossing the threads (the symmetric counterpart of #40's EventInbox,
   which solved radio→engine; this solves engine→render).
2. **The float quarantine, now a thread boundary**: the sim publishes integers; interpolation
   happens in the *vertex shader* (`mix(prev, curr, alpha)`, alpha as a push constant) — floats
   quarantined not just to the view but to the GPU. CPU interpolation cost per unit: zero.
3. **The CPU→GPU handover**: the snapshot memcpys into a persistently-mapped Vulkan buffer and
   draws as **one instanced draw** (health bars = a second). Per-instance `Type`/`Team` select
   atlas UV and tint in the shader. All inside the Vulkan portability subset — MoltenVK-safe.
   Bandwidth at thousands of units: ~KB-to-MB/s memcpy; nothing.

Wire *sends* stay at tick/press cadence — the ≥1 kHz pump services the link; it must never emit at
pump rate (one-outstanding-write GATT flow control).

## 7. Parked, with wake conditions

- **Snapshot resync** — wake: measured cold-rejoin replay > ~2 s at the shipping unit cap (§4).
- **Hand-written SIMD intrinsics** — wake: a profiled tick phase misses budget after the grid.
- **GGPO-style pacing nudge** — wake: visible micro-stalls at the ceiling in real matches.
- **Pathfinding** — still never for this game (open field + separation, v1-spec position holds).
- **`ClockSync`** — wake: a feature needing latency display or tighter pacing.

## 8. Knobs deliberately left to slice 3 (playtest, don't debate)

Shipping unit cap (the engine ceiling is not the gameplay answer) · economy/build-time rebalance to
reach it · field size vs density · counter multiplier · input delay 2–4 · grid cell size · worker
flee · sudden-death timer.
