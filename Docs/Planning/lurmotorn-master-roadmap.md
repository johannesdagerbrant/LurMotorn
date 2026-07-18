# LurMotorn — Master Roadmap: Sequencing Everything Around Game #2

> **⚠️ LEGACY ARTIFACT (frozen July 2026).** The **GitHub issues are the source of truth** for sequencing, priority, and current state — see the tracker at #12. This document is the original planning synthesis, kept for its rationale and narrative; it is **not** updated as work lands and must not be treated as authoritative. When it disagrees with an issue, the issue wins. Do not add new authoritative planning here — file/label an issue instead.

*Synthesizes: Review #1 (architecture lens), Review #2 (Handmade lens), the RPS-RTS spec, and the earlier CoC epic. July 2026.*

---

## The organizing principle

**The RTS is not queued behind the reviews — the RTS is the mechanism that executes most of them.** Both reviews agreed on the destination and disagreed on order; the spec resolves it: every review item is either (a) *blocking* — the RTS steps on it, so it comes first; (b) *forced* — an RTS slice makes it necessary at a specific moment, so it lands exactly then, with a real client instead of speculation; or (c) *evidence-dependent* — it needs two live games to be done right, so it comes after. Almost everything is (b) or (c). Only five *fixes* are truly "before" — plus one deliberate investment that earns its place ahead of the game: **the Workbench (Phase 0.5)**, the desktop platform and observability tooling from review #2, validated against chess so the RTS is written inside a good iteration loop from its first line.

A rule that falls out of review #2 and should govern every phase: **new code follows the doctrines immediately** (POD state, asserts, no platform code in game folders, Lur::Log) — retrofitting old code happens opportunistically or at extraction time, never as its own project.

---

## Phase 0 — Stop the bleeding, raise the floor *(before any RTS code; ~a week)*

The only true "before" items. Three are live chess bugs or direct RTS blockers; two are one-day floor-raises you want in place *while writing* the new sim, not retrofitted onto it.

| Item | Why it can't wait | Source |
|---|---|---|
| Android BLE threading race — fix **as the event-queue inbox** | Live data race in shipped chess; RTS multiplies traffic ~50×; the queue is the future `BleLinkController`'s front door, so nothing is throwaway | R1-P0 / R2 agrees |
| `Session::Send` 64-byte cap + regression test | Live chess bug (>61-ply resync silently fails); the RTS resync payload (input history) exceeds 64 B on *any* mid-game reconnect — blocks spec §7 outright | R1-P0 |
| `-Wall -Wextra -Werror`, exceptions/RTTI decision, `LUR_ASSERT` + warning cleanup | One day; every line of RTS sim code should be written under loud asserts and full warnings | R2 §3.5/§5 |
| Core hardening batch (varint shift guard, `EncodeMove`/`MoveList` asserts, `TickClock` clamp, misc) | An afternoon with the assert macro in hand; the varint guard specifically protects the RTS's varint-tick-delta wire format, and the tick clamp protects its loop | R1-P3 |
| `Session::Tick(ElapsedNs)` — real-time, not frame-denominated | The desktop build is about to run Session at arbitrary frame rates; frame-denominated keepalives break there first | R1-P2 |

## Phase 0.5 — The Workbench *(desktop platform + observability, validated by chess; ~1–2 weeks, timeboxed)*

Review #2's iteration-loop items, pulled ahead of the RTS — because tooling compounds, and because **chess is a working client for all of it today**. The porting rule that justifies the phase: *one variable at a time*. Bringing up a new platform against the known-good game means every black screen, dead input, or stalled link is a platform bug by construction; bundling it into RTS slice 0 would have meant debugging a new platform against a new sim simultaneously.

What lands here:

