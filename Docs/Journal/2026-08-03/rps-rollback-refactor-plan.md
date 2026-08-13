# RPS: replace lockstep with rollback — the responsiveness experiment (plan)

Frozen snapshot, 2026-08-03. **Live status lives in the issues** — this is history; re-verify
every code claim against HEAD before acting.

## What sparked this

A chain that started from a determinism question and landed on a concrete netcode experiment:

1. **`Fixed` recap.** RPS's sim is deterministic because it's fixed-point (`Lur::Sim::Fixed`,
   Q16.16 integer math) on a fixed timestep — bit-exact across CPUs/compilers, the precondition for
   networked re-simulation.
2. **Box2D as the counter-case.** Box2D uses float and claims *cross-platform* determinism as of
   3.1 (via `-ffp-contract=off`, no fast-math, hand-rolled `atan2f`/trig, ordered Gauss-Seidel
   solver) — **but explicitly has no rollback determinism**: "no mechanism to set a world back to a
   prior state and resume expecting identical results." That limitation is a missing snapshot API +
   a large amount of internal solver state (warm-starts, contact islands, broadphase tree, constraint
   colouring), not a mathematical impossibility.
3. **The real target: a co-op physics puzzle game** (cranes/machinery, puzzle not twitch). Two ways
   to get deterministic rollback physics for it: **fork Box2D** and build a complete `memcpy`
   snapshot layer over its internals (fights float determinism per-toolchain + forks a dependency +
   breaks the no-libraries rule), or **write our own physics in `Fixed`** (determinism and a
   complete, trivially-correct snapshot fall out *by construction*, and it reuses RPS's already-proven
   lockstep/snapshot/resync stack — the cost moves to authoring a stable fixed-point constraint
   solver). Undecided; that decision is downstream of this experiment.
4. **The pivot.** The felt problem is **input delay**. In a *linked* (two-phone) RPS match, a
   place/queue is sampled at wall tick `W` and executes at `W + InputDelayTicks` — 3 ticks at
   `TickRateHz = 10` = **300 ms** — and that delay applies to the player's **own** action (lockstep
   applies every input at `W+Delay` on both peers to stay bit-identical). Solo/AI applies at ~1 tick
   (~100 ms), so the ~200 ms gap is **only noticeable when playing a linked opponent, never vs local
   AI**. This is a genuine latency problem, not a missing-feedback problem — so a view-side "instant
   ghost" acknowledgement is explicitly **not** the fix and is not being tested.

**The decision:** prove the rollback *feel* on RPS's cheap, already-deterministic sim **before**
betting the physics game on the technique. RPS isolates the **netcode-feel** risk from the
**fixed-point-physics-authoring** risk, so we never debug both at once.

**User constraints that shape this plan:**
- **Replace lockstep with rollback — do not run both.** No parallel `RollbackPeer` / A-B toggle;
  git history is the "keep the old model" mechanism. The refactor converts `LockstepPeer`'s execution
  model in place.
- **Skip the feedback pre-test.** The delay is latency, confirmed by the fact that it vanishes vs
  local AI.

## What we are testing

Does converting RPS's netcode from **delay-based lockstep** to **rollback** (apply local input
immediately + speculate the peer + re-simulate on misprediction) remove the felt input delay on the
**local player's own actions** in a linked two-phone match — making linked play feel as responsive as
solo/AI — **without** introducing distracting correction artifacts or new desyncs?

RPS is deliberately the *friendly* case, which is why a clean result here is necessary-but-not-
sufficient evidence for the physics game, and a bad result here is nearly decisive against:
- **Sparse input** — human taps are a few per second against a 10 Hz tick, so the "peer did nothing
  this tick" prediction is right the large majority of ticks → most rollbacks are no-ops.
- **Local authority** — each phone owns its team (interaction-local authority), so the local player's
  own action is never mispredicted by the local peer; only the *opponent's* actions can mispredict,
  and they usually touch peer-owned units away from what you just did.

The future physics game is the opposite on both axes (densely-coupled bodies, continuous direct
manipulation), so its corrections would be more frequent and larger.

## What we want to measure

On real hardware, two phones (Android + iPhone), linked match:

1. **Felt responsiveness (primary, subjective A/B).** Local place/queue registers in the sim at
   0–1 tick instead of 3 — linked play should feel indistinguishable from solo for *your own*
   actions. This is the pass/fail signal.
2. **Correction frequency.** How often does a received peer input differ from the "peer idle"
   prediction and force a rollback+resim (vs a no-op advance). Expect low, given sparse input.
3. **Correction magnitude.** When a correction happens, the visible world-space displacement of
   affected (peer-owned) units. At 10 Hz a correction can snap up to `horizon` ticks of movement;
   RTS units move slowly so this should be small — quantify it.
