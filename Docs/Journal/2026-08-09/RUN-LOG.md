# RUN-LOG — autonomous engine-extraction run

Started 2026-08-09 against `master` @ `32c8886`. This is the agent's own progress record, written
during execution — not a frozen snapshot. Read this FIRST after any context compaction, then the
current phase's issue, then continue from the first unticked item.

Prompt: `Docs/Journal/2026-08-09/autonomous-run-prompt.md`. Plan: `engine-extraction-plan.md`.

## Devices at start of run

| Peer | Identity | Transport | Status |
|---|---|---|---|
| Android | Galaxy A14 `R83WA14EAMK` | wireless ADB (`adb-R83WA14EAMK-ME8WPy._adb-tls-connect._tcp`) | alive |
| iOS | iPhone 11 Pro `00008030-001645420A32802E`, iOS 26.5.2 | USB (`pymobiledevice3 usbmux`) | alive |

Note: `adb devices` was empty at start and `adb mdns services` listed nothing; issuing a throwaway
`adb connect <lan-ip>:5555` forced mDNS resolution and the phone appeared. Worth trying before
concluding the Galaxy is off the network.

## Standing findings (apply to every later phase)

- **`build.ps1` green says nothing about the Vulkan backend.** `VulkanBackend.cpp` is compiled only
  in the app builds, so a change to it is invisible to the host suite. It took an Android build to
  catch a broken constructor call. Compile-check Android after touching `Modules/Render/Private`.
- **The acceptance grep needs word boundaries.** `grep -riE "rps"` matches "le**rps**" in
  `Renderer.h`/`VulkanBackend.cpp`. Use `grep -rnE "\b([Cc]hess|RPS|Rps|rps|OnlyChess|OnlyRps)\b"`.
- **`.gitignore` only honours `#` at the start of a line.** `/build-agent/   # comment` matched
  nothing for as long as it has existed. Fixed in this run; don't reintroduce the pattern.
- **Avoid backslash escapes inside a python heredoc** driven from the Bash tool — `'\0'` and `\n`
  reached the file as literal NUL/newline twice. Write the script to the scratchpad and run it.

## Phase 0 — Sweep dead code + remove game names from the engine (#200)  [IN PROGRESS]

