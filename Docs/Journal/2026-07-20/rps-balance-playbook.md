# RPS — #84 Balance-Pass Playbook

*2026-07-20. How the balance pass runs: measurable, mostly headless, human evenings reserved for feel. The core insight: the recorder format + `SimRunner::InputFn` make balance a **data problem** — a scripted strategy is just an input schedule, a match is a deterministic replay at sprint speed, and every metric extracts from `(mask0, mask1)` sequences. Desktop runs a strategy-vs-strategy matrix in seconds per match; phones are for humans.*

*Decisions in: #6 = B (7–10 min median) and #8 = B — **big battles are the ambition**, with playtest evidence already in hand: mines deplete ~2 minutes in and carts trek cross-map for the remnants. So sweep #1 runs first and its direction is known: many more mines (density, not just capacity). Re-run the matrix after boid slices A/B land (spread formations change effective combat density) and after Perf-5's fallback (changes engagement timing).*

---

## 1. North-star metrics (auto-extracted per match from the replay)

| Metric | Definition | Target (decision-#6 B assumed; shift if A/C) |
|---|---|---|
| Match length | ticks → minutes | median 7–10 min, p90 ≤ 14 (sudden-death knob if p90 blows) |
| Time-to-first-soldier | first non-miner spawn tick | 45–90 s (opening has a real decision) |
| Action cadence | presses/min over match thirds | rising curve (the acceleration thesis visible in inputs) |
| Peak army size | max simultaneous soldiers/side | decision-#8 B: mid-hundreds/side (pin the exact number after the first post-scale-up playtest) |
| Type entropy | distribution of soldier types built per match | no type < 15% across the matrix (all three exist) |
| Comeback rate | behind in gold+army at half → wins | 15–30% (thesis says snowball, not coinflip; 0% = runaway) |
| Economy duration | tick the last mine empties vs match end | mines empty in the final third (starvation is an endgame, not a wall) |
| Cart trip distance | median deposit→nearest-live-mine distance, per match third | stays local all game — carts never trek cross-map (the reported failure mode) |

Harness: a small `LUR_INTERNAL` tool (desktop) that runs N seeded matches per strategy pair via `InputFn` schedules, replays at sprint speed, and emits one CSV row per match with the table above. The rig can run the whole matrix unattended.

## 2. Scripted strategy roster (the degenerate checklist)

Each is an `InputFn` schedule, parameterized by timing knobs so the matrix probes a small family, not one line:

1. **Standard** (the reference): 3→6 miners, then mixed soldiers reacting to nothing (fixed rotation).
2. **All-in rush**: zero extra miners; soldiers from tick 0 (StartGold buys one + starting miners keep digging).
3. **Pure boom**: miners only until X gold, then flood one deep stack (the snowball's best case).
4. **Single-type spam** ×3: all-Rock / all-Paper / all-Scissor.
5. **Queue-snowball**: boom into one type stacked to the acceleration cliff (tests `BuildTicks/count` runaway).
6. **Miner-raid focus**: minimal army, timed pushes when the opponent's carts cross midfield (approximated open-loop by timing).
7. **Turtle-starve**: safe-mines only, minimal army, bets on out-lasting (tests whether finite gold punishes passivity).
8. **Mirror**: every strategy vs itself (stall/draw detection; simultaneous-annihilation frequency).

## 3. Pass/fail gates (a knob change ships only if all hold)

- All-in rush wins ≤ **60%** vs Standard family (rush is scary, not dominant) and ≥ **25%** (openings stay honest).
- Pure boom loses ≥ **60%** to any timed pressure family (greed is punishable).
- Each single-type spam loses ≥ **65%** to the reactive mixed family (the triangle works at army scale — *the game's thesis*).
- No strategy pair's match length p90 exceeds the cap; mirror matches draw < **10%**.
- Type entropy target holds across the winning strategies (no dead unit).
- Comeback rate inside 15–30% for Standard-vs-Standard with one injected early blunder (scripted: skip 30 s of production).

## 4. Tunables sweep order (one knob per pass; expected sensitivity, high → low)

1. **Mine density first** (`NumMines` up a lot, layout regenerated; `MineGoldCapacity` follows) — decision #8B + the cart-trek evidence make this the opening move, not a sweep: raise density until the cart-trip metric holds all game, then sweep total gold for the army-ceiling/length targets. (Watch: `MineGold` is hashed and sized by `NumMines` — build-locked as usual; `AddMineRepel` and nearest-mine scans scale with mine count — confirm on the stress scene.)
2. **CounterMultiplier** (3 → 2.5/3.5 probes) — the triangle's sharpness; watch gate 3.
3. **Miner economics** (`DigTicks`, `CarryCapacity`, cost 30/build 30) — boom-vs-rush equilibrium; watch gates 1–2.
4. **Soldier costs/build asymmetries** — currently symmetric 50/50; only touch if entropy fails with symmetric numbers.
5. **`WorldHeight`** — tempo/raid dial (longer marches = softer rushes, more raid windows).
6. **`StartGold`/`StartMiners`** — opening decision space; small moves only.
7. `PerTypeQueueCap` — only if the snowball runs away *and* gold-total tuning can't tame it (protect the thesis first — decision #7).

Method per pass: pick the failing gate → sweep the one knob across 3–5 values × full matrix × ≥20 seeds each (headless minutes) → take the value that fixes the gate with the smallest movement → human evening confirms feel → living-doc changelog entry (value + gate it fixed).

## 5. Human playtest evenings (what the matrix can't measure)

Fixed build per evening; 5+ matches; the ship-cycle culture as-is. Structured notes per match (one line each): who felt ahead and when · was the counter *read* off the field (the boid-readability check) · rage moment · best moment · length feel. Capture: recorder files kept for every match (any anomaly replays on desktop); the rig pulls them automatically. Feel-only questions the matrix can't answer: does snowball feel *earned* or inevitable; does raiding feel like counterplay or grief; is the contested row worth it.

## 6. Deliverables

- [ ] Matrix harness + CSV metrics (one `LUR_INTERNAL` tool, desktop).
- [ ] Strategy schedules 1–8 as data (tick→mask tables), committed with the harness.
- [ ] Baseline report at current tunables (the "before" picture) — run first, before touching anything.
- [ ] **Comeback lever (decision #7B): pick + prototype the mechanism.** Candidates, all deterministic and one-knob: (a) small build-time discount when behind in army value, (b) mine-income bonus when behind, (c) flat cost discount when behind. Gate-tested like any other knob; the comeback-rate target moves to 25–40% once the lever exists.
- [ ] Gate table wired as a script that reads the CSV and prints pass/fail — a knob change's review is one command.
- [ ] Living-doc changelog entries per shipped knob change.
