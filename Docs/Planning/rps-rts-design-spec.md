# RPS-RTS — Design & Engine Spec (v1)

> **Partially superseded (2026-07-19):** the game is now **RocksPappersScissors**
> (`Games/RocksPappersScissors/`, namespace `Rps`); §7's wire details (sparse watermarks → per-tick
> input-or-empty, 1-byte event codec, chunked resync), the §2/§3 ~64-units/side cap (raised; spatial
> grid pulled into Phase 1), and the §6 entity layout (AoS struct → SoA arrays + snapshot handover)
> are replaced by **`rps-rts-netcode-and-unit-system.md`**. Game design (tick phases, behavior,
> economy, win rule) still stands. Issues #75/#76 are the actionable source of truth.

*Working title: **Sten Sax Skog**. Two phones, BLE, LurMotorn. Purpose-built as game #2: it must stress what chess never touched (ticks, simultaneous input, continuous space, RNG, entity arrays) while staying inside the engine's thesis — derive everything from a tiny input stream.*

*Locked by your answers: open 2D field · lumberjacks physically walk to trees and are raidable · win by wiping all enemy units. Standing assumptions (veto anytime): input is only the four buttons; whole battlefield on one screen, view flipped per player; RPS = damage multipliers, not instakills; ~64 units/side cap; nearest-enemy targeting as the first playtest knob.*

---

## 1. The game in one paragraph

Each player owns a camp on opposite ends of a shared field. Four buttons queue units: **Lumberjacks** (gather wood), **Rock throwers** (ranged, beat Scissors), **Paper wrappers** (tanky melee, beat Rock), **Scissor cutters** (fast melee, beat Paper). Units act autonomously — workers walk to trees, chop, carry wood home; soldiers seek and fight the nearest enemy. You win by killing *everything* they have. The skill is reading the opponent's composition and out-producing it while keeping your economy alive — and because workers are physical and raidable, the map itself is the second battlefield.

## 2. World & map

- World units are abstract; field is **60 × 34** (16:9-ish). All sim coordinates are `Lur::Sim::Fixed` (Q16.16).
- **Camps** at (6, 17) and (54, 17). A camp is a *location* (spawn point + wood drop-off), **not an entity** — it cannot be attacked or destroyed. (Win is annihilation; an attackable base would need its own HP/win rules — out of v1.)
- **Trees** are infinite wood stations (you chose raidable, not depleting): per side, a **safe grove** of 4 trees near the camp and a **contested grove** of 4 trees near mid-field. Contested trees are *closer to mid* → shorter walk → higher income → higher risk. That one placement decision creates the whole eco-risk dial.
- Map layout is generated from a **derived seed** — `Hash(LowGuid ^ HighGuid ^ MatchIndex)`, computed identically on both phones like chess derives colors: zero bytes on the wire. v1 layout is actually fixed + mirrored; the seed exists so later variation is free.
- Trees do **not** block movement; there is **no pathfinding** in v1 (deliberate — open field + separation steering is enough; pathfinding is a project, not a feature).

## 3. Units

Stats live in one `constexpr` tunables table — every number below is a placeholder to be beaten into shape on the desktop build.

| Unit | Cost | Build | HP | Speed | Attack | Range | Cooldown | Beats (3×) |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| Lumberjack | 30 | 3.0 s | 40 | 6/s | 2 | 1.2 | 0.8 s | — |
| Rock thrower | 50 | 5.0 s | 60 | 5/s | 8 | 6.0 | 1.0 s | Scissors |
| Paper wrapper | 50 | 5.0 s | 90 | 4.5/s | 9 | 2.0 | 1.0 s | Rock |
| Scissor cutter | 50 | 5.0 s | 45 | 8/s | 7 | 1.2 | 0.6 s | Paper |

- **Counter rule:** attacker deals **3×** damage to the type it beats; everything else 1×. Lumberjacks deal their (tiny) damage at 1× to all — they have axes, and a mob of desperate workers should be *slightly* scary.
- The triangle has movement texture, not just multipliers: Rock is ranged/slow, Scissors is fast/fragile, Paper is tanky/short-reach. In an open field that makes counters positional (Scissors can run down Rock; Paper walls for Rock; Rock kites Paper) — composition *and* geometry.
- **Economy:** a Lumberjack walks to the nearest tree with a free slot (max 2 workers per tree), chops **15 wood per 1.5 s** into its carry (capacity 15), walks home, deposits. Income therefore scales with tree distance — the contested grove pays better purely by geography.
- Workers **do not flee** in v1 — they keep working while being murdered (brutal, simple, deterministic). A flee-toward-camp-when-enemy-within-R behavior is the first v1.1 candidate, behind a tunable flag.

