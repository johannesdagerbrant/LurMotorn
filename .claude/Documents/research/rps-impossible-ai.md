# Research: a near-unbeatable, non-cheating RTS AI for a two-verb macro game

**Date:** 2026-07-28
**For:** RPS `PerhapsImpossible` tier redesign (see
`.claude/Documents/interviews/rps-impossible-ai-2026-07-28/`)

## Problem statement

Build an AI for `Games/RocksPapersScissors` that beats a strong, motivated human ~90% of the time
while playing by identical rules: same two input events (`EventPlaceBuilding`, `EventQueueUnits`),
same income, no hidden state, no superhuman action rate. The game has **no unit control at all** —
movement and combat are emergent — so the entire contest is build order, economy, placement, and
composition.

Constraints: pure function of `(Sim, Tick)` + seeded `SplitMix64`; integer/`Fixed` only; no
allocation; runs inside a 10 Hz tick on a phone alongside the sim.

---

## The single most important finding

**There is no literature on a game shaped like this, and the RTS search literature mostly solves a
problem we do not have.** Nearly all of it exists to tame a per-unit combinatorial action space
`L^U`. We have the opposite: a tiny branching factor over a long horizon. The closest architectural
match anywhere is **Prismata's Hierarchical Portfolio Search** (Churchill & Buro, AIIDE 2015), which
is also micro-free — and it is turn-based.

And the sobering companion finding: **search over scripts is repeatedly measured as no better than
the best single script.** Puppet Search's mean win rate was *statistically indistinguishable* from
its best script; Nash strategy selection scored exactly its guaranteed 50.5%; on 8×8 µRTS a plain
hardcoded WorkerRush beat every search agent. Two independent papers, same conclusion.

**Implication: do not build a searcher. Build a script whose arithmetic is correct.** The wins
available here are in replacing four wrong formulas with four right ones — each of which is a handful
of integer operations.

---

## Option comparison

| Option | Complexity | Risk | Fit | Integration cost |
|---|---|---|---|---|
| **Fix the four formulas** (best-response, Lanchester-with-production, worker saturation, capacity-from-income) | Low | Low | Excellent | ~1 file |
| **Portfolio of named plans + scouted plan selection** (Halo Wars 2 / Prismata shape) | Medium | Low | Excellent — also gives the tier ladder for free | New decision path |
| **Regret matching over composition** | Low | Medium | Good, but may be overkill vs. closed-form Nash | ~30 lines |
| **Build-order search (BOSS-style branch & bound)** | High | High | Poor — 10⁵ nodes, 3 s search instances | Large |
| **MCTS / learned evaluation** | High | High | Incompatible — 271 µs per playout, floats, heap | Prohibitive |

---

## The four wrong formulas, and their replacements

### 1. Counter selection — `argmax(A·q)`, not `counter(mode(q))`

Current code picks the **dominant** enemy soldier type and builds its single counter
(`AiController.cpp` ~line 344). This is wrong three separate ways:

- **It is not the best response to a mixture.** Best-responding to enemy composition `q` is
  `argmax_i (Aq)_i` — an expectation weighting *every* enemy type. "Counter the mode" is
  `argmax_i A[i][argmax_j q_j]`, which discards everything except which type is most numerous. Over
  200 000 random draws of (asymmetric 3-cycle matrix, enemy mixture), the two **disagree 37.2% of
  the time**. *(Verified numerically by the research agent, not a literature result.)*
- **Against an adapting opponent it is a period-3 orbit.** Two sides best-responding to each other's
  last pure choice cycle `(2,2) (3,3) (1,1) …` forever, each perfectly countered at every step.
- **"Build more of what's winning" spirals outward.** That intuition is discrete replicator dynamics,
  which in a 3-cycle is *unstable* — simulated 200 000 steps drove the strategy from near-uniform out
  onto a simplex vertex (a pure, maximally exploitable strategy) at every step size tested.

