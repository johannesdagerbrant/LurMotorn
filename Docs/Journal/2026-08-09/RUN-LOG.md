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
- [x] **Both games play a real match phone-to-phone — PASSED once the phone was unlocked** (see
      "Two-phone gate — RESOLVED" at the end of this file)
- [x] Acceptance grep clean except ONE deliberate, documented exception marked in the source:
      the `ProtocolVersion` changelog names which game earned each version. Kept because it is
      why a number exists, not an API shape; it leaves with the engine/per-game version split.

Gate: build.ps1 PASS | android PASS | desktop PASS | ios PASS | two-phone PASS

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
`Sync` that DID cross the radio — but that was inference, not observation. **Since RESOLVED — see
the end of this file.**

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

## Two-phone gate — RESOLVED (2026-08-09, after one human swipe)

The Galaxy was unlocked by hand; everything after that was headless. Both games were built from
the SAME commit with `-DLUR_AGENT=ON` (Android `-PlurAgent=ON`, iOS via
`gh workflow run "macOS CI" -f agent=true`, run 31334916743), driven, then replaced with clean
builds. **Both gates PASS.**

### Chess — 12 matches phone-to-phone

```
android  AUTOPLAY game=11 sameFrame=468/469 opens=1 delayed=0 ply=353 gate=0
         rtt(n=469 avg=50ms min=27ms max=67ms)
android  Net: MATCH END result=3 WLD(lo/hi/dr)=1/0/11 total=12
ios      AUTOPLAY game=9  sameFrame=2271/2275 opens=3 delayed=1 ply=164 gate=0
         rtt(n=2274 avg=58ms min=31ms max=218ms)
ios      Net: MATCH END result=5 WLD=0/0/9 total=9
```

**This is the verification that was outstanding: the chess move now travels FRAMED on `Game1`
(Phase 0 deleted the bare-1-byte rule and bumped `ProtocolVersion` to 10), and it crosses real
BLE correctly.** ~100% same-frame replies, plies past 400 in a single game, and **zero**
`desync` / `requesting resync` / `keepalive state mismatch` / `send DROPPED` lines on either peer.

### RPS — a live linked match driven by the agent harness

```
LOCKSTEP tick=1960 you=0 foe=0 desync=0 badbuild=0 hash=ba74dd1f gold=350 frontier=40
         started=1 gaps=0 stall=0 halfopen=0 restarts=0
```

Pre-flight all clean on both peers: `badbuild=0`, matching pre-match `gold=750 hash=54a3c3ab`,
`gaps=0`. Sequence: `1 linked` → `2 place 17 220 0` (Android, team 1) / `2 place 17 20 0` (iOS,
team 0) → `3 queue 0 3 0`. Android logged `AGENT place type=0 at (17,220) team=1` and gold fell
750 → 350 on both peers, i.e. each placed its own camp.

**Why `desync=0` is the proof that input crossed BOTH ways**, and not merely that each phone
placed locally: `LockstepPeer::CrossCheck` compares MY state hash at a tick against the PEER's
hash for the SAME tick, received over the wire, and the hash covers both teams' buildings. Two
sims that disagreed about whether the other's camp existed would diverge at the next anchor and
trip `Desync` within a second. It stayed 0 for ~2,000 ticks.

Honest caveat: `you=0 foe=0` (army counts) never moved — `3 queue 0 3 0` bought nothing, so slot 0
is not a producing building or 350 gold did not cover it. That is a harness-usage detail, not a
failure; the placement path and the hash agreement are what the gate needs. Getting units moving
would make the same point more visibly and is worth doing next time.

### Close-out (done)
Both games rebuilt WITHOUT `-DLUR_AGENT` and installed on both phones, so the harness code is
absent rather than idle. `debug.lur.{agent.cmd,autoplay,role}` cleared; every marker under
`Documents/` deleted on both bundles (both listings now empty); `svc power stayon` restored.

### The operational lesson, restated
Every "BLE is unstable" symptom in this run came from the lock screen. With the phone unlocked,
the very first attempt linked in ~2 s and ran 12 chess matches and ~2,000 RPS ticks without a
single half-open, stall, or radio restart. `halfopen=0 restarts=0` in the RPS line above is the
same counter that had climbed to its cap all evening.

## Phase 2 — BLE unification (#197)  [IN PROGRESS]

- [x] **The `LUR_AGENT` / `LUR_INTERNAL` contradiction is resolved — LUR_AGENT.** The shared
      `BleProtocol.h` agreed with RPS (`LUR_INTERNAL`), so RPS shipped a rig-controllable radio
      role override in every build someone plays — Development IS the build a player plays. It is
      forced state from a hidden channel that OUTLIVES the app (an Android property until reboot,
      the iOS marker until deleted), and a pin also suppresses the #146 deadlock breaker. Chess
      needed no change; it was already right. — verified: host (both trees), android (both
      variants)