## 4. Production

- Each player has **one queue, depth 4**. A button press appends `{type}` if `wood ≥ cost` and the queue has room; otherwise the press is **ignored** (deterministically — no partial reservations). Wood is deducted on enqueue.
- The head of the queue builds down its timer; on completion the unit spawns on a small ring around the camp, position chosen by `SpawnCounter % RingSlots` (deterministic placement, no RNG).
- Start state: **3 Lumberjacks + 60 wood** each. (Enough for one more worker immediately or a soldier soon — the first decision is the game's thesis in miniature.)

## 5. Autonomous behavior (the deterministic AI)

Simple rules, every tie broken by **lowest entity id**:

- **Soldiers:** if no target or target dead → acquire **nearest enemy unit** (squared-distance compare, id tie-break); keep the target until it dies (no re-evaluation — hysteresis for free, no dithering). Move straight toward it; attack when within range and cooldown ready.
- **Lumberjacks:** state machine `ToTree → Chop → ToCamp → deposit → ToTree`. Tree choice: nearest tree with a free worker slot, id tie-break. They ignore combat entirely (v1).
- **Movement:** straight-line seek plus **separation** — same-team units within radius r push apart along the offset vector. No flocking, no avoidance, no paths.
- **Normalization without sqrt:** v1 movement uses **Chebyshev normalization** — `step = speed · (dx, dy) / max(|dx|, |dy|)` — pure Fixed mul/div, fully deterministic, diagonal moves are ~1.4× faster and *nobody will notice at this scale*. If it ever reads wrong, the fix is a deterministic integer `Sqrt` in `Modules/Sim` (Newton on the raw int — cheap, exact). Either way: **no float ever touches the sim**.

## 6. The tick (order is law)

Sim runs at **10 Hz** (100 ms ticks); rendering interpolates between the previous and current sim positions with a float alpha — floats are quarantined to the view, exactly chess's rule. Per tick, phases execute in this fixed order, iterating entity slots 0→N:

1. **Apply inputs** scheduled for this tick (player 0 then player 1 — their inputs only touch their own queue/wood, so cross-order is moot, but define it anyway).
2. **Production** timers; completed units spawn.
3. **Target acquisition** for targetless units.
4. **Movement + separation.**
5. **Attacks:** all damage for the tick is accumulated into a buffer, then **applied simultaneously** — so two units can kill each other in the same tick, which the win rule below needs.
6. **Deaths:** entities are fixed slots with an `Alive` flag — **no compaction, ever** (stable ids, stable iteration order, zero handle machinery; 160 slots is nothing).
7. **Economy:** deposits credit wood.
8. **Win check** (below). 9. **State hash** every 10th tick.

**Entity storage:** one POD array, `Entity Slots[160]` (2 × 64 units + margin + trees-as-entities if convenient), trivially `memcpy`-able — review #2's §3.3 made mandatory. `StateHash` = one pass over the live bytes.

**Win/lose rule (deterministic, edge-proof):** at phase 8, a player **loses** if `AliveUnits == 0 && QueueLen == 0 && Wood < CheapestCost`. Zero units with wood ≥ 30 is *not* a loss — you can rebuy (buttons are player input, not unit-issued). Both players meeting the rule on the same tick = **draw** (simultaneous damage makes this genuinely reachable). Optional safety: hard cap at 15 min → draw, mostly to bound flight-recorder files.

## 7. Lockstep over BLE (the engine's real test)

**The core realization: BLE GATT is reliable and ordered**, so this is lockstep *without* UDP's hard parts — no input redundancy, no resend logic, no packet-loss recovery. The only failure mode is link death, which the existing `Session` reset/reconnect/adopt flow already owns.

- **Input delay:** a local button press at tick T executes at **T + 3** (300 ms) *on both sides*. For production commands this is imperceptible; it's the whole reason this design needs no rollback.
- **Wire format** — an `Input` message (framed, via the de-chessed generic path from the review issues; presses don't fit the 1-byte bare fast path and shouldn't try):
  - `[varint TickDelta][u8: low 4 bits = button mask, high 4 = flags(0)]` → **2–3 bytes per press**, a handful per minute.
  - **Watermark:** the same message with mask 0 means "I have no inputs through tick T" — sent every ~200 ms when idle. The peer may simulate tick T only when it holds inputs-or-watermark ≥ T. This makes lockstep **self-clocking**: no wall-clock agreement needed, so `ClockSync` *stays a stub for v1* (I was wrong earlier that it becomes mandatory — it's only needed later for latency display/tighter pacing). Watermarks can eventually double as keepalives; v1 keeps both.
  - If the peer's watermark hasn't arrived when the sim reaches T: **stall the sim**, keep rendering (interpolation freezes gracefully), surface via the existing `LinkStatusBar`.
- **Resync on reconnect = chess's pattern, scaled:** send the tick-stamped input event list (a 5-minute game is a few hundred bytes) and **fast-forward replay** — `RebuildBoard` generalized from "per move" to "per tick." 128 entities × 3,000 ticks replays in single-digit milliseconds. Budget it, test it, done.
- **Desync detection:** piggyback `[tick, u32 truncated StateHash]` every 10 ticks. With reliable transport + deterministic sim a mismatch is always *a bug* — on mismatch: log both hashes, dump the flight-recorder file, declare a draw. This is a bug-finding instrument, not a recovery system.
- **Match seed & identity:** derived from GUIDs + match index (§2) — the match starts with zero setup bytes, very LurMotorn.

## 8. What this forces the engine to grow (the point of the exercise)

| Engine gap | Forced by | Ties to review item |
|---|---|---|
| `TickClock` used in anger; stall-tolerant fixed-tick loop | §6, §7 | Sim module graduates from seed to substrate |
| POD entity slots + `StateHash` | §6 | Review #2 §3.3 / #7.5 — now mandatory, not advice |
| Deterministic integer `Sqrt` *or* blessed Chebyshev movement in `Modules/Sim` | §5 | First real growth of Fixed math |
| Seeded deterministic RNG utility | §2 | New ~30-line Sim piece |
| Generic tick-stamped input message (not chess's 1-byte bare path) | §7 | Validates de-chessing `Session::SendMove` (review #1 §3.3) |
| Hud **Button** widget + wood counter + queue display; `TouchEvent` finally instantiated | §4 | Review #1 §3.5 input finding |
| ~150 dynamic quads + health bars; sprite flip/rotation in `Sprite2D` | §3 | First render-path pressure; batching stays *unneeded* — verify, don't assume |
| Team-tint materials for 4 unit types | §3 | Direct reuse of chess's PieceLight/Dark tint trick |
| Desktop 2-window build + flight recorder + sim fuzz | balancing | Review #2 §4 — tuning RPS numbers on two phones is torture; on desktop it's an afternoon |
| Generalized per-opponent match record (W/L/D) | meta | `ChessRecord`'s player-agnostic merge pattern, extracted |

**What it deliberately does NOT force:** pathfinding, rollback, fog of war, unit selection/orders, attackable buildings, CoC bandwidth (presses are bytes — GATT is plenty). Every one of those is a real project; none is needed to answer the engine questions.

## 9. Chess ↔ RTS overlap (this table *is* the future IGame)

| Axis | Chess | RTS | Generalization |
|---|---|---|---|
| State authority | Derived from move list | Derived from input stream + seed | `State = Replay(Inputs, Seed)` — the engine's one law |
| Time | Turn-alternating | Fixed ticks, simultaneous | `ApplyInput(tick, player, bytes)` + `Step()` |
| Input | Index into legal-move list | Button mask @ tick | Game-owned codec over the same bit tools |
| Resync | Send move history, rebuild | Send input history, fast-forward | Identical mechanism, one knob: replay budget |
| RNG | None | Seeded, derived | Seed derivation joins GUID/parity in the handshake-free zone |
| Persistence | ChessRecord + MergeIfNewer | Same shape (W/L/D per opponent) | Extract the record pattern |
| View | Board flip, tint materials | Field flip, tint materials | Same tricks, literally |

Two real games, one law, five joints. *That* is the IGame contract — discovered, as review #2 insisted, not guessed.

## 10. Build order (vertical slices)

1. **Slice 0 — sim on desktop, no net:** entity array, tick phases, one window, keyboard buttons, on-screen wood/queue. Playable vs. yourself. *(This is where the desktop build from review #2 gets built — it's not optional for this game.)*
2. **Slice 1 — two windows, loopback lockstep:** input delay, watermarks, stall, desync hash, flight recorder. The whole netcode proven without a phone in the room.
3. **Slice 2 — phones over BLE:** swap loopback for the real transport (this is the moment the Transport abstraction earns its keep). Soak mode: two phones auto-playing random button presses overnight.
4. **Slice 3 — balance & feel:** tunables table warfare on desktop at 4× speed with replays; worker-flee flag; juice (hit flashes, death poofs) within the byte budget.

## 11. Open knobs (playtest, don't debate)

Counter multiplier (3× vs 2.5×) · worker flee on/off + radius · target hysteresis (until-death vs re-eval every K ticks) · queue depth (4 vs 2 — smaller = more decisive commitment) · contested-grove distance (the entire risk economy in one number) · input delay 2–4 ticks · sudden-death timer.
