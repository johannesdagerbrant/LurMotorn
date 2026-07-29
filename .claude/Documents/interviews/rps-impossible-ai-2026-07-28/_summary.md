# Summary: rebuilding the RPS "Perhaps Impossible" AI

**Date:** 2026-07-28
**Status:** plan complete, not implemented (this was a planning session; no production code changed)

## Problem

The top AI tier (`EAiTier::PerhapsImpossible`) is meant to be almost unbeatable and is instead
**0–31** against its author across one day of play (64 recordings pulled from the Galaxy A14; 31
genuinely-played matches, all human wins; two nominal "AI wins" are forfeits where the human stopped
issuing input). By the end of the evening he had a **159-second** kill.

It is not gold-limited, not reaction-limited, and not information-limited — it has perfect
whole-board vision, `staleness 0`, `precision 1`, an unlimited building cap, and a *higher* input
event rate than the human. The tier's difficulty axes are exhausted: there is nothing left to turn
up. What is wrong is the decision procedure.

## Solution

Keep the AI structurally fair — same two input events, same income, no hidden state — and replace
**wrong arithmetic** with right arithmetic, then add a small portfolio of named plans so it is not one
memorisable strategy. Explicitly **not** building a search: the literature repeatedly measures
search-over-scripts as no better than the best single script (Puppet Search's mean was statistically
indistinguishable from its best script; Nash strategy selection scored its guaranteed 50.5%).

## Key decisions

| Decision | Choice | Rationale |
|---|---|---|
| Difficulty target | Owner wins ~1 in 10, **plus a scalar readout** | A near-guaranteed loss with no stated goal is 9 unstructured failures; with a scalar it's 9 measurable attempts (Juul, Burgun) |
| Cross-match learning | **No persistence**; strong *in-match* style adaptation | Keeps the AI a pure function of `(Sim, Tick, seed)` — determinism, replay, shadow-AI harness all preserved; sidesteps cross-session DDA failure modes entirely |
| Blast radius | New decision procedure for the **top tier only** | Lower tiers are tuned against current behaviour (~15 measured experiments recorded in code comments); ladder ordering must not move |
| Queue batching | **Phase-dependent `+1` → `+5`**, and this one **is** shared across tiers | Owner's correction: early batching is *bad* — gold in a queue is gold not buying the next camp. Defect is that the AI never *transitions*. Retune ladder only if it breaks |
| Counter feel | **Composition lag as a per-tier CVar**, larger on easier tiers | This is the existing `rps.ai.<tier>.staleness`; top tier simply stops being 0. Counter-picking is *the* cheating tell in macro games |
| Architecture | Four formula fixes, then a **named-plan portfolio** | Prismata (the only micro-free architecture in the literature) got 6 difficulty tiers out of recombining portfolio pieces in 15 min of config, and ran 14 months without architectural change |
| `∫ ArmyValue·dt` objective | **Deferred** | Best-evidenced change in the literature (+18.4 pts vs top-5 bots) but needs a projection model — the one piece of genuinely new machinery |
| Validation | **Owner-bot** built from the recordings | Only instrument that can see a 10% win rate; `--aivs` measures AI-vs-AI and `--aibeginner` measures AI-vs-passive |

## Architecture

All changes land in `Games/RocksPapersScissors/Core/` (`AiController.{h,cpp}`, `Tunables.h`) plus a
new harness in `Desktop/DesktopMain.cpp`. No engine-module changes. Everything stays integer/`Fixed`,
allocation-free, and a pure function of `(Sim, Tick)` + seeded RNG.

Reference cost budget: a state evaluation in this literature is 0.012–0.087 µs; the expensive thing
is always a forward model (~271 µs/playout), which none of this needs. Every item below is tens of
integer ops.

## Implementation status (2026-07-28, end of session)

- **Phase 1 — DONE, committed, green** (`81925a0`). The 4000-gold stall is fixed; regression test
  `TestChestNeverPricesOutEveryAction` fails 400/400 silent ticks with the fix disabled and passes
  with it on. `--replay`'s shadow AI now counts silence.
- **Phase 2.1 — ATTEMPTED, MEASURED WORSE, REVERTED.** See
  [negative-result-best-response.md](negative-result-best-response.md). Best-response composition
  scored 11–5 / 12–4 against Hard where argmax scores 15–1.