4. **Rollback cost.** Ticks re-simulated per correction and worst-case CPU per frame (must stay well
   inside the 100 ms tick budget; RPS's sim is cheap so this should be trivial — confirm).
5. **Determinism preserved.** Both peers still converge to identical **confirmed** state; the anchor
   hash still matches at confirmed ticks; no *new* desyncs vs the lockstep build. Rollback exercises
   re-simulation far more than resync-by-replay ever did, so this doubles as a determinism stress
   test — any latent nondeterminism in `Sim::Step` surfaces as constant corrections.
6. **Peer-action latency (sanity, expected unchanged).** The *opponent's* actions still appear ~RTT
   later on your screen — unavoidable and correct. Confirm it isn't bothersome (you don't feel
   latency on the other player's actions the way you feel your own).

## Implementation plan (rollback refactor)

Canonical model (GGPO-family): input issued during wall tick `W` is applied at tick `W` on **both**
peers (no `+Delay`). The local peer applies its own input immediately at its head; the remote applies
it at `W`, rolling back if it had already speculated past `W`. Both peers' **confirmed** timelines are
therefore identical — the only difference is that one peer computed some ticks speculatively first.
The **wire frame stays the same** (per-tick `MsgInput` batch + the `#163` low-byte sequence); only the
local *execution scheduling* changes, so the anchor / `#161` recovery / `#163` gap-detection /
resync machinery is inherited and re-pointed, not rewritten.

Phases (each = its own commit; host-green before device):

0. **(Deliberately skipped) feedback pre-test.** Recorded as skipped: the delay is latency, not
   feedback — a view-side ghost would not change the sim timing that is the actual complaint.

1. **Snapshot ring + prediction scaffolding (host).** Add a ring buffer of `Sim` snapshots keyed by
   tick (`memcpy` — the `Sim` header already guarantees POD/trivially-copyable and promises exactly
   this). Add the peer-input predictor (predict "no `InputEvent`s this tick"). Define **confirmed
   tick** = highest tick for which *both* peers' real inputs are known. Ring depth ≥ the rollback
   horizon. All exercised by the existing two-window loopback (both peers pumped on one main loop —
   the workbench already exists) and `NetTests`.

2. **Replace the execution model (the core change).** Remove `W+Delay` scheduling and the ceiling
   gate; instead, each `Tick`: apply local input at the current tick immediately, speculate forward
   to the wall tick using known + predicted peer inputs, snapshotting each tick. On a received peer
   frame for tick `T`: if it equals the prediction, advance confirmed and retire old snapshots; if it
   differs, **restore the snapshot at `T`, re-apply the corrected combined batch, re-simulate to the
   speculative head.** Cap speculation at a **rollback horizon** `N`; if the peer is more than `N`
   ticks behind, **stall** (reuse today's ceiling-stall + `#162` bound as the backstop) so rollback
   degrades gracefully to waiting under a bad link. Bump `Lur::Net::ProtocolVersion` (execution
   semantics changed; the `#166` fingerprint gate already refuses cross-build matches, this is
   belt-and-suspenders).

3. **Correction smoothing (View — the one genuinely new feel piece).** A rolled-back unit position
   must **ease** into its corrected value over a few render frames rather than snapping. RPS already
   interpolates `Prev→Pos` per unit (`Snapshot` + `mix(prev,curr,alpha)` in the vertex shader);
   extend that with correction blending so a resim'd jump reads as motion, not a pop. This is the
   skill that transfers directly to the physics game.

4. **Reconcile the safety nets to the rollback model.** Re-point each hardened path (owned by
   `LockstepPeer`, hardened through `#147`–`#183`): anchor hash cross-check compares **confirmed**
   state only; `#161` desync recovery and `#163` gap/dup detection carry over (they replay input
   history → deterministic, model-independent); `#162` ceiling-stall becomes the horizon-exceeded
   fallback; resync/reconnect frontier logic re-based onto confirmed ticks. **Change the execution
   core only — keep the wire and the recovery machinery** to protect the hard-won robustness.

5. **Verify on two phones.** Measure §1–§6 above. A/B own-action feel (linked vs solo), correction
   frequency/magnitude, worst-case resim cost, zero new desyncs, on **both** Android and iOS.

## Risks

- **Correction pops at 10 Hz are coarse** (up to ~300 ms of movement per correction). Main feel
  risk; Phase 3 view-blending is the mitigation. If corrections still read badly *here* — RPS being
  the friendly case — that is a strong signal against rollback for the denser physics game.
- **Rollback horizon vs BLE jitter.** If RTT jitter exceeds `N`, stalls become frequent. Tune `N`;
  keep the stall fallback.
- **Re-simulation exposes any latent `Sim::Step` nondeterminism** far more often than before —
  a benefit (stress test) and a risk (constant corrections) if any hidden nondeterminism exists.
- **Touching the most safety-critical code.** `LockstepPeer` is the hardest-won file in the game;
  replacing its execution core risks regressing the recovery paths. Mitigate by keeping wire +
  recovery intact and leaning on the host two-window loopback + `NetTests` before every device trip.
- **Two mains route local input from separate copies** (`RouteLocalEvent` vs `placeLocal:`,
  CLAUDE.md gotcha) — change both.

## What each outcome teaches the physics-game decision

- **Rollback makes linked RPS feel like solo →** the technique works over real BLE for local co-op;
  we've built and tuned the snapshot-ring / prediction / correction-blend machinery on a cheap sim;
  we carry it to the physics game knowing only the fixed-point *solver* remains as new risk.
- **Corrections feel bad even on RPS →** the densely-coupled, directly-manipulated physics case
  would be worse; pivot toward diegetic latency masking (the "machines rev up" fiction) with eyes
  open — learned in days, not months.
- Either way, a more responsive RPS is shippable value regardless of the physics decision.
