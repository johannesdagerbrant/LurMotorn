<!--
  Blocks delimited by "===== ISSUE =====" are individual GitHub issues; first line is
  "Title:", body is Markdown. File the Epic first, then paste its number into the
  children. Source: Docs/Planning/rps-android-perf-stability-plan.md (verified against
  the code at d1dbb76). This IS the #86 execution breakdown — file the epic as #86's
  child or retitle #86 to it, owner's call. Where an issue and the plan disagree, the
  issue wins.
-->

===== ISSUE =====
Title: Epic: #86 execution — Android sluggish input / bad FPS / crashes

## Goal

Close the three device symptoms with the verified mechanisms from the perf plan: the unwired #69 sim thread (everything on one thread), the `-O0` everyday install, the uncapped `Execute`/synchronous `RebuildFromHistory` ANR class, assert-trap policy under thermal, and the TargetAcquire ring-search scaling wall.

**Doctrine:** instrument first — but Step 0 is two zero-code tests that may resolve most of it in an afternoon. Children are in dependency order; Step-0 findings may reprioritize (record what they showed in this epic before proceeding).

## Symptom → child map

Sluggish input → #NN (SimRunner) amplified by #NN (build config) · Bad FPS → build config + SimRunner + ring cap (+ thermal measured) · Crashes → #NN (ANR caps) + #NN (assert policy).

## Children (dependency order)

- [ ] #NN — Step 0: zero-code triage (optimized A/B + crash-log classification)
- [ ] #NN — Instrumentation: LOCKSTEP timers + rig capture matrix
- [ ] #NN — Build config: optimized-Development as the everyday phone install
- [ ] #NN — ANR class: Execute service cap + chunked RebuildFromHistory + anchor suppression
- [ ] #NN — Wire SimRunner on Android (#69)
- [ ] #NN — TargetAcquire ring cap + march-toward-camp fallback
- [ ] #NN — Swapchain-loss policy in on-device Development
- [ ] #NN — Measured follow-ons: frames-in-flight=2 (gated) + heap AppState

## Exit criteria (from the plan)

