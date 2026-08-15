# #197 — the BLE policy cutover, the defect it exposed, and the soak

Frozen 2026-08-15 against `master` @ `d72478e`. History, not status: act from **#197**, **#206**
and epic **#39**, and re-verify every path against HEAD.

## 1. What was actually wrong at the start

`BleStartRetry` and `BleDiscoveryTimers` existed, had thorough host tests, and were **called by
nothing**. The complete set of references outside their own `.cpp` and test files was one comment
in `BleShim.kt` naming them as the reason the Kotlin does not make decisions — above Kotlin that
went on making them with `Handler.postDelayed`.

This is the worst shape a defect takes in this codebase: it reads as covered. The 08-11 status
report had predicted the cost precisely — *RPS silently never advertises if its first attempt
fails*, because #194's backoff lived in chess's Kotlin and not RPS's — and that was still true.

## 2. The cutover

A control inversion, which is why it was larger than the send-queue cutover: the driver no longer
decides, it reports and asks.

| Direction | Android |
|---|---|
| Kotlin → C++ | `onAdvertise/ScanStartFailed`, `…Started`, `onConnectStarted`, `onConnectResolved`, `scheduleRescan`, `onRadioLinked/Unlinked/Stopped`, `onAdapterOn/Off` |
| C++ → Kotlin | `startAdvertising`, `startScanning`, `goSymmetric`, `abortConnect` |

Deleted from Kotlin: `retryDelayMs`, the retry counters and log-rate flags, the retry Runnables,
`watchdogHandler` + the watchdog Runnable, the 6 s connect watchdog, the 1.5 s rescan. iOS lost its
`NSTimer _DiscoveryWatchdog` and gained the connect watchdog and delayed rescan it never had.

**Not moved, deliberately:** releasing our own stale advertise/scan registration before a retry.
That is a platform verb and it is required — without it the retry earns the same `ALREADY_STARTED`
forever, which was #194's actual bug.

### Two traps, both the silent kind

- `fun f() = handler.post { }` infers **Boolean** (`Handler.post` returns one), so the JNI signature
  is `()Z`, `GetMethodID(…, "()V")` returns null, and the verb never fires **with nothing logged**.
  `RegisterNatives` guards Kotlin→C++ by refusing the library load; this direction has no guard, so
  `nativeSetShim` now checks all four ids and logs an error naming the consequence.
- The policy's discovery watchdog is **periodic**; the `NSTimer` it replaced was one-shot *and
  re-armed itself*. Keeping both would have doubled the rate while reading as a faithful port.

## 3. The defect running it exposed

```
adb shell svc bluetooth disable ... enable
```

and the phone never advertised or scanned again for the life of the process. A fresh launch seconds
later came up fine, so the radio was healthy and the app was not.

