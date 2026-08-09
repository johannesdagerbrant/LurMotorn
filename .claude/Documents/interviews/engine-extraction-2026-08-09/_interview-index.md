# Interview: Engine Extraction — cementing two games' lessons into LurMotorn

**Date:** 2026-08-09
**Status:** Complete

## Context

Two games are near feature-complete: **Chess** (turn-based, 1 byte/move, the original proving
ground) and **RocksPapersScissors** (real-time RTS-ish, rollback netcode, continuous input). The
third game — **co-op physics puzzles** — is next.

The Handmade-lens review (`Docs/Journal/2026-07-17/lurmotorn-review-2-handmade-lens.md` §1) set the
method explicitly: *don't pre-abstract; write game #2, then compress what actually repeats.*
Muratori's semantic compression — "the duplication IS the specification for the engine". That
condition is now met: two real games exist, so extraction is no longer speculation.

The review also drew the dividing line we must respect (§3.4): **count the implementations.**
Multiple-at-runtime → interface. One-per-platform → link-time seam. One total → just code.
And (§8) **library, not framework**: the game's `main` owns the loop and calls the engine.

### The goal (user's framing)

- Games should need **zero platform-specific code**.
- Game folders should hold **only unique content + gameplay code**.
- Gameplay code should mostly be *using engine libraries* and *extending interface classes*.
- Where two games have similar-but-not-identical features, look for **one engine feature that
  satisfies both via API arguments**, not two.
- **Highest value = multi-axis exponential boilerplate**: code reimplemented *per platform × per
  game*. That's the 2×3 (or worse) matrix — fixing one such item removes N implementations.

### Raw size signal (2026-08-09, `master` @ 22468e7)

| Area | Chess | RPS |
|---|---|---|
| Android shim | 7,916 | 7,291 |
| iOS shim | 1,328 | 2,261 |
| Desktop | 675 | 2,021 |
| Core (pure C++) | 2,084 | 6,867 |
| View | 6,651 | 14,795 |
| Net | — | 4,720 |
| Runtime | — | 741 |

~18.5k lines of *platform shim* across two games is the headline number. `Games/RPS/Net` at 4,720
lines sitting beside a `Modules/Net` is the second.

## Synergies

### Already-specified: the rollback coordinator extraction
`Docs/Journal/2026-08-04/rps-rollback-outcome-and-agnostic-responsiveness-workflow.md` §"The
generalized workflow" **already designs** the `Games/RPS/Net/LockstepPeer` → `Modules/Net`
extraction. Do not re-derive it:
- Coordinator owns snapshot ring, peer predictor seam, confirmed-tick, speculate/rollback loop,
  confirmed-only anchors + cross-check, #161/#163/#162 recovery, resync, per-tick wire framing.
  *All already game-agnostic in spirit — merely entangled with RPS types.*
- Game supplies: POD `Sim` (`Step`, `StateHash`, `static_assert(is_trivially_copyable)`), POD
  `Input` + batch codec + predictor, and tunables (tick rate, horizon, send-lead).
- **Two execution modes on one coordinator**: rollback (RPS, physics) and lockstep (chess, the
  degenerate confirmed-only case). Chess gets transport/session/wire/recovery/determinism-harness
  for free and declares it doesn't need rollback.
- Physics-game verdict: hand-rolled `Fixed` physics + this shared stack. Netcode risk retired; the
  remaining risk is a fixed-point constraint solver, and `resim_ticks × step_cost` under continuous
  two-hand manipulation is the go/no-go measurement.

This is the strongest existing evidence that the *pattern* the user wants works: RPS becomes a thin
instantiation, chess supplies different arguments to the same machine. **It is the template for
every other extraction in this interview.**

### Method already settled by the Handmade review
- §1: extract *after* two games, from real duplication (now satisfied).
- §3.4: **count the implementations** — multi-at-runtime → interface; one-per-platform → link-time
  seam; one total → just code. This constrains *how* we extract, and pushes back on "extending
  interface classes" as the default idiom.
- §8: **library, not framework** — the game's `main` owns the loop. A `GameHost` that calls the game
  was explicitly resisted. The user's "games contain only content + gameplay code" ambition pushes
  toward a framework; this tension is a **primary interview topic**.

### Recent history signal (git log, last ~25 commits)
Chess just received a latency/immediacy pass (#186–#193: touch-DOWN commit, send-queue jump, drain
before present, selection hint) that **mirrors** the RPS immediacy pass. The same *workflow* was
applied twice by hand — evidence that the responsiveness checklist wants to be engine-enforced
(e.g. an engine-owned frame/input pipeline), not re-derived per game.

## Themes Discovered

1. **Three diseases, not one** — platform dedup (A), game-side promotion (B), reverse leakage (C).
   Each needs a different cure; conflating them was the main risk to the plan.
2. **The one-way ratchet** — the `Modules`↛`Games` wall means facilities built in whichever game
   needed them first never come back. This, not duplication, is the game-side problem.
3. **The framework tension is resolvable** — review §8 defended the game owning its *frame*, not
   process startup. ~90% of a phone main is OS ceremony.
4. **"One feature, different arguments" is the repeated shape** — the rollback/lockstep mode
   selector, the agent parser's verb table, and (best example) per-opponent persistence where the
   engine owns the mechanism and the game declares the schema.
5. **Build tooling is a third axis** the user hadn't named but which fits the definition exactly.
6. **A durable doctrine emerged**: delete all dead code; build in the game; promote on the second
   consumer — which makes the promotion pass load-bearing.

## Files Created

- `_interview-index.md` — this file
- `scope-and-strategy.md` — entry-point ownership, the three diseases, promotion tiers, ratchet cure
- `promotion-tiers.md` — Tier 1/Tier 2 verdicts + exclusions with reasons
- `architecture-decisions.md` — BLE diet, coordinator shape, stdlib diet, threading, flight recorder
- `wire-input-and-cleanup.md` — wire-version split, input pipeline + multi-touch, dead-code sweep
- `_summary.md` — **final synthesis: decisions, architecture, 8-phase plan, risks**

Research: `.claude/Documents/research/engine-extraction-inventory.md` (4-part inventory —
per-platform shims, engine modules, game-side logic, issues + journals)
