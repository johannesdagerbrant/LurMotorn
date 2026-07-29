# Negative result: best-response composition measured WORSE than argmax

**Date:** 2026-07-28
**Status:** implemented, measured, **reverted**. Patch kept at
`<scratchpad>/best-response-attempt.patch` (117 lines).

## What was tried

Formula fix #1 from the plan: replace `CounterTo(argmax_j q_j)` — counter the single most numerous
enemy soldier type — with `argmax_i (A·q)_i`, the best response to the whole observed enemy
composition. Gated to `PerhapsImpossible` only, so the measured ladder below it was untouched.

The payoff `A[i][j]` was derived from the sim's own stats rather than a hand table, so it re-derives
itself when balance is tuned:

```cpp
MatchupValue(i, j) = EffDmg(i→j) * MaxHp[i] − EffDmg(j→i) * MaxHp[j]
EffDmg(x→y)        = Attack[x] * (UnitTable[x].Beats == y ? CounterMultiplier : 1)
```

Hysteresis was preserved in its original currency (the challenger must beat the incumbent by
`K.Hysteresis` *enemy units' worth* of score, not a bare epsilon).

## Measured result — it got worse, twice

`--aivs`, 16 matches per pairing, on the new (post-CVar-promotion) defaults:

| Version | Impossible vs Hard | Impossible vs Medium |
|---|---|---|
| argmax (shipped) | **15–1** | — |
| best response, raw strength | 12–4 | 14–2 |
| best response, normalised per 1000 gold of building cost | **11–5** | 13–3 |

## Why the first cut failed, and why the fix for it also failed

**Raw strength systematically over-picks Scissor.** With the current stats (rock 100 hp / 6 dmg,
paper 40/8, scissor 60/15, multiplier 3), the matchup values are wildly asymmetric:
Scissor-into-Paper is **+2380** while Rock-into-Scissor is only **+900**. Against a near-uniform army
the score picks Scissor — which is the **4000-gold** building, four times a Rock building's price,
while the AI is capacity-bound. So it kept buying the most expensive answer.

**Normalising by building price over-corrects in the opposite direction.** Dividing the score by the
building's price makes it prefer cheap Rock even when the enemy army is *actually* near-pure Paper
and Scissor is the correct hard counter. That is worse, not better: it talks the AI out of the
expensive-but-right answer.

Both failures are the same missing term: **a building's cost should be amortised over the units it
will produce over its lifetime**, not charged against a single engagement's payoff. Neither
formulation models that, so both are wrong in a direction that depends on the board.

## The real lesson — this reorders the plan

The theory behind this change is sound and I still believe it (argmax discards every enemy type but
the mode; it is a period-3 orbit against an adapting opponent; and it is the steering handle behind
the 4000-gold exploit). What was wrong was the *process*: I was iterating on a value function using
`--aivs` as the scoreboard, and `--aivs` cannot answer the question the change is for. The change
exists to remove a **human**-exploitable handle. Against another AI that also plays argmax, no such
exploitation happens, so the harness can only see the change's costs and never its benefit.

Shipping it anyway on the strength of the literature would have been exactly the failure this
research warned about — the AoE4 mistake of changing a number because it should work, rather than
because it measured better.

**Therefore: Phase 4 (the owner-bot) must come BEFORE Phase 2 (the formulas).** The original plan had
them the other way round, and that was wrong. No composition change should be judged until there is
a sparring partner that plays like the person who beat it 31–0.

## The rescue path (late research, 2026-07-29)

The industry strand returned after this was written, and it contains the mechanism that fixes the
over-picking directly. **The good production AIs do not pick a type at all — they hold a
distribution and cap each type's share.** Three independent implementations:

- **Zero-K / BAR CircuitAI** (`data/config/response.json`): units carry *roles*; a scouted threat
  multiplies the base distribution rather than replacing it, and every role carries a
  **`max_percent` ceiling on its share of the army** (riot ≤ 45%, transport ≤ 30%). Production
  *samples* the distribution instead of taking an argmax. A hard cap on any single counter's share is
  precisely what stops the AI hard-countering into a composition that then hard-counters it.
- **0 A.D. Petra** (`attackPlan.js`): a least-satisfied-quota scheduler — sort candidate types by
  `(have + queued) / targetSize − priority`, train whichever is proportionally furthest from quota,
  and push satisfied types to the bottom with `+= 1000`. Mixes by construction; cannot over-invest;
  one knob (`priority`) biases the mix without breaking it.
- **Age of Empires IV** (GDC 2022): pick the unit that most improves a learned combat-fitness oracle
  `ComputeCF(mine + candidate, scouted enemy)`. Because CF is nonlinear over a heterogeneous army,
  the third copy of the best counter is worth less than the first copy of the second-best — **the mix
  emerges from the nonlinearity with no explicit "build a mix" rule.**

All three avoid the trap my patch fell into, and for the same reason: **the failure was taking an
argmax of a scalar score at all.** Scoring types and picking the best one keeps *every* pathology of
the argmax it replaced — it just changes which single type gets over-bought. Petra's quota scheduler
is the cheapest of the three to implement here and needs no value function at all.

Also relevant to whether the 10% target is even reachable: **Prismata's fair AI — built by David
Churchill, in a turn-based perfect-information game with no micro, running 3 s of MCTS — reached the
top 25% of ranked humans and no further**, with experts still beating it consistently. That is the
best-case empirical ceiling for a hand-built non-cheating RTS AI, and it is *below expert*. Worth
holding in mind before promising 90%.

Two corollaries worth carrying forward:

- The payoff function needs a **cost-amortisation** term, not a cost-division term. Something closer
  to "expected value per building-lifetime" than "value per gold spent today".
- `--aivs` should be treated as a **regression guard** ("did I break the ladder?"), never as a
  fitness function. It measures the wrong opponent.
