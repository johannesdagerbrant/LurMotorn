# Session log — 2026-08-16

Frozen against `9668799`. Ordered by what happened, not by importance.

---

## 1. Closing out the BLE work (#206, #207, #208, #42, #197)

Carried over from the previous session and closed with evidence.

**#206** — the root cause was `startGattServer()` never closing the previous `BluetoothGattServer`.
After a radio power cycle it called `openGattServer` again, so two server instances registered against
one callback object; `sendResponse` answered only on the newest, and the peer's device-id read went
unanswered forever. The tell was doubled log lines arriving on two threads. Peripheral-role recovery
went from *never within 90 s* to **1.65–1.83 s across 5/5 cycles**. Six hypotheses were wrong before
this one and are recorded on the issue rather than quietly dropped.

Two engine facilities came out of it, both host-tested: `BleRadioState` (a power cycle invalidates
every started-state flag — both platforms had the hole) and `GattLongRead` (the ATT MTU read cap,
which is the *second* time that five-line arithmetic broke the link; #17 ignored the offset, #206
ignored the length).

**#207** — `pullrec` fetched nothing and then diffed three-week-old files that happened to be on disk,
printing a verdict that read as a real result. Three stacked causes: a raw-string listing that never
matched, an unquoted path containing a space, and a 60 s timeout. The reason it mattered more than a
tooling bug: *a tool that fails by producing an older answer instead of no answer.* The rig now skips
the diff outright when 0 recordings were pulled.

**#200** — the acceptance criterion was a literal grep for game names in `Modules/`. Re-measured: 100
raw hits, 3 of which matched **inside another word** (`rps` is a substring of **`lerps`**, so
`// the vertex shader lerps Prev->Cur` counted as a game name in the renderer). After word boundaries
and stripping comments the real count is **zero** — every remaining hit is a provenance comment.
Recorded the corrected reading: *no game concepts in the API; comments naming a game are fine and
often required*, because they are the evidence trail the doctrine depends on.

---

## 2. Phase 3 of the engine extraction (#43)

### Section C — thread topology as config

The section turned out to be about exactly one decision:

