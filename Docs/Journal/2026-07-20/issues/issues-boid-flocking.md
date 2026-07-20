<!--
  Blocks delimited by "===== ISSUE =====" are individual GitHub issues; first line is
  "Title:", body is Markdown. File the Epic first, then paste its number into the
  children's "Part of #NN" lines. Source: Docs/Planning/rps-boid-flocking-plan.md
  (grounded in Sim.cpp at d1dbb76). Where an issue and the plan disagree, the issue wins.
-->

===== ISSUE =====
Title: Epic: Soldier flocking (boids) — composition readability + motion quality

## Goal

Fix the two playtest findings: units are dumb/uninteresting to watch, and army composition is unreadable when both sides bundle into one linear stream. Mechanism: a boid force stack for **soldiers only** (miners keep their state machine) — separation with corrected falloff, **type-affinity cohesion** (papers blob with papers; the army coheres loosely as a whole), and velocity-based alignment — plus a targeting-layer economy-defense preference. **Zero wire impact**: pure `State = Replay(Inputs, Seed)`.

## Diagnosis being fixed (from the code)

Point spawn + beeline Chebyshev seek + separation whose force *grows with distance* (stacking is nearly free) + no cross-team separation (melee pixel-piles) + no affinity or momentum. Plan §1 maps each cause to a slice below.

## House rules (every child inherits these)

- Every neighbor force: order-independent sum over start-of-tick state (`Prev*`/`Vel*`), one factored `Add*` helper shared verbatim by brute + grid paths, `rps_sim_tests` grid-equivalence extended per force.
- No float, no sqrt, no allocation in the tick. Chebyshev normalization; `(R − cheb)` falloff. `Fixed::Sqrt` stays unbuilt (wake: a force that visibly needs circular symmetry).
- **One gather pass**: the existing separation walk widened to the largest flock radius accumulates all sums in a single grid query per unit; finalize (weights, normalize, clamp, integrate) is a separate scalar per-unit loop — gathers stay auto-vectorizable.
- All knobs land in `Tunables.h` (starting values: plan §7).

## Sequencing vs #86

Slice A proves its tick budget on the **desktop StressFill scene** before any phone landing; device landing waits for #86's instrumentation (per that plan). Note: spread formations plausibly *reduce* worst-case overdraw density — flag any measured render delta.

## Children

- [ ] #NN — Slice A: un-bundle (hash-neutral)
- [ ] #NN — Slice B: flow (velocity + alignment)
- [ ] #NN — Slice C: guard-lite (ThreatBits targeting preference)
- [ ] #NN — Slice D: per-type spawn lanes + tunables consolidation

## Parked, with wake conditions

- **Counter-drift force** (attraction toward prey-type centroids): targeting already counter-prefers within bands; double-steering risks oscillation. Wake: counters still read as passive after B lands.
- **Interpose positioning** (stand between raider and cart): needs a position-goal concept `Target[]` can't express. Wake: raid defense still feels passive after slice C.


===== ISSUE =====
Title: Boids A — un-bundle: corrected separation + enemy separation + two-tier cohesion (hash-neutral)

Part of #NN (epic). Ships in one cycle; **no new state, StateHash layout untouched.**

## Scope

