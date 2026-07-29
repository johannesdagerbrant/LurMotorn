# Action budget, queue batching, and the scalar — decisions round 2

**Date:** 2026-07-28

## Key points

1. **Queue batch size is PHASE-DEPENDENT, not a flat number.** `+1` while building the economy,
   `+5` once the economy is strong. This corrects the diagnosis in
   [flight-recording-analysis.md](flight-recording-analysis.md).
2. **The batching change is shared across all tiers**, with a ladder re-measure only if the ordering
   actually breaks.
3. **All three scalars ship**: survival time + personal best, how close the AI came to dying, and the
   end-of-match economy differential.

## Details

### 1. Phase-dependent batching — the owner's correction

I framed the AI's 1.1-units-per-command as a flat defect and proposed a flat `+5`. That framing was
wrong, and the correction is a strategic point, in the owner's words:

> "use +1 when building economy to keep cart production rolling but still having money for new
> minecart buildings as early on as possible. stacking unit queues is bad for expansion during the
> first minute of the game."

So the rule is: **gold sitting in a unit queue is gold that cannot buy the next mining camp.** Early,
a shallow queue is not thrift, it is *tempo* — you want carts trickling out continuously while the
surplus banks toward the next camp. Late, once income exceeds what shallow queues can absorb, the
`+5` plate is how you convert a bank into army.

The defect is therefore **not** "the AI batches too small". It is that the AI batches `+1` *for the
whole match* — it never makes the transition. Measured: 1.1 units/command averaged over ~250 s,
i.e. flat. The human averages 3.7 because they *do* switch.

**This is already modelled in the codebase and switched off at the top tier.** `AiController.cpp`
lines 575–586 compute `MinerDepth`/`SoldierDepth` with exactly this two-phase shape, gated on
`K.WaveLead > 0 && WaveLanding && State_ != Building`, and `PerhapsImpossible` ships
`wave_lead = 0` and `miner_queue = 0` — so the gate is dead and both depths collapse to the flat
value. The mechanism exists; the top tier does not use it.

Open sub-question for the plan: what defines "economy is strong" — the current `wave_lead` (ticks
until the enemy's wave lands) is one trigger, but the owner's phrasing ("after economy is strong")
suggests an *economic* trigger as well, e.g. income rate exceeding what standing production can
absorb. Likely both: switch on `min(wave landing, income saturates production)`.

### 2. Blast radius — shared, retune only on breakage

Chosen over top-tier-only. Rationale: leaving every lower tier permanently unable to convert a bank
into army is a mechanical handicap, not a designed one, and the tuning knobs that *are* meant to
carry difficulty (`queue_depth`, `max_buildings`, `soldier_ratio`, `worker_target`) can absorb the
change if the ladder shifts.

Procedure: implement the phase switch for all tiers → run `--aivs` 16 matches per pairing → if the
strict ordering (currently measured 16-0 / 16-0 / 16-0) holds, done. If Easy becomes too strong for
a first-timer, pull it back with `queue_depth` / `max_buildings` and re-measure with `--aibeginner`,
**not** `--aivs`, which is blind to that failure (see the #155 note in `Tunables.h`).

Note this partially reopens the round-1 "top tier only" decision — that still holds for the
*strategy* redesign; only the batching mechanic crosses the line.

### 3. Scalars — all three

- **Survival time + personal best**, per device, per tier.
- **How close the AI came to dying** — lowest HP its home base reached (`Cv.HomeBaseHp` is 900), or
  deepest frontier achieved.
- **Economy differential at the end** — income rate his vs its. The most *diagnostic* of the three:
  it says why the match was lost, not merely that it was.

Design backing: a near-guaranteed loss needs a stated goal or the 9 losses are unstructured; and
legibility of the loss ("expanded at 1:40 while you teched") is what converts a mystery defeat into
a lesson. All three are computable from state the `MatchRecorder` census already tracks
(`c <tick> <g0> <w0> <s0> <b0> <g1> <w1> <s1> <b1> …`), so nothing new needs recording.

## Open questions

- Exact trigger for the `+1 → +5` transition (wave ETA, income saturation, or the min of both).
- Where the scalars surface: post-match screen, opponent-selector row, or both.
- Whether `wave_lead`/`miner_queue` should simply be turned ON for `PerhapsImpossible`, or whether
  the new decision procedure supersedes them entirely.