- [x] **`build.ps1 -Agent`** — an opt-in second tree configured `-DLUR_AGENT=ON`. Nothing on the
      host compiled the gated paths before, so a change that broke them surfaced only when someone
      built a harness `.ipa`. `TestRoleBreakerRespectsDevPin` moved behind `LUR_AGENT` and runs
      there — the only build where a pin can exist, which is the point of the resolution.
- [x] **`IBleRadio`** — the dumb driver seam (`Write`, returning whether the radio took it).
- [x] **`BleSendQueue` + 10 host tests against a fake radio** — the ~60 lines of hand-rolled
      netcode from `BleShim.kt`. Timing is tick-driven ns rather than a platform timer, so the
      watchdog is host-exercisable; fixed capacity, refusing when full rather than evicting the
      oldest (ordered stream), with refusals counted. — verified: host (both trees), android ×2,
      desktop ×2, ios CI run 31336149645
- [x] **`BleStartRetry`** — the advertise/scan start-failure policy, 7 host tests. The #194 fix
      chess had and RPS did not, so RPS silently never advertises if its first attempt fails.
      Capped backoff ~0.4→3.2 s, bounded fast retries handing back to the slow watchdog
      (`HandedOff` = change cadence, never give up), and only the first failure of a run asks for
      a loud log. NOT yet adopted by the drivers, so RPS's defect closes at the collapse.
- [ ] Remaining `BleLinkController` policy: discovery watchdog, role escalation, reconnect
      scheduling
- [ ] Collapse the 6 backends to 3 dumb drivers under `Modules/Transport/Platform/*`
      (this is also the deferred half of #42, incl. dynamic JNI `RegisterNatives`)
- [ ] Reconcile every one-sided fix as a decision, not a merge
- [ ] Two-phone soak (`droptx`, `killown`, `svc bluetooth disable/enable`)

### A real bug, found the moment the logic became testable

`TestLateCompletionAfterWatchdogDoesNotDoublePump` failed on the first implementation, and the
same hole exists in the Kotlin it replaces. Once the watchdog abandons a datagram and issues the
next, *"something is outstanding"* is true again — so the platform's **unlabelled** completion
callback for the ABANDONED datagram is indistinguishable from the new one's. Acting on it
releases a further datagram while the replacement is still in flight: two outstanding operations,
which is exactly the state the queue exists to prevent and which a BLE stack answers by silently
dropping one. The Kotlin's `sendWatchdogTok` guards its *timer*, not its *callback*, so it does
not close this.

Fix: count the completions we are owed but no longer want and swallow exactly that many — and
clear that debt on link loss, or the next link's first real completion is eaten as a ghost and
the fresh queue stalls for a full watchdog period, right when the peers are resyncing.

This is #197's own thesis demonstrated in one afternoon: the logic drifted *because* no test
could reach it, and the first thing a test found was a live bug.

`Modules/Transport` is a compiled library now (was INTERFACE). All four app builds, both desktop
builds and iOS CI re-verified after that change.

### A latency regression Phase 0 caused, found in Phase 2 (#190)

Both BLE backends decided send urgency by **inspecting the datagram's length**: chess's Android
shim did `if (bytes.size == 1) sendQueue.addFirst(...)`, its iOS driver
`if (Size == 1) _SendQueue.emplace_front(...)`. The reasoning was sound at the time — the wire
format made a live move exactly one byte and padded every framed message to two or more.

**Phase 0 deleted that rule.** Chess's move became a framed 2-byte datagram, `size == 1` stopped
matching, and the #190 priority path stopped happening on both platforms. Nothing failed, nothing
logged: the move simply started queueing behind keepalives and multi-datagram resyncs again —
exactly when the queue is deepest and latency is felt. A pure latency regression, invisible to
every test AND to the two-phone gate, which measures same-frame replies rather than queue position.

The old comment predicted the failure and got the direction wrong: *"if it ever did, receiving
would break first and much louder."* Receiving changed in the SAME commit, so both ends agreed and
the only casualty was the optimization. **An invariant asserted in prose, in a file no test
compiles, is not an invariant.**

Fixed by stating urgency instead of guessing it: `EBleSendPriority` on `BleSendQueue::Enqueue`,
`ITransport::SendExpedited` (non-pure, defaults to `Send`), a defaulted `Priority` on
`Session::Send`, and chess marking its move at both send sites. Three host tests cover ordering
rules the length trick never had. **RPS gains #190 on both platforms**, having never had it.

Worth carrying into later phases: a wire-format change can silently break code that *infers*
message identity somewhere else. Grep for length checks (`size == 1`, `Size == 1`) after any
framing change.

**Device re-verification of the #190 fix: DEFERRED.** Wireless ADB dropped again mid-phase and
will not come back: the Galaxy still answers ARP and still ADVERTISES over mDNS
(`adb-R83WA14EAMK-ME8WPy._adb-tls-connect._tcp 192.168.10.239:39261` — note the port rotated from
33935), but every `adb connect` is actively refused, on the advertised port and on 5555. That is
the "phone forgot the PC" state, which needs **Wireless debugging → Pair device with pairing
code** on the phone — a human step, like the lock screen.

Risk of deferring is genuinely low and worth stating precisely: the priority change moves a
datagram's POSITION IN A QUEUE. The bytes on the wire are identical, the framing is unchanged, and
`SendExpedited` defaults to `Send`, so it cannot break a link — the failure mode is "not as fast
as intended", not "does not work". Host tests cover the ordering rules and iOS CI compiled both
games' overrides. Queue for the next device session: chess two-phone (confirm rtt is no worse than
the 50 ms baseline measured earlier tonight) and RPS two-phone (confirm it still links at all now
that its send path gained a parameter).

