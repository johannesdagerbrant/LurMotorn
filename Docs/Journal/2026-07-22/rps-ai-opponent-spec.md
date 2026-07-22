# RPS — AI Opponent Design Spec (v1)

> **Repo-sync 2026-07-21 (checked against HEAD `f11e228`).** The dev-tools stack shipped since this spec was drafted; the AI design is **unchanged in its core** (AI = an `InputFn` filling `mask1`; difficulty = knowledge + cadence; deterministic), but four concrete details are now pinned to what exists:
> - **`SimRunner::InputFn` is unchanged** — the AI hook (§1) is exactly as specified.
> - **CVar latching is real and already the model** — the Sim owns a `CvSnapshot Cv`, latched once at `Init` via `LatchCvs()` (`Tunables.h`), constant within a match unless a synced `MsgCvar` override applies. AI knobs follow this exact pattern (§7), not a bespoke mechanism.
> - **Gameplay CVars use a 1-byte `ECvId`** from the `LUR_RPS_GAMEPLAY_CVARS` X-macro with `static_assert(CvIdCount <= 256)` — the C.0.2 build-derived id, implemented as declaration-order in that list. `AffectsGameplay` AI CVars join it; single-player-only AI knobs do **not** (§7, revised).
> - **The desktop `--tune` double-wide panel was removed** (`f11e228`); the dev console is now **one cross-platform overlay** (collapsible categories, boxed value column, per-row reset; § key on desktop, two-finger triple-tap on phone, on-screen **numpad** for numeric entry — the soft-keyboard spike closed **NO-GO**, numpad is the answer). Only affects *where AI knobs are tuned*, not the AI.

*2026-07-20. A single-player AI that fills the opponent seat. Grounded in the code at `d1dbb76`: `SimRunner::InputFn` (`void(void* Ctx, uint32_t Tick, uint8_t& Mask0, uint8_t& Mask1)`), `Sim::TeamState { Gold, QueueCount[4], BuildProgress[4], SpawnCounter }`, `Sim::AliveCount/QueuedTotal`, `Modules/Sim/Random.h` (`SplitMix64`). Decisions locked via Q&A: single-player only; scripted/FSM strategy; **fair** (same rules as a human); difficulty = **information quality + reaction cadence**, combined into **three tiers** (Easy/Medium/Hard); deterministic (fixed-point + seeded RNG); numbers in CVars, structure in code; **no separate "personalities"** — the tiers read as distinct opponents on their own.*

## 1. The one architectural rule: AI acts only through button-presses

The AI's *sole* interface to the game is the four buttons a human has (worker + three unit types). It **never touches sim state directly** — it fills the opponent's input mask, and the sim applies it identically to a human's.

Concretely: the AI **is an `InputFn`.** Today `SimRunner` drives the sim with `InputFn(ctx, tick, mask0, mask1)`; single-player wires an AI context that:
- leaves `mask0` to the human (from `PendingLocalMask`),
- fills `mask1` from `AiController::DecideMask(state, tick)`.

This gives three things for free:
- **Structural fairness** — the AI literally cannot do anything a player can't; "fair" is enforced by the interface, not by discipline.
- **Determinism & replays** — the AI's presses flow through the same tick pipeline, so the flight recorder and `--auto` replays work unchanged (a recorded single-player game replays bit-identical).
- **Zero new sim plumbing** — no AI-specific `Step` path; the sim never knows whether `mask1` came from a finger or the bot.

Rule stated once: **AI → input mask → sim. Never AI → sim.**

## 2. Determinism (required, even single-player)

The AI runs *inside* the deterministic sim, so:
- **Fixed-point only** in any AI arithmetic that influences a decision (ratios, thresholds) — no float. Integer/`Fixed` comparisons on `Gold`, counts, ticks.
- **Seeded RNG** via `SplitMix64`, seeded from the match seed (so any "randomized" choice — see §5 jitter — is reproducible). The AI's RNG is a distinct stream from the sim's (separate seed derivation) so adding AI randomness never perturbs map/sim generation.
- **No wall-clock.** All timing is in ticks.
- Why it matters even with no peer: it keeps single-player inside the same `State = Replay(Inputs, Seed)` model — replays, the flight recorder, and desktop `--auto` soak all keep working, and a desync-hash on a recorded game still means "bug."

