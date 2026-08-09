# Autonomous run prompt — the engine extraction, all eight phases

Frozen 2026-08-09. Companion to `engine-extraction-plan.md` in this batch: the plan says *what* and
*why*, this says *how to hand it to an agent*. Written for a fresh agent with **no human
intervention until all eight phases are done**, TDD throughout, and **both phones connected**.

## How to use it

Copy the fenced block below verbatim as the agent's opening prompt. To run a single phase instead of
the whole ladder, change `START AT: Phase 0` and add `STOP AFTER: Phase N`.

## Before you launch it — one check worth 30 seconds

```
powershell -File Tools\DeviceRig\device-rig.ps1 -Game rps -Action install -Peer ios
```

Free Apple dev provisioning profiles last **7 days**, and renewal needs one Sideloadly run with the
Apple ID — the one step an agent genuinely cannot do. If that install fails, everything from Phase 1
onward degrades to Android-only verification, and **Phases 2 and 4's gates are specifically about
cross-platform behaviour**, so those results would be far weaker than they read. The prompt handles
this without halting, but you want to know which run you're getting before it starts.

## Known limits of a single unattended run

Recorded honestly, because the prompt is deliberately written to push through them:

- **Context.** Eight phases will exceed one context window. Mitigated by `RUN-LOG.md`, which the
  prompt makes the agent write before anything else and re-read after every compaction.
- **Phase 2's real signal is qualitative.** The soak gate answers "does the link still recover", but
  the thing a human actually judges is whether it *feels* right. An agent can only check the former.
- **Phase 4 breaks both installs at once.** The engine `ProtocolVersion` resets, so a half-finished
  Phase 4 leaves two phones that cannot link and no obvious reason why. The prompt calls this out
  twice; it is the single most dangerous point in the run.

---

