# The 4000-gold stall — a reproducible hard exploit of the top tier

**Date:** 2026-07-28 (second batch, matches 19:52–21:57)
**Source:** 31 further recordings pulled from the A14. 19 genuinely played, **19–0 to the human.**
Combined with the first batch: **31–0 across the day.**

## The human found a faster kill, and it is an exploit, not a grind

Match length collapsed over the evening as the owner converged on a specific line:

| Window | Match length | His camps | His soldier bldgs | His first soldier bldg |
|---|---|---|---|---|
| 19:52 – 21:04 | 320–352 s | 26–35 | 27–37 | t1680–1782 |
| 21:09 – 21:57 | **159–208 s** | 10–19 | **4–11** | **t970–1180** |

He roughly halved the kill time while building *a third* of the economy and *a fifth* of the army.
That is not better execution of the same plan — it is a different, cheaper win condition.

## The mechanism: the AI banks ~4000 gold and stops playing

Measuring the longest continuous stretch in which the AI emitted **no input event at all**, and the
gold it was holding at that moment:

| Match | Length | Longest AI silence | Gold held | % of samples idle |
|---|---|---|---|---|
| 211753-4 | 179 s | **18.0 s** | **3695** | 33% |
| 212353-7 | 208 s | 18.0 s | **3950** | 32% |
| 212726-8 | 179 s | 16.0 s | **3990** | 34% |
| 213031-9 | 199 s | 16.0 s | **3870** | 31% |
| 213356-10 | 175 s | 20.0 s | **3945** | 39% |
| 213806-11 | 173 s | 22.0 s | **3715** | 41% |
| **214108-12** | **159 s** | **20.0 s** | **3945** | **45%** |
| 214353-13 | 180 s | 20.0 s | **3945** | 38% |
| — the slow matches, for contrast — | | | | |
| 203043-11 | 319 s | 12.7 s | 0 | 15% |
| 204427-2 | 324 s | 18.0 s | 35 | 18% |
| 215332-20 | 203 s | 4.0 s | 45 | 16% |

The number is unmistakable: **3695–3990 gold, against a Scissor building price of exactly 4000.**
Every fast kill has it; no slow match does. In the fastest match the AI is idle for **45% of the
whole game**.

## Why: `counter_chest` deadlocks against the most expensive building

`PerhapsImpossible` ships `counter_chest = 100`. In `AiController::DecideEvents` (~line 715):

```cpp
const bool SaveForCounter = ChestPct > 0 &&
                            (State_ == EState::Reacting || State_ == EState::AllIn) &&
                            SurveyType(S, MyTeam_, Soldier).Owned == 0;
const int32_t Chest = SaveForCounter ? BuildingCostFor(S.Cv, Soldier) * ChestPct / 100 : 0;
const int32_t Spendable = Gold > Chest ? Gold - Chest : 0;
```

and the never-stand-idle cart fallback (~line 737) **applies the same `Spendable`**. So while the AI
wants a counter type it does not yet own, it reserves that building's *full* price and everything
below it is priced out — soldiers *and* the cart fallback that exists precisely to prevent silence.

The building costs are wildly asymmetric: **camp 600, rock 1000, paper 2500, scissor 4000.** And
`CounterTo(paper) = scissor`. So:

> **Make Paper your dominant soldier type → the AI locks onto Scissor as its counter → it reserves
> 4000 gold → it cannot afford a single cart or soldier → it stands still for ~20 s while you take
> the map.**

Confirmed in the trace of the 159 s match: the AI's countered type flips to `paper` at t1061, its
worker count then freezes at **37 for 24 seconds** while gold climbs 1200 → 2325 → 3645, and its
building count is stuck at 4 while the human goes 6 → 18.

```
 t1141 | human  55 w / 1 s /  8 bldg |  AI  37 w / 0 s / 4 bldg  gold 2325  Reacting paper
 t1221 | human  60 w / 1 s / 11 bldg |  AI  37 w / 0 s / 4 bldg  gold 3645  Reacting paper   <-- frozen
 t1301 | human  68 w / 6 s / 12 bldg |  AI  37 w / 3 s / 5 bldg  gold  810  Reacting rock    <-- finally buys it
```

## Why this matters beyond the bug

1. **It is the cheapest possible refutation of argmax counter-selection.** The AI commits its entire
   treasury to countering the *mode* of the enemy army, and the mode is the one thing the human
   controls for free. A best-response to the whole mixture (`argmax_i (A·q)_i`, research §1) cannot be
   steered this way, because no single enemy type dominates the decision.
2. **The `Reacting` idle rate — 31–45% of samples — reframes the whole diagnosis.** The measured
   "1.1 units per queue command" from the first batch understates the problem: a large fraction of the
   time the AI is not queuing *anything*.
3. **The code already knows about this failure mode and the fix did not cover this case.** The comment
   at line 730 documents an earlier instance ("it emitted NOTHING on 1121 ticks in Reacting and ended
   on 3930 unspent gold") and adds the cart fallback — but routes the fallback through the same
   `Spendable`, so the chest still starves it. The stall survived its own fix.
4. **The owner-bot must encode the fast line, not the boom.** The strategy worth reproducing as a
   sparring partner is the 21:09-onward one: ~14 camps, first soldier building at ~t1000, Paper-led
   mixed composition, ~175 s kill.

## Immediate implications for the plan

- Formula fix #1 (mixed best response) is now the **highest-priority** item, not merely the most
  elegant — it removes the steering handle.
- Add a fifth, smaller fix: **the reserve must never price out the cheapest useful action.** Cap the
  chest at some fraction of income-per-second, or exempt the cart fallback from it, or both. A
  reserve that can stop the AI from spending *at all* is strictly worse than no reserve.
- The asymmetric building prices (1000 / 2500 / 4000) mean any rule denominated in "a building's
  price" behaves 4× differently depending on which counter is selected. Prefer rules denominated in
  income or in unit cost.
