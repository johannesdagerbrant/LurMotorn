# RPS Android — Sluggish Input / Bad FPS / Crashes: Hypotheses & Execution Plan (#86)

*2026-07-20. Grounded in the code at `d1dbb76` — every hypothesis below cites the mechanism in the tree, its predicted signature, a cheap falsifying test, and the fix. Doctrine per #86's own words: **instrument first, then optimize** — but four of these are verified facts, not guesses, so the plan front-loads two zero-code tests that may resolve most of it in an afternoon.*

---

## 0. Verified facts (read before the hypotheses)

1. **`SimRunner` — the #69 dedicated sim thread + `SnapshotMailbox` — exists, is tested, and is NOT used on Android.** `android_main` runs everything on one thread: `Session.Tick` (BLE inbox pump) → `Lp.Tick` (lockstep produce + **execute sim Steps**) → `ALooper_pollOnce` (input!) → `CaptureFrom` (~110 KB copy) → `View.Render`. The runner's own header says the seam was "in place for slice 1/2 to fill"; slice 2 didn't fill it.
2. **The everyday phone install is almost certainly `-O0`.** `build.gradle.kts` defines no `buildTypes`/optimization config, so `installDebug` (the adb dev loop) drives `CMAKE_BUILD_TYPE=Debug` → **unoptimized native code**, while `EngineFlags.cmake` controls only the macros (`LUR_CONFIG=Development` default: asserts ON, `LUR_INTERNAL` ON). The desktop where the numbers were tuned builds optimized Development. The phone has been benchmarking a different program.
3. **`LockstepPeer::Execute()` is an uncapped while-loop, and `RebuildFromHistory()` replays the whole match in one synchronous call — on the input thread.** `AdvancePreserving(…, 64)` caps *production* only. After backgrounding, execution bursts ≤64 Steps per frame repeatedly; on reconnect-adopt, a long match replays start-to-frontier in one call. `EmitAnchor()` also fires every 10th tick *during* those bursts, flooding the one-outstanding-write GATT queue.
4. **`LUR_ASSERT` = `__builtin_trap()` (SIGILL) in Development builds, and #73 deliberately made swapchain failures LOUD.** A thermal/driver hiccup on the A14's Mali now traps by design in dev builds.
5. Renderer: instanced path exists (good — one draw for units); frames-in-flight is still **one** (single `InFlight` fence — CPU waits the GPU on the same overloaded thread).

## 1. Hypotheses, ranked