**Replacement, ~12 integer ops:** `argmax_i (A·q)_i` where `q` is the observed enemy composition.

**And the mixed target has a closed form.** For a 3-cycle with margins `m₂₋₁ = c`, `m₃₋₂ = a`,
`m₁₋₃ = b`, the unique Nash equilibrium is **`p ∝ (a, b, c)`** — play each type in proportion to the
strength of the matchup it is *not* part of. Three multiplies and a normalise; no LP solver.

★ **Crucially, no RNG is needed.** The usual objection to mixed strategies — sampling breaks
determinism — does not apply, because **our army composition *is* the mixture**. Producing 40/35/25
of three types realises the mixed strategy exactly, with zero variance. This is the
continuous-allocation setting, not the one-shot-sample setting, and it makes the whole
game-theoretic apparatus deterministic-friendly. That is an unusually clean fit for our constraints.

Corroborating industry practice: **Homeworld 2** does the enemy-facing version well
(`DetermineAntiChassisDemand`): net uncovered strength (subtract counters already owned, clamp at
zero), per-chassis urgency scales, normalise to a *share*, then two hysteresis bands (>70% → +3,
>35% → +1.5) — so a 50/50 enemy mix produces a 50/50 counter mix and a 20% sliver is ignored.
**Supreme Commander's** Sorian AI gets a mixed composition a different way: ceilings on each support
role relative to your core (arty ≤ 30% of direct-fire, AA ≤ 15%, scouts ≤ 10%) rather than a counter
matrix at all.

### 2. Attack commitment — Lanchester *with a production term*

The current `AllIn` test is `MySoldiers - EnemyArmy >= AllinLead + Incoming` with
`Incoming = FoeCombatBldg * WalkTicks / Bt`. The intent is right (out-kill their replacement rate);
the arithmetic is a crude linear approximation, and the measured result is a false positive that
fires when the human simply hasn't started their army.

**The correct closed form.** Lanchester square law with reinforcement (Morse & Kimball 1946;
Vandenbroucke, FRB St. Louis WP 2023-007) gives a **division-free integer predicate**:

```
u = α·A − ρ_B          // α = our kill rate, ρ_B = their reinforcement rate
v = β·B − ρ_A
if  β·u·u > α·v·v  :  winner = (u > 0) ? A : B
else               :  winner = (v > 0) ? B : A
```

Six multiplies and a compare. *(Verified by the research agent against direct ODE integration on 600
randomised parameter sets, 600/600 correct, and it reduces exactly to the classic `αA² > βB²` when
both reinforcement rates are zero.)*

Three consequences that matter here:

- **The deciding quantity is not army size — it is distance from the saddle point `A* = ρ_B/α`**,
  which depends on *the enemy's production rate divided by our kill rate*. "Am I strong enough to
  attack?" is unanswerable without estimating enemy reinforcement.
- ★ **In our game, `ρ_enemy` is directly observable: production is flat per building, so enemy
  reinforcement rate *is* their soldier-building count.** This is the single highest-value piece of
  scouting information in the game, and it is a *count*, not a composition. The code already counts
  it exactly (`FoeCombatBldg`) and justifies doing so (a building is static and huge — a human sees
  it the moment they look). So the input is already there; only the formula is wrong.
- **Long games are won by `ρ`, short games by initial army.** In simulation a side starting 10-vs-30
  with 2× production still won — but took ~10× longer than a typical engagement.

**Warning from the same literature:** rebuy rules that are *nonlinear* in current strength produce
coupled logistic maps and **provable chaos** — fractal win/lose basins, period doubling (McCartney,
*Physica A* 2021). Not a desync risk in a lockstep sim, but it makes tuning irreproducible and
difficulty tiers unpredictable. **Keep the rebuy rule constant or linear in strength.**

### 3. Worker saturation — the missing diminishing-returns term

