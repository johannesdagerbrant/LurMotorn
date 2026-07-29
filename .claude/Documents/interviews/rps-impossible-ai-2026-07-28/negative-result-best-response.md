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

Two corollaries worth carrying forward:

- The payoff function needs a **cost-amortisation** term, not a cost-division term. Something closer
  to "expected value per building-lifetime" than "value per gold spent today".
- `--aivs` should be treated as a **regression guard** ("did I break the ladder?"), never as a
  fitness function. It measures the wrong opponent.
