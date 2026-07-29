# Interview: RPS "Perhaps Impossible" AI — rebuild for near-unbeatability

**Date:** 2026-07-28
**Status:** In Progress

## Context

The owner played tier 3 (`PerhapsImpossible`, `EAiTier::PerhapsImpossible = 3`) all day on
2026-07-28 and beat it repeatedly. 33 flight recordings were pulled off the Galaxy A14
(`run-as com.lurmotorn.onlyrps`, `files/rps-match-*.rec`), 19 of which are matches longer than
150 s.

**Scoreboard at tier 3, 2026-07-28:** 14 matches reached a result — 12 human wins, 2 AI wins.
Both "AI wins" are forfeits: in `171936-2` and `160803-7` the human stops issuing *any* input
partway through and banks 40 k gold while their army decays to zero. Discounting those, the
genuine record is **12–0 to the human**.

The goal set by the owner: this tier should be *almost impossible for him to beat*, and it must
stay structurally fair — the AI's only interface is the same two input events a human has
(`EventPlaceBuilding`, `EventQueueUnits`), it may not read hidden state or act faster than a
person could.

## Constraints that shape every option

- **Action space is tiny.** `Rps::InputEvent` has exactly two kinds: place a building of one of
  4 types at (x,y), and queue N units at a building slot. There is **no unit control at all** —
  movement and combat are emergent (boids + auto-target). So the entire game, for both players,
  is a build-order / economy / placement / composition problem. Difficulty can only come from
  better macro.
- **Determinism is load-bearing.** `AiController::DecideEvents` must stay a pure function of
  `(Sim, Tick)` + its own seeded `SplitMix64`, integer/`Fixed` only, no allocation — it runs on
  the sim thread inside the same replay/rollback model as a networked match.
- **Runs on a phone**, inside a 100 ms tick that also runs the sim.
- `MaxEventsPerTick = 16` is the per-tick input cap shared by human and AI.

## Measured tunables these matches ran with (decoded from the `cv` lines)

All soldier types: cost 50, build 15 ticks, `counter_mult` 3.
Buildings: camp 600, **rock 1000, paper 2500, scissor 4000**. `queue_max` 20, starting gold 800,
home base 900 hp, `initial_frontier` 40, map 34 × 240.

Top tier knobs (`LUR_AI_TIER(PerhapsImpossible, "impossible", …)`):
`open_workers 5, worker_target 110, staleness 0, precision 1, cadence 12, jitter 2,
hysteresis 1, allin_lead 10, soldier_ratio 55, queue_depth 8, max_buildings 0 (unlimited),
defence_floor 5, build_cluster 3, miner_queue 0, wave_lead 0, counter_chest 100`.

Note the top tier already has **perfect information** (staleness 0, precision 1) and an
**unlimited building cap**. There is no headroom left on the axes the current tier system uses.

## Synergies

- `Docs/Journal/2026-07-22/rps-ai-opponent-spec.md` — the original AI design (tier structure =
  staleness / precision / cadence / hysteresis + Opening/Building/Reacting/AllIn FSM).
- `Docs/Journal/2026-07-19/rps-rts-netcode-and-unit-system.md`, `2026-07-17/rps-rts-design-spec.md`.
- `Games/RocksPapersScissors/Core/Tests/AiTests.cpp` — existing AI regression tests, including a
  `MatchRecorder` round-trip.
- `Games/RocksPapersScissors/Desktop/DesktopMain.cpp:610` — `--replay` reads a device recording;
  `--aivs`, `--aidiag`, `--aibeginner` harnesses already exist for measuring tiers against each
  other. These are the measurement rig any redesign must be scored on.
- **`--replay` already contains a SHADOW AI** (`DesktopMain.cpp:645`): it re-steps a recorded match
  from the recorded events and runs a fresh `AiController` alongside, fed the real board every tick
  with its events discarded. So "what would the new AI have decided at each moment the old one was
  losing?" is *already answerable against the owner's 12 wins* with no new tooling. It has a
  fidelity gate that compares against the recorded census and stops trusting itself after
  divergence. This is the single most valuable existing asset for this work.
- Long comment blocks in `AiController.cpp` and `Tunables.h` record ~15 previously measured
  balance experiments; several encode "this exact ordering is load-bearing, changing it cost hard
  N of 16". Any rewrite must respect that the *lower* tiers are tuned against current behaviour.

## Themes Discovered

1. The tier's difficulty axes are exhausted — the next tier must be a different *decision procedure*,
   not a parameter change.
2. The AI is spend-bandwidth and capacity limited, never gold or reaction limited.
3. Argmax counter-selection is not merely suboptimal — it is a *steering handle* the human exploits.
4. Commitment (`AllIn`) has the wrong sign and fires when the human is at their most vulnerable.
5. Fairness in a macro game is spent almost entirely on information lag, not resources.
6. The record is 31–0 and the kill time halved within one evening — memorability is as big a problem
   as strength.

## Status

**Complete.** Plan in `_summary.md`. No production code was changed.

## Files Created

- `flight-recording-analysis.md` — what the first batch of 2026-07-28 recordings shows (12–0)
- `scope-and-strategy.md` — round 1: difficulty target, adaptation model, blast radius
- `action-budget-and-batching.md` — round 2: phase-dependent batching, tier sharing, scalars
- `architecture-and-validation.md` — round 3: four formulas + portfolio, counter lag, owner-bot
- `the-4000-gold-stall.md` — second batch (19–0, 159 s kill): the reproducible hard exploit
- `_summary.md` — decision record and six-phase implementation plan
- `../../research/rps-impossible-ai.md` — the research findings
