# Flight-recording analysis — why "Perhaps Impossible" loses

**Date:** 2026-07-28
**Source:** 33 recordings pulled from the Galaxy A14, `com.lurmotorn.onlyrps/files/rps-match-20260728-*.rec`.
19 matches over 150 s; 17 "genuinely played" (human still issuing input in the final quarter).
Build fingerprint on every file: `504f25c52f70-dirty+Development`.

## Scoreboard

| Tier | Played to a result | Human | AI |
|---|---|---|---|
| 3 — Perhaps Impossible | 14 | **12** | 2 (both forfeits) |
| 2 — Hard | 1 | 1 | 0 |
| 0 — Easy | 5 | 4 | 1 (forfeit) |

The two tier-3 "AI wins" (`171936-2`, `160803-7`) are not wins. In both the human stops issuing
input entirely partway through — in `171936-2` the human's last event is at t≈2420 of a 3982-tick
match, after which their gold climbs to 42 150 while their army decays 212 → 0 untouched. That is
a phone put down, not a defeat.

## Aggregate over the 17 genuinely-played matches

| Metric | Human | AI |
|---|---|---|
| Mining camps placed | 19.8 | 8.9 |
| Soldier buildings placed | 16.7 | 11.2 |
| Gold spent on buildings | 50 271 | 34 365 |
| Total units queued | 1 274 | 484 |
| **Units per queue command** | **3.7** | **1.1** |
| Buildings alive at the end | 34.2 | 7.9 |
| Soldiers alive at the end | 265.2 | 20.4 |
| Workers alive at the end | 301.8 | 109.8 |
| Input events per minute | 60–115 | 78–180 |

The AI *acts more often than the human* and still queues 2.6× fewer units, spends 1.5× less on
buildings, and finishes with 4.3× fewer buildings and 13× fewer soldiers.

## The five mechanisms behind the loss

### 1. Spending bandwidth — the AI's queue command is worth 1 unit, the human's is worth 3.7

`AiController::DecideEvents` emits **at most one `InputEvent` per tick**, and the queue branch tops
up only *the shallowest building of the wanted type* to `queue_depth`
(`AiController.cpp:698`, `N = Depth - Cap_.Queue`). In steady state every building is already
near depth, so `N` is 1. The human instead taps the x5 plate on building after building.

Measured: AI 484 units over ~440 queue commands (1.1 each); human 1 274 over ~340 commands (3.7
each). The AI issues **more** commands and converts **less** gold. This is not an advantage the
human has been given — the x5 plate is a button the AI structurally never presses. It is the
single largest gap and it is pure implementation, not balance.

Consequence: the AI is never gold-limited in the mid-game (mean float ≈ 1 100, peaks 7 500) — it is
*spend-rate* limited. Raising its income would change nothing.

### 2. Economy expansion — it mines the home cluster and stops

Mining camps: 8.9 vs 19.8. Placement depth from own baseline (map is 240 deep, midfield 120):

| Building | Human median / p90 | AI median / p90 |
|---|---|---|
| Mining camp | 27 / **121** | 21 / **37** |
| Rock | 102 / 179 | 66 / 104 |
| Paper | 49 / 174 | 101 / 192 |
| Scissor | 132 / 202 | 106 / 174 |

The human's camps reach midfield; the AI's stop at depth 37. `AiBestMineTarget` deliberately picks
the *nearest* unclaimed deposit (changed from furthest-forward for good reasons — a forward camp
dies), but combined with the `Gold >= Price × AiExpandGoldFactor/100` gate (1 200 for a 600 camp)
and one action per tick, the AI simply never gets round to the far rows. It captures roughly a
third of the map's gold.

### 3. Production capacity — 8 buildings vs 34, and production is flat per building

Since #132 each building produces one unit per `build_time` regardless of how many you own, so
**building count *is* army throughput**. The human ends with 34.2 buildings to the AI's 7.9. The
AI *places* 20 and *keeps* 8 — its buildings are being razed while the human's are not, which is
downstream of (1) and (3) compounding.

### 4. The AllIn trigger is a false positive, and it fires at the worst moment

`State_ = AllIn` when `MySoldiers - EnemyArmy >= AllinLead + Incoming`. In the long matches the
human builds **zero soldiers until ~t1700** — they are pure economy. So at t≈1620 the AI has ~12
soldiers, the human has 0, the test passes, and the AI enters AllIn — where `Want` is soldiers
*exclusively*. It stays there 40–70 s.

Trace from `193426-6` (a 346 s human win):

```
 t1541  human 168 workers / 0 soldiers / 19 bldg    AI 111 / 3 / 15    Building
 t1621  human 192 / 0 / 21                          AI 134 / 12 / 17   AllIn   <-- commits on a 12–0 "lead"
 t1861  human 268 / 25 / 26   bank 7 310            AI 202 / 68 / 19   AllIn
 t2582  human 435 / 252 / 36  bank 50 355           AI 404 / 118 / 25  Reacting
 t3457  human 463 / 581 / 63                        AI  17 /  30 / 10  Reacting  -- human wins
```

The state that is supposed to mean *"I can win right now"* actually detects *"the human has not
started their army yet"* — i.e. it fires precisely when the human is compounding economy, and
answers by stopping its own economy. The 12 soldiers achieve nothing against 318 carts.

### 5. Single-counter policy against a genuinely mixed army

The AI locks onto the **argmax** enemy soldier type and builds only its counter, with hysteresis.
The human's soldier-building mix across the 17 matches is rock 7.1 / paper 4.7 / scissor 4.9 —
close to uniform, and each match ends with all three types on the field.

With `counter_mult = 3`, a pure counter against a uniform mix is 3× against a third of the enemy,
1× against a third and 1/3× against a third. Argmax is the wrong operator here: the correct
response to a mixed composition is a **mixed composition**. The recordings show the AI switching
its countered type 1–7 times per match, chasing the human's shifting argmax and never being
correct against the whole army.

## Secondary observations

- **The features written for this tier are switched off in it.** `wave_lead = 0` and
  `miner_queue = 0` on `PerhapsImpossible` — the two knobs added on 2026-07-27 from the owner's own
  described strategy ("expand mining until the wave is nearly at my camp, *then* commit a cluster
  of counters with stacks maxed") are disabled at the top tier and only shape the lower rungs.
- **The tier axis is exhausted.** Difficulty in the current design is carried by staleness /
  precision / cadence / production volume. `PerhapsImpossible` already runs staleness 0, precision
  1, `max_buildings` 0 (unlimited). There is nothing left to turn up — the next tier cannot be a
  parameter change, it has to be a different *decision procedure*.
- **The AI has no cross-match memory.** The owner learned its script in a day; it never learned his.
- Cadence 12 (1.2 s between re-decisions) is well inside human reaction time, so the reaction-speed
  budget is not where the remaining strength is either.

## What this rules out

- Giving it more income, faster reactions, or a bigger building cap. It is neither gold-limited nor
  reaction-limited, and its cap is already unlimited.
- Any fix that is only a CVar retune. The five mechanisms above are structural: one action per tick,
  argmax counters, a commitment test with the wrong sign, and an economy that stops at the home rows.