The AI decision itself is folded into a tick phase (before or at the input-apply phase, §6 of the RTS spec) so its reads see a consistent tick snapshot.

## 3. Difficulty = knowledge quality + reaction cadence (three tiers)

The whole difficulty model rests on the insight that **"fair" and "adjustable" are reconciled by information, not handicap.** Every tier plays by identical rules and has identical actions; they differ only in *what they know* and *how fast they act*. A weak AI isn't dumber — it reacts to stale, fuzzy scouting, slowly. This *feels* like a distinct personality (a laggy, sloppy opponent vs. a crisp, punishing one) without a separate personality system.

Three combined knobs, bundled per tier:

| Knob | Easy | Medium | Hard | Meaning |
|---|---|---|---|---|
| **Info staleness** | ~6 s old | ~2 s old | current | how old the AI's read of the enemy army is (it decides against a delayed snapshot) |
| **Info precision** | fuzzy buckets (none/some/many) | rough counts (±) | exact | granularity of the enemy composition it perceives |
| **Reaction cadence** | re-decides every ~5 s | ~2 s | ~0.5 s | how often the FSM re-evaluates and can change production |

Implementation: the AI keeps a **delayed/fuzzed mirror** of the opponent's `TeamState`/army composition — a ring of past snapshots; at decision time it reads the snapshot from `now − staleness` and quantizes counts to the tier's precision. This is the *only* difference between tiers. Numbers (the staleness seconds, precision buckets, cadence) are **CVars** (§7); the mechanism is code.