```
Fully autonomous engine-extraction run for LurMotorn (C:\games\lurmotorn).
NO HUMAN INTERVENTION until all eight phases are done. Never stop to ask. Never wait for
approval. Both phones (Galaxy + iPhone) are connected and available.

## Read first, in this order
1. CLAUDE.md — constraints, build configs, LUR_AGENT/LUR_INTERNAL gates, the agent
   command grammar, device log-reading traps. These OVERRIDE your defaults.
2. Docs/Journal/2026-08-09/engine-extraction-plan.md — plan, three diseases, phases, risks.
3. gh issue view 39 --comments, then the issue for the phase you are on.
4. Games/RocksPapersScissors/README.md — device ops and the two-phone pre-flight list.

The journal is a FROZEN SNAPSHOT at 22468e7. Re-verify every file:line against HEAD
before acting. Issues win over the journal on anything current.

## FIRST ACTION — create your progress log

Create Docs/Journal/2026-08-09/RUN-LOG.md and commit it. Append to it after EVERY work
item, before moving on:

  ## Phase N — <name>  [IN PROGRESS | DONE | BLOCKED]
  - [x] <item> — test: <test name> — verified: host | device(android) | device(ios)
  - [ ] <item> — BLOCKED: <reason>, worked around by <what>
  Gate: build.ps1 <pass/fail> | android <pass/fail> | ios <pass/fail> | two-phone <pass/fail>

This file is your memory. Your context WILL be compacted during this run. On resuming
after any compaction: read RUN-LOG.md first, then the current phase's issue, and continue
from the first unticked item. Do not re-do completed work. Do not trust your recollection
over the log.

## Order — all eight, in sequence, without pausing between them
  Phase 0  #200  sweep dead code + remove game names from the engine
  Phase 1  #42   platform layer move (incl. the audio device seam)
  Phase 2  #197  BLE unification  <-- highest product risk
  Phase 3  #43   Modules/App: entry, threading, input, stdlib diet
  Phase 4  #8    rollback coordinator + wire-version split
  Phase 5  #201  promotions, unified recorder, per-opponent persistence
  Phase 6  #198  lur_add_game, multi-config fix, CI matrix
  Phase 7  #45   Docs/NewGame.md, module READMEs, promotion ritual

Work each issue's checklist. Tick items off with gh as you go.

## TDD — non-negotiable, and it is the design tool here

Red -> green -> refactor, per work item:
1. Write a FAILING host test first. Run build.ps1 and SEE IT FAIL. A test that passes on
   its first run is not a test — fix the test, not the expectation.
2. Minimum code to pass. build.ps1 green.
3. Refactor with the test holding.

If you cannot write a host test, that is a FINDING, not an excuse. The shared-first
doctrine says: "could this logic be unit-tested on the host against a fake? Then it must
live where the host can build it." Untestable means it is in the WRONG PLACE — move it
until it is testable. This is exactly the BleShim.kt bug: ~40% decision logic, no host
test, drifted 299 lines between the two games.

Genuinely untestable on host (Vulkan backend, UIKit/NDK lifecycle, real radio): substitute
an on-device verification and record which it was in RUN-LOG.md and the commit message.

Modules/Hud, Modules/Render and Modules/Platform have ZERO tests today. Anything you touch
or promote there gets tests. Do not preserve that gap.

## Build
  powershell -ExecutionPolicy Bypass -File build.ps1     # host + unit tests, after EVERY change
  cd Games/<Game>/Android && ./gradlew.bat assembleDebug # also compile-checks the shim files
  powershell -File Tools\DeviceRig\device-rig.ps1 -Game <chess|rps> -Action install -Peer <ios|android>

iOS is macOS-CI-only. You are PRE-AUTHORISED to push master to get an .ipa (CLAUDE.md
says so explicitly). Push, poll the run, `gh run download <id> -n <artifact> -D dist`,
then install via the rig. For a harness build: `gh workflow run "macOS CI" -f agent=true`.

## Autonomous two-phone verification — use the LUR_AGENT harness

This is what makes the gates machine-checkable. Build both phones with -DLUR_AGENT=ON for
verification, drive both, then reinstall clean builds (see "phase close-out").

  Android: adb -s <serial> shell setprop debug.lur.agent.cmd "<seq> <verb> [args]"
  iOS:     push the command file into Documents/agent.cmd via the rig

Traps that will waste a whole run if you miss them:
- Both channels are LEVEL-TRIGGERED. The sequence number must STRICTLY INCREASE or the
  command is ignored as a repeat.
- Send `linked` FIRST in any two-phone scenario. The app opens in a SOLO AI match, so a
  `place` sent before crossover lands in the solo sim — accepted, logged, useless — and
  the peer waits forever. Both mains log an ERROR when this happens; watch for it.
- RPS WorldHeight is 240. Team 0's opening camp is (17,16), team 1's mirror is (17,224).
  A placement outside a team's frontier is rejected SILENTLY — gold just never drops.
- Verbs: place queue stress corrupt droptx console gesture killown linked.

Reading logs — do NOT burn context on noise:
  adb -s <serial> logcat -d -s OnlyChess:*      (or OnlyRps:*)
  timeout 30 python -m pymobiledevice3 syslog live -pn OnlyChess -o ios.txt >/dev/null 2>&1
  then grep the FILE. NEVER pipe the iOS stream through `grep > file` — it drops ~80% of
  lines and makes a working feature read as dead. That cost hours on 2026-08-01. A 30s
  capture owes you ~15 RPS diagnostic lines; if you got 6, your capture is broken, not the
  feature.
  iOS BLE lines are prefixed "OnlyChess BLE:" (with a space), NOT "OnlyChess:".
  Grep vocabulary: "BLE up", "role decided", "central: linked", "peripheral: central
  linked", "disconnected", "link lost".

Two-phone pre-flight before any match verification (each has cost a wasted run):
  1. badbuild=0 on BOTH.  2. matching pre-match gold= and hash= on BOTH.
  3. rps.dev.flight_recorder ON on both (the Galaxy's cvars.cfg fixture persists it false).
  4. read gaps= on BOTH — a one-directional fault is invisible from the good end.

iOS install hangs silently while the app is running. Kill it headlessly first:
`dvt signal <pid> 9` — kill/pkill silently no-op and will lie to you.

## Phase gates — verify autonomously, do not proceed until green
After EVERY phase:
  - build.ps1 green
  - both games build: host, Android, iOS
  - BOTH GAMES PLAY A REAL MATCH PHONE-TO-PHONE, driven via the agent harness. A "linked"
    log line is NOT proof — drive actual moves/placements and confirm both sims agree.
Phase 2 additionally: a soak. Drive repeated link/drop/reconnect cycles (`droptx`,
  `killown`, adb `svc bluetooth disable/enable`) and confirm recovery every time.
Phase 4 additionally: RPS desync=0 over a full match within the 2026-08-04 baseline
  (~0.5-0.7 rollbacks/s, exactly 1.00 tick re-simulated per rollback, peer lag ~1 tick);
  and a chess game past 700 plies to confirm #74 is closed by chunked resync. Use
  `device-rig.ps1 -Action pullrec` to collect and diff BOTH peers' recordings.

## Phase close-out — every phase, no exceptions
1. Rebuild both games WITHOUT -DLUR_AGENT so the harness code is ABSENT, not idle.
2. Install those clean builds on both phones.
3. Clear device state: `adb shell setprop debug.lur.agent.cmd ""`, delete any
   Documents/agent.cmd, remove capture files. "Inert by default" is not "not there".
4. Update RUN-LOG.md, push, move to the next phase immediately.

## Hazards that will silently ruin things
1. Deleting EMsgType::ClockPing/ClockPong RENUMBERS every later slot = silent wire break
   with no version guard. In Phase 0 delete ClockSync.h ONLY, not the enum values. The
   slot removal rides Phase 4's version reset.
2. Phase 4's engine ProtocolVersion RESETS low. An old peer must not read it as "older".
   REINSTALL BOTH PHONES IN THE SAME STEP — never leave one upgraded, or you have two
   phones that cannot link and no way to tell why.
3. Phase 5's binary recorder breaks --recdiff and pullrec (your primary desync instrument
   and your Phase 4 evidence). Ship format + text dump tool in ONE commit.
4. iOS render thread must be a pthread with a 4MB stack. std::thread's 512KB overflows
   into SIGBUS (___chkstk_darwin). Do not "simplify" it.
5. Move ordering IS the wire protocol for chess. Changing GenerateLegalMoves order is a
   breaking wire change.
6. No floats in simulation state. Modules/Math floats are render-only.
7. The renderer INTERPOLATES, never PREDICTS. Extrapolation was built and reverted twice.
8. Fixed-capacity containers in the hot path, no heap in the tick, sim state stays
   trivially copyable (static_assert it).
9. LUR_AGENT = remote control, ABSENT from a play build. LUR_INTERNAL = dev tooling a
   player DOES get. Phase 2 must resolve the existing contradiction (chess gates the BLE
   role override on LUR_AGENT, RPS on LUR_INTERNAL) — choose LUR_AGENT, it is forced state.
10. The RPS sim loop `continue`s for solo, decided and post-match — anything that must
    always run (agent poll, diagnostics) belongs at the TOP of the loop.

## Standing rules
- Delete ALL dead code even if it looks useful. If a game needs it later, build it in that
  game; promote on the SECOND consumer.
- Modules/* must never name a game, in code OR comments. Before each commit:
  `grep -riE "onlychess|onlyrps|chess|rps" Modules/` must be clean.
- Games/* may depend on Modules/*; never the reverse.
- Engine owns the entry point, game owns the tick, per-step override. If two of three
  games would override the same step, the DEFAULT is wrong — fix the default.
- Count the implementations: multiple-at-runtime -> interface; one-per-platform ->
  link-time seam; one total -> just code.
- No third-party libraries. MoltenVK is the only runtime exception.

## Git
- Commit straight to master, never branch. Small coherent green commits, one per work item
  or tight group. Every commit leaves master green.
- Message says what changed and WHY, in the style of recent commits (`git log`). Name what
  was device-verified vs unit-tested.
- End every message with:
  Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>
- Push at each phase close-out and whenever you need CI for an .ipa.

## Never halt — how to handle blockers
Do not stop and wait for a human under any circumstance. When blocked:
1. Finish everything that does NOT depend on the blocker.
2. Comment the blocker on the relevant issue: what you tried, what you observed, what you
   need. Record it in RUN-LOG.md as BLOCKED.
3. Continue to the next independent item, then the next phase.

Specific fallbacks:
- iOS INSTALL FAILS / profile expired. Free Apple dev profiles last 7 days and renewal
  needs one Sideloadly run with the Apple ID — the one thing you genuinely cannot do.
  Do NOT halt. Continue with Android + host verification for every remaining phase, mark
  each affected gate "ios: DEFERRED — profile expired" in RUN-LOG.md, and list every
  deferred iOS verification in your final report so it can be run in one batch later.
- CI red on macOS: read the log, fix forward, push again. Do not disable the job.
- A phase gate fails: fix it. If you cannot after genuine effort, revert that phase's
  commits so master is green, record why in RUN-LOG.md and on the issue, and CONTINUE to
  the next phase if it does not depend on the reverted one. Never leave master broken.

## Final report
Per phase: what landed, host-tested vs device-verified, what was deferred or reverted and
why, every place the journal's file:line no longer matched HEAD, and the full list of
deferred iOS verifications.
```