- **The order below is therefore WRONG and has been corrected: Phase 4 must come before Phase 2.**
  No composition change can be judged on `--aivs`, because the change exists to remove a *human*-
  exploitable handle and two AIs playing argmax never exploit each other. `--aivs` is a regression
  guard, not a fitness function.

Revised order: **1 → 4 (owner-bot) → 2 → 3 → 5 → 6.**

## Implementation order

### Phase 1 — Stop the bleeding: the 4000-gold stall
*Files: `Core/Private/AiController.cpp`, `Core/Public/Rps/Tunables.h`*

The reserve (`counter_chest`) can price out **every** action including the cart fallback that exists
to prevent silence, because the fallback routes through the same `Spendable`. Measured: 16–22 s of
total inaction holding ~3945 gold (Scissor building = 4000), 31–45% of samples idle, and it is
*steerable* — lead with Paper and the AI locks onto Scissor and freezes. See
[the-4000-gold-stall.md](the-4000-gold-stall.md).

Fix: a reserve must never make the cheapest useful action unaffordable. Exempt the cart fallback
from the chest, and denominate the reserve in income-seconds rather than in a building price (prices
run 1000 / 2500 / 4000, so a price-denominated rule behaves 4× differently by counter type).

**Success:** longest AI silence < 3 s and idle-sample rate < 10% across a replay of all 31 recordings.

### Phase 2 — The four formulas
*File: `Core/Private/AiController.cpp`*

1. **Composition = best response to the mixture, not to the mode.** Replace
   `CounterTo(argmax(enemy))` with `argmax_i (A·q)_i`, and drive production toward the 3-cycle Nash
   mix `p ∝ (a, b, c)`. **No RNG needed** — the army composition *is* the mixture, so building 40/35/25
   realises it exactly and deterministically. (~12 int ops. Removes the Phase-1 exploit's steering
   handle permanently.)
2. **Commitment = Lanchester with production.** Replace
   `MySoldiers − EnemyArmy ≥ AllinLead + FoeBldg·Walk/Bt` with
   `u = α·A − ρ_B; v = β·B − ρ_A; β·u·u > α·v·v`. `ρ_enemy` is directly observable here because
   production is flat per building — it *is* their soldier-building count, which the code already
   counts exactly. (6 muls + a compare. Fixes the AllIn false positive that fires at t1620 on a 12–0
   "lead" while the human is pure-economy.)
3. **Worker diminishing returns.** Add a saturation term (`Σ rate/⌈k/10⌉`, or per-deposit
   `(x+y)/x`). Without one, more workers is monotonically better and the policy degenerates to
   workers-only — which is exactly the observed 520-cart/25-building endgame. (1 divide.)
4. **Capacity from income, not from wealth.** Replace `Gold ≥ Price × ExpandGoldFactor/100` with
   income ÷ per-building drain. Steamhammer's shipped rule is ~1 production building per 3 mining
   workers; the human sits at 34 buildings / 302 workers, the AI at 8 / 110. (1 divide.)

**Success:** end-of-match building count within 20% of the human's on the owner-bot harness; no AllIn
entered while the enemy army is zero.

### Phase 3 — Phase-dependent queue batching (all tiers)
*Files: `AiController.cpp`, `Tunables.h`*

The two-phase depth logic already exists (lines 575–586) but is gated on `K.WaveLead > 0`, and the
top tier ships `wave_lead = 0` / `miner_queue = 0` — so it is dead code there. Un-gate it and give the
transition an economic trigger as well as the wave-ETA one (`+1` while expanding, `+5` once income
exceeds what standing production can absorb).

Then re-measure: `--aivs` 16 matches per pairing. If the strict 16-0/16-0/16-0 ordering holds, done.
If Easy becomes too strong for a first-timer, pull back with `queue_depth`/`max_buildings` and verify
with **`--aibeginner`**, which is the only harness that can see that failure.

### Phase 4 — Owner-bot harness
*File: `Desktop/DesktopMain.cpp`, `#if LUR_INTERNAL`, alongside `--aibeginner`*

Encode the owner's **fast line** (21:09 onward), not the earlier boom: ~14 mining camps pushed to
depth ~120, first soldier building at ~t1000, Paper-led mixed composition, `+1`→`+5` switch, ~1.9
input events/sec. Run candidates headless, hundreds of matches, tune to the ~10% target.

