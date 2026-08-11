# Engine extraction — status report for whoever picks this up next

Frozen 2026-08-11 against `master` @ `6f450cc`. Written for a **fresh agent with no memory of this
work**. Read this, then act from the issues — per the repo's precedence rule, a snapshot is history
and **the issue always wins** on anything current. Re-verify every path and symbol against HEAD
before acting; they moved a lot in the last two days.

Companion documents:
- `Docs/Journal/2026-08-09/engine-extraction-plan.md` — the plan, the three diseases, the phases.
- `Docs/Journal/2026-08-09/RUN-LOG.md` — the blow-by-blow working record, including every dead end.
- `Docs/Journal/2026-08-09/autonomous-run-prompt.md` — how the run was driven.

---

## 1. Where the work stands

| Phase | Issue | State |
|---|---|---|
| 0 — sweep + de-name the engine | #200 | **closed** |
| 1 — platform layer move | #42 | done for Android + iOS; **Windows pair + a CLAUDE.md check remain** |
| 2 — BLE unification | #197 | policy written + host-tested; **send queue cut over; retry/watchdog not**; **soak not run** |
| 3 — `Modules/App` | #43 | not started; prerequisite now met |
| 4–7 | #8 #201 #198 #45 | untouched |

Also closed on the way: **#44** (de-chess `Modules/Net`) — done, but the *opposite* way to its own
plan; see its closing comment. **#47** is half done (Pairing deleted; `EBleRole` and docs remain).

### What physically changed

~3,400 lines of duplicated platform code became ~1,700 shared:

```
Modules/Transport/Platform/Android/BleShim.kt        ONE Kotlin radio, both games
Modules/Transport/Platform/Android/BleTransport.cpp  ONE JNI bridge, both games
Modules/Transport/Platform/Ios/BleTransport.mm       ONE CoreBluetooth driver, both games
Modules/Render/Platform/{Android,Ios,Windows}/VulkanSurface.*
Modules/Audio/Platform/{Android,Ios}/AudioDevice.*
Modules/Transport/{Public,Private}/  BleSendQueue, BleStartRetry, BleDiscoveryTimers, IBleRadio
```

`Modules/Transport` is a **compiled library** now (was header-only). Engine test binaries link no
game. `grep -rnE "\b([Cc]hess|RPS|Rps|rps|OnlyChess|OnlyRps)\b" Modules/` is clean except one
deliberate, in-source-documented exception: the `ProtocolVersion` changelog in `Session.h`, which
leaves with the version split in Phase 4.

---

## 2. The single most important thing to know

**This codebase's failures are usually silent, and usually look like the radio.** Every real defect
found in two days was invisible at the point of failure and pointed somewhere else:

| Symptom | Actual cause |
|---|---|
| Two phones never link, no error | one game's BLE service UUID inherited from a **default** that was chess's |
| Moves feel slow | a **data race** — a queue documented single-threaded, driven from a Binder callback thread |
| Log says "escalating to a full radio restart ×3" | the transport had **no restart implementation**; the escalation was narrating nothing |
| A latency optimization stops working | both backends inferred urgency from datagram **length**, and a framing change broke the inference |
| "BLE is unstable", half-open, radio restarts | the **phone's lock screen** — the app loop stops, so the peer sees connected-but-silent |

Practical consequences, in order of how much time they save:

1. **Screenshot the phone before believing any BLE verdict.** A locked Galaxy reproduces #163's
   half-open signature exactly. The tell is a wake-up burst of `hello RECV` sharing one millisecond.
2. **A green autoplay gate is not proof of absence for a timing bug.** The slow-moves race passed my
   load test at 49 ms and was caught by a human playing.
3. **After any framing/wire change, grep the whole stack for length checks** (`size == 1`,
   `Size == 1`). An invariant asserted in prose, in a file no test compiles, is not an invariant.

---

## 3. Doctrines this work established (follow these)