StressFill cap holds stable target fps on the A14 (number set from Step 1's optimized baseline) · touch-to-pan ≤ 1 frame · HOME-return and BT-toggle reconnect: zero ANRs, ≤ 1 s recovery · 15-min match holds fps within 20% of minute one · overnight rig soak: zero unexplained tombstones.


===== ISSUE =====
Title: Perf 0 — zero-code triage: optimized-build A/B + crash-log classification

Part of #NN (epic). **Do nothing else until both answers are in.** ~½ day.

1. Build one optimized install (`-DCMAKE_BUILD_TYPE=RelWithDebInfo` via gradle cmake `arguments`, `LUR_CONFIG=Development` unchanged). Same scene A/B vs the `installDebug` build: small skirmish + StressFill. Record fps + subjective input feel for both. → adjudicates H1.
2. `adb bugreport` + `adb logcat -d | grep -E "ANR|LUR_ASSERT|SIG"` from the phone that crashed. Classify every recent incident: ANR (H3) / `LUR_ASSERT failed:` + SIGILL (H4, note file:line) / other (new hypothesis).

Acceptance: findings written into the epic (numbers + classifications); children reprioritized if warranted.


===== ISSUE =====
Title: Perf 1 — instrumentation: LOCKSTEP timers + DeviceRig capture matrix

Part of #NN (epic). One ship cycle; all `LUR_INTERNAL`.

## Scope

- Extend the existing 2 s `LOCKSTEP` logcat line: `stepMsMax/avg`, `execBurstMax` (Steps per `Execute` call), `frameMsMax`, per-phase Step µs (`acq/move/atk`).
- Rig scripts (dev-rig additions): logcat filter · `dumpsys SurfaceFlinger --latency` · one perfetto config (sched+gfx+freq) · thermal zones sampled per minute · `bugreport` pull · chaos: HOME-return-after-2-min, `svc bluetooth disable/enable` mid-match, `am force-stop` + relaunch (also produces the cold-rejoin replay-time number the snapshot-resync wake condition needs).
- Run the capture matrix (skirmish / far-apart-at-scale / StressFill / background-return / reconnect-after-5-min) on the optimized build; attach numbers to H2/H5/H6/H7 in the epic.

Acceptance: matrix results tabled in the epic; the per-phase numbers name the top Step cost.


===== ISSUE =====
Title: Perf 2 — build config: optimized-Development is the everyday phone install

Part of #NN (epic). Approved (decision #5).

Add a proper optimized dev flow so `-O0` can never masquerade as a perf problem again: either a gradle buildType carrying Development macros with optimization, or force `RelWithDebInfo` onto the debug native build via cmake `arguments`. `-O0` remains available behind explicit `LUR_CONFIG=Debugging`. Document the matrix (gradle buildType × LUR_CONFIG → flags) in the Android README, both games.

Acceptance: `installDebug` (or the documented replacement) produces optimized native code with asserts on; chess app updated identically; README table committed.


===== ISSUE =====
Title: Perf 3 — ANR class: Execute service cap + chunked RebuildFromHistory + anchor suppression

Part of #NN (epic). Small, deterministic-by-construction (scheduling never changes results — design doc §3 sprint law). Kills the ANR class even before threading lands.

## Scope

- `LockstepPeer::Execute`: cap Steps per call (mirror `SimRunner::MaxTicksPerService`; tunable). Remaining backlog drains over subsequent calls — never discarded.
- `RebuildFromHistory`: chunk the replay across service calls (e.g. ≤ N ticks per call, resume state kept); the view already freezes gracefully — show the existing "resyncing…" affordance while it drains.
- Suppress `EmitAnchor` during rebuild/catch-up bursts; emit one final anchor at the frontier (prevents flooding the one-outstanding-write GATT queue with hundreds of stale anchors).

Acceptance: HOME-return after 2 min and BT-toggle reconnect after a 5-min match produce **zero ANRs** on the A14 (rig-scripted, run 10×); NetTests cover chunked rebuild reaching frontier-hash equality with the synchronous path; anchor count during a rebuild ≤ 2.


===== ISSUE =====
Title: Perf 4 — wire SimRunner on Android (#69): sim+net off the input thread

Part of #NN (epic). The designed lever; do after Perf 3 so bursts are already bounded.

## Scope

- `Session` + `LockstepPeer` move onto the `SimRunner` thread (the EventInbox already made radio→engine thread-safe; the runner's `InputFn` reads the tap mask — `PendingLocalMask` becomes an atomic exactly as the runner's header designed).
- The glue thread keeps only: input polling, camera, `Mailbox.Consume`, `View.Render`. `CaptureFrom` moves to the tick thread's publish (per-tick, not per-frame — also deletes the current every-frame ~110 KB copy).
- Team/link bootstrap (`Session.IsReady` → `Lp.Init`) sequenced onto the sim thread; logging unchanged (the LOCKSTEP line now reports from the runner).
- iOS main gets the same wiring in the same cycle if cheap, else its own follow-up (the heat complaint suggests it wants this too).

Acceptance: touch-to-camera-pan ≤ 1 frame at StressFill cap; frame-time histogram loses the 10 Hz hitch signature; datagram-to-Step latency ~ms (log it once); desync soak (desktop `--auto` + device pair evening) clean; no data crosses threads except Mailbox + the input atomic (review checklist).


===== ISSUE =====
Title: Perf 5 — TargetAcquire ring cap + march-toward-camp fallback

Part of #NN (epic). Approved as written (decision #3: march-toward-camp fallback).

## Scope

- `TargetSearchMaxK` tunable caps the expanding-ring search in `NearestEnemyGrid`; **the brute path applies the same Chebyshev cutoff** so grid ≡ brute holds.
- No enemy within the cap → deterministic goal: march toward the enemy camp line (seek `(CampX, CampY(1−Team))`); re-search continues each tick (targets acquired naturally as armies close).

Acceptance: grid ≡ brute equality with the cap on (seeded matrix incl. far-apart armies); far-apart StressFill scenario's `acq` µs drops to the measured budget; playtest note: army-march behavior reads as intended (armies advance, no straggler-chasing across the map).


===== ISSUE =====
Title: Perf 6 — swapchain-loss policy: heal + log in on-device Development

Part of #NN (epic). Approved (decision #4). Small.

On-device Development builds **heal + loud-log** swapchain/device-loss (the #73 self-healer path) instead of trapping; the trap remains in Debugging (and host Development). Verify the healer covers the thermal device-lost path on the A14 (perf-1's thermal capture can provoke it). Keep all other asserts trapping.

Acceptance: a provoked/simulated device-loss on the A14 recovers within a few frames with a `swapchain healed` log line; Debugging build still traps; the policy documented in Assert.h's comment block.


===== ISSUE =====
Title: Perf 7 — measured follow-ons: frames-in-flight = 2 (gated) + heap AppState

Part of #NN (epic). Both gated on Perf-1 evidence; ship separately if timing differs.

- **Frames-in-flight = 2**: only if perfetto shows the render thread blocked on the GPU (H7). Per-frame command buffers/semaphores/fences; portability-subset safe; chess inherits.
- **Heap `AppState` + size budget**: `android_main` currently stacks ~450 KB (Sim-in-Lp + Snapshot + `GameView::Instances`) on the ~1 MB glue thread — fine today, one cap-raise from not. Heap-allocate; add a `static_assert`/log line for the size budget so growth is visible.

Acceptance: (a) GPU-wait number before/after if taken, else the gate's "not needed" evidence recorded; (b) stack audit line in logs; AppState heap-owned in both games' mains.
