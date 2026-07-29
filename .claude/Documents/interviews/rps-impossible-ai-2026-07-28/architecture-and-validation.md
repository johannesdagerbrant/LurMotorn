# Architecture, counter-lag, and validation — decisions round 3

**Date:** 2026-07-28

## Key points

1. **Four formula fixes, then a named-plan portfolio.** No search.
2. **Composition lag is a CVar that scales with tier — more lag the easier the tier.** This is the
   existing `rps.ai.<tier>.staleness` knob; `PerhapsImpossible` just stops being 0.
3. **Validation is an owner-bot** extracted from the 2026-07-28 recordings, run headless hundreds of
   times against candidate AIs.

## Details

### 1. Scope — formulas first, portfolio second, no search

Deliberately **not** building a build-order searcher or MCTS. The literature is consistent that
search over scripts is not measurably better than the best single script (Puppet Search's mean was
statistically indistinguishable from its best script; Nash strategy selection scored its guaranteed
50.5%; on small maps a hardcoded rush beat every search agent). The available wins here are in
replacing wrong arithmetic, and each replacement is a handful of integer ops.

**The four formulas** (full derivations and sources in
`.claude/Documents/research/rps-impossible-ai.md`):

| # | Current | Replacement | Cost |
|---|---|---|---|
| 1 | `counter(argmax(enemy))` — one counter for the dominant type | `argmax_i (A·q)_i` — best response to the whole mixture; target mix `p ∝ (a,b,c)` (3-cycle Nash closed form) | ~12 int ops |
| 2 | `MySoldiers − EnemyArmy ≥ AllinLead + FoeBldg·Walk/Bt` | Lanchester-with-production: `u = α·A − ρ_B; v = β·B − ρ_A; β·u·u > α·v·v` | 6 muls + compare |
| 3 | no worker diminishing-returns term at all | harmonic falloff `Σ rate/⌈k/10⌉`, saturation `(x+y)/x` per deposit | 1 divide |
| 4 | `Gold ≥ Price × ExpandGoldFactor/100` (a wealth test) | capacity from income ÷ per-building drain (Steamhammer's ~1 building per 3 mining workers) | 1 divide |

Note #1 needs **no RNG**: our army composition *is* the mixture, so producing 40/35/25 realises a
mixed strategy exactly, deterministically. And #2's key input — enemy reinforcement rate `ρ` — is
directly observable in this game because production is flat per building, so `ρ_enemy` *is* their
soldier-building count. `AiController.cpp` already counts that exactly (`FoeCombatBldg`) and
justifies why; only the formula consuming it is wrong.

**Then the portfolio.** A small set of named plans, selected in-match from observable board state.
This is what satisfies the round-1 requirement ("adapt to and counter different opponent
playstyles") without any cross-match persistence. Shape borrowed from Prismata's Hierarchical
Portfolio Search (the only micro-free architecture in the literature) and Halo Wars 2's named
strategy tables.

Two properties of that shape that motivated the choice, beyond strength:
- Prismata built **six difficulty tiers by recombining portfolio pieces in a config file, in under
  15 minutes** — which maps onto our `EAiTier` + CVar system directly, and is a far better answer to
  "what is the tier ladder" than the current exhausted staleness/precision axis.
- Prismata ran **14 months live with no architectural changes despite dozens of balance changes.**
  This game is under active tuning; that robustness matters more than peak strength.

Named plans also make the AI's intent **legible and scoutable** — which the design research
identifies as what separates "hard" from "cheating".

**Explicitly deferred:** BOSS-IM's `∫ ArmyValue·dt` objective. It has the best evidence of anything
in the literature (+18.4 points against top-5 bots, and it makes the economy→army switch emergent
rather than a tuned `soldier_ratio`) but it needs a projection model, which is the one piece of real
new machinery. Revisit if the portfolio alone does not reach the target.

### 2. Composition lag — reuse `staleness`, un-zero it at the top

Decision: **a CVar, scaling with tier, more lag the easier the tier.**

This is exactly the existing `rps.ai.<tier>.staleness` knob — ticks of delay on reading the enemy
army composition through the ring buffer in `AiController`. `PerhapsImpossible` currently ships
`staleness 0` (instant, exact). It should ship a **small non-zero** value; the lower tiers keep their
larger ones, preserving the monotonic ladder.

So this needs **no new mechanism** — it is un-zeroing a knob that already exists and is already
wired through the recorder and the CVar sync.

What stays exact, deliberately: **enemy soldier-BUILDING count**. A building is static and huge — a
human sees production the moment they look at it — and the existing code comment already makes this
argument. The fuzzed mirror exists to model reading a *moving army*, which is the right distinction.
It also happens to be the input formula #2 needs.

Why this matters for feel, not just balance: in a macro game, counter-picking is the thing players
call cheating. AoE2:DE's Extreme AI provably receives no bonuses and is accused anyway, because "it
knows what units you're making." A visible reaction delay is the one handicap that shipped games use
universally and players never complain about.

### 3. Validation — an owner-bot from the recordings

Build a scripted sparring partner encoding how the owner actually beats the AI, extracted from the
33 recordings of 2026-07-28. Observed strategy to encode:

- pure economy until ~t1700 (zero soldiers), ~20 mining camps
- camps pushed to depth ~121 at p90 (midfield), not the home rows
- soldier buildings at depth 102–132, genuinely mixed (rock 7.1 / paper 4.7 / scissor 4.9)
- `+1` queue batches while expanding, `+5` once the economy is strong
- ~1.9 input events/sec, ~34 buildings and ~300 workers at the end

Run candidate AIs against it headless, hundreds of matches, and tune to the ~10% target. This is the
only instrument that can see the target number: `--aivs` measures AI vs AI, `--aibeginner` measures
AI vs a passive player, and neither plays like the person who beat it.

Complementary, free, and worth wiring first because it needs no new code: **`--replay`'s existing
shadow AI** (`DesktopMain.cpp:645`) re-steps a recorded match with a fresh `AiController` alongside
and reports what it *would* have decided. It cannot say the new AI would have won, but it answers
"does it still enter AllIn at t1620 against a player with zero soldiers?" immediately, against the
real board.

Caveat to respect (already documented in `MatchRecord.h`): queue events carry a building **slot
index**, and slots are shared across both teams, so replaying a human's recorded events against a
*different* AI is best-effort re-simulation, not exact replay. The owner-bot must therefore be a
**strategy re-implementation**, not an event replay.

## Open questions

- Which named plans, and how many. Candidate axes from the recordings: economy-first vs early-army;
  home-cluster vs map-wide expansion; mixed vs spiked composition.
- What `staleness` value the top tier gets (2–4 s was the discussion range; needs measurement).
- Whether the owner-bot lives behind `LUR_INTERNAL` alongside `--aibeginner` (almost certainly yes).
