# RPS — Buildings Rework Tech Spec (v1)

*2026-07-21. A mechanical pivot: production moves from "press a button → unit trickles from your camp" to a **spatial base-builder** — you drag buildings onto the map, each building produces its unit type on x1/x5/x20 queue commands, buildings can be destroyed, and placement is spatially constrained. Grounded in the code at HEAD `f11e228`: `Sim` SoA (`Sim.h`), `UnitTable`/`EUnit`/`CvSnapshot`/`LUR_RPS_GAMEPLAY_CVARS` (`Tunables.h`), `Sim::Step` phases + `SpawnUnit` + economy deposit (`Sim.cpp`).*

*Locked decisions (Q&A 2026-07-21):* buildings are **entities in the same SoA** (a type/flag on a slot); **instant on drop** (placed = built, if affordable); **camps are gone** — miner buildings are the *only* gold drop-off; buildings **block movement via a repulsion force** (no pathfinding); **start with only gold**, first building is a **forced mining camp**; **the match starts when both players have placed their mining camp** (placement = "ready"); production **queue cap CVar, default 40**; **stack acceleration removed**; targeting treats an enemy building **as the unit type it produces**; a **looping tutorial hand** demonstrates the first placement every match.

---

## 1. What changes, in one list

**Added:** buildings as placeable, producing, destroyable entities; drag-to-place input; per-building x1/x5/x20 queue commands; building repulsion; placement validity (no overlap with buildings or mines); a pre-match "place your mining camp" phase that doubles as the ready gate; building CVars (health, cost) per unit category; the tutorial hand.

**Removed:** the camp as a magic spawn point and gold sink (`CampX`/`CampY` as production/economy anchors); the per-team `SpawnCounter` ring; **stack acceleration** (`BuildProgress += QueueCount`); the four per-team parallel queues living on `TeamState` (queues move onto buildings).

**Unchanged (leaned on):** the boid movement/flocking system; the counter triangle + `CounterMultiplier`; the mine reserves model (`MineGold[]`); the deterministic tick/`StateHash`/POD-`memcpy` discipline; the CVar latch (`CvSnapshot`); lockstep sync; units auto-target and self-drive.

---

## 2. Buildings in the sim state (SoA, same arrays as units)

Buildings are **entities in the existing per-unit SoA** — a slot with a "this is a building" marker — so they inherit `Hp`, `Team`, position, the `AliveBits` liveness, `StateHash`, and rollback-memcpy for free, with **no new parallel arrays** (keeps `Sim` trivially copyable, the `static_assert` intact).

Concretely, add to the per-unit SoA (pinned declaration order → append at the end so existing `StateHash` layout for units is undisturbed, then the building fields extend it):