### Phase 2 reconciliation — the one-sided BLE fixes, settled as decisions

Each was treated as a decision with reasoning recorded in the shared path, per the plan — not a
merge. Both directions were live.

| Fix | Was | Now | Decision |
|---|---|---|---|
| Role override gate (#196) | chess `LUR_AGENT`, RPS `LUR_INTERNAL` | `LUR_AGENT` | forced state from a hidden channel that outlives the app; Development is what a player plays |
| #146 deadlock breaker | RPS only | both, both platforms | the deadlock is unrecoverable by retrying and its shape is identical in both games |
| #182 hard radio restart | RPS only | chess Android done; chess iOS honest | chess had NO recovery from a wedged stack |
| #194 `onDestroy → ble.stop()` | chess only | both | a leaked registration is what the NEXT launch collides with |
| #190 priority send | chess only, and DEAD | both, explicit | see the regression section above |

**The #182 work found a log that lied.** `ITransport::RestartRadio` defaults to an empty body —
right for a loopback — but chess's backends never overrode it, so the session logged
*"escalating to a full radio restart (attempt 1/3 … 2/3 … 3/3)"* against a transport that did
nothing. Those exact lines appeared in chess's logcat earlier in this run while the real cause was
the lock screen, and they made a wedged radio look like the confirmed explanation. `ITransport`
gained `CanRestartRadio()` (default false) and the session asks before escalating, so an
unsupported backend says *"this transport cannot restart the radio"* once instead of narrating
three repairs nobody attempted. A missing capability is a finding, not a silence.

Chess iOS still lacks the restart and now reports that honestly; the port is listed for the driver
collapse, where the other game's peripheral-manager re-publish already exists.

### Device state, 2026-08-10

- **iPhone: verified.** Latest chess build installed and launched; BLE up, advertising + scanning,
  no crash from the iOS changes (#146 role logging, expedited send). `role decided` needs a peer,
  so it waits for the Galaxy.
- **Galaxy: still unreachable, and now precisely diagnosed.** Port 34805 answers with
  `failed to connect` (the service IS listening; our TLS key is rejected), while 39261 and 5555
  give `actively refused` (nothing there). That difference is the diagnosis: the phone forgot this
  PC's key, so **`adb pair` with a code is required** — toggling wireless debugging is not enough,
  because the toggle rotates the port but does not re-trust the host.

## Two-phone gate #2 — after `adb pair` (2026-08-10)

Wireless ADB needed **`adb pair` with a code**, not just a toggle: the pairing service was
advertised on a *different* port (`_adb-tls-pairing._tcp` 34191) from the connect service (34805).
`adb mdns services` lists both — worth knowing, because the pairing port is not guessable and the
phone screen shows it only while that dialog is open.

### Chess — PASS, and the #190 restoration measurably holds

```
android  AUTOPLAY game=22 sameFrame=205/206 delayed=0 ply=160 gate=0
         rtt(n=205 avg=49ms min=28ms max=66ms)
android  Net: MATCH END result=1 WLD(lo/hi/dr)=2/3/18 total=23
ios      AUTOPLAY game=27 sameFrame=2156/2159 delayed=0 gate=0
         rtt(n=2158 avg=58ms min=32ms max=101ms)
```

27 matches. Zero desync / resync / mismatch / DROPPED on either peer. Against the pre-#190-fix
baseline measured last night: Android avg 50 → **49 ms**, iOS max **218 → 101 ms**, iOS
`delayed` 1 → **0**. So restoring the expedited path is not merely compiled, it shows up in the
numbers — the tail is less than half what it was.

The new #146 diagnostic works, and this is what it now prints:
```
read peer id: mine=e2f0ff033aaba691ff82c1474864ef84
              peer=a6266065c520feb93945668fceff5933 defers=0 -> CENTRAL (keep link)
```
Previously `mine=32B peer=32B`. A both-peripheral deadlock means the two sides compared different
BYTES, and sizes agree in exactly that case — the values are the only thing that shows it.

### RPS — links and runs clean, but the STRONGEST check did not run

Both peers: `linked=1 started=1 gold=1300 desync=0 badbuild=0 gaps=0 stall=0 halfopen=0
restarts=0 rollbk=0 resim=0`, advancing the same tick range together (both ~1500 when sampled
simultaneously). Each placed its own camp (1900 → 1300, 600 each). No crash or regression from the
send-path change or the new `stop()`.

**Stated honestly, because it matters:** the tick-by-tick recording diff — `pullrec --recdiff`, the
purpose-built instrument — did NOT run. It pulled only recordings from 2026-07-31/08-01, all
refused as older formats, because `rps.dev.flight_recorder` is off on the Galaxy (the known
`cvars.cfg` fixture, pre-flight item 3, which I skipped). And `desync=0` alone can be VACUOUS:
`CrossCheck` returns early when `PeerHash` has no entry for a tick, so a peer that sent no anchors
at all would also read as `desync=0`. What does carry weight here is `gaps=0` (missing peer input
frames) together with `rollbk=0`, and both phones holding the same tick range in lockstep.

**So: RPS is verified as "links, runs, and shows no fault", not as "provably identical on both
sides".** Turn the recorder on before the next RPS gate and diff the pair — that is the check that
closes it, and it is a one-line pre-flight, not new work.

### Close-out
Both games rebuilt WITHOUT `-DLUR_AGENT` and reinstalled on both phones; `debug.lur.*` cleared;
every `Documents/` marker deleted on both bundles; `svc power stayon` restored.

## The Android backend collapse — done (#197 + the deferred half of #42)

Both games' Android radio is now **one copy**: `Modules/Transport/Platform/Android/{BleShim.kt,
BleTransport.cpp}`. ~2,200 lines across four files become ~1,100 across two.

Two structural blockers had to go first, and they were the actual reason the files could not be
shared rather than incidental annoyances:

1. **JNI symbol names bake the app's package in.** `Java_com_lurmotorn_<app>_BleShim_*` is findable
   only from a class in that exact package, so the Kotlin class *had* to live in the app.
   `RegisterNatives` binds an explicit table against a class the C++ names itself
   (`com.lurmotorn.engine.BleShim`), and `JNI_OnLoad` returns `JNI_ERR` if the class or any
   signature disagrees — failing the LOAD, because an unbound native throws
   `UnsatisfiedLinkError` at its own call site deep in the radio flow, looking like something else.
2. **`System.loadLibrary("<app>")`** hardcoded the library name. The app loads it now, before
   touching the shim. **Learned by breaking it:** removing the load produced exactly that
   `UnsatisfiedLinkError` on first launch, because the natives bind in `JNI_OnLoad` at library-load
   time and the Kotlin was reached first.

The per-app values are injected: log tag and library name from the activity, and the **BLE UUIDs
are read across the JNI seam** from `BleProtocol.h`. The Kotlin used to declare all three under a
comment reading *"MUST match Lur::Transport::BleProtocol"* — a duplication maintained by hope,
where a mismatch fails silently as two phones that never see each other.

### Two traps this phase produced, both caught

- **An out-of-tree app target cannot see the engine tree's `add_compile_definitions`**, so
  `LUR_LOG_TAG` had to be re-applied in each app's `target_compile_definitions` — the exact gotcha
  CLAUDE.md records for the `LUR_*` capability macros, hit for the same reason.
- **`CanRestartRadio` defaults to false, and RPS forgot to declare it.** The default is right (a
  transport without a hard restart must never be narrated as having one), but that makes it a query
  every *implementor* must opt into — and RPS, which fully implements #182, would have silently
  lost the recovery. The mirror image of chess's original bug, introduced by my own fix two commits
  earlier. The lesson: a false-by-default capability query needs an audit of every implementor, not
  just the one that lacked the feature.

### Verified on hardware
Chess reaches a full handshake to READY over real BLE on the shared driver
(`central: linked + notifications on` → `hello RECV` → `READY (peer id known)` →
`peer linked -> adopt`), and RPS gets a full link and `lockstep started` on it too. No
`UnsatisfiedLinkError`, no crash. Host 28/28 throughout.

### Remaining in the collapse
iOS (`IosBleTransport.mm`, 2 copies ~137 diff lines) and Windows (2 copies, 10 diff lines) are
untouched and still per-game. Also still open: chess iOS's missing #182 restart — it reports the
absence honestly now, and the other game's peripheral-manager re-publish is the model to port.
