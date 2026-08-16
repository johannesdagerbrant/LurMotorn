# Lessons — 2026-08-16

Frozen against `9668799`. These are the ones that generalise. Every item is something that actually
happened today, not advice in the abstract.

---

## 1. A test you have not tried to break is not a test

This happened **three separate times in one session**, and every time the suite was green first.

| where | the sabotage that passed anyway |
|---|---|
| `Lur::Save` (section F) | made the recursive `mkdir` create only the leaf level → **passed**. Stopped `ListKeys` skipping `.tmp` → **passed**. |
| `Rps::TouchRouter` (section D) | **three of nine** tests survived a deliberately broken router |
| `Rps::AgentCommandRouter` (section E) | the `queue` test survived `Emit(Queue(Team, Slot, 1))` — it only checked `Team` |

The `Lur::Save` case is the clearest. Every existing test used a directory whose parent already
existed, so **one** `mkdir` was always enough — and the deep-tree case is the *real device case*: iOS
hands back `…/Application Support/OnlyRps` and nothing has made it.

The `TouchRouter` case is the most instructive: the three vacuous tests all asserted *"nothing was
emitted"* in situations where nothing could have been emitted anyway. They read like real tests. They
were shaped exactly like the real ones next to them.

**The rule that came out of it:** after writing a test, break the thing it tests and watch it fail.
Tests that cannot be made to fail should be **deleted, not kept to pad the count** — a vacuous test is
worse than an absent one, because it reads as coverage. `TouchRouterTests.cpp` says so in its header,
along with what it still does *not* cover and why.

---

## 2. Success-shaped signals, five of them, in one day

CLAUDE.md's *Evidence* section is not abstract. Today's crop:

**`--aidiag` has no lockstep in it.** Trying to find out whether the phones' divergence was mine, I ran
the desktop AI diagnostic for 1195 ticks, saw zero desyncs, and nearly reported it as evidence of
determinism. `RunAiDiag` is a **single `Rps::Sim` with two AI controllers** — zero `LockstepPeer`. It
could not have shown a desync if one existed.

**Then the real two-peer path reported `desyncA=0 desyncB=0`** — and `ticks A=0 B=0`. It had not
simulated anything.

**A stale binary printed "All save tests passed"** after a build that had just failed, because the
command was `cmake --build …; ./exe` with a `;`. Use `&&`.

**A capture read mid-flight** looked like it had died four minutes early. It had not; it was still
being written. I reported the wrong thing and had to correct it.

**`gh run download` succeeded from a run whose overall conclusion was `failure`.** The failure was the
*chess* artifact upload timing out — infrastructure, not a compile error — but "CI failed, artifact
installed anyway" is exactly the shape that puts the wrong build on a phone.

---

## 3. Two causes can wear identical symptoms — measure before tuning

The console gesture was reported as "more picky about timing". The temptation was to widen a window.

- **First theory:** Android's switch from `NowNs()` to `AMotionEvent_getEventTime` stopped event
  batching from flattering the hold window. **Killed by the live trace** — `input.dispatch` is a steady
  ~15 ms applied to *both* the down and the up, so it cancels out of every interval measured.
- **Second theory:** after two samples showing 2.1 s and 5.6 s gaps, I asserted *"no sane widening
  catches them"*. **Wrong.** Seven samples later, three sat in the 634–935 ms band.
- **Actual first cause:** `multipleTouchEnabled` was never set. The iPhone could not physically
  perform the gesture. Nothing about timing.
- **Actual second cause:** once two fingers were delivered, the 600 ms chain window really was too
  tight at the margin.

**Two independent causes, identical symptom, and the first fix made the second one visible.** Had I
widened the window on the first report, I would have "fixed" the symptom, shipped a phone that still
could not reliably open its console, and buried the real bug under a tuning change.

The instrumentation is what settled it, and it stayed in: `ConsoleGesture::LastLift()` reports hold,
chain and resulting count on every two-finger lift. *Feel is not debuggable; 634 ms against a 600 ms
window is.*

---

## 4. Run a control round

The iOS power-cycle test required reaching Settings, which backgrounds the app — so a failure could
have been the radio cycle *or* the backgrounding, which was separately unverified.

Round 1 (background, **no** Bluetooth toggle) and round 2 (the actual cycle) were run separately. That
single extra round is what made the result readable, twice:

- first pass — both rounds "failed" identically, which immediately ruled out the power-cycle fix as the
  cause (round 1 never touched the radio). It later turned out the user had switched to a solo AI
  match, which legitimately resets to the pre-match hold.
- second pass — round 1 recovered cleanly and round 2 did not, which is what made the divergence
  attributable to the radio cycle at all.

---

## 5. Device traps that cost real time

**A locked Galaxy fakes a half-open link.** Already in CLAUDE.md, and it still cost a round: `presented=0`
with a healthy-looking `LOCKSTEP` line is the signature. `mScreenOn=false`/`mWakefulness=Dozing`
confirms it. Injected input cannot unlock it. The fix for the rest of the session was
`adb shell svc power stayon true`.

**Wireless ADB ports rotate.** The Galaxy vanished mid-session; `adb reconnect`, the old port and mDNS
all failed. A parallel TCP scan of 30000–65535 found it on **53601**, and — usefully — restarting the
adb server made the mDNS transport (`adb-<serial>._adb-tls-connect._tcp`) resolve again, which is
port-independent and the better handle to keep.

**Never chain `adb` in front of a capture you need.** A background command of the form
`adb logcat -c; python -m pymobiledevice3 syslog …` hung on `- waiting for device -` and the iOS
capture **never started**, losing a full 9-minute window on both phones. Separate commands.

**Bound every capture and check it overlapped the event.** Two windows recorded nothing because the
"tap now" instruction reached the user after the window had already opened. A capture that caught
nothing looks identical to a feature that produced nothing.

---

## 6. When the code's own comment is the specification, read it before overruling it

Twice today a load-bearing comment turned out to be exactly right about *why* something could not be
shared — and the correct move was to remove the reason rather than the comment.

- `IosViewHost.h`: *"the RENDERER half is deliberately NOT here, because the two games legitimately
  differ."* The difference was the park. Section C removed it; the function then followed.
- `LockstepPeer`: *"a draw is the one outcome that can be declared SYMMETRICALLY."* True — but
  `IsRecoverySurvivor()` already declares one symmetrically. The draw was never required by the
  argument that justified it.

And once the comment was right while the code was not: the `ResyncTagRequest` handler said *"only the
survivor answers"* while implementing `RecoveryAdopting_ && !IsRecoverySurvivor()`, which enforces it
only while the loser happens to be mid-adoption. It answered 12 times in the measured match.

---

## 7. Say which half of a fix is proven

Two changes went into the #210 repair. Only one is demonstrated: reverting the survivor-never-adopts
change fails 7 checks; reverting the answer-guard tightening fails nothing, and it is in fact
*unreachable* now that only the loser sends requests.

It was kept — it makes the stated invariant true and is the line that would matter if a future path
made the survivor ask again — but it is **labelled in the code as unproven** rather than bundled into
the commit as part of the repair. Two changes shipped; the message says which one is load-bearing.

---

## 8. The gap this session actually exposed

CLAUDE.md says *"the whole netcode is proven in two windows on one desktop before a phone is in the
room"*. **That is not true today for a long match with real input.** `RunLoopback` is the only two-peer
lockstep path and its auto-soak was never wired (`(void)Auto;` — "#137b: re-wires to events in #140",
which never happened). Every AI mode is single-sim.

Which is why this class of bug only ever surfaces on hardware, after a two-phone session, at the end
of a long day. See `03-two-peer-soak.md`.