Nice property: because a weak AI acts on old info, it naturally mis-counters (builds rock when you've already switched off scissors), which is exactly how a beginner-friendly opponent should feel — punishable, readable, not random.

## 4. Strategic logic (FSM): opening → reactive → economy management

Per the "opening + reactive + economy/army-ratio" scope. One **shared opening** across all tiers (difficulty lives in the reactive layer, not the script), then a small FSM.

### 4.1 Opening (fixed, all tiers)
A short scripted build order from the start state (3 workers + starting gold), e.g. "workers up to N, then first soldiers" — a handful of tick-triggered presses. Identical for every tier; what differs is how quickly/accurately the AI *leaves* the opening to react (cadence + info). Opening steps are CVar-tunable counts/thresholds.

### 4.2 Reactive counter (the core loop)
On each reaction tick (cadence-gated), the AI reads its (tier-degraded) mirror of the enemy composition and decides what to **queue next**, using the counter triangle:
- Perceive enemy type mix (rock/paper/scissors counts, fuzzed per tier).
- Want the type that **beats the enemy's dominant type** (paper→rock, scissors→paper, rock→scissors), weighted by how dominant it is.
- This is a tiny **scored choice over four buttons** (the three counters + worker), not a planner. Even though the strategy is "scripted/FSM," the counter decision is naturally a 3-way argmax — cheap, deterministic, and it *is* the game's core tension expressed directly.

### 4.3 Economy / army-ratio management
The FSM balances worker vs soldier production against a target ratio (the eco-vs-army tension that is the game's point):
- Maintain a **worker count target** (grow economy early, taper as the game turns military).
- A **soldier:worker ratio** target that shifts over match time / under threat: if the enemy army is growing faster than mine (per the tier mirror), bias toward soldiers; if safe and behind economically, bias toward workers.
- Gold-gating: only queue what `Gold` allows (the sim ignores unaffordable presses anyway, but the AI shouldn't waste decisions — it picks the best *affordable* action).
- **States** (rough FSM): `Opening → Building (eco-biased) → Reacting (army-biased under pressure) → AllIn (commit when ahead or losing)`. Transitions on tick, gold, and the (degraded) enemy-strength read.

### 4.4 What v1 deliberately omits
No scouting *units* (the AI's "knowledge" is the abstract mirror, not a physical scout — simpler and the units auto-target anyway); no retreat/regroup micro (units are autonomous — the AI only controls *production*, exactly like the human); no expansion decisions beyond worker count; no multi-front planning. The AI's whole job is **what to build and when** — which, because units drive themselves, is the entire player decision too.

*(Note: the sim already computes a per-tick `ThreatSet`/`ThreatBits` for boids slice C — "enemy soldier within `GuardAlertR` of my cart" — for unit-level interpose steering. The AI operates at a coarser, strategic level (army *counts and composition*, tier-degraded), not per-cart threat bits; it doesn't consume `ThreatBits` directly, but "are my miners under attack" is exactly the kind of cheap signal it can derive from its mirror to trigger the army-biased state.)*

## 5. Feel & anti-robotic touches (deterministic)

To avoid a mechanical feel while staying deterministic:
- **Reaction jitter:** the cadence tick is ±a small seeded-random offset (via `SplitMix64`), so it doesn't act like a metronome. Deterministic (seeded), still feels organic.
- **Decision hysteresis:** don't flip production every reaction tick on tiny count changes — require a margin before switching the countered type (prevents twitchy back-and-forth, reads as "committed").
- **Imperfect execution at low tiers** falls out of staleness/precision already — no need to inject artificial mistakes.

## 6. Integration & module placement

**UI entry point — the opponent dropdown.** The AI is chosen the way a real opponent is: as entries at the **bottom of the existing opponent selector** (`GameView::Selector`, the same `Lur::Hud::Dropdown` chess uses). As of `f11e228` the selector builds a small fixed `DropdownItem[]` and its selection hook is **still stubbed** — `Selector.TookSelection()` with the comment *"selection has no target yet (#85 follow-up)."* The AI is the natural thing to fill that stub: selecting a tier row is the first selection that actually *starts* something. No new UI — it reuses the `DropdownItem` model verbatim, which already has the needed fields:
- A **non-selectable section header** (`DropdownItem::Header = true`) — e.g. "Practice" or "vs AI" — separates the real-peer list above from the AI rows below.
- **Three selectable rows**, "Easy / Medium / Hard", each a normal `DropdownItem` (Label = tier name; `Sublabel` optional, e.g. a one-word flavor; the status **dot** can encode tier, e.g. green/amber/red `LeadFill`, reusing the dot the peer rows use for link state).
- Selecting an AI row starts a single-player match against that tier — the same selection path a peer row uses (`Selector.TookSelection()` / the match-start hook), just routed to "construct an `AiController` at tier T + wire the AI `InputFn`" instead of "await a peer link."

So the player sees: their known peers/opponents, then a divider, then Easy/Medium/Hard — one coherent "who do you want to play?" list. The AI rows are always present (no peer or radio needed to practice), which also makes the game playable solo out of the box.

**Placement of the rest:**
- **New `AiController`** (game-side, `Games/RocksPapersScissors/`), pure logic over `Sim` read state → produces a `uint8_t` mask. Deterministic, host-testable.
- **Wiring:** selecting an AI tier constructs an `AiController` and an `InputFn` adapter that fills `mask1` from it (human fills `mask0`). No `SimRunner` change — it already takes an `InputFn` + ctx.
- **Difficulty tier** (Easy/Medium/Hard enum) is passed from the selected dropdown row into the controller at match start; it selects the CVar-driven knob set.
- **Testable headless:** because it's an `InputFn` over sim state, AI-vs-scripted and AI-vs-AI run on the desktop `--auto` harness (also a balance tool — AI-vs-AI at each tier is a cheap way to sanity-check the tiers actually differ in strength).

## 7. CVars (structure in code, numbers in CVars)

Now concrete against the shipped model (`CvSnapshot`/`LatchCvs`/`ECvId` in `Tunables.h`). Two distinct cases, and the split matters:

- **AI knobs that feed the deterministic sim's evolution** (reaction thresholds, ratio targets, opening counts — anything the FSM reads to decide a press) are read *by the AI, which runs inside the sim*, so they must be **deterministic-safe (fixed-point/int, never float)** and, to be tunable the same way gameplay knobs are, they should join the **`LUR_RPS_GAMEPLAY_CVARS` X-macro list** — which automatically gives them a latched `CvSnapshot` field, an `ECvId`, console visibility, persistence, and (if a match were ever networked) sync. Since single-player has no peer, the *sync* is moot, but riding the same list is the path of least resistance and keeps them latched-at-`Init` like everything else. **Caveat:** every entry in that list counts against the 256 `ECvId` budget and is treated as gameplay-latched — fine, the AI knob count is small.
- **AI knobs that are purely presentational or single-player-only meta** (e.g. the seeded-jitter magnitude, if you decide it should be live-twiddleable but never networked) can be ordinary non-gameplay CVars (category `"AI"`) *outside* the X-macro list — but note that a non-gameplay CVar is **not latched into `CvSnapshot`**, so if it influences a sim decision it must be read consistently; simplest is: **if it affects the sim at all, put it in the gameplay list.** Only keep something out of the list if it genuinely never touches sim evolution.

Practical rule for v1: **put every AI tunable that changes the FSM's behavior in the gameplay CVar list** (deterministic, latched, tunable via the console). Reserve non-gameplay AI CVars for things that don't affect the sim (none obvious yet).

Knobs to expose (category `"AI"`, in the gameplay list): opening worker target and soldier-start threshold; per-tier staleness ticks, precision bucket edges, reaction-cadence ticks; jitter magnitude; switch-hysteresis margin; worker-count target; soldier:worker ratio targets and threat-bias amounts; all-in trigger thresholds.

**Where they're tuned:** the shipped **cross-platform dev console overlay** (§ key on desktop / two-finger triple-tap on phone, numpad for numeric entry) — the AI category shows up there automatically once its CVars are in the list. (The old `--tune` double-wide panel is gone; a future desktop-only `CVarEditor` #115 would also surface them, but the console already suffices.)

## 8. Build order (slices)

1. **Slice 0 — dumb `InputFn`:** an `AiController` that only does the fixed opening + a naive "always build the counter to whatever I can perfectly see, react every tick." No tiers, perfect info. Proves the input-mask integration end to end (AI plays a full game vs a human on desktop).
2. **Slice 1 — the difficulty mirror:** add the delayed/fuzzed enemy snapshot + reaction cadence; wire the three tiers. This is where it becomes fun and fair-feeling.
3. **Slice 2 — economy/ratio FSM:** worker-vs-soldier management, the state transitions, hysteresis + jitter.
4. **Slice 3 — tune:** AI-vs-AI `--auto` to verify tier strength ordering (Hard beats Medium beats Easy convincingly), then human playtests per tier; CVar-tune to taste.

## 9. Open questions

1. **Difficulty enum home:** a game-side `EAiTier`, or fold into a broader single-player match-config struct? (Recommend game-side enum for v1.)
2. **Does "reaction cadence" also gate the opening,** or only post-opening reactions? (Recommend: opening runs at full speed for all tiers — it's a fixed script, not a reaction — so a low tier isn't *slow to start*, just slow to *adapt*. Confirm.)
3. **AI seed:** derive the AI's `SplitMix64` seed from the match seed (reproducible) or from wall-clock (varied each game)? Single-player + deterministic argues reproducible, but players may want variety game-to-game. (Recommend: match seed XOR a per-match salt, so it's deterministic within a replay but different across new games.)
4. **AI rows vs. the peer list:** are Easy/Medium/Hard always pinned at the bottom under a fixed header (recommended — always-available practice), and do AI matches get recorded/tracked like peer matches in the opponent registry, or kept ephemeral (no W/L history)? (Recommend: pinned rows; AI results ephemeral in v1 — no per-tier record — unless you want a "beat Hard" milestone later.)
