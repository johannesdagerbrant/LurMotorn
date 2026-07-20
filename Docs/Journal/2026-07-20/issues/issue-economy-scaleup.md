<!-- Single issue, same splittable format. File under the #84 balance effort. -->

===== ISSUE =====
Title: Economy scale-up: mine density + total gold for the big-battle direction (#84 opening move)

## Evidence & decision

Playtest, 2026-07-20: mines near each team deplete **~2 minutes in**, and carts spend the rest of the match trekking cross-map for the remnants. Decision #8B: big battles are the ambition — armies toward mid-hundreds per side. Both point the same way: **many more mines** (density — income stays local all game), then total gold retuned for the army ceiling.

## Scope

- Raise `NumMines` substantially and regenerate the layout: distributed lattice/rows preserving the safe → contested gradient per team (the risk dial from the design doc stays a *placement* property). Keep generation deterministic from the derived seed.
- Retune `MineGoldCapacity` after density: total per-side gold sets the army ceiling — target mid-hundreds of soldiers lifetime (decision #8B), mines emptying only in the final third (playbook gate).
- Revisit `StartGold`/`StartMiners` only if the new density breaks the opening-decision timing (time-to-first-soldier gate).

## Measurement (playbook metrics)

- **Cart trip distance** (new metric): median deposit→nearest-live-mine distance per match third — must stay local all game; this is the reported failure mode, made measurable.
- Economy duration, match length p50/p90, peak army size — per the playbook table.

## Engineering notes

- `MineGold` is hashed and sized by `NumMines` — a build-locked sim change as usual, no wire-format impact.
- `AddMineRepel` and nearest-mine scans scale with mine count: confirm cost on the stress scene after the raise (if it grows teeth, mines join the spatial grid like units — but measure first).
- Mid-hundreds armies become the *real* load target: coordinate exit numbers with the perf epic (StressFill stops being a synthetic ceiling) and the boid gather budget.

## Acceptance

- [ ] Cart-trip metric holds local across all match thirds at the new layout (matrix run).
- [ ] Match length p50 in the 7–10 min target (decision #6B) with the retuned totals.
- [ ] Peak army size reaches the #8B range in boom-strategy matrix runs.
- [ ] Stress-scene tick budget unchanged or the mine-scan cost accounted for.
- [ ] Living-doc changelog entry: new layout + totals + the evidence line that drove it.