| # | Hypothesis | Mechanism (evidence) | Predicted signature | Falsifying test | Fix (effort) |
|---|---|---|---|---|---|
| **H1** | Testing `-O0` builds | Fact 2 | *everything* 5–15× slower than desktop; scales with army size | Build once with optimization (see §3 step 0), same scene A/B | Make optimized-Development the default phone loop (S) |
| **H2** | Single-threaded main (unwired #69) | Fact 1 | frame hitch at exactly 10 Hz (one Step per hitch); touch/drag latency = frame time → "sluggish input" | Per-phase timers (§2): Step ms lands inside frame ms | Wire `SimRunner` on Android (M) — the designed lever |
| **H3** | ANR perceived as "crash" | Fact 3: uncapped `Execute` after background-return; synchronous `RebuildFromHistory` on reconnect | freeze/ANR dialog after HOME+return or after a mid-match BT reconnect; logcat `ActivityManager … ANR com.lurmotorn.onlyrps` | `adb shell input keyevent HOME`, wait 2 min, return; `svc bluetooth disable/enable` after a 5-min match | Cap Execute per service (SimRunner's `MaxTicksPerService` pattern — scheduling never changes results, §3 sprint law); chunk RebuildFromHistory across frames; suppress `EmitAnchor` during rebuild, send final only (S–M) |
| **H4** | Assert-traps = real crashes | Fact 4. Known trap sites: loud swapchain (#73), lockstep monotonic, codec non-monotonic ticks. (Slot-exhaustion is *unreachable* in real games — total map gold 36×300 bounds armies to ~220/side ≪ 2048; StressFill only.) | SIGILL tombstone; logcat `LUR_ASSERT failed:` with file:line | Pull logs/tombstones from the last crashes (§2) — classification, not reproduction | Policy: on-device Development **heals + logs** swapchain loss (trap only in Debugging); keep other asserts (S) |
| **H5** | TargetAcquire ring-search blowup | `NearestEnemyGrid` expands rings until the first enemy: two large, *distant* armies ⇒ every soldier scans the empty cells between them, O(units × separation) | Step-ms spike during the early march phase, shrinking as armies meet | Per-phase timers: TargetAcq µs vs Movement µs during a far-apart StressFill | Cap ring `K ≤ KMax`; no enemy within it ⇒ deterministic "march toward enemy camp Y" goal — cheaper *and* better-looking than exact NN at distance (S) |
| **H6** | Thermal throttling (Helio G80) | sustained load, known throttler; iPhone heat already logged | fps decays over minutes; CPU freq drops in perfetto; hot thermal zones | 15-min session with §2 thermal capture | Everything above *is* the fix (less work = less heat); measure before/after |
| **H7** | 1-frame-in-flight GPU serialization | Fact 5 | frame time ≈ CPU + GPU serial sum; GPU-bound scenes stall the input thread | perfetto gfx track after H1/H2 land | 2 frames in flight (M) — wake condition: measured GPU wait, not before |
| **H8** | Stack budget (hardening, not a current crash) | `AppState` by value on the glue thread (~1 MB stack): Sim-in-Lp ~160 KB + Snapshot ~107 KB + `GameView::Instances[4096]`+`LastCarry` ~150 KB ≈ **~450 KB**, +~42 KB Step locals | SIGSEGV near stack guard — *only after a future cap raise* | `sizeof` audit log line | Heap-allocate `AppState`; `static_assert` a size budget (S) |
| H9 | Input polish | MOVE history samples unused; pointer 0 only; camera dt at frame rate | drag still rough *after* H1/H2 | subjective A/B | batch-consume history samples (S) — only if still needed |

**Symptom → hypothesis map:** sluggish input = H2 (+H1 amplifying); bad FPS = H1 + H2 + H5 (+H6 over time, H7 at the margin); crashes = H3 (ANR) + H4 (traps).

## 2. Instrumentation first (one evening, agent-drivable via the DeviceRig)

**In-app (all `LUR_INTERNAL`, lands in one ship cycle):** extend the existing 2 s `LOCKSTEP` logcat line with `stepMsMax/avg`, `execBurstMax` (Steps per Execute call), `frameMsMax`, and per-phase Step micro-timers (`acq/move/atk` µs). This one log line, filtered over a session, adjudicates H2/H3/H5 numerically.

**adb captures (scriptable into `dev-rig`):**
- `adb logcat -s OnlyRps` (the vocabulary line) · ANR watch: `adb logcat | grep -E "ANR|LUR_ASSERT"`
- Frame truth: `adb shell dumpsys SurfaceFlinger --latency` · deep dive: one perfetto trace (sched + gfx + freq)
- Thermal: `adb shell "cat /sys/class/thermal/thermal_zone*/temp"` sampled each minute
- Crash forensics: `adb bugreport` after any incident (tombstones + ANR traces without root)
- Chaos, scripted: `input keyevent HOME` → 2 min → relaunch (H3a) · `svc bluetooth disable && sleep 20 && svc bluetooth enable` mid-match (H3b) · `am force-stop` + relaunch after a long match (cold-rejoin replay cost, §4-of-design measure)

**Capture matrix** (each in Debug AND optimized build — the pairing itself settles H1): small skirmish · far-apart armies at scale (H5) · `StressFill` cap · background-return · reconnect-after-5-min.

## 3. Execution plan

- **Step 0 — two zero-code tests (½ day):** (a) build one optimized install (`-DCMAKE_BUILD_TYPE=RelWithDebInfo` via gradle cmake `arguments`, `LUR_CONFIG=Development` kept) and A/B the same scene → H1 resolved; (b) pull logcat/bugreport from the recent crashes and classify each as ANR (H3) vs `LUR_ASSERT` (H4) vs other. *Do nothing else until both answers are in.*
- **Step 1 — land the timers + run the matrix** (§2). Attach numbers to H2/H5/H6/H7.
- **Step 2 — fixes, in dependency order:**
  1. **Build config:** add a proper optimized dev flow (gradle `release`-style buildType with Development macros, or force `RelWithDebInfo` on debug) so `-O0` can never masquerade as a perf problem again. Document in the repo README.
  2. **H3 caps:** `Execute` service cap (mirror `MaxTicksPerService`); chunked `RebuildFromHistory` with a "resyncing…" frame yield; anchor suppression during rebuild. Small, deterministic-by-construction, kills the ANR class even before threading.
  3. **Wire `SimRunner` on Android (#69):** Session + LockstepPeer move onto the sim thread (EventInbox already made the radio side thread-safe; `PendingLocalMask` becomes the atomic the runner's `InputFn` was designed to read); the render thread consumes the Mailbox and *only* renders + polls input. This is the input-latency fix and the hitch fix in one designed move — and it relocates any residual H3 burst off the input thread entirely.
  4. **H5 ring cap** + march-to-camp fallback (tunable `TargetSearchMaxK`), behind the grid-equivalence test (the brute path gets the same cap so equivalence holds).
  5. **H4 policy:** swapchain loss heals+logs in on-device Development; trap stays in Debugging. Re-verify #73's healer covers the thermal device-lost path.
  6. **Measured follow-ons:** frames-in-flight = 2 iff perfetto shows GPU wait (H7); heap `AppState` (H8) opportunistically.
- **Step 3 — verify:** re-run the full §2 matrix on the optimized build.

**Exit criteria:** `StressFill` cap holds a stable target fps on the A14 (set the number from Step 1's optimized baseline); touch-to-camera-pan ≤ 1 frame; HOME-return and BT-toggle reconnect produce zero ANRs and ≤ 1 s recovery; 15-min match holds fps within 20% of minute-one (thermal); an overnight rig soak produces zero unexplained tombstones.

## 4. Parked, with wake conditions (unchanged from the design doc)

Hand-SIMD (a profiled phase misses budget *after* the grid + H5 cap) · lock-free triple-buffer mailbox (publish contention observed — it won't be) · GGPO pacing nudge (visible ceiling micro-stalls in real matches) · snapshot resync (cold-rejoin replay > ~2 s measured — Step 1's `am force-stop` capture produces exactly this number).