**Required build parameters, never defaults.** What is genuinely per-app is a compile definition
with **no default**, so a game that forgets fails to build: `LUR_BLE_SERVICE_UUID` (#200) and
`LUR_LOG_TAG` (#42), both `#error`ing via their headers. The rule was earned: the UUID's default was
chess's, so a forgetful game inherited chess's *identity* rather than none, and that fails silently
as two phones that never see each other.

**Read across the seam instead of duplicating.** The Kotlin declared all three BLE UUIDs under a
comment reading *"MUST match `Lur::Transport::BleProtocol`"* — a duplication maintained by hope. They
are now fetched from C++ over JNI. Prefer this to any "keep these in sync" comment.

**Capability queries need an implementor audit.** `ITransport::CanRestartRadio()` defaults false so a
transport without a hard restart is never narrated as having one — which silently disarmed RPS's
*working* #182 recovery until every implementor was checked. Adding a false-by-default query means
auditing everyone who has the feature, not just whoever lacks it.

**Platform files hold verbs; engine C++ holds decisions.** The test: *could this be unit-tested on
the host against a fake?* If yes and it is in Kotlin/ObjC++, it is in the wrong place — that is
exactly how 650 lines of drift accumulated.

**A missing capability is a finding, not a silence.** Prefer saying "this transport cannot restart
the radio" once over logging three repairs that never happened.

---

## 4. Traps that cost real time here

- **Out-of-tree app targets cannot see the engine tree's `add_compile_definitions`.** Every app
  re-applies `LUR_*` (and now `LUR_LOG_TAG`) to its own target. Hit twice.
- **`-fobjc-arc` is not a property of the file.** It is set by whichever target compiles the `.mm`,
  so it must move *with* the file.
- **JNI natives bind at `JNI_OnLoad`, i.e. library-load time.** The app must load its own library
  before touching the shim, or the first call throws `UnsatisfiedLinkError` somewhere confusing.
  `JNI_OnLoad` now returns `JNI_ERR` on a signature mismatch so the *load* fails instead.
- **iOS log redaction.** `NSLog` redacts a **dynamic** C string to `<private>` (compile-time
  literals still render, which hides it). Adding `%{public}s` to an `NSLog` format is *worse* —
  `<decode: missing data>`, because that is `os_log` syntax. Working shape: format with `NSString`,
  emit as one `%{public}s`.
- **`build.ps1` green says nothing about `VulkanBackend.cpp` or any platform file** — they compile
  only in app builds. Compile-check Android after touching `Modules/*/Platform` or `Render/Private`.
- **`.gitignore` only honours `#` at the start of a line.** `/build-agent/  # comment` matched
  nothing for as long as it existed.
- **Never `git add -A`** here; untracked scratch (`MoltenVK/`, `build-opt/`, screenshots) gets swept
  in. Stage explicit paths.

---

## 5. Device operations (both phones, verified working)

| Peer | Identity |
|---|---|
| Android | Galaxy A14 `R83WA14EAMK`, wireless ADB |
| iOS | iPhone 11 Pro `00008030-001645420A32802E`, iOS 26.5.2, USB |

- **Wireless ADB drops constantly and the port rotates.** `adb mdns services` shows the current one.
  `failed to connect` on the advertised port means the service is up but our TLS key is rejected →
  **`adb pair <ip>:<PAIRING-port> <code>`**, where the pairing port is a *different* mDNS entry
  (`_adb-tls-pairing._tcp`) shown only while that phone dialog is open. `actively refused` means
  nothing is listening (wireless debugging off). Toggling rotates the port but does **not** re-trust
  the host.
- **Autoplay needs an agent build.** Chess autoplay is `#if LUR_AGENT` in both mains, so an ordinary
  APK/`.ipa` has no autoplayer and the rig waits forever at `matches ended=0`. Use
  `./gradlew assembleDebug -PlurAgent=ON` and `gh workflow run "macOS CI" -f agent=true`.
  (`Tools/DeviceRig/README.md` still says `LUR_INTERNAL` — stale, fix in Phase 7.)
- **iOS install fails while the app runs** — the rig says so plainly, and the message scrolls past
  above passing output. It nearly produced a false pass here; kill the process and reinstall, then
  confirm.
- **Close-out is mandatory**: rebuild without `-DLUR_AGENT`, reinstall both, clear
  `debug.lur.{autoplay,agent.cmd,role}` and every `Documents/` marker.

---

## 6. What to do next, in order

1. **Run the two-phone soak** — #197's gate to Phase 3, and the one piece of Phase 2 with no
   substitute. `droptx`, `killown`, `adb shell svc bluetooth disable/enable`, confirming recovery
   every time. Everything else in Phase 2 is landed and verified; this is the missing evidence.
2. **Cut the drivers over to `BleStartRetry` + `BleDiscoveryTimers`.** Bigger than the send queue
   was: those **invert control** — the driver must *ask* the policy what to do rather than being
   handed bytes to write. Until this lands, **RPS still silently never advertises if its first
   attempt fails** (#194); the fix is written and tested but not wired.
3. **Finish #42's tail**: the Windows desktop pair (10 diff lines) and the CLAUDE.md architecture
   table, which is now *true* and needs verifying rather than rewriting.
4. **Then Phase 3 (#43).** See its comment for what changed underneath it.

### Before the next RPS device gate
Turn `rps.dev.flight_recorder` **on** on the Galaxy (its `cvars.cfg` fixture persists it false).
Without it `pullrec --recdiff` finds only stale captures, and the tick-by-tick peer diff — the
strongest desync instrument — does not run. `desync=0` alone can be **vacuous**: `CrossCheck`
returns early when no peer hash exists for a tick.

---

## 7. Honest state of verification

**Chess is well verified.** 72+ matches across the run, ~100% same-frame, zero desync, on builds
confirmed installed. The framed-move change (`ProtocolVersion` 10) and the restored #190 fast path
are both measured on hardware — iOS max round-trip improved 218 ms → 101 ms.

**RPS is verified as "links, runs, no fault" — NOT "provably identical on both sides."** Both peers
link, hold the same tick range, place their own camps and report clean counters, but the recording
diff has never run (see §6). Treat RPS's netcode as unre-verified since Phase 0 until it does.

**The soak has not run at all.** No drop/reconnect cycle has been exercised since the collapse, and
the collapse touched the exact code that handles it.