| | parking the renderer means |
|---|---|
| **Dedicated** (RPS, #183) | set the request, then **wait** for the render thread's ack. Skip it and main frees a `CAMetalLayer` the render thread is still drawing into. |
| **Inline** (chess, CADisplayLink) | main **is** the frame loop. That same wait is a self-deadlock. |

Both naive unifications are broken. `Lur::App::RenderHandshake::IsParked()` is that decision made
once, with 11 host tests; verified by sabotaging it in *both* directions (always-wait trips the two
Inline tests, never-wait trips the Dedicated one).

It absorbed eleven loose atomics out of `RpsMain.mm`, and then `LurReattachRenderHost` — the whole #73
heal, shared. `IosViewHost.h` had been carrying the reason that was impossible: *"the RENDERER half is
deliberately NOT here, because the two games legitimately differ… RPS must park its render thread
first."* That difference **was** the park. It went away, so the function followed.

Three silent drops were introduced and caught during the cutover, all from `TakeWork()` being a
*consuming* call where the old code read a flag. The nastiest: a reinit can arrive with **no park in
front of it** (MAIN proceeds anyway if the render thread misses its 1 s park cap), so handling reinit
inside the park branch consumes and discards it — and the renderer keeps drawing into the layer the
heal meant to replace.

**Android needed nothing.** `AndroidApp` already owns the surface lifecycle and both games render on
the glue thread, so there is no cross-thread renderer handoff to absorb. An earlier note in this
session claiming otherwise was wrong.

### Section F — stdlib diet

`Modules/Save/Private/Store.cpp` was the **last** shipping use of `<filesystem>` or `<fstream>`.
Replaced with stdio plus a ~40-line `opendir`/`FindFirstFile` shim. Then `-fno-exceptions` for the
host build, whose blocker (`Lur::Save` leaning on the throwing API) the same change removed.

The interesting part is in `02-lessons.md`: the suite went 33/33 immediately and that was worthless.

### Section D — input pipeline

`Lur::Input::TouchEvent` carried a `PointerId` with **no reader anywhere**, hard-coded to 0 by its one
writer — so "distinguishes fingers for multi-touch" described an intention. `PointerCount`, which two
consumers actually needed, had no room. Swapped.

`Ppu` / `WorldHeightF` / `WorldToFixed` / the ghost offset were defined separately in all three RPS
mains, character-for-character identical. `WorldToFixed` is the one that mattered: it produces the
value a `Place` event carries **across the wire** into a deterministic sim, so three copies is a latent
desync whose cause would be a duplicated one-line cast.

Then `Rps::TouchRouter`. The dispatch existed **four** times and had drifted **seven** ways:

| behaviour | Android | iOS | Desktop solo | Desktop loopback |
|---|---|---|---|---|
| ghost offset + magnetic snap | yes | yes | yes | **NO** |
| production-button press flash | yes | yes | yes | **NO** |
| cancel the console gesture mid-drag | **NO** | yes | n/a | n/a |
| pan only while one finger down | yes | **NO** | n/a | n/a |
| suppress the HUD tap after a chain | yes | yes | n/a | n/a |
| 24 px tap slop | yes | yes | **NO** | **NO** |
| a second finger is not a new press | yes | **NO** | n/a | n/a |

Resolved as the **union, not the intersection** — every row takes what the shipping phones do. A
router that split the difference would just be a fifth copy.

### Section E — agent verb table

96 lines on Android, 80 on iOS, same ten verbs. The drift was cosmetic *except* for one real defect in
both: **`console` wrote `GameView::SetDevOverlayOpen` from the sim thread**, while the view is owned by
another thread on both platforms and `DevOverlayOpen_` is a plain `bool`. The `gesture` verb three
cases below it hands its request across an atomic for exactly this reason and says so in a comment.

It had no tests because it is `#if LUR_AGENT` — absent from every app build, so until it left the
platform mains there was **nowhere a test could reach it**. That is why two copies were free to drift.
`rps_agent_router_tests` builds with `-DLUR_AGENT=1`.

---

## 3. The device pass

Both phones, non-agent builds (deliberately: the thing under test was touch, and an agent build can
inject input and fight the tester).

Verified automatically: `Lur::Save` **read** path on real persisted identity, BLE link in ~6 s, both
phones agreeing pre-match (`hash=8e1a062e gold=1900`), 60 fps both, Android background/foreground
clean.

### The two iOS bugs the host suite could not see

**`multipleTouchEnabled` was never set anywhere in the tree.** UIKit's default is `NO`, so the view was
handed exactly one touch — the second finger never arrived, `allTouches.count` could not reach 2, and
the iPhone was *physically incapable* of the two-finger console gesture. Pre-existing, and the
unfinished tail of #151, whose note says the iPhone "could not open it AT ALL". That was still true
afterwards, just less visibly: the gesture had been wired to a view that could not feel a second
finger.

Set on the **RPS** view controller, not the shared `LurMetalView` — chess is a one-finger game whose
taps commit moves.

Enabling it exposed two more: UIKit calls `touchesEnded` **once per lifting finger** (so a two-finger
tap arrived as two releases, the second falling through to the tap hit-test), and `touchesCancelled`
was never implemented at all.

**`LurRebuildViewHost` reset the replacement view to UIKit defaults**, so a #73 heal would have
silently undone the multi-touch fix — nothing logged, indistinguishable from the bug being unfixed. It
now carries the outgoing view's touch configuration across.

### The console gesture, measured rather than tuned

Reported as "picky about timing". Two independent causes produced identical symptoms, and the full
story is in `02-lessons.md`. The outcome: hold is fine everywhere (31–115 ms against 350 ms), the chain
window was the only failing gate, and the measured distribution has a clean gap —

```
chained fine   148 150 150 152 166 167 222 251 350 384 ms
reset          634 685 935 | 1503 1553 2083 2605 ms
```

At 600 ms the first three failures were **near misses** (634 ms is a 34 ms miss, which reads to a
player as "broken", not "fractionally slow"). Widened to 1000 ms, which sits in the empty band.

### #206's iOS half verified

`b219d2d` fired exactly as designed on the peripheral — the role #206 broke:

```
BLE radio powered OFF - dropping cached role, connect state and any link
peripheral powered on, publishing service
central powered on, scanning
```

No longer reasoning from Android's fix. Observed.

---

## 4. The netcode work the device pass triggered

### A draw is no longer an acceptable outcome (owner ruling)

Three things produced a draw. The gameplay one (both home bases destroyed on the same tick) **stays** —
it is a real outcome the sim earned. The two netcode ones are gone.

What made the change small: the old code justified the draw as the only outcome declarable
*symmetrically*, and that was true as far as it went — but `IsRecoverySurvivor()` **already** decides
whose timeline stands, from the device id, with nothing negotiated. Giving up was never a case where
that rule failed; it was the case where we stopped applying it.

`FailRecovery` now ends the **round**, not the match: counter reset, backoff armed (1 s doubling to a
15 s cap), `Result` untouched. `CeilingStall` holds the match open — an absent peer is not a
disagreement, so there is nothing to reconcile and nothing to declare.

Device-verified: **zero** draws across both full captures, backoff measured at **1000 → 2000 →
4000 ms** on hardware, match still `started=1` after interruptions that previously ended it twice.

### The history-exchange deadlock (#210), located from a capture

The no-draw change stopped the draw from *masking* the real problem, and the phones then showed two
live divergent games — iPhone frozen at tick 3042 with 10 units, Galaxy running to 3111 with 20.

`RequestRecovery` made the **survivor** an adopter like anyone else. A gap in the survivor's inbound
stream set it Awaiting a history the peer is *forbidden* to send — the answer path only lets the
survivor hand one over. Both then waited on each other. The counts said it plainly: the iPhone, which
logs *"we hold the lower device id"*, **asked for history 8 times**. It should be structurally
incapable of asking.

Fix: a gap on the survivor means we are missing the *loser's* input. The survivor's timeline stands by
definition, so publish it and let the peer conform; the lost input is dropped. Same trade the tie-break
makes everywhere else — consistency, not fairness, with both players in one room.

Device result: survivor asked **0** times (was 8), published **16**, and recoveries that **completed**
went from **0 to 45**.

### What is still open, and honestly unattributed

The pair still re-diverges **at the very next anchor** after every successful recovery — 46 desyncs,
every 10 ticks:

```
recovered — resuming from the peer's timeline at tick 3299
DESYNC at tick 3300 — mine 77412c5f, peer 71ff3a6d
recovered — resuming from the peer's timeline at tick 3300
DESYNC at tick 3310 — mine 3db4e882, peer a1fa8287
```

Two candidates and no evidence separating them: **cross-platform nondeterminism** (NDK clang vs Apple
clang), or **the survivor dropping inputs** as a consequence of the fix above.

It cannot currently be diagnosed, and that is the finding worth carrying forward — see
`03-two-peer-soak.md`.
