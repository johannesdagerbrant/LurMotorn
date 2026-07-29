# Scope & strategy — decisions round 1

**Date:** 2026-07-28

## Key points

1. **Target: the owner beats it ~1 in 10.** Not a wall, not a fair fight. The other 9 matches must be
   *measurable*, so the tier ships with a scalar readout.
2. **No cross-match memory. Strong in-match style adaptation.** The AI must not persist a model of a
   specific person between matches — but it must read and counter *the style being played against it
   right now*, not merely the dominant enemy unit type.
3. **New decision procedure, `PerhapsImpossible` only.** Easy / Medium / Hard keep today's FSM
   unchanged so the measured ladder ordering does not move.

## Details

### 1. Difficulty target — ~10% owner win rate, plus a scalar

The owner's current win rate at this tier is ~100% (12–0 on 2026-07-28, discounting two forfeits).
Target is ~10%.

A scalar is part of the deliverable, not a nice-to-have. Rationale from the design research: a
near-guaranteed loss with no stated goal is 9 unstructured failures, whereas the same 9 losses
against a continuous measure are 9 attempts. Candidate scalars, to be chosen later:

- survival time (ticks to home-base loss)
- final economy differential (his gold income rate vs the AI's at the end)
- furthest frontier reached / closest the AI's home base came to dying
- a personal best per tier, stored per device

This also protects the *name*: "Perhaps Impossible" over-claims if the real rate is 30%, and under a
0% rate there is nothing to look at. 10% + a best-ever number keeps the label honest.

### 2. Adaptation — within-match style reading, no persistence

**Explicitly ruled out:** persisting anything about a specific human between matches. That keeps the
AI a pure function of `(Sim, Tick, seed)`, which preserves determinism, replayability, the shadow-AI
harness, and the `LUR_AGENT` / fairness posture. It also sidesteps the whole cross-session DDA
failure mode (patronisation, sandbagging).

**Explicitly required:** the AI must classify and counter the *playstyle* it is facing, in-match.
Today it classifies exactly one thing — the argmax enemy soldier type — and that is why a mixed army
beats it. The style axes visible in the recordings that it currently ignores:

- economy-first vs early-army (the owner plays pure economy to ~t1700; the AI's AllIn test misreads
  this as weakness)
- expansion geography (does the opponent take the far mine rows, or sit at home?)
- composition *mix* vs composition *spike* (uniform rock/paper/scissor vs committing to one type)
- production capacity growth rate (buildings/minute — the term that actually decides the match)
- queue behaviour (deep stacks vs shallow-and-expand)

This is the Halo Wars 2 "named strategies" shape: a small set of recognisable opponent plans, each
with an authored response, chosen from what is observable on the board. It is *not* opponent
modelling in the machine-learning sense and needs no storage.

### 3. Blast radius — top tier only

`AiController` keeps its current FSM for Easy/Medium/Hard. `PerhapsImpossible` branches to a new
decision procedure. Accepted cost: duplication, and two code paths to maintain. Bought: no
re-measurement of the ladder, no risk to the beginner pacing work (#155), and total freedom on the
top tier's design.

Open question this raises: several defects are *not* strategy, they are plainly worse-than-human
mechanics that every tier suffers from — above all queueing one unit at a time when a human's x5
plate queues five. The user chose "top tier only", so the default is that lower tiers keep the
defect. **Flag for round 2:** whether the queue-batching fix in particular should be shared, since
leaving it top-tier-only is a large, deliberate handicap on the lower rungs rather than a tuning
choice.

## Open questions

- Which scalar(s) to surface, and where (post-match screen vs the opponent selector row).
- What decision procedure replaces the FSM (pending the macro-optimization research).
- What the AI's *fair action budget* is — measured human rate is 60–115 events/min at 3.7 units per
  queue command; the AI currently runs 78–180 events/min at 1.1 units per command.
- Whether the queue-batching fix crosses the tier boundary.
