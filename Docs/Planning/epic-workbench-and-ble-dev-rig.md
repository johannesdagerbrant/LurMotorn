<!--
  Each block delimited by "===== ISSUE =====" is one GitHub issue.
  First line of each block is "Title:"; everything after is the issue body (Markdown).
  Example (repo root):
    gh issue create --title "<title>" --body-file body.md --label <labels>
  Create the Epic first, then paste its number into the child issues' "Part of #NN" lines.
  Source: master-roadmap Phase 0.5 + Review #2 §4 (the Handmade "you cannot see your program"
  agenda). These issues were NOT in the extraction epic (Review #1) — they are drafted here from
  the roadmap because Phase 0.5's tooling is pulled ahead of the RTS and is validated against chess.
-->

===== ISSUE =====
Title: Epic: Phase 0.5 — The Workbench (desktop platform + observability) + the Windows↔Android BLE dev rig

## Goal

Build the iteration loop **before** game #2, validated against chess as a known-good client. Today the
only way to exercise the thing that actually breaks — the BLE link — is build two apps, deploy to two
phones, poke by hand, read logcat: minutes per experiment, unreproducible failures, evidence split
across two devices. Every hard bug in the history (#17, #33, #38) was fought in that loop.

Phase 0.5 replaces that loop with: **the game running on the PC** (two windows, loopback between them,
alt-tab instead of deploy), **a flight recorder** so bugs become replayable files, **loud asserts +
full warnings**, and — the piece to protect above all — a **hybrid Windows↔Android BLE rig that an
agent can drive end to end**: build the PC endpoint, install+launch the phone over wireless adb, tap
through a match, toggle Bluetooth mid-game to force reconnects, tail both logs, pull flight-recorder
files. The entire two-endpoint radio test cycle — link-death chaos included — becomes **one command**,
so developing, debugging, and optimizing the Bluetooth networking becomes an autonomous loop.

Full context: `Docs/Planning/lurmotorn-master-roadmap.md` (Phase 0.5) and
`Docs/Planning/lurmotorn-review-2-handmade-lens.md` §4.

## The porting rule that justifies the phase

**One variable at a time.** Bringing up a new platform (Windows) against the *known-good* game (chess)
means every black screen, dead input, or stalled link is a platform bug by construction. Bundling it
into RTS slice 0 would mean debugging a new platform against a new sim simultaneously. So: Windows +
observability now, on chess; the RTS is written inside a good loop from its first line.

A standing rule for everything built here (and after): **new code follows the doctrines immediately**
— POD state, loud asserts, no platform code in game folders, `Lur::Log`. Retrofitting old code happens
opportunistically, never as its own project.

## Child issues

- [ ] #NN — Windows platform backend: window + Vulkan surface + input seam (in `Modules/*/Platform/Windows`), `TouchEvent` first instantiation
- [ ] #NN — `Lur::Log`: one engine-wide logging seam + desktop console sink, app-supplied tag
- [ ] #NN — Chess on the desktop: two windows, two game instances, one process, `LoopbackTransport` between them
- [ ] #NN — Debug overlay: frame ms, link state, ticks-since-datagram, send/recv counters — desktop + phones
- [ ] #NN — Flight recorder + replay: record every session, replay through loopback to a hash-identical final state
- [ ] #NN — Fuzz + determinism + byte-budget CI: frame-parser/`DecodeGame` fuzz, chess determinism fuzz, size-regression asserts
- [ ] #NN — Chess soak mode on phones: auto-play random legal games overnight, failures saved as flight-recorder files
- [ ] #NN — **The Windows↔Android BLE dev rig**: `WindowsBleTransport` (WinRT central) + `dev-rig.ps1` one-command agentic radio loop

## Definition of done (anti-gold-plating checklist)

- Chess playable two-window on Windows.
- Overlay toggles on both platforms.
- One recorded session replays hash-identical through loopback.
- Fuzz + byte-budget suites green in CI.
- The BLE dev rig brings the Windows endpoint up against the **unmodified** Android app over real BLE,
  and `dev-rig.ps1` drives a full match + a forced reconnect + log capture + flight-recorder pull in
  one command.

**Explicit non-goals for this phase:** hot reload, editors, renderer features beyond bring-up, and
**no `GameHost`/`IGame` extraction yet** — the desktop `main` stays copy-pasted from the phone mains on
purpose (it's Phase-4 extraction evidence). If the phase threatens its timebox (~1–2 weeks), cut scope
from the tail of the checklist, never extend the box. The BLE rig is scheduled **outside** that timebox
as an agent-delegable parallel track through RTS slices 0–1, ready before Phase 2.


===== ISSUE =====
Title: Workbench — Windows platform backend: window + Vulkan surface + input seam, first TouchEvent

Part of #NN (epic). The foundation the rest of Phase 0.5 sits on. Roadmap Phase 0.5; Review #1 §3.5 (input); Review #2 §4.1.

## Problem / goal

`build.ps1` already implies a Windows box, but there is no runnable game off-device — the host build
produces unit-test binaries only. Add a Windows platform backend so chess runs on the PC: a window, a
Vulkan surface, and an input seam. This turns the build-deploy-two-phones loop into alt-tab and gives
the flight recorder, replay viewer, and fuzzers a place to live.

## Scope

- **Place platform code in `Modules/*/Platform/Windows` from day one** — the chess mistake (engine code
  parked in the game folder), deliberately not repeated. A Win32 window + a Vulkan `VK_KHR_win32_surface`
  swapchain seam mirroring the existing Android/iOS `Lur::Render::Vk::PlatformSurface` split.
- **Input:** mouse/keys normalize into `Lur::Input::TouchEvent` (phase, pixel coords, `TimeNs`,
  `PointerId`) — the struct is defined but instantiated nowhere today; this is its first real use. The
  desktop routes native events → a `TouchEvent` queue → the game consumes one stream (no inline
  `OnTap(x,y)` shortcut).
- Vulkan backend is already desktop-dialect; expect surface/swapchain/present bring-up, not renderer
  rewrites. Turn on validation layers in the dev run.
- Zero dependency additions (Win32 + Vulkan are OS/GPU APIs; the sanctioned set).

## Acceptance criteria

- [ ] Chess renders and is playable in a Windows window against the local move flow.
- [ ] Input reaches the game only via `TouchEvent` (grep: no new inline `OnTap` translation in the desktop main).
- [ ] Platform files live under `Modules/*/Platform/Windows`, not in `Games/Chess`.
- [ ] Validation layers clean on the dev machine.


===== ISSUE =====
Title: Workbench — Lur::Log: one engine-wide logging seam + desktop console sink

Part of #NN (epic). Roadmap Phase 0.5; Review #1 §3.5 (logging scorecard).

## Problem

Logging is four ad-hoc seams across nine files: two `LOGI` macro sets, `Vk::PlatformLog`, per-object
`SetLogger` lambdas — and the tag is hardcoded (`OnlyChess`) inside engine-grade code. The desktop
build needs a console sink; introduce it as the *unified* seam rather than a fifth ad-hoc one.

## Fix

- `Lur::Log::Init(Sink, Tag)` called once by the platform shell; `Lur::Log::Info/Error(fmt, …)`
  everywhere else. ~30 shared lines + a small per-platform sink (desktop = console; Android = logcat;
  iOS = os_log).
- Existing `SetLogger` lambdas forward into it during migration (opportunistic, not a campaign).
- The `OnlyChess` tag becomes an app-supplied string — closes the tag-coupling from the platform
  extraction work for every module at once.

## Acceptance criteria

- [ ] Desktop logs go through `Lur::Log` to a console sink.
- [ ] Tag is supplied once by the app; grep for `OnlyChess` in `Modules/` returns nothing new.
- [ ] At least the desktop path uses `Lur::Log`; other call sites migrate opportunistically.


===== ISSUE =====
Title: Workbench — Chess on the desktop: two windows, two instances, one process, loopback between them

Part of #NN (epic). Depends on the Windows-backend + Lur::Log issues. Roadmap Phase 0.5; Review #2 §4.1.

## Goal

Run two full chess game instances in one process, each in its own window, with `LoopbackTransport`
(or UDP-localhost) between them: human-vs-human on one PC, both peers visible in a debugger
simultaneously. Every net-flow bug becomes reproducible without a phone in the room.

## Scope

- A desktop `main` that constructs two game instances + two windows + a loopback pair wiring each
  side's transport to the other. **This is deliberately a third copy-pasted chess main** — do not
  extract `GameHost`/`IGame` here; the duplication is Phase-4 extraction evidence.
- Reuse the exact session/handshake/sync flow the phones use, so desktop behavior mirrors device
  behavior (the whole point of validating tooling against chess).

## Acceptance criteria

- [ ] Two windows, two instances, one process; a move on one appears on the other via loopback.
- [ ] Handshake / ready / sync-on-link / persist paths run exactly as on device.
- [ ] No `GameHost`/`IGame` introduced (intentional).


===== ISSUE =====
Title: Workbench — Debug overlay: frame ms, link state, ticks-since-datagram, counters (desktop + phones)

Part of #NN (epic). Depends on the Windows-backend issue. Roadmap Phase 0.5; Review #2 §4.5.

## Goal

Point the text renderer at ourselves. A toggleable overlay showing frame ms, `ELinkState`,
ticks/ms since the last datagram, short peer GUID, and send/recv counters — replacing logcat
squinting for ~80% of on-device questions. "If you can't see it, you can't fix it."

## Scope

- One overlay drawn via the existing `Hud`/`Text` path over `IRenderer`; a runtime toggle.
- Reads engine state (session link state, counters, timers) — no new coupling into game logic.
- Runs on desktop and both phones.

## Acceptance criteria

- [ ] Overlay toggles on desktop and on device.
- [ ] Shows frame ms, link state, since-last-datagram, send/recv counts, short peer id.
- [ ] Zero cost when hidden (no per-frame layout when off).


===== ISSUE =====
Title: Workbench — Flight recorder + replay: record every session, replay to a hash-identical state

Part of #NN (epic). Depends on the desktop two-window issue. Roadmap Phase 0.5; Review #2 §4.2.

## Goal

Exploit the architecture's deepest property — *state is derived from the input stream* — as a
debugging tool. Record every datagram in/out, every touch, every link-state transition, timestamped,
appended to a ring file, in every build (bytes/sec at these rates — no cost argument). A crash or
desync then ships as a file that **replays**: feed it back through loopback on the desktop and watch
the exact failure re-happen in a debugger.

## Scope

- A compact record format (event type + timestamp + payload) written to a ring file.
- A replay driver that feeds a recording through `LoopbackTransport` into a fresh game instance.
- Chess's tiny stream is the first client; assert the replayed final state hashes identically to the
  recorded one (this leans on the POD-state / `StateHash` direction — a cheap FNV over a canonical
  serialize is enough for chess).

## Acceptance criteria

- [ ] Sessions are recorded to a ring file with bounded size.
- [ ] A recorded chess session replays through loopback to a byte-identical final record / hash.
- [ ] Replay runs on the desktop build with both peers inspectable.


===== ISSUE =====
Title: Workbench — Fuzz + determinism + byte-budget CI (attacker surface, sim, and the payload as product)

Part of #NN (epic). Roadmap Phase 0.5; Review #2 §4.4, §7; Review #1 §5.3.

## Goal

Make machines find the bugs the design leans on being absent. Three host-side suites in CI:

- **Codec/session fuzz:** throw random/mutated buffers at the frame parser and `DecodeGame` —
  `Session::OnDatagram` parses radio bytes from a peer we don't control, so it must survive garbage by
  construction. (The varint shift-UB from the hardening batch is exactly the class this finds
  mechanically — this suite is its regression net.)
- **Chess determinism fuzz:** millions of random *legal* games on two host instances over loopback,
  asserting byte-identical records and identical results. Ten seconds of CI guaranteeing the
  determinism the whole design rests on.
- **Byte-budget regression tests:** the manifesto says the payload is the product, so *test the
  product* — CI asserts `encoded move ≤ 1 byte`, `hello ≤ N`, `sync(60 plies) ≤ M`. A size regression
  fails the build exactly like a logic regression.

## Acceptance criteria

- [ ] Fuzz loop runs in CI and survives N million mutated inputs without crash/UB (ASan/UBSan if available).
- [ ] Determinism fuzz asserts identical records across two instances over many random legal games.
- [ ] Byte-budget asserts wired into CI with named bounds; a deliberate oversize fails the build.


===== ISSUE =====
Title: Workbench — Chess soak mode on phones (auto-play random legal games overnight)

Part of #NN (epic). Needs only the Phase-0 threading fix, not the desktop build. Roadmap Phase 0.5; Review #2 §4.3.

## Goal

Turn the phones into bug hunters for the hardest class — role collisions, reconnect races, silent link
death — which only appears across hours of real radio behavior. A burn-in mode: two devices auto-play
random legal games continuously, auto-reset between matches, flight-record everything, with a failure
counter on screen. Leave them on a shelf overnight; every morning either the counter is zero or there
are N recorded repros.

## Scope

- A soak toggle that drives random legal moves through the normal move path at a chosen cadence.
- Auto-restart a new match on game end; increment an on-screen failure counter on any assert / desync /
  unexpected link death and save the flight-recorder file (depends on the flight-recorder issue for the
  file format; can ship a minimal log-only version first).
- Explicitly hunts the historical #17/#33/#38 bug class starting now, not at RTS slice 2.

## Acceptance criteria

- [ ] Two phones auto-play and auto-restart unattended for hours.
- [ ] Failures increment an on-screen counter and (once flight recorder lands) save a repro file.
- [ ] No steady-state growth that itself causes failure (bounded logs/records).


===== ISSUE =====
Title: Workbench — The Windows↔Android BLE dev rig: WindowsBleTransport (WinRT central) + dev-rig.ps1 one-command agentic radio loop

Part of #NN (epic). **The piece to protect above all.** Agent-delegable parallel track through RTS slices 0–1, ready by Phase 2. Roadmap Phase 0.5 (parallel track); Review #2 §4.3.

## Goal

Loopback has no radio; two phones have no debugger. This rig has both: it runs the engine's **real wire
protocol over real BLE** between a Windows endpoint (under a debugger, with logs and flight recorder)
and the **unmodified** Android app — and wraps the whole two-endpoint test cycle, link-death chaos
included, into **one command an agent can drive**. That makes developing, debugging, and optimizing the
Bluetooth networking an *autonomous loop*: an agent changes code, runs the rig, reads the captured
evidence, and iterates — no human poking two phones.

## Part A — `WindowsBleTransport` (a third radio, native, debuggable)

- Implement a **GATT central-only** BLE transport in **C++/WinRT** against `Windows.Devices.Bluetooth`
  — a platform API, so the zero-dependency rule holds (no third-party libs). Central-only on purpose:
  Windows' peripheral mode is its flaky half, and the Android side already plays peripheral.
- Speak the **existing wire protocol** (same GATT services/characteristics/UUIDs, same datagram
  framing, same `DecideBleRole` in-band handshake) to the unmodified Android app — the phone must not
  know it's talking to a PC.
- Expose it behind `ITransport` (later the third `IBleRadio` driver once BLE unification lands in
  Phase 5 — build it so that migration is cheap, but don't block on unification here).
- A few hundred lines plus WinRT interop debugging. The dev laptop's built-in radio is confirmed, so
  **no dongle needed**.

## Part B — `scripts/dev-rig.ps1` (the one-command loop)

One PowerShell entry point (mirrored as `scripts/dev-rig.bat` per the scripts convention) that:

1. Builds the Windows endpoint.
2. `adb install`s + launches the Android app over **wireless debugging** (pin `ANDROID_SERIAL` so
   Gradle/`installDebug` isn't ambiguous when two transports point at the same phone).
3. Taps through a match with `adb shell input tap`.
4. **Forces reconnects mid-game** with `adb shell svc bluetooth disable/enable` — reproducing the exact
   link-death bug class on demand.
5. Tails both logs (Windows endpoint + filtered `adb logcat -s OnlyChess:*`).
6. Pulls flight-recorder files on exit (steer them into `Download/` so `adb pull` retrieves them).

## Operational notes (bake into the script / its README)

- **Disable the BLE adapter's power management** (Device Manager → adapter → Power Management → untick
  "Allow the computer to turn off this device") before long soak sessions, or Windows suspends the radio
  mid-run and produces phantom link deaths that mimic the very bug class being hunted.
- **Check the adapter's LMP version** (adapter properties → LMP; 9+ = Bluetooth 5) before trusting any
  throughput numbers — a 4.2-era laptop radio caps PC↔phone measurements below the phone↔phone ceiling.

## Honest scope

Windows' BLE stack ≠ iOS's, so this is a **development instrument, not final validation** — phone↔phone
soak remains the truth. Bonus: it partially wakes the parked stress-test epic's *measurement* half
(PC-side high-resolution latency/throughput against a real phone radio, nearly free).

## Acceptance criteria

- [ ] `WindowsBleTransport` connects to the unmodified Android app over real BLE and exchanges moves both directions via the existing protocol.
- [ ] `dev-rig.ps1` runs the full cycle — build → install/launch → play a match → force ≥1 reconnect → capture both logs → pull flight-recorder file — in one invocation.
- [ ] The Windows endpoint runs under a debugger with `Lur::Log` output and (once it lands) flight-recording.
- [ ] Operational notes (power management, LMP check) documented in `scripts/README.md`.
- [ ] Zero third-party dependencies added (WinRT/Windows.Devices.Bluetooth only).