Note the constraint from `MatchRecord.h`: queue events carry a building **slot index** shared across
both teams, so replaying recorded human events against a *different* AI is best-effort. The owner-bot
must be a **strategy re-implementation**, not an event replay.

Free and worth wiring first: `--replay`'s existing shadow AI (`DesktopMain.cpp:645`) already re-steps
a recorded match with a fresh `AiController` alongside. It cannot prove a win, but it answers "does
the new logic still stall / still go AllIn at t1620?" against the real board, with zero new code.

### Phase 5 — Named-plan portfolio
*Files: `AiController.{h,cpp}`, `Tunables.h`*

A small set of plans selected in-match from observable board state — the round-1 requirement ("adapt
to and counter different opponent playstyles") with no cross-match persistence. Style axes visible in
the recordings that the AI currently ignores entirely: economy-first vs early-army; home-cluster vs
map-wide expansion; mixed vs spiked composition; production-capacity growth rate.

Set `PerhapsImpossible`'s `staleness` to a small non-zero value here (2–4 s discussion range, needs
measurement); keep enemy *building* counts exact, since a building is static and huge and the
existing code already argues this correctly.

### Phase 6 — Scalars
*Files: `View/`, `RpsMain.cpp`*

Survival time + personal best; how close the AI's home base came to dying; end-of-match economy
differential. All computable from what the census already records.

## Change made during this session (deviation from "no code changes")

At the owner's explicit request, the nine CVar overrides live on his Galaxy A14
(`files/rps-cvars.cfg`, 2026-07-28 21:00) were promoted to code defaults in `Tunables.h`, so that all
subsequent tuning and measurement happens in the same balance context he actually plays:

| CVar | was | now |
|---|---|---|
| `rps.unit.scissor.hp` | 80 | **60** |
| `rps.unit.scissor.damage` | 12 | **15** |
| `rps.boid.sep_strength` | 1.5 | **3** |
| `rps.boid.coh_same_radius` | 15 | **10** |
| `rps.boid.coh_all_radius` | 9 | **5** |
| `rps.boid.predator_flee_radius` | 15 | **12** |
| `rps.boid.noise_gain` | 0.5 | **2** |
| `rps.boid.align_radius` | 5 | **8** |
| `rps.boid.max_accel` | 0.10 | **0.20** |

`build.ps1` green (20/20). **Ladder re-measured, and it moved:**

| Pairing | Before | After |
|---|---|---|
| Impossible vs Hard | 16–0 | 15–1 |
| Hard vs Medium | 16–0 | **11–5** |
| Medium vs Easy | 16–0 | 16–0 |

Ordering still holds directionally, but Hard-vs-Medium loosened substantially — the scissor change is
a real balance shift. **Not committed.** This must be re-checked (and probably re-tuned) as part of
Phase 3's ladder pass, and `--aibeginner` should be re-run before shipping, since Easy's beginner
pacing (#155) was calibrated against the old combat numbers.

## Expected results

- The Paper-lead exploit is dead after Phase 1; the *class* of exploit is dead after Phase 2.1.
- Phases 1–2 alone should move the AI from 0% to genuinely competitive — those four errors account
  for every measured symptom (idle stalls, 8-vs-34 buildings, 520 carts, false AllIn, single counter).
- Phase 5 is what stops it being one memorisable strategy, which is what the 31–0 record and the
  halving of kill time actually demonstrate: the owner learned it in a day.

## Risks

- **The ladder moves under Phase 3.** Mitigated by measuring before/after and by the fact that
  `queue_depth`/`max_buildings` exist precisely to absorb this.
- **The owner-bot overfits to one human.** It encodes exactly one person's line. A 10% rate against it
  is not a 10% rate against everyone — but it is the person the tier is for.
- **Perfect counter-picking reads as cheating** even when provably fair (AoE2:DE's Extreme AI gets
  zero bonuses and is accused anyway). Mitigated by the `staleness` lag in Phase 5.
- **A stronger AI may not be more fun.** Design research is emphatic that legibility of the loss
  matters more than the loss rate; the Phase 6 scalars and the named (scoutable) plans are the hedge.
- **Two research strands died on session limits** (AoE2-specific mechanisms, part of the industry
  sweep). The StarCraft strand covered most of that ground independently, but the AoE2 `.per`
  scripting language — the one shipped RTS with a genuinely strong non-cheating community AI — went
  unexamined and is the most likely place a better idea is hiding.