- **Windows platform backend** (the box `build.ps1` already implies): window + Vulkan surface + input seam, placed **in `Modules/*/Platform/Windows` from day one** — the chess mistake, not repeated. Mouse/keys normalize into `Lur::Input::TouchEvent` — the struct's first instantiation *(R1-P2 input item)*.
- **Chess running on the desktop**: two windows, two full game instances, one process, `LoopbackTransport` between them. Human-vs-human on one PC. (Yes, this creates a *third* copy-pasted chess main — deliberately. It's Phase-4 extraction evidence, not a problem to solve now.)
- **`Lur::Log`** *(R1-P2)*: the desktop's console sink is introduced as the unified seam rather than a fifth ad-hoc one; other call sites migrate opportunistically.
- **Debug overlay** *(R2 §4.5)*: frame ms, link state, ticks since last datagram, send/recv counters — on desktop and phones alike.
- **Flight recorder + replay** *(R2 §4.2)*: record a chess session, replay it through loopback to a hash-identical final state. Chess's tiny stream is the ideal first client.
- **Codec/session fuzz + chess determinism fuzz + byte-budget CI tests** *(R2 §4.4, #7)*: garbage at the frame parser and `DecodeGame`; millions of random legal chess games on two instances asserting identical records; CI asserting `move ≤ 1 byte`, `sync(60 plies) ≤ M`.
- **Chess soak mode on phones**, in parallel (needs only Phase 0's threading fix, not the desktop): two devices auto-playing random legal moves overnight, failures arriving as flight-recorder files — hunting the historical #17/#33/#38 bug class starting *now* instead of at RTS slice 2.

**Parallel track — the hybrid rig: Windows ↔ Android over *real* BLE.** Loopback has no radio; two phones have no debugger; this rig has both. A `WindowsBleTransport` (GATT **central-only** — Windows' peripheral mode is its flaky half, and the Android side already plays peripheral) implemented in C++/WinRT against `Windows.Devices.Bluetooth` — a platform API, so the zero-dependency rule holds — speaking the existing wire protocol to the **unmodified** Android app. A few hundred lines plus interop debugging; the dev laptop's built-in radio is confirmed, so no dongle needed. Two practical notes for long rig sessions: **disable the adapter's power saving** (Device Manager → Bluetooth adapter → Power Management → untick "Allow the computer to turn off this device"), or Windows will suspend the radio mid-soak and produce phantom link deaths that mimic exactly the bug class being hunted; and before trusting any throughput numbers from the rig, **check the adapter's Bluetooth version** (adapter properties → LMP version; LMP 9+ = Bluetooth 5) — a 4.2-era laptop radio would cap PC↔phone measurements below what the phone↔phone pair can actually do. Paired with a `scripts/dev-rig.ps1` that builds the PC endpoint, `adb install`s + launches the phone over wireless debugging, taps through a match (`adb shell input tap`), toggles Bluetooth mid-game to force reconnects (`adb shell svc bluetooth disable/enable`), tails both logs, and pulls flight-recorder files on exit — the entire two-endpoint radio test cycle, link-death chaos included, becomes **one command an agent can drive**. Honest scope: Windows' BLE stack ≠ iOS's, so this is a development instrument, not final validation — phone↔phone soak remains the truth. Scheduling: **outside the 0.5 timebox**, as an agent-delegable parallel track through slices 0–1, ready before Phase 2. Bonus: it partially wakes the parked stress-test epic (PC-side high-resolution latency/throughput measurement against a real phone radio, nearly free).

**Definition of done (the anti-gold-plating checklist):** chess playable two-window on Windows · overlay toggles on both platforms · one recorded session replays hash-identical · fuzz + byte-budget suites green in CI. **Explicit non-goals:** hot reload, editors, renderer features beyond bring-up, and *no `GameHost`/`IGame` extraction yet* — the third main stays copy-paste on purpose. If the phase threatens to exceed its timebox, cut scope from the tail of this list, never extend the box.

## Phase 1 — RTS slices 0–1: sim + netcode on a ready bench *(no phones)*

Each item lands **because the slice forces it**, with a real client:

- **Slice 0 — the sim, pure:** entity slots, tick phases, tunables table — built **POD-first** per spec §6 (review #2 §3.3 as birthright, not refactor), rendered in the already-proven desktop window, driven through the already-proven `TouchEvent` path, instrumented by the already-live overlay. First playable-vs-yourself in days, not weeks, because Phase 0.5 removed every non-sim unknown.
- **Slice 1 — two-window loopback lockstep:** the generic tick-stamped input message forces the **minimal de-chess of `Session`** — guard/generalize `SendMove`, add the framed input path; full comment-recasting rides along cheaply *(R1-P1)*. **Desync hash**, the **RTS flight-recorder format**, RTS-specific **sim fuzz** (random button mashing across two instances), and the RTS **byte-budget tests** extend the Phase-0.5 harnesses rather than being built from scratch.

Chess keeps working untouched throughout. The RTS copies chess's bootstrap **shamelessly** — that duplication is Phase 4's raw material, on purpose.

## Phase 2 — RTS slice 2: phones *(the platform reckoning)*

- **Prerequisite: the mechanical platform extraction** *(R1-P1 §3.1: `git mv` BLE backends + Vulkan seams into `Modules`, dynamic JNI registration, log-tag parameterization)*. This was "important" before; the RTS makes it **mandatory** — the alternative is forking 1,600 lines of chess-package-coupled code into a second app. Zero logic change; the RTS shells then arrive thin, proving the target shape before chess's shells are even converted.
- RTS Android/iOS shells; **safe-area/window-metrics** through the platform surface *(rest of R1-P2 input item — the RTS HUD needs it immediately)*.
- **Soak/burn-in mode** on real phones *(R2 §4.3)* — the chess soak from Phase 0.5 gains an RTS twin: auto-play random presses overnight; failures arrive as flight-recorder files.
- **Wire & render polish batch** *(R1-P3: `WRITE_NO_RESPONSE`, Kotlin API-33 migration, glyph-drop log)* — opportunistic while BLE files are open; land **before** establishing the soak baseline so the baseline reflects final wire behavior.
- **Slice 2 arrives de-risked** by the hybrid rig: the exact wire protocol has already run over a real radio against a debugger-hosted endpoint throughout slices 0–1.

## Phase 3 — RTS slice 3: balance & ship *(+ parallel tidying)*

Tunables warfare on desktop at 4× with replays; worker-flee flag; juice within the byte budget. In parallel, the cheap cleanups: **delete-or-document `Modules/Pairing`, drop the vestigial `EBleRole`, sync CLAUDE.md/README** *(R1-P2)* — now trivially, because the docs describe a layout Phase 2 made true. Chess backlog items (e.g. the captured-pieces tray) fit here as palate cleansers.

## Phase 4 — The Extraction *(the reviews' centerpiece, now earned)*

With **two live games and four-plus main files** as evidence:

- **`GameHost`** extracted from the real, thrice-copied bootstrap *(R1-P1 §3.2 — shaped per R2/§8's arbitration: a toolbox the game's `main` calls, not a framework that calls the game)*.
- **`IGame`** written from the spec-§9 overlap table — discovered, not guessed *(R1-P2 §4, on R2's timeline)*. Chess and the RTS both port onto it; the state-hash/desync plumbing generalizes in the same motion.
- **`ChessRecord` → generalized per-opponent match record** (W/L/D + `MergeIfNewer`); chess's state goes POD opportunistically while the file is open *(R2 §3.3 retrofit half)*.
- **`Docs/NewGame.md`** — now checkable against two implementations.

## Phase 5 — Engine deepening *(post-extraction, evidence in hand)*

- **BLE unification: `BleLinkController` + `IBleRadio` drivers** *(R1 §3.5 / R2's most-agreed item)*. Sequenced here deliberately: the Phase-2 soak baseline on the *old* radio code becomes the unification's before/after regression proof, and the fake-radio tests inherit real scenarios from recorded soak failures. The Windows backend then becomes the **third** `IBleRadio` driver — more shape-evidence for the interface, and a home where the shared controller runs natively under a debugger.
- **Allocation diet, opportunistically**: `Guid` struct, function-pointer callbacks, the `std::filesystem`/iOS-floor decision *(R2 §3.1–3.2)* — each when its file is touched, never as a campaign.

## Parked — consciously, with wake conditions

| Item | Wake condition |
|---|---|
| **CoC/L2CAP epic + stream stress test** (the earlier issue file) | A game that actually needs bandwidth — the RTS provably doesn't (presses are bytes; spec §8). The hybrid rig partially wakes the *measurement* half early (PC↔phone latency/throughput, nearly free); the CoC transport itself still waits for a game that needs it. |
| Crane co-op physics game | After Phase 4: it needs the flight recorder, desync hashes, and desktop replay to make physics-determinism debugging humane — plus a deterministic 2D solver, which is its own project. |
| `ClockSync` implementation | A game needing latency display/tight pacing — lockstep is self-clocking (spec §7). |
| Magic bitboards, word-wise bitstream, ≥2 frames in flight | The first game that measurably needs each (R1 "later" — unchanged). |
| Rollback netcode | A reflex game; the POD-state work in Phases 1/4 is its entire prerequisite, so it gets cheaper by itself. |

---

## Complete disposition table (nothing dropped)

| Review item | Phase |
|---|---|
| R1-P0 threading race | **0** |
| R1-P0 Send cap | **0** |
| R1-P3 core hardening | **0** |
| R1-P2 real-time Session timing | **0** |
| R2 flags + LUR_ASSERT | **0** |
| R2 desktop build | **0.5** (chess as validation client) |
| R1-P2 Lur::Log | **0.5** |
| R1-P2 TouchEvent | **0.5** (metrics/safe-area in 2) |
| R2 POD state | **1** for new code; **4** retrofit |
| R2 debug overlay | **0.5** |
| R2 flight recorder / fuzz / byte-budget tests | **0.5** (chess first); RTS variants in **1** |
| R1-P1 de-chess Net | **1** minimal (slice 1); comments ride along |
| R1-P1 platform extraction (mechanical move) | **2** (slice-2 prerequisite) |
| R2 soak mode | **0.5** (chess) → **2** (RTS) |
| Hybrid Windows↔Android BLE rig *(new)* | **0.5→1** parallel, agent-delegable track; ready by **2** |
| R1-P3 wire & render polish | **2** |
| R1-P2 Pairing/EBleRole/docs | **3** |
| R1-P1 GameHost | **4** |
| R1-P2 IGame + NewGame.md | **4** |
| R1 §3.5 / R2 BLE unification | **5** |
| R2 Guid/fn-ptr/filesystem | **5**, opportunistic |
| CoC epic, ClockSync, crane game, bitboards, frames-in-flight, rollback | **Parked** (wake conditions above) |

**Critical path:** Phase 0 → **0.5 (Workbench)** → slice 0 → slice 1 → platform move → slice 2 → slice 3 → extraction. The chess soak and the hybrid BLE rig run in parallel from 0.5 onward. Phases 0 and 3's tidying items are parallel-friendly; everything else is genuinely sequential because each step is the next one's evidence.