Two individually-correct things combined. #194 made the starts idempotent (`if (advertising)
return`) because asking twice earns `ALREADY_STARTED`, and that churn is what wedges the radio. But
the flag behind that guard was set when we *asked* and cleared only by our own stop or our own
failure callback — and an adapter power cycle is neither. **Nothing in the shim listened for adapter
state at all**: `isEnabled` was checked once at startup and never again.

On the device it showed as the 8 s watchdog line repeating with nothing after it, because by our own
bookkeeping nothing had changed.

The fix splits the way the doctrine says: the FACT (a `BroadcastReceiver` on
`ACTION_STATE_CHANGED`) is a platform verb; the RULE (*a radio power cycle invalidates every
started-state*) is `Lur::Transport::BleRadioState`, host-tested, shared — iOS had the identical hole
through `CBManagerState`, where both `didUpdateState` handlers read `if (state != PoweredOn)
return;` and invalidated nothing.

## 4. Numbers from the pair

Galaxy A14 + iPhone 11 Pro, chess AGENT builds, matching fingerprints.

**Baseline:** 2 matches in 27 s. `android sameFrame=103/103 gate=0 rtt(avg=46ms max=75ms)`,
`ios sameFrame=696/698 gate=0 rtt(avg=54ms max=100ms)`.

**Radio state recovery — verified, ~725 ms:**

```
20:46:02.285  BLE adapter ON - every started-state was cleared, discovery re-armed
20:46:02.329  advertise failed: 3            <- INTERNAL_ERROR, stack not ready
20:46:03.010  advertise confirmed            <- BleStartRetry's 400 ms backoff, healing a real failure
```

That is the #194 path running on hardware for the first time, having been dead code that morning.

**iOS ran the shared watchdog on device**, at 8.016 s intervals — `BleDiscoveryTimers` clocked from
`pumpInbox`.

**Re-link after the cycle — and the asymmetry (#206):**

| Android's role | Adapter-on → linked |
|---|---|
| CENTRAL | **1.9 s**, five consecutive cycles (1.896 / 1.895 / 1.947 / 1.904), `defers=0` |
| PERIPHERAL | **did not link in 135 s**; an earlier run resolved at 33 s via the #146 breaker |

## 5. The result I did not get

iOS rescanning immediately (where Android waits 1.5 s, #17) was the hypothesis for the slow case,
and it was wired onto the shared delayed rescan. The 1.9 s numbers came *after* that change and look
like a win.

They are not evidence. **Every fast run had Android in the CENTRAL role and every slow run had it
PERIPHERAL**, so role and rescan-delay are confounded and the hypothesis is untested. Filed as #206
with the confound stated. The follow-up is a pinned-role soak so role stops being a free variable.

## 6. Still open on #197

`BleLinkController` does not exist — the link-state composition sits as small functions in both
platform files, untestable on the host, which by this issue's own doctrine puts it in the wrong
place. `FakeBleRadio` exists but is private to the send-queue tests; with one consumer, promoting it
would be the speculative-engine-facility mistake the repo's own rule forbids. And the iOS
power-cycle fix is reasoned symmetry with Android's, **not device-verified** — toggling Bluetooth on
the iPhone needs a hand on the phone.

---

## 7. Addendum — the controller, and the result that overturned §5

Written later the same evening, after `86d56aa`.

### BleLinkController

The cutover left both drivers holding four policy objects and composing them by hand, and the two
compositions had already begun to differ (Android cancelled the start-retries on link; iOS did not).
Both defensible — iOS has no start-failure edge — but that is a judgement being re-made in a
platform file, which is the drift mechanism this phase exists to end. The controller owns all four
policies; drivers report facts and apply `Actions`. Android's policy block went from ~60 lines of
forwarding to one member and a `Tick`.

Device-verified behaviour-preserving: 2 matches, `sameFrame=99/99` and `636/637`, `gate=0`, and
radio-cycle recovery unchanged.

### §5's hypothesis is dead, and the finding is bigger

Five cycles on one build with roles alternating naturally between them — a cleaner A/B than pinning,
because no agent override was involved:

| cycle | Android's role | adapter-on → linked |
|---:|---|---|
| 1 | CENTRAL | 2.01 s |
| 2 | PERIPHERAL | none in 90 s |
| 3 | CENTRAL | 4.53 s |
| 4 | PERIPHERAL | none in 90 s |
| 5 | CENTRAL | 4.33 s |

Perfect correlation with role, 5/5. **The delayed rescan is not the fix and the 1.9 s figure was not
a win** — those five fast cycles all happened to land CENTRAL. And the peripheral case is worse than
§4 recorded: not a slow resolve but *deterministic non-recovery*, which means roughly half of all
radio-cycle recoveries fail, because the pair alternates.

The lesson is the one this repo keeps re-learning from the other direction. A measurement that
agrees with your hypothesis is the one to distrust: 1.9 s across five cycles looked like proof, and
the variable was never controlled. What made it decidable was not more cycles — it was recording the
role alongside the timing, i.e. instrumenting the thing that could explain the result away.