### A. Delete dead code — DONE
- [x] `Modules/Pairing` deleted (whole module + `add_subdirectory`) — verified: host. (#47)
- [x] `Hud/LinkStatusBar.{h,cpp}` deleted — verified: host
- [x] Hud→Net layering unpicked: `DebugOverlay` took `ELinkState` only to run a switch, so Net
      gained `LinkStateName()` and `lur_hud` dropped `lur_net` — test: `TestLinkStateNames`
      (walks every slot, RED first) — verified: host
- [x] `Text/FontRegistry` + its tests deleted; the multi-font test rewritten against bare `Font`
      — verified: host
- [x] `Math`: `Quat.h`, `Mat4::Perspective`, `Mat4::LookAt` deleted — verified: host. #9's
      prerequisite (re-add WITH tests) already recorded on that issue.
- [x] `Net/ClockSync.h` deleted. `EMsgType::ClockPing/ClockPong` KEPT as `Retired1/Retired2` —
      removing an enumerator renumbers every later slot. Reclaimed in the version-reset phase.
- [x] `Core/Hash.h` resolved by making it LIVE, not by deleting: RPS's `Sim::StateHash` carried a
      byte-identical private copy of FNV-1a, and now mixes through the engine primitive — test:
      `TestStateHashGoldenValues` (pins the hash before/after; RED first with placeholder values)
      — verified: host
- [x] Swept further: `net_tests` and `transport_tests` both linked `chess::core`. Ten chess
      integration tests moved verbatim to `Games/Chess/Core/Tests/ChessSessionTests.cpp` — test:
      the new `chess_session_tests` target — verified: host

### B. Remove game names from the engine — DONE
- [x] `VulkanBackend.cpp` `pApplicationName = "OnlyChess"` → `VulkanRenderer::Create(AppName)`,
      required (no default). Not host-testable (needs a real Vulkan device) — verified:
      device gate below
- [x] `BleProtocol.h` `LUR_BLE_SERVICE_UUID` default REMOVED (`#error` if undefined). Chess states
      its previous value, so no wire identity moved. The compile-time guard is the test —
      verified: host + android + ios builds
- [x] `Session.h` `SendMove`/`SetMoveHandler` and the "1-byte datagram is always a move" dispatch
      rule DELETED; chess moves framed on `Game1` — test:
      `TestOneByteDatagramDispatchesByType` (RED first) — verified: host
      **`ProtocolVersion` 9 → 10.** Chess wire change: both phones must install together.
- [x] Comment sweep across 14 modules — verified: word-bounded acceptance grep

### C. Acceptance
- [x] `build.ps1` green — 25/25 (was 24; the chess/net test split adds one)
- [x] chess Android `assembleDebug` — BUILD SUCCESSFUL
- [x] RPS Android `assembleDebug` — BUILD SUCCESSFUL
- [x] chess desktop + RPS desktop — both green
- [x] iOS via CI — run 31329545375, all four jobs green (both games' `.ipa`s built)
- [ ] **Both games play a real match phone-to-phone — BLOCKED, see below**
- [x] Acceptance grep clean except ONE deliberate, documented exception marked in the source:
      the `ProtocolVersion` changelog names which game earned each version. Kept because it is
      why a number exists, not an API shape; it leaves with the engine/per-game version split.

Gate: build.ps1 PASS | android PASS | desktop PASS | ios PASS | two-phone **BLOCKED**

### BLOCKED — the two-phone match needs a human to unlock the Galaxy

**The blocker.** The Galaxy sits on a "Swipe to unlock" lock screen and will not accept injected
input there: `input swipe`, `input keyevent MENU/POWER`, `wm dismiss-keyguard` and
`locksettings set-disabled true` all report success and change nothing (`mCurrentFocus` stays
`NotificationShade`, `mDreamingLockscreen=true`). Screenshots confirm it. This is the same class
of limit as [[android-headless-verification]]'s "no multitouch (SELinux blocks evdev)". **A human
must swipe the phone once**; after that the whole loop is headless again.

**Why it looked like a BLE fault for two hours.** With the app behind the lock screen the Android
main loop stops ticking, so `Session::Tick` never runs — no Hello, no keepalive — while inbound
datagrams keep queueing in the `EventInbox`. The peer sees a connected link that has gone silent,
which is *exactly* the #163 half-open signature, and Android duly escalated through all three #182
radio restarts. On the next wake the app drained the whole backlog in one tick (a burst of
`hello RECV` at a single millisecond) — the tell that it was a stalled loop, not a wedged radio.
**Check the lock screen before believing a half-open verdict.**

**What WAS proven on the two phones**, repeatedly, with fresh processes on both sides:
- v10 ↔ v10 Hello handshake completes over real BLE, both directions
  (`hello RECV → READY → hello SENT (ready=1)` on iOS; `hello RECV → READY` on Android).
- A FRAMED message crosses the real radio and is applied: `recv msg type=7 size=5` →
  `resync received — moves enabled` on both peers.
- MTU 517, `central: linked + notifications on`, role tie-break settled — unchanged by this phase.

**What is NOT yet proven on device:** a chess MOVE crossing, now that it is framed on `Game1`
rather than a bare 1-byte datagram. Covered on the host by `chess_session_tests` through the real
`Session` + `LoopbackTransport` composition, and it uses byte-identical machinery to the framed
`Sync` that DID cross the radio — but that is inference, not observation. **Deferred, retry at the
Phase 1 gate.**

**Second finding, cost ~1h:** chess autoplay is `#if LUR_AGENT` in BOTH mains
(`AndroidMain.cpp` and `AppMain.mm`), so the ordinary CI `.ipa` and an ordinary Gradle APK contain
no autoplayer at all — arming it does nothing and the rig waits forever with `matches ended=0`.
An autonomous match needs `-PlurAgent=ON` (Android) and `gh workflow run "macOS CI" -f agent=true`
(iOS). **`Tools/DeviceRig/README.md` says the autoplayer is `#if LUR_INTERNAL` — that is STALE**
(it moved with #195/#196). Fix in Phase 7's doc pass.

Third: the rig's `-Action run` never aborts once a link has been seen — it sat at
`matches ended=0 elapsed=1,391s`. `-LinkTimeoutSec` only guards the pre-link phase. Worth a
match-progress deadline; noted for the Phase 6 tooling pass.

**Close-out done** (per the prompt, no exceptions): both phones rebuilt and reinstalled WITHOUT
`-DLUR_AGENT` so the harness code is absent rather than idle; `debug.lur.{autoplay,agent.cmd,role}`
cleared; `Documents/autoplay` deleted; `svc power stayon` and the lock-screen setting I had changed
both restored.

## Phase 1 — Platform layer move (#42)  [PARTIAL — render + audio done, transport deferred]

- [x] **Vulkan surface seam** → `Modules/Render/Platform/{Android,Ios}/VulkanSurface.{cpp,mm}`.
      Four app copies collapse to two engine files; after name normalization they were
      byte-identical, differing only in the log tag. The Windows branch of `Modules/Render` had
      been doing this correctly all along — Android and iOS now match it, and the games supply no
      render code at all. — verified: host, android(both games), ios(CI both games)
- [x] **Log tag parameterized** — `LUR_LOG_TAG`, set by each app tree before it adds the engine
      root, consumed via `Lur/Core/LogTag.h`. Required, no default (`#error`), same discipline as
      `LUR_BLE_SERVICE_UUID`. The desktop tree leaves it empty deliberately: its seam logs through
      `Lur::Log` and never includes the header. — verified: android + ios builds
- [x] **Audio device seam** → `Modules/Audio/Platform/{Android,Ios}/AudioDevice.{cpp,mm}`.
      Moved BEFORE RPS gets sound (#81), so this prevents a duplication rather than fixing one.
      No null device added for host/desktop — nothing calls `CreateAudioDevice` there, so it stays
      undefined and a build wanting sound without a backend fails to LINK. — verified: host,
      android, ios(CI)
- [ ] **BLE transport move + dynamic JNI registration — DEFERRED, see below**
- [ ] `CLAUDE.md` architecture table refresh — belongs with the transport move

Gate so far: build.ps1 PASS (25/25) | android PASS (both games) | ios CI PASS (run 31333080944,
both games) | desktop PASS | two-phone still blocked (Phase 0 blocker, unchanged)

### Why the BLE move is deferred rather than attempted

Measured at HEAD, after normalizing the game names away, the six backends are **650 diff lines
apart** over ~3,300 lines:

| File | Chess | RPS | Normalized diff |
|---|---:|---:|---:|
| `BleShim.kt` | 740 | 760 | 299 |
| `AndroidBleTransport.cpp` | 286 | 324 | 221 |
| `IosBleTransport.mm` | 575 | 636 | 131 |

Collapsing six copies to three means **choosing**, hunk by hunk, between two divergent versions of
the most battle-hardened code in the repo (#83/#146/#163/#182/#190/#194) — the plan's own words.
Right now that merge could be verified by neither of the two things that would catch a mistake:

1. **No host test reaches it.** `FakeBleRadio` is Phase 2's deliverable; today the decision logic
   lives in Kotlin and ObjC++ where `build.ps1` cannot see it. That is the whole finding behind
   #197.
2. **No two-phone verification is available** — the Phase 0 blocker (the Galaxy needs one human
   swipe).

Landing a blind 650-line merge into the path whose failure mode is *"two phones silently never
link, with no error to point at"* is precisely the risk the plan flags. It waits for either a host
fake or a working device pair; both are close (Phase 2 builds the fake). Everything in Phase 1 that
does NOT depend on that — the render and audio seams, 4 → 2 files and 2 → 2, plus the log-tag
parameter they both needed — is landed and verified on all three toolchains.
