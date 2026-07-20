# RPS — Soldier Flocking (Boids) Plan

*2026-07-20. Target problems, from playtest: (1) units are dumb and uninteresting to watch; (2) army composition is unreadable — both sides bundle into one linear stream, so you can't estimate the opponent's type mix or numbers. Goal: type-clustered, fluid formations that make composition readable at a glance, plus light economy-defense behavior — **all sim-side, zero wire impact** (the press mask and event codec are untouched; this is pure `State = Replay(Inputs, Seed)`).*

*Grounded in `Sim.cpp` at `d1dbb76`. House rules honored throughout: every neighbor force is an order-independent sum over start-of-tick state, factored into one `Add*` helper shared verbatim by the brute and grid paths, with the grid-equivalence run in `rps_sim_tests` extended per force. No float, no sqrt, no allocation in the tick.*

---

## 1. Why it bundles today (diagnosis from the code)

1. **Point spawn:** both teams emerge from one camp point via an 8-slot ring; nothing ever un-mixes them.
2. **Beeline seek:** `MoveToward` steers every soldier straight at its target at identical speed (all warriors 0.7); the every-tick banded rescoring funnels the whole army at the same nearest blob → single-file stream.
3. **Separation is inverted:** `AddSeparation` pushes with `offset × strength` — force *grows with distance* within the radius and vanishes at contact. Classic separation is the opposite (strongest at contact). Today stacking is nearly free; the blob is the equilibrium.
4. **No enemy separation:** `AddSeparation` early-outs on `Team[J] != Team[I]`, and in-range soldiers hold position — engaged fights collapse into a cross-team pixel pile.
5. **No affinity, no momentum:** nothing distinguishes types spatially, and direction is recomputed fresh each tick (no velocity state), so motion snaps rather than flows.

Each numbered cause maps to one fix below; (3) and (4) alone are half the readability win and don't even need new state.

## 2. The force stack (soldiers only; miners keep their state machine)

Per soldier, one neighbor **gather pass** accumulates order-independent sums from `Prev*` (and, in slice B, `Vel*`) — the existing separation walk widened to the largest flock radius, so it's *one* grid query per unit, not four:

| Force | Over | Falloff / form | Delivers |
|---|---|---|---|
| **Separation** (fixed) | all friendlies + **enemies** (new, smaller radius) | `dir_cheb × (R − cheb)/R × strength` — strongest at contact, zero at R; Chebyshev-normalized direction, no sqrt | un-stacks blobs and combat scrums (§1.3, §1.4) |
| **Cohesion, same type** | same team + same type, radius `CohSameR` | toward neighbor centroid (`Σoffset / count`) | **the readability mechanism**: papers blob with papers |
| **Cohesion, army** | same team, any warrior, radius `CohAllR > CohSameR`, weight `≪` same-type | toward army centroid | blobs travel loosely together instead of scattering |
| **Alignment** *(slice B)* | same team + same type | average neighbor `Vel` | laminar per-type streams — the fluid look |
| **Seek** | current target (unchanged targeting) | as today; **zeroed while in attack range** | goal pursuit |
| Mine repel (fixed form) | live deposits | same inverted falloff as separation | carts still ring deposits |

**Composition:** `desired = Σ weight_k · force_k`, then move. Slice A (stateless): normalize `desired` (Chebyshev) and step at `Speed` — no new state, no hash change. Slice B (velocity): `Vel += clamp(desired − Vel, MaxAccel)`, `Vel` clamped to `Speed`, `Pos += Vel`, with damping toward zero when in range — momentum smooths the every-tick retarget snaps and produces the flock-like curves. In-range soldiers keep separation only (cohesion/alignment/seek off) so engaged lines spread into arcs instead of piles.

**Determinism notes:** all neighbor reads are start-of-tick (`Prev*`, `Vel*` before integration — integration happens in a second per-unit loop after all gathers, mirroring the buffered-damage pattern); centroid division is one `Fixed` divide per unit per term (finalize is scalar; the gather stays an auto-vectorizable sum, per design-doc §5); `(R − cheb)` falloff and Chebyshev normalization keep `Fixed::Sqrt` unnecessary — it stays unbuilt until a force *visibly* needs circular symmetry (wake condition, §6).

