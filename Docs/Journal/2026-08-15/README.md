# Journal batch — 2026-08-15

Frozen snapshots against `master` @ `d72478e`. **Nothing here is live status.** Per the repo's
precedence rule the issues win on anything current — sequencing, priority, state *or* design. Read
this batch for rationale; act from epic **#39** and **#197**. Re-verify every code claim against
HEAD before acting on it: paths and symbols drift.

## Documents

| File | What it is |
|---|---|
| `doc-consistency-sweep.md` | Every stale/contradictory claim found in the checked-in guidance and the tracker, what it was, and what it cost |
| `ble-policy-cutover-and-soak.md` | The #197 cutover on both platforms, the hardware defect it exposed, and the two-phone soak numbers |

## The three findings worth remembering

1. **The dead policy modules were not free.** `BleStartRetry` and `BleDiscoveryTimers` were written,
   host-tested and called by nothing — while `BleShim.kt` carried a comment naming them as the
   reason the Kotlin does not decide, and went on deciding. Green tests on a module nothing calls
   reads as covered. The predicted cost (RPS never advertising after a failed first attempt) was
   real, and the fix had been sitting in the tree unused since 08-11.

2. **Running it found a bug that no amount of reading would have.** A Bluetooth off/on under a
   running app left the phone invisible for the life of the process. #194's idempotence guard
   (`if (advertising) return`) met a fact nothing tracked: an adapter power cycle takes the
   registrations away *without* a callback, so the flag stayed true and suppressed every recovery.
   Nothing in the shim listened for adapter state at all — `isEnabled` was checked once at startup
   and never again. Both platforms had the hole.

3. **The measurement that mattered was the one that contradicted the hypothesis.** Recovery timed at
   1.9 s across five consecutive cycles looked like a clean win for the delayed-rescan change. It
   was not evidence: every fast run had Android in the CENTRAL role and every slow run had it
   PERIPHERAL, so the variable was never isolated. Filed as #206 with the confound stated rather
   than as a fix.

## Method notes

- **A failure-shaped signal deserves the same suspicion as a success-shaped one.** The first soak
  reported 0/5 with zero adapter events. The app was not running — the rig's `run` leaves it
  stopped, and five cycles of Bluetooth toggling went to a dead process. Every later cycle asserts
  the pid before and after.
- **`gh run download` refuses to overwrite.** It failed with "The file exists" and left the previous
  `.ipa` in place. Installing that would have tested the wrong commit while every log line looked
  right — and the build-fingerprint gate would not have caught it, because both phones would have
  been consistent with each other and wrong together.
- **TDD with the red state proven.** `BleRadioState`'s tests passed on their first run, so the exact
  device bug was reintroduced to confirm six assertions fail naming the symptom, then reverted. A
  test that has never failed is not evidence.