★ **This explains the 520-cart pathology directly.** From the COEP paper (Justesen & Risi, GECCO
2017): without a falloff term, **more workers is always monotonically better and every optimizer
degenerates into a worker-only build.** Our AI has no such term and ends matches with 520 carts and
25 buildings.

Their model is one integer divide: workers 1–10 full rate, 11–20 half, 21–30 a third —
`Σ rate/⌈k/10⌉`. The cleaner community model, which maps onto our mining camps exactly: decompose a
cart cycle into `x` = dig time, `y` = round trip, `z` = wait; then per-worker rate is `5/(x+y+z)`,
saturated patch rate is `5/x`, and **workers to saturate a deposit is `(x+y)/x`**. A camp placed near
a deposit shortens `y`, which raises *both* the per-worker rate and the saturation count — so camp
placement and worker count are the same decision.

Measured payback numbers (community, StarCraft): a worker pays for itself in **92 s** below
saturation and **153 s** at saturation — a 66% lengthening, which is simultaneously the economic
argument for expanding and the thing that creates the three-phase shape of a real build order.

Also worth stealing, one subtraction: COEP's expansion signal `numBases·14 − mineralWorkers`, which
punishes both expanding with too few workers *and* not expanding with many.

### 4. Production capacity — scale it off income, not off a gold threshold

Current rule: place another building when `Gold >= Price × AiExpandGoldFactor/100` (200%). That is a
*wealth* test, and it is why capacity plateaus at ~25 buildings while income keeps compounding.

Every good AI examined uses **income ÷ per-facility drain** instead:

- **Sorian (SupCom):** add a factory only when `massIncome × 10 > Σ factoryDrain` at per-tier weights
  6/15/22.5, plus a hard per-base cap `{Land 4, Air 5}`.
- **Steamhammer (Brood War):** `maxSensibleHatcheries = nDrones/3 − gasDrones` — **~1 production
  facility per 3 mining workers** — with a separate cap on *concurrent* construction that scales with
  economy size (`nDrones > 30 ? 4 : nDrones > 20 ? 3 : 2`).