1. **Invert the separation falloff**: replace `offset × strength` with `dir_cheb × (R − cheb)/R × strength` — strongest at contact, zero at R. Same shape for `AddMineRepel`.
2. **Enemy separation** (new, smaller radius `EnemySeparationRadius`, own strength knob) — un-piles engaged fights into arcs. *(Approved, decision #2.)*
3. **Two-tier cohesion** (soldiers only): toward same-type centroid within `CohSameR` (weight `WCohSame`) + toward any-warrior centroid within `CohAllR > CohSameR` (weight `WCohAll ≪ WCohSame`). Centroids from `Σoffset/count`; one Fixed divide per unit per term in the finalize loop.
4. **Single gather pass**: widen the existing separation grid walk to `CohAllR`; accumulate sep/coh sums together. In-range soldiers keep separation only (cohesion + seek zeroed) — they already hold position; now they spread.
5. Stateless composition: `desired = ΣWk·Fk`, Chebyshev-normalize, step at `Speed`.

## Acceptance

- [ ] Grid ≡ brute hash-sequence equality across seeds with all new forces on (both `UseBruteForce` paths).
- [ ] Desktop StressFill at cap: tick budget within the #86-instrumented baseline + a stated margin; gather cell-visit count logged.
- [ ] Visual A/B (two desktop windows or `--flockdemo` if trivial to add here): two mixed armies marching — same-type blobs distinguishable at a glance; no oscillation/jitter at rest.
- [ ] No float/sqrt/alloc introduced (review + the existing static checks).
- [ ] Tunables added with plan-§7 starting values; each documented as playtest placeholders.


===== ISSUE =====
Title: Boids B — flow: implicit (Verlet) velocity via Pos−Prev, accel clamp, damping, alignment

Part of #NN (epic). Depends on slice A (rides its gather/finalize split). **Zero new arrays** — decision-sheet #1 resolved as the Pos−Prev variant: with the fixed tick, `Δ = Pos − Prev` *is* last tick's velocity.

## Scope

- **Enabler (one reorder):** move the bulk `Prev = Pos` copy (top of Movement, ~Sim.cpp:496) to **after** the gather phase. Gathers read `Pos` and `Δ` — both still end-of-last-tick for every unit — then `Prev = Pos`, then integrate. Slice A's two-phase split creates exactly this window; today the copy destroys Δ before anything can read it.
- **`PrevX/PrevY` join `StateHash`** (+2 `Mix` lines; update the "deliberately excluded" comment). Once Δ feeds behavior, Prev is authoritative state. It remains derivable from the replay today, but a future Pos-only write or snapshot-restore would silently break that with the anchor alarm blind — hash it.
- **Integration (finalize loop, buffered):** `NewPos = Pos + Damp·Δ + A`, where `A = ChebClamp(desired − Δ, MaxAccel)`; then `ChebClamp(NewPos − Pos, Speed)`; engaged soldiers use `InRangeDamping` (stronger decay, no orbiting). Miners excluded — their movement stays direct.
- **Alignment:** same-team + same-type sum of neighbor `Δ` joins the single gather (order-independent).
- Spawn already zeroes Δ (`Prev=Pos` on spawn, Sim.cpp:109 ✓); the world-bounds clamp naturally zeroes Δ at walls (correct behavior).
- **Verlet caveat, managed:** every position write becomes next tick's velocity, so separation projections feed momentum (dense-pack jitter risk). Mitigations: `Damp < 1` on the carried Δ; separation stays inside the blended desired direction (slice A style), never a post-projection. **Escape hatch:** if flockdemo shows untamable jitter, promote to explicit `VelX/VelY` — the swap is isolated to the integrate function; gathers and forces are untouched.
- `LUR_INTERNAL --flockdemo` desktop scene: mixed blobs, combat off, for visual tuning (plan §8).

## Acceptance

- [ ] Grid ≡ brute equality re-proven; determinism soak via desktop `--auto` (hash-sequence identical run-to-run).
- [ ] Momentum visibly smooths the every-tick retarget snaps (A/B capture vs slice A build).
- [ ] `|NewPos − Pos| ≤ Speed` invariant `LUR_ASSERT`ed in the finalize loop.
- [ ] Dense resting blobs in flockdemo show no Verlet jitter/oscillation (the caveat check).
- [ ] Hash change (Prev added) called out in commit + living-doc changelog; view interpolation semantics verified unchanged (Prev still = start of latest completed tick).


===== ISSUE =====
Title: Boids C — guard-lite: ThreatBits economy-defense targeting preference

Part of #NN (epic). Independent of B; targeting layer only, no steering change.

## Scope

- Per-tick **`ThreatBits`** pass (transient scratch, not state, not hashed): one grid walk flags enemy *soldiers* within `GuardAlertR` of any friendly miner. Order-independent bit-sets; brute twin for the equivalence test.
- `TargetScore` tuple gains a slot: `(Band, Threat, Prefer, Dist, Id)` — within a distance band, a flagged raider beats an unflagged target; counter-preference breaks the remaining tie. Both `NearestEnemyBrute` and `NearestEnemyGrid` updated identically; the grid ring cutoff reasoning is unchanged (Threat only reorders *within* bands, like Prefer).

## Acceptance

- [ ] Grid ≡ brute with the new tuple (seeded matrix incl. raid scenarios).
- [ ] Scenario test: raiders hitting a mining cluster get engaged by roughly-equidistant defenders who previously marched past (scripted `InputFn` scenario, asserted via target ids).
- [ ] Per-tick cost of the ThreatBits pass measured on the stress scene (it's one bounded grid walk; prove it).


===== ISSUE =====
Title: Boids D — per-type spawn-ring lanes + flock tunables consolidation

Part of #NN (epic). Small juice + cleanup cycle after A/B settle.

## Scope

- Offset the deterministic spawn ring by unit type (e.g., ring slot = `(SpawnCounter + Type·2) % RingSlots`, or per-type ring tables) so blobs are born pre-sorted — cohesion then maintains what spawning started. Pure `Tunables.h`/spawn change; deterministic; hash-neutral.
- Consolidate all flock knobs into one commented block in `Tunables.h` with the playtested values from A/B; record the final numbers in the living design doc changelog.

## Acceptance

- [ ] Fresh armies leave the camp visibly grouped by type without waiting for cohesion to sort them.
- [ ] Living doc changelog entry: final tunables + one-line rationale each.