## 3. State & hash impact

- **Slice A:** zero. No new arrays, `StateHash` layout untouched — ships in one cycle, tunables-only lockstep note as usual (both peers same build).
- **Slice B:** `Fixed VelX[MaxUnits], VelY[MaxUnits]` join the SoA **and the pinned hash order** (declaration-order rule). +16 KB at the 4096 cap; snapshot/render unaffected (interpolation still `Prev→Pos`). This is the one deliberate hash-layout change; the anchor-hash alarm covers any mixed-build session within a second.

## 4. Economy defense ("guard") — targeting layer, not a force

The counter-preference fold-in the playtest already shipped (banded tuple) extends by one slot rather than by a steering behavior: a per-tick **`ThreatBits`** pass (one grid walk: flag enemy soldiers within `GuardAlertR` of any friendly miner; order-independent bit-sets) and the tuple becomes `(Band, **Threat**, Prefer, Dist, Id)` — a raider gets picked over an equally-distant non-raider, so defenders peel toward the economy *through the existing targeting machinery*, deterministically, with zero new movement code. Full interpose positioning (stand *between* raider and cart) is **parked**: it needs a position-goal concept `Target[]` can't express — wake condition: playtests where raid defense still feels passive after the threat bit.

## 5. Slices (ship-cycle sized, each behind the equivalence test)

- **A — un-bundle (hash-neutral):** inverted separation falloff + enemy separation + the two cohesion terms via the widened single gather; stateless blend; retune `SeparationRadius/Strength`. Desktop A/B with `StressFill` armies. *Delivers most of the readability.*
- **B — flow:** `Vel*` arrays + accel/turn clamp + damping + alignment. The "satisfying to look at" slice.
- **C — guard-lite:** `ThreatBits` + tuple slot.
- **D — knobs/parked:** per-type spawn-ring lanes (born pre-sorted — cheap juice); counter-drift force toward prey-type centroids (**park**: targeting already counter-prefers; double-steering risks oscillation); interpose goals (§4).

## 6. Perf & sequencing vs #86

The gather is the separation loop with a bigger radius: visited cells go from ~1 cell² to ~`(2·CohAllR/GridCellSize+1)²` (~9–25 cells) — **the flock radius is the new hot knob**, and at the 2048/side stress cap this becomes the heaviest tick phase. Therefore: land slice A on the **desktop stress scene first** and hold the tick budget there before phones; on devices, sequence *after* #86's instrumentation exists (its own doctrine: instrument first), noting boids plausibly *help* #86's render side — spread formations cut worst-case overdraw density. `CohAllR` starts modest (≈6–7 wu); if the gather blows budget at cap, the escalation path is the design doc's: grid cell retune → then hand-SIMD wake condition. `Fixed::Sqrt` wake: a force that visibly needs circular falloff after Chebyshev ships.

## 7. Starting tunables (placeholders for slice-3-style warfare)

`SeparationRadius F(12,10)` · `EnemySeparationRadius F(1)` · `SeparationStrength F(1,2)` (inverted falloff needs more) · `CohSameR F(5)`, `WCohSame F(1,4)` · `CohAllR F(7)`, `WCohAll F(1,12)` · `AlignR F(4)`, `WAlign F(1,6)` · `WSeek F(1)` · `MaxAccel F(15,100)`/tick (≈0.5 s to turn) · `InRangeDamping F(1,2)` · `GuardAlertR F(6)`.

## 8. Test plan

Extend `rps_sim_tests`: grid≡brute per new force (same seeds, `UseBruteForce` both ways, hash-sequence equality); a flock-invariant test (two same-type groups spawned apart converge to bounded spacing, no NaN-analog: no `Fixed` overflow at cap — assert |force sums| bounds); determinism soak via desktop `--auto`; visual eval via a `LUR_INTERNAL` `--flockdemo` scene (mixed blobs, combat off) since golden replays break by design across tunable changes.