- **AMAI (WC3):** `requiredFactories = income / (unitCost / buildTime)`, clamped by difficulty.
- **C&C (from EA's open source):** `count < ceil(ratio × totalBuildings) && count < limit`.

Since our production is flat per building, `buildings × unitCost / buildTicks` *is* our drain, and the
target is simply the count at which drain ≈ income. Our human's 34 buildings against 302 workers is
almost exactly Steamhammer's 1-per-3 rule; the AI's 8 buildings against 110 workers is not.

---

## The objective function: integrate, don't sample a horizon

Two independent teams converged on this, which makes it the most trustworthy single result in the
literature:

- **Churchill, Buro & Kelly (IEEE CoG 2019), BOSS-IM.** Maximising *army value at a horizon T* is
  provably exploitable — the optimal plan more than doubles its army value in the last 200 frames, so
  an opponent optimising for an earlier `T` attacks and wins. Replacing it with the **integral of army
  value over time** (`I += δ × V` per event — one multiply-add) fixed it, **and endogenously produced
  the economy→army transition with zero hand-authored ratio**. Same bot, only the module swapped: win
  rate vs the top-10 AIIDE-2017 bots **27.7% → 37.2%**, and **vs the top 5, 14.4% → 32.8%**. Bigger
  gains against stronger opponents. This is the *only* case in this literature where a macro optimizer
  was dropped into a real bot in a real tournament with a controlled, significant improvement.
- **Justesen & Risi (GECCO 2017)** hit the same failure two years earlier and fixed it the same way
  (discount 0.9 over the trajectory), explicitly because otherwise "a build order with a very strong
  economy in the beginning and a large unit production in the end would give a high fitness, even
  though the player has no army and is defenseless during most of the evaluated period."

★ **That failure description is literally our AI's losing pattern in reverse** — and it means the
economy-vs-army switch does not have to be a hand-tuned `soldier_ratio` at all. It falls out of
integrating army value.

**Supporting theory (economy→army as a control problem).** The worker-vs-army allocation is formally
identical to "workers vs. reproductives" in eusocial-insect colonies (Cohen 1971; Macevicz & Oster
1976). Because the objective is *linear in the allocation*, Pontryagin gives **bang-bang with exactly
one switch**, at `t* = T − 1/r` — a fixed time before the end, independent of horizon length and
current worker count. Two caveats: it needs a known horizon `T` (the principled escape is to let
*scouting* set `T` = estimated time until you must fight), and what turns the hard switch into a
graded ramp is precisely the **diminishing returns on economy** from §3 above. Note these two
literatures — turnpike control and RTS build-order search — **do not cite each other**; the bridge
looks real but has no RTS validation.

---

## Production references: how shipped games build a hard tier

**The headline industry finding, and it is bleak:** in almost every shipped RTS examined, *the hardest
fair tier and the hardest cheating tier run identical code*. Difficulty is a multiplier applied
outside the decision logic.

- **StarCraft II** (read from Blizzard's shipped Galaxy source, `SC2Mapster/SC2GameData`): tiers are
  built by **subtracting** capability down from Elite — a global APM cap below Very Hard, a combat-APM
  cap below Elite, a tactical delay ladder of **10 / 7 / 5 / 3 / 2 / 1 / 0** by tier, deliberate
  mis-targeting at Medium and below, a targeting-quality limit of `1 << difficulty` — and then
  **adding** cheats above it (vision, then a harvest multiplier ramping to 1.5× / 2.0× over 20
  minutes). Elite is the honest ceiling, and its Elo is ~603 on AlphaStar's anchored scale, i.e. a
  floor rather than a benchmark.
- **Warcraft III**: `MELEE_INSANE` is *never referenced* in the shipped scripts. Normal and Insane run
  identical code; Insane gets ~2× gold from the engine. The fair ceiling was reached at Normal.
- **Age of Empires IV**: gave Hardest a 2× resource boost in Jan 2023, was rejected by its community
  in a 176-post thread, and **reverted it three months later** — replacing it with the fair Hardest
  tier plus three *named* cheating tiers at 1.2× / 1.5× / 2.0×. ★ This is the canonical answer:
  **separate the skill dial from the handicap dial.**
- **Company of Heroes 3**: Expert is 1.4× manpower; the community mod author who reads the attrib
  files notes Relic "seems to have only lowered [bonuses] over time".
- **Offworld Trading Company** (Soren Johnson) — the closest genre analogue, an economic RTS with no
  unit micro: the AI **does not cheat**, and players attribute its difficulty to "perfect optimization
  and play… near instantly while making optimal calculations". Notably, the same designer defended
  cheating AI in *Civilization* and chose non-cheating here. **The "we can't afford good AI" argument
  is weakest exactly in narrow-verb games** — which is ours.
- **Legion TD 2** (macro auto-battler, no unit control — our action space): has been *walking bot
  income bonuses back* as its AI improved.

**Cheat magnitudes cluster narrowly, 1.2×–2.0× income**, and players' stated tolerance for a
*fair-feeling* handicap is much lower (~1.25×). A relevant warning from Rise of Nations players: a
flat income multiplier doesn't just make the AI stronger, it **deletes the player's ability to
interact with the AI's economy** — raiding stops mattering.

### Cheat-free difficulty knobs found in shipping code

Ranked by how well they transfer to us:

1. **Distort the AI's own perception, not the world.** SupCom 2's `aggression` value modifies the
   *inputs* to its threat evaluator "to mimic the effect of the AI's units being stronger than they
   actually were." Costs nothing, and is invisible to the player as a rule violation.
2. **Difficulty as a noise band.** Homeworld 2's `cp_shipDemandRange` = 1.0 / 0.5 / **0.1**: the same
   evaluator, with the hard AI simply sampling *closer to its own argmax*. This is a very clean way to
   build a ladder from one decision procedure.
3. **Concurrency limits.** CoH1's attack/defend queue slots (2/3 → 5/6 → 8/6 → 10/7); AMAI's factory
   clamp at `difficulty`. "How many things can it pursue at once" is a genuine skill axis.
4. **An explicit attention model.** The CoH3 Advanced AI mod ships an *"'Attention' system, AI will
   make more mistakes if multiple go on at once"* plus a player-facing action-speed slider — the most
   humane difficulty dial found anywhere, and a direct model of the cost a human pays that an AI
   doesn't.
5. **Reaction/commitment delay.** SC2's tactical-delay ladder; WC3 Easy's 240 s first-wave delay.

### AlphaStar's handicaps, as the canonical "make it fair" reference

- **22 non-duplicate actions per 5-second window**, where one agent action counts as up to 3 toward
  in-game APM — so **~792 in-game APM as a burst ceiling**. The canonical criticism is exactly that: a
  cap on a *window mean* is not a cap on an *instantaneous rate*, and it could "allocate its action
  quota unevenly across the window in order to launch superhuman bursts."
  **Lesson for us: cap the instantaneous rate, not an average.**
- Camera restriction cost it real strength (96% → 87% vs the Elite bot) — a genuine handicap.
- Delays: ~110 ms observation→execution, ~370 ms average self-chosen inter-observation wait.
- ★ **The APM ablation is the surprising one: *increasing* APM beyond their limit also reduced
  performance**, "possibly because the agent spends more effort refining micro-tactics than learning
  diverse strategies." Raw action rate is not the lever.

### Why fair bots still lose to humans — the mechanism, and it is our problem too

Brood War bots have superhuman everything (build orders within 0–8% of professional makespans
computed in 0.02% of real time; zero-overkill globally-coordinated targeting; **19 000 APM** measured
on one bot; no control-group limits) and still lose to any competent human. An unrated human is
roughly as strong as the best Brood War bot in existence.

The diagnosis is consistent across sources and is **exactly our AllIn bug**:

> "We professional gamers initiate combat only when we stand a chance of victory with our army and
> unit-control skills… the bots tried to keep their units alive **without making any bold decisions**."
> — Song Byung-gu, after going 4-0 against the top bots, 2017

A per-frame `score < 0 → retreat` rule over a myopic playout *is literally* a policy of keeping units
alive without bold decisions; it cannot represent "lose this fight to win the game". The binding
constraint is **reading intent and committing under uncertainty**, which none of the dominant
architectures represent, because they all evaluate the *current state* and never the opponent's
*plan*.

And the counterpart failure from the other direction — SupCom 2's neural net *refused to commit*
against enemy commanders because its fitness only saw the losses; fixed by making the winning move
worth "whatever the cost". **Both failure modes are commitment failures.**

---

## Architecture: the portfolio shape, and why it also gives the tier ladder

**Prismata's Hierarchical Portfolio Search** (Churchill & Buro, AIIDE 2015) is the closest
architectural match — a game with no unit micro, where decisions decompose by *phase* (economy,
defence, offence). A **Partial Player** computes a partial move for one phase; a portfolio groups them
by phase; children are the Cartesian product, so branching is a handful.

★ **Two results matter more than its win rates:**

1. **Six difficulty tiers were created "in less than 15 minutes" purely by recombining Partial
   Players in a text config file** — only the top two tiers use search at all. That is the cleanest
   published answer to "how do I get a difficulty ladder out of one decision procedure", and it maps
   directly onto our `EAiTier` system and CVar-driven tuning.
2. **14 months of live operation with no architectural changes "despite dozens of individual unit
   balance changes."** For a game under active tuning — ours is — that robustness is worth more than
   peak strength.

Its human result is also a calibration point: the Master Bot played ~200 unannounced ranked ladder
games unnoticed, finishing in the **top 25% of humans** — while experts still beat it 100%.

**Halo Wars 2** is the same idea authored rather than searched: a single Legendary strategy table
built from a top competitive player's actual openings (220 XML tables), with lower tiers scaling
*down* by restricting available plans and upgrades. Named, scoutable plans (Rush / Boom / Map Control
/ Fast Tech / Turtle) also make the AI's intent *legible*, which the design research says is what
separates "hard" from "cheating".

---

## Fairness and feel — what the design literature says

- **Legibility of the loss is the fairness variable, not the loss rate.** Juul's attribution work:
  players who blame *themselves* rate games higher, and players who completed a game without ever
  losing rated it *lower*. Miyazaki's test is "when players are killed and they can understand why."
- **A near-guaranteed loss needs a stated scalar goal** (Burgun) or the losses are unstructured. Hence
  the three scalars decided in round 2.
- **In a macro game, counter-picking is the thing players call cheating.** AoE2:DE's Extreme AI
  provably gets no bonuses and is still accused, because "it knows what units you're making."
  ★ Direct consequence for our fix #1: the mixed composition must be visibly derived from what is on
  the board, and ideally *lagged*, or it will read as mind-reading no matter what the code does.
- **Disclosure protects you.** DOOM's `Nightmare!` shipped a confirmation dialog reading "Are you
  sure? This skill level isn't even remotely fair." Blizzard names its cheating tiers "Cheater 1/2/3".
  "Perhaps Impossible" already does this work — the hedge sets the expected outcome to *loss*, so a
  loss confirms the label rather than indicting the design.
- **On adapting to one player:** persist *behaviour*, never *strength*; cap the model to options the
  player has demonstrated (the Tekken 8 Ghost constraint); decay old data (Drivatar); surface what it
  learned (Nemesis). **This is moot here — round 1 ruled out cross-match persistence** in favour of
  in-match style reading, which sidesteps the entire patronisation/sandbagging failure mode.

---

## Integration considerations

Everything recommended above fits the budget with enormous margin. Reference costs from the
literature: a state evaluation is 0.012–0.087 µs; the expensive thing is always a *forward model*
(~271 µs per 100-frame playout), which none of these need.

**Fits easily:** `argmax(A·q)` (~12 int ops) · 3-cycle Nash `p ∝ (a,b,c)` (3 muls) ·
Lanchester-with-production predicate (6 muls + compare) · `∫ ArmyValue dt` (1 multiply-add per event)
· harmonic worker falloff (1 divide) · expansion signal (1 subtract) · income/unspent macro score (2
accumulators) · exhaustive candidate-site scoring (~200 sites × 20 deposits ≈ 4 000 int adds, <10 µs —
**we can afford the exact answer and do not need a placement heuristic**).

**Only amortised:** anything resembling a search — split across ticks, replanned at ~1 Hz. Note the
survey taxonomy puts strategy-layer decisions at a **minutes** cadence, not frames; our `cadence 12`
(1.2 s) is already far faster than it needs to be.

**Incompatible:** MCTS with real playouts, learned evaluation (147 µs minimum, floats, heap), online
evolution (~10 s per replan), ASP/local-search placement (20–200 ms, **and stochastic — a lockstep
determinism hazard**).

---

## Honest gaps

- No literature on a game with no unit control (Prismata is turn-based).
- No published work on placing an economy structure to shorten a worker round trip — treated as a
  library implementation detail everywhere.
- The turnpike ↔ build-order bridge has **no RTS validation**; the discrete switch formula is a
  research-agent derivation over a peer-reviewed continuous model, not a citable RTS result.
- Evolved build orders were **never shown to win a game** in any paper found.
- Several numbers above are community measurement rather than peer review (worker payback, saturation
  constants). The *structures* generalise; the constants do not.
- Two of five research strands died on session limits (AoE2-specific mechanisms, and part of the
  industry sweep); the StarCraft strand covered most of that ground independently.