- `uint8_t Kind[MaxUnits]` — `EKind { KindUnit = 0, KindBuilding = 1 }`. (Alternative: fold into `Type` with a high bit, but a separate `Kind` byte is clearer and the SoA has no padding cost.)
- Buildings reuse existing fields with building meaning:
  - `Type[i]` = which unit type this building produces (`UnitMiner`/`Rock`/`Paper`/`Scissor`) — this is also what targeting treats it as (§7).
  - `Hp[i]` = building health (from the building CVars, §8).
  - `PosX/PosY[i]` = placement center; `PrevX/PrevY[i] = Pos` (buildings never move → Δ=0, so they're inert in the momentum/interp math and need no special-case there).
  - `Team[i]`, liveness via `AliveBits` (a destroyed building clears its bit exactly like a dead unit).
- **Miner buildings are the economy drop-off:** a worker carrying gold seeks and deposits at the **nearest own miner building** (fixed-point nearest, slot-index tie-break); replaces the old single-camp deposit. If a team's last miner building dies while carts carry gold, that gold is **stranded** (carts hold it) until a new miner building exists (§12.4).
- **Per-building production queue** (buildings produce; units don't) — the four `TeamState.QueueCount/BuildProgress` arrays are replaced by **per-building** counters:
  - `int32_t Queue[MaxUnits]` — units queued at this building (0 for unit slots). Capped at `CvBuildingQueueMax` (default **40**, §8).
  - `int32_t BuildProgress[MaxUnits]` — this building's current unit's construction progress, ticks.
  - Buildings produce **one type** (their `Type`), so one queue counter per building, not four.

`TeamState` slims to just `Gold` (+ `SpawnCounter` removed). Queues are now spatial (per building), which is the whole point. Two per-team **frontier** `Fixed` values (`FrontierT0`, `FrontierT1`, §5.3) are added to `Sim` (hashed, monotonic) to gate forward placement.

**Budget note:** these are `MaxUnits`-sized arrays (`MaxUnits = 4096`); adding `Kind` (4 KB), `Queue` (16 KB), `BuildProgress`-per-slot (16 KB) is negligible and keeps everything in one memcpy-able block. Buildings and units share the `[0, Count)` slot space and the same allocation path.

---

## 3. Production (no stack acceleration)

A building with `Type = ty` builds units of `ty`:

- Each tick, for each **alive building** with `Queue > 0`: `BuildProgress += 1` (flat — **not** `+= Queue`; stack acceleration is removed per decision). When `BuildProgress >= UnitTable[ty].BuildTicks`: spawn one unit of `ty` at the building, `--Queue`, `BuildProgress -= BuildTicks`. Empty queue zeroes `BuildProgress` (no banked progress, as today).
- **Deterministic iteration:** buildings processed in slot order `[0, Count)`, teams implicitly interleaved by slot — same determinism guarantee as the current spawn loop (slot order is identical on both peers).
- **Spawn position:** units spawn adjacent to the producing building (a small deterministic ring around the building center, reusing the old `RingX/RingY` offsets relative to `PosX/PosY[building]` instead of the camp). The building's own slot index (or a per-building spawn counter packed into an existing int field) drives the ring slot deterministically — no global `SpawnCounter`.
- Removing acceleration changes balance meaningfully (a big queue no longer builds faster) — this is a deliberate lever handoff: **throughput now scales by building count, not queue depth.** More buildings = more parallel production. That is the new economic decision (spend gold on more buildings vs. more units), replacing the old stack-snowball. Flag for the balance pass (§10).

---

## 4. Input: drag-to-place + x1/x5/x20

Two input modes now, both dev-console-independent (this is game input, not dev input):

### 4.1 Placement (drag a building from a button onto the map)
- The four buttons are now **building types** (miner/rock/paper/scissor buildings). The player **drags** from a button; a ghost/preview building follows the pointer; releasing on a valid tile places it.
- **Placement is an input event, tick-stamped**, carried like a unit command over lockstep: `[applyTick][team][buildingType][tileX][tileY]`. Both peers apply it at the stamped tick → identical placement. (This extends the input model beyond the old 4-bit press mask — see §6; placements are richer than a mask bit, so they ride the framed input path, not the bare mask.)
- **Cost:** deducted on placement (from `Gold`); if unaffordable or invalid, the placement is rejected deterministically (both peers reject identically — the validity check is pure sim state).
- **Team colors are cyan and yellow** (team 0 / team 1) — **red is reserved for the invalid-placement signal** and is no longer a team color. (Palette change with reach beyond this spec — see §4.3.)
- **Invalid-placement feedback (view-only):** while dragging, the ghost building **blinks red** whenever the current pointer position is an **invalid drop** — i.e. it overlaps another building, overlaps a live mine, or is **past the build line** (beyond the team's frontier, §5.3). The ghost shows normal (team-tinted) when valid, blinking red when not. This mirrors the exact §5.1 validity predicate, evaluated live against sim-readable state each frame (view reads sim; writes nothing).
- **Drop while red → slide back:** releasing on an invalid tile does **not** place; the ghost **animates sliding back to its source build button** (a view-only tween), gold unspent, no input event emitted. A valid drop places normally (emits `EvtPlaceBuilding`). Because the sim's placement validity is authoritative and identical on both peers, the view's red/valid state is just a live preview of that same predicate — no desync risk (the worst case is the view briefly disagreeing with itself, never the two peers disagreeing).

### 4.3 Team palette (cyan / yellow) — cross-cutting note
Team colors change from the old scheme to **cyan (team 0) and yellow (team 1)**; **red is now exclusively the invalid/error signal** (placement blink, and any future "can't do that" feedback). This touches everything currently keyed to team color: **unit tints** (the per-type silhouette tints are modulated by team — now cyan/yellow families), the **frontier lines** (§5.3, drawn in team color), the opponent-selector dots, and any HUD element that encodes team. Audit for hardcoded team reds and reroute to the cyan/yellow palette; make the two team colors CVars (category `"Render"`, non-`AffectsGameplay` — they're visual only, so local/live-tunable, not synced). This is a small but repo-wide sweep, not confined to the buildings feature.

### 4.2 Production commands (x1 / x5 / x20)
- **The x1 / x5 / x20 buttons are attached to every building** and always present (no building-selection or highlight concept). The player taps the buttons **on the building they want to produce at**; that tap enqueues that many of *that building's* unit type (clamped to `CvBuildingQueueMax`, default 40).
- Also a tick-stamped input event: `[applyTick][team][buildingSlot][count]` where `buildingSlot` is the building whose buttons were tapped → `Queue[slot] = min(Queue[slot] + count, CvBuildingQueueMax)`, gold permitting (deduct per unit enqueued, up to what's affordable — deterministic partial enqueue if gold runs out mid-batch: enqueue as many as gold covers, in order).
- x20 onto a near-full queue clamps; the clamp is deterministic and identical on both peers.
- **Each button shows its total price.** x1/x5/x20 display the gold cost of that batch: `unitCost × multiplier`. For a 50-gold unit → **x1: 50, x5: 250, x20: 1000**; for the 30-gold miner → 30 / 150 / 600. The number is derived view-side from the building's unit-type cost CVar × the button's multiplier (a plain read of the sim-authoritative cost; the label isn't sim state). It updates automatically if the cost CVar is tuned live.
- **Affordability cue (view-only):** a button whose price exceeds the player's current `Gold` reads as **dimmed/unaffordable** (and its tap is a no-op or a partial enqueue per the gold-permitting rule above). This is presentation only — the authoritative "how many actually enqueue" is the deterministic sim rule; the dim state is just a live read of `Gold` vs. the button's price so the player sees what they can afford at a glance.
- **View:** the price renders on/under each x1/x5/x20 button (small numeral in the button plate), alongside the buttons anchored to each building.

*(Input encoding details — exact byte layout, how placement/production events slot into `LockstepPeer`'s framed messages alongside `MsgCvar` — in §6.)*

---

## 5. Placement rules & building repulsion

### 5.1 Validity (checked identically on both peers — pure sim state)
A placement is valid iff:
- **In-bounds** (within the world rect, with a margin for the building's footprint).
- **Not beyond the team's frontier** (§5.3) — you cannot build closer to the enemy than your own units have advanced.
- **No overlap with another building:** the footprint (a radius/box CVar per building, or one shared building footprint) must not intersect any existing building's footprint.
- **No overlap with a mine:** footprint must not intersect any `MineX/MineY` with `MineGold > 0` (a depleted mine, `MineGold <= 0`, is gone — placement over it is allowed).
- Overlap tests are fixed-point distance/box checks over existing state → deterministic, no RNG. Cheap (buildings are few; mines are 48).
- On invalid drop: no-op, gold unspent, a UI nudge (view-only). Both peers compute "invalid" identically, so no desync even if a player drops on a bad tile.

### 5.2 Blocking via repulsion (no pathfinding)
Buildings **block movement through a strong static separation/repulsion force**, consistent with the existing boid model — **no pathfinding, no navmesh** (honors the engine's "pathfinding is a project, not a feature" line):
- In the movement phase, each unit gets a repulsion push away from any nearby building (buildings within a radius), using the **same inverted-falloff form as unit separation** (`dir × (R − d)/R × strength`), with a building-specific radius/strength CVar. A building is essentially a big immovable separation source.
- Units therefore **flow around** buildings organically (lava-lamp style, matching the current flocking feel) rather than pathing around them. A dense wall of buildings creates a soft channel, not a hard maze — acceptable and on-theme.
- Buildings don't move, so this is one-directional (units pushed, building static). Reuse the grid neighbor query (buildings are entities in the same SoA, so they're already in the spatial grid) — add buildings to the repulsion accumulation in the one gather pass; negligible cost.
- **Determinism:** same fixed-point, order-independent-sum discipline as all boid forces; extend the `grid==brute` equivalence test to cover building repulsion.

### 5.3 Frontier: build only as far as your units have advanced
You can **never place a building closer to the opponent than one of your own units has reached** — base expansion follows military progress, not the reverse.

- **Per-team high-water frontier (decided):** each team has a monotonic frontier — the **furthest-forward position any of its units has *ever* reached** (high-water mark, toward the enemy). It **only advances, never retreats**: if your forward units die, the line you've earned stays. Symmetric per team (team 0's frontier advances upward from the bottom; team 1's downward from the top).
- **State:** two `Fixed` values in `Sim` (`FrontierT0`, `FrontierT1`), **hashed** (they gate a gameplay-affecting rule and must agree on both peers). Updated each tick in the movement/economy phase: `FrontierT0 = max(FrontierT0, max PosY over alive team-0 units)`; team 1 uses `min` (advancing toward smaller Y). Monotonic by construction. Cheap — folded into the existing per-unit iteration.
- **Initial value (screen-independent, deterministic):** the frontier does **not** start at the camp — it starts at a **fixed world-space constant** `CvInitialFrontier` (world units from your baseline), chosen to *correspond to* the initial bottom screen band (§ camera below) **without** being derived from pixels. So at match start you may build within a starting band ~one screen deep, and further only as units push. Because it's a world constant, both peers (and every device/aspect) share the same initial line → no desync. (Tune the constant so it visually matches the locked initial camera; the correspondence is a tuning choice, not a runtime coupling.)
- **Placement check (§5.1):** a team-0 placement is rejected if `tileY > FrontierT0`; team-1 if `tileY < FrontierT1`. Pure sim state, identical on both peers.
- **Interaction with the forced first camp:** the mining camp is placed within the initial band (it's ≤ `CvInitialFrontier`), so the frontier rule and the "first placement" gate coexist — you place the camp in the visible starting area, then expansion unlocks forward as your produced miners/soldiers move out.
- **View — the frontier lines:** **both** frontiers are drawn — your own **and** the opponent's — each a **horizontal dotted line in that team's color** at its frontier Y, so you can read how far each side has pushed (your build boundary and theirs as intel). A **hammer icon** sits over each line at its **left end** as the "build up to here" legend. View-only — reads the sim's `FrontierT0`/`FrontierT1`, writes nothing, not hashed. Rendering: each dotted line is a row of short quads (or a dashed-texture strip) in its team tint; the hammer is a cooked icon sprite pinned to the line's left end, following the line's Y as that team's high-water mark advances. Float/view-side positioning — presentation of a sim value, not sim state.

---

## 6. Lockstep input model (extends the mask)

Today input is a 4-bit press mask per team per tick (`Mask0`/`Mask1` in `Step`). Buildings need richer events (placement coords, building slot, counts), so:

- **Keep the bare mask path for nothing** — the four "buttons" are no longer instantaneous unit-queues; they're drag sources and selection. So the per-tick mask is largely replaced by **framed input events** on `LockstepPeer` (the same channel `MsgCvar` rides), each tick-stamped:
  - `EvtPlaceBuilding { team, type, tileX, tileY }`
  - `EvtQueueUnits { team, buildingSlot, count }`
  - `EvtDestroy`? — no; destruction is a sim consequence of combat, not an input.
- Events are applied at their `applyTick` on both peers in the input-apply phase, in a deterministic order (by team, then arrival order within the tick — same discipline as today). `Step`'s signature grows from `(Mask0, Mask1)` to accept the tick's event list (or keeps masks for any residual instantaneous input and adds an events queue — implementer's call; the sim must consume them deterministically).
- **Single-player / AI:** the AI (separate spec) emits the *same* events — it "drags" buildings and "taps" x1/x5/x20 by producing `EvtPlaceBuilding`/`EvtQueueUnits`, never touching sim state. The building rework doesn't change the AI's "acts only through input" rule; it just enriches the event vocabulary.
- **Flight recorder / replay:** events are inputs, so recording/replay/`--auto` keep working unchanged.

---

## 7. Targeting: an enemy building = the unit type it produces

Decision: **soldier target-priority treats an enemy building as its produced unit type.** This makes attacking the economy an organic extension of the counter triangle rather than a special rule:

- In target acquisition (the `NearestEnemyGrid`/brute scoring), a building candidate is scored **using its `Type`** exactly as if it were a unit of that type:
  - A Scissor soldier sees an enemy **Paper building** as prey (Scissor beats Paper) → prioritizes it like a Paper unit; sees an enemy **Rock building** as a predator → deprioritizes/avoids it like a Rock unit.
  - A **miner building** (produces `UnitMiner`, which `Beats = UnitNone`) is neutral prey — attackable, but not preferred by the counter weighting (matches miners being non-threatening).
- Mechanically: the existing counter-preference term in the target tuple already keys off unit `Type` and `Beats`; buildings entering the candidate set with their `Type` set means **the same scoring code covers them** — minimal new logic, just include `KindBuilding` entities in the enemy-candidate scan.
- **Buildings don't fight back** (no attack, no cooldown — they're production, not combat), but they *are* valid targets and take damage from adjacent enemy soldiers via the normal combat phase (a building in range is attacked like a stationary enemy; `CounterMultiplier` applies by produced type, so a Scissor hits a Paper building for the 3× counter damage — thematically perfect: you counter their economy the same way you counter their army).
- **Destruction:** a building whose `Hp` reaches 0 clears its `AliveBits` bit (same as a unit death), its queue evaporates, and it stops producing/being a drop-off. Deterministic, buffered in the same damage-apply phase as unit deaths.

This also gives the interpose/guard behavior (boids slice C) natural meaning: defenders already peel toward threatened miners; miner *buildings* being high-value drop-offs makes base defense emergent.

---

## 8. Building CVars (in each unit type's category)

Per decision, **buildings get CVars in the same category as their unit type** (so the console groups "Rock" unit + Rock building together). Add per-building-type:

- `building health` (int, `Hp` at placement) — category = the unit's category (e.g. `"Combat"`/per-type as units use).
- `construction cost` (int, gold deducted on placement).
- `CvBuildingQueueMax` (int, default **40**) — the max production queue per building (shared, or per-type if wanted; default one shared CVar).
- Building **footprint radius** and **repulsion radius/strength** (Fixed) — category `"Buildings"` or per-type.

Mechanism: these join the `LUR_RPS_GAMEPLAY_CVARS` X-macro list (they affect the deterministic sim → latched into `CvSnapshot`, get an `ECvId`, sync over lockstep, tunable in the console). Watch the **256 `ECvId` budget** (`static_assert(CvIdCount <= 256)`): adding ~2–4 CVars × 4 building types (~8–16 entries) is fine. Numbers are placeholders for the balance pass; structure (which CVars exist) is code.

Example additions (illustrative):
```
LUR_CVAR(CvRockBuildingHp,   "rps.rock.building_hp",    200, ..., "Combat")
LUR_CVAR(CvRockBuildingCost, "rps.rock.building_cost",  120, ..., "Combat")
... paper/scissor/miner analogously, miner-building in "Economy" ...
LUR_CVAR(CvBuildingQueueMax, "rps.build.queue_max",      40, ..., "Buildings")
LUR_CVAR(CvInitialFrontier,  "rps.build.initial_frontier", <world-units>, ..., "Buildings")  // starting buildable depth (§5.3), ~1 screen; world-space, NOT pixel-derived
LUR_CVAR(CvStartingGold,     "rps.econ.starting_gold",    <amount>, ..., "Economy")   // §12.6: one mining camp + a few carts, too little for camp + combat building
LUR_CVAR(CvBuildingFootprint,"rps.build.footprint",       F(<r>), ..., "Buildings")  // §12.2: one shared footprint radius for all buildings
```

`CvInitialFrontier` is `AffectsGameplay` (gates placement) → in the X-macro list, latched, synced. It's a world-space distance; its value is *tuned to match* the initial camera band but is never computed from screen size (that would desync across aspects).

---

## 9. Match start: place-your-mining-camp = ready

Camps are gone; the match bootstraps through a **pre-match placement phase** that *is* the ready handshake:

- Each team starts with **only gold** (a starting-gold CVar), no units, no buildings.
- The **first building each player places is forced to be a mining camp** (miner building) — the UI only offers/enables the miner-building drag until it's placed (a view-side constraint; the sim also rejects a non-miner first placement defensively).
- **The match (tick clock) does not start until both players have placed their mining camp.** This maps onto the existing lockstep ready/seed handshake: placement of the first camp is each peer's "ready" signal; the sim advances from tick 0 only once both readies are in. Mechanically, the placement of camp #1 is an input event exchanged during the pre-game handshake; "both placed" is the gate that starts `Step`ping — the same shape as the current seed/ready exchange in `LockstepPeer::Init`.
- Pre-match, each player places their camp on their own side (a starting region); after both are down, normal play begins (gold flows once miners are produced from the camp and start mining).
- **Single-player:** the AI "places" its mining camp as its first event; the match starts once both the human and the AI have placed. (The AI placing instantly would start the match the moment the human places — fine, or a tiny delay for feel.)
- **Determinism:** the placement events and the "both ready" condition are exchanged/derived identically on both peers; no wall-clock. This is just the existing ready gate re-skinned as a placement.

### 9.1 Camera before the first camp (view-only)
- Before a player has placed their mining camp, the **camera is locked at the bottom** (their own baseline) — showing the starting band where the camp goes. View-only; no sim involvement.
- The **initial buildable band** (`CvInitialFrontier`, §5.3) is tuned so its world-space depth **corresponds to what the locked bottom camera shows** — i.e. you can build within roughly the initial visible area, and the frontier rule blocks building *beyond* it until units advance. The correspondence is a tuning choice (pick the world constant to match the camera framing); the sim never reads the screen.
- After placement / once the match starts, the camera behaves per normal play (follows/pans as the game already does). As the frontier advances with your units, the buildable area grows ahead of you.

---

## 10. Balance implications (for the #84 pass)

The rework hands several new levers to the balance playbook and invalidates some old tuning:

- **Throughput scales by building count, not queue depth** (§3) — the core economic decision is now "more buildings vs. more units," replacing the removed stack-snowball. Needs fresh tuning: building cost vs. unit cost, how many buildings a gold income supports.
- **Buildings as targets** (§7) change army value — raiding the economy (miner buildings) is now a real strategy, and production buildings are counter-attackable, so map control matters more.
- **Placement/space** becomes a skill axis (spreading vulnerable miner buildings vs. clustering for defense; walling with buildings via repulsion).
- **The win rule is updated for buildings (§12.1):** loss = **no alive units AND `Gold < CheapestUnitCost`** (cheapest unit = miner, 30). Buildings don't enter the test — a building can't mine or produce without gold, so no-units-and-broke is unrecoverable regardless of buildings standing (this closes the "buildings alive but no units/gold" stalemate). A gold-carrying cart *is* a unit, so it's already covered by "no units." Both peers meeting it same tick = draw. This replaces the old unit-only rule and the interim building-inclusive draft.
- Re-run the strategy matrix (rush a building vs. boom buildings vs. destroy-their-camp) once the mechanics land.

---

## 11. Onboarding hints (view-only)

Two diegetic hints teach the two new verbs — **place** a building, then **queue** units — each looping until the player performs the action, each purely view-layer (`GameView`), not sim state, not hashed, not networked. Both read a sim-readable condition from local view/match state and animate; neither touches determinism (float view-side timers are fine).

### 11.1 Placement hand
A looping pointing-hand hint demonstrates the first placement:

- **Behavior:** loops the drag gesture — a hand sprite animates from a start anchor (the miner-building button) out onto the map along a demo drag path, repeating, **until the player places their mining camp** (loop, not once).
- **When:** the **first mining camp of every match, always** (not a one-time tutorial) — it appears during the pre-match placement phase and stops the instant the camp is placed.
- **Placement:** purely **view-layer** — an animated sprite over the board, reading "has this player placed their camp yet?" from local match state. Zero determinism involvement.
- **Rendering:** the existing sprite/instanced quad path + a hand texture (cooked like other art); a simple looped position/scale interpolation along the demo path, driven by a local float timer.
- Stops/hides immediately on placement; if a real drag is active, it takes precedence and the hand fades.

### 11.2 Production-button pulse
Once the mining camp exists, the player must discover that a building *produces* — so the **x1/x5/x20 buttons animate to draw the eye**:

- **Behavior:** the production buttons (x1/x5/x20) **pulse/animate** (e.g. a gentle scale/glow throb) to signal "tap me," looping **until the first unit(s) are queued** at that building.
- **Scope (decided):** the **first building only, once per match** — specifically the **mining camp** (the first building placed). The moment that building has anything in its queue (`Queue > 0`), the pulse stops for the rest of the match and **never re-triggers** — subsequent buildings do *not* pulse (the player has learned the mechanic). This teaches production exactly once, on the building the player is already looking at, without nagging a veteran who spams buildings later.
- **Trigger condition (view-reads-sim):** pulse while `(this is the match's first building) AND (its Queue == 0)`; a local per-match "production taught" flag latches true on the first queue and suppresses it thereafter. Reading `Queue` is a read-only peek at sim state from the view (like the HUD reading unit counts) — the hint itself writes nothing to the sim.
- **Placement:** view-layer only — the pulse is a render/animation property of the button widgets, driven by a local float timer; not sim state, not hashed, not networked.
- Stops immediately on the first x1/x5/x20 tap for that building.

---

## 12. Resolved decisions (were open questions)

All resolved 2026-07-21:

1. **Win condition:** a player **loses when they have no alive units AND `Gold < CheapestUnitCost`** (the cheapest unit is the miner at 30). This single predicate covers everything, including the case that motivated the revision — **buildings standing but no units and no money**: a building can't gather (only miner *units* mine) and can't produce without gold, so with no units and not enough gold for even the cheapest unit, the player can *never* generate a unit or gold again → doomed, regardless of buildings. Buildings therefore **do not appear in the loss test at all** — they're irrelevant dead weight in that state. This also subsumes the old "can't afford a mining camp" clause (if you can't afford a 30-gold miner unit, you can't afford a miner building either). Both peers meeting it the same tick = **draw**. Gold carried by mining carts needs no special handling: **a cart carrying gold is a unit**, so "no alive units" already excludes that case. **Supersedes the old `AliveUnits==0 && QueueLen==0 && Gold<CheapestCost`** rule (and the earlier draft's building-inclusive version): the final predicate per team is simply *no alive units AND `Gold < CheapestUnitCost`*.
2. **Building footprint:** **one shared footprint size** for all buildings (a single radius/box CVar). Per-type footprints deferred unless a need appears.
3. **x1/x5/x20 targeting — per building, buttons on the building:** production is **per building**; the **x1/x5/x20 buttons render at (are attached to) every building** and are always present. The player taps the buttons **on the specific building** they want to queue at — **no building-selection/highlight concept exists**. Each tap enqueues at that building. (So the `EvtQueueUnits` event's `buildingSlot` is the building whose buttons were tapped — unambiguous, no selection state.)
4. **Miner gold drop-off:** carts deposit to the **nearest own miner building**. If a team's **last miner building is destroyed while carts carry gold, that gold is stranded** (carts hold it) until a new miner building is placed, then they resume depositing to the nearest. Deterministic (nearest by fixed-point distance, tie-break by slot index).
5. **Rally / spawn:** **no custom spawn behavior** — units spawn adjacent to their building and immediately enter the normal **boid system** (auto-seek nearest enemy, flock, etc.). No rally points, no hold-and-gather. The autonomy premise is unchanged: the player controls *production*, the units drive themselves.
6. **Starting gold:** enough for **one mining camp + a few mining carts**, but **deliberately too little for a mining camp + a combat building** — so a player can't open with military and risk being stuck with no economy and no gold to fix it. This forces the healthy opening (camp → miners → gold → choices) and is a CVar (`CvStartingGold`) tuned to sit in that window. (Since it gates the deterministic opening, it's `AffectsGameplay`/latched.)
7. **First mining camp placement region:** anywhere within the **initial buildable band** — the screen-deep strip from the baseline up to `CvInitialFrontier` (§5.3). No narrower starting zone; the whole initial band is legal for camp #1.

**Follow-up (not blocking this spec):** the **AI** (separate spec) needs updating for the building vocabulary — placement decisions, where to put miner buildings, defending them. Flag once this lands.
