# Research: Engine Extraction — the boilerplate inventory

**Date:** 2026-08-09 · `master` @ 22468e7

## Problem Statement

Two games are near feature-complete (Chess = turn-based/degenerate; RocksPapersScissors = real-time
rollback RTS). A third (co-op physics puzzles) is next. Per the Handmade review §1, the extraction
precondition is now met: *the duplication IS the specification for the engine.* Find every feature
in the engine and both games, and decide engine vs game vs platform — prioritising the **multi-axis
exponential boilerplate** (reimplemented per game × per platform).

---

## Part 1 — Per-platform × per-game inventory

### Census (excludes build artifacts)

| File | Chess | RPS |
|---|---|---|
| Android main (C++) | `AndroidMain.cpp` 354 | `RpsMain.cpp` **1344** |
| Android Vulkan seam | 41 | 41 |
| Android BLE JNI (C++) | 286 | 324 |
| Android audio seam | 80 | *absent* |
| Android BLE shim (Kotlin) | `BleShim.kt` **740** | **760** |
| Android Activity (Kotlin) | 57 | 46 |
| Android manifest/gradle/CMake | 206 | 206 |
| iOS main (ObjC++) | `AppMain.mm` 595 | `RpsMain.mm` **1572** |
| iOS Vulkan seam | 53 | 53 |
| iOS BLE (ObjC++) | 575 | 636 |
| iOS audio | 105 | *absent* |
| iOS CMake + plist | 131 | 101 |
| Desktop main | 413 | **1527** |
| Desktop BLE dev-rig | 262 | 267 |
| Desktop CMake | 21 | 21 |
| **Total (game × platform)** | **≈3,919** | **≈7,658** |

**≈11,600 lines of (game × platform) code for 2 games × 3 platforms.**
Note: `CLAUDE.md:54` claims a "Swift shim" — **there is no Swift anywhere**; iOS is 100% ObjC++.

### The exponential boilerplate — items already in 4+ copies

| Item | Copies | Normalized drift |
|---|---|---|
| **A. Vulkan surface seam** (`PlatformSurfaceExtensions`/`CreatePlatformSurface`/`PlatformDrawableSize`/`PlatformLog`) | 4 | **0 lines differ** after name substitution. 188 lines that should be 2 engine files. |
| **B. Renderer create → `View.CreateResources` → loop → `Shutdown`** | 6 | Same 5-step ritual everywhere |
| **C. Elapsed-ns loop timer** (`steady_clock`/`CACurrentMediaTime` → `Tick(ElapsedNs)`) | 8+ | Identical shape; only the clock source varies |
| **D. BLE radio state machine** (advertise+scan+GATT server+client+CCCD+MTU+role tie-break+send queue+watchdog) | 6 backends × 2 games ≈ **3,240 lines** | ~65% textually identical; the differing 35% is **drift, not design** |
| **E. JNI byte-bridge + device/peer-id JNI** (14–16 entry points each) | 2 | Only the `Java_com_lurmotorn_onlyXXX_` prefix is per-game |
| **F. `Store` + `LoadOrCreateDeviceId` bring-up** | 6 | Only the directory API differs |
| **G. NativeActivity + BLE runtime-permission dance** | 2 | 30 of 46 lines byte-identical |
| **H. AndroidManifest permission/configChanges block** | 2 | **0 lines differ** after normalization |
| **I. Gradle/NDK/CMake/Xcode scaffolding** | 6 | RPS's `cpp/CMakeLists.txt` still carries *Chess's* comments verbatim ("shared engine + **chess** core") — proof of copy-paste, now wrong |
| **J. `LUR_*` capability macro re-declaration per app target** | 6 | `LUR_AGENT` was silently **missing** from RPS/iOS for a while |
| **K. `UnblockStdio()` MoltenVK guard** | 2 | Body *and* its 16-line crash-postmortem comment duplicated verbatim |
| **L. #73 reattach-for-activation heal** | 2 | ~120 lines of window-server archaeology, twice |
| **M. LUR_AGENT channel poll + verb dispatch** | 4 | Only the *transport* (`__system_property_get` vs `Documents/*`) is genuinely platform-specific — and it's reimplemented 4× |

**Extraction ceiling: A–L ≈ 6,000–6,500 of the ~11,600 lines are mechanically duplicated.**

### Essential differences between the two games (keep)

| Difference | Verdict |
|---|---|
| **Threading model** — Chess single-threaded everywhere; RPS has a sim/net thread (+ a pthread render thread on iOS) | **Essential** (turn-based vs 10 Hz real-time). But the *mechanism* — mailbox + ~30 atomics + pause/ack handshake, 88 lines Android / 149 iOS — is pure plumbing any real-time game needs. Engine-extractable. |
| **Input richness** — Chess 5 lines (tap, commit-on-DOWN); RPS 161 (plates, drag-place+snap, ghost, pan, x1/x5, console gesture) | Essential in *content*, accidental in *placement*: the dispatch skeleton is copy-pasted 3× **inside RPS alone** |
| **Audio** — Chess has device seams; RPS has none | Essential today; when RPS gets sound these seams get copied a 3rd and 4th time |
| **Desktop mode set** — RPS has ~640 lines of AI harnesses + `--replay` + `--recdiff` | **Essential game tooling** — exclude from extraction estimates |
| **Safe-area insets** — RPS only | Essential-ish, but RPS/Android is a **hardcoded 28/56 dp guess** with a comment admitting the real `WindowInsets` seam is unwritten |

### Accidental divergence — the real cost (same problem, opposite answers)

**Drift runs in BOTH directions.** Neither game is the good copy.

| Divergence | Chess | RPS | Consequence |
|---|---|---|---|
| **Role-override gating** | `#if LUR_AGENT` (with an explicit #196 rationale) | `#if LUR_INTERNAL` | **The same policy resolved oppositely.** RPS ships a rig-controllable BLE role override in every Development build. One of these is wrong. |
| Role deadlock breaker (#146) | absent | present | Chess still has the both-Peripheral deadlock RPS fixed |
| Bad device-id read guard (#146) | absent | present | Chess can settle a role from garbage |
| Hard radio restart (#182/#163) | absent | `restartRadio()` + reflective `gattRefresh()` | Chess cannot recover a wedged BLE stack |
| **Advertise/scan retry w/ backoff** | **present** | **absent** (just `Log.e` and give up) | RPS silently never advertises if the first attempt fails |
| Priority send queue (#190) | present (1-byte move jumps FIFO) | absent | Essential difference, but the *reasoning* lives only in Chess's copy |
| `onDestroy → ble.stop()` (#194) | present | **absent** | RPS leaks advertiser/scanner registrations — the exact `ALREADY_STARTED` bug #194 fixed |
| `APP_CMD_PAUSE`/persist-on-background | present | absent | RPS Android does nothing on background |
| Touch-latency instrumentation | both platforms | Android only — **iOS absent** | The one platform where the fps work happened has no touch-latency trace |
| **Session wiring** | hand-rolled handler table ×3 *inside Chess* | `Rps::RouteSessionToPeer`, one line per main | RPS already solved this after iOS silently dropped `MsgCvarSync`. **Chess still has the shape RPS abandoned.** |
| **Engine log sink** | **never installed** on Android/iOS — `Lur::Log::*` goes to a discarded stdout | installed first thing | Chess repeats verbatim the bug RPS's comment says cost a real diagnosis on 2026-07-30 |

`RpsMain.mm:545` names the pattern outright: *"iOS the odd one out every time"* (#147 cvar slots,
#151 gesture, #159 recorder sentinel).

### Kotlin/Swift: transcription vs thought

Doctrine (`CLAUDE.md:292`): shims are *"bytes and a GPU surface only."*

- **Kotlin, 1,603 lines.** Compliant where the boundary was deliberately drawn — role tie-break,
  peer binding, device-id all call into C++ (`"The Kotlin shim asks; C++ decides."`).
- **~40–45% of `BleShim.kt` is *thought* that escaped C++**, untested by any host suite:
  send queue + in-flight + watchdog + token timeout (~60 lines of **netcode, in Kotlin**);
  discovery watchdog + role escalation (~45); advertise/scan backoff (~50); `restartRadio()` +
  `gatt.javaClass.getMethod("refresh")` private-API reflection (~50); reconnect/rescan scheduling
  (~75); GATT long-read offsets (~17).
  *Because it is thought, it drifted* — which is exactly why each game holds fixes the other lacks.
- **Swift: 0 lines.** ObjC++ sidesteps the doctrine by *being* C++ — at the cost of `RpsMain.mm`
  mixing UIKit lifecycle with engine logic across 1,572 lines and a 149-line ivar block.

### Ranked extraction targets (agent's ranking)

1. **BLE backends → `Modules/Transport/Platform/{Android,iOS,Windows}`** — ~3,240 lines in 6 copies;
   parameterised only by service UUID (already a compile definition) + JNI package prefix.
   Removes ~1,600 duplicated lines and **collapses 12+ one-sided bug fixes into one code path.**
2. **App skeleton / `GameHost`** — the `android_main` / `UIApplicationDelegate+VC` / Win32 triads,
   ~1,400 duplicated lines incl. #73 reattach and `UnblockStdio`.
3. **`Lur::Agent` control channel** — 4 copies; RPS already has the grammar, only the per-platform
   transport is missing from the engine.
4. **Vulkan surface seams** — 188 lines, **provably 0-diff**. Cheapest possible win.
5. **Android project scaffolding** — ~430 lines, <40 genuinely per-game.
6. **Session/save/score bring-up** — 6 copies; `Rps/SessionWiring.h` is the proof it extracts cleanly.

---

## Part 2 — Engine module inventory

16 modules, **~8,205 LOC of engine** (excl. tests + 9,662 lines of cooked font atlases), 3,437 LOC
of module tests.

| Module | LOC | Kind | Chess uses | RPS uses | Leakage | Maturity |
|---|---:|---|---|---|---|---|
| **Core** | 1,062 | INTERFACE + `buildfp` | 10f/11inc (Assert, Log, FlightRecorder) | 17f/26inc, **~100 `LUR_CVAR`** | Low (all examples `rps.*`) | Mature; `Hash.h` dead |
| **Serialization** | 207 | compiled | 6f/11inc | 3f/7inc | Low | Mature |
| **Sim** | 269 | INTERFACE | **0** | 14f/16inc | Low–Med (`AdvancePreserving` for RPS) | Solid, **RPS-only** |
| **Math** | 241 | INTERFACE | 0 direct | 2inc (`Spring`, `Mat4`) | None | **Quat/Perspective/LookAt dead** |
| **Save** | 376 | compiled | 10f/19inc (`SyncManager` ×15) | 7f/13inc (**`Store`+`DeviceId` only**) | None | Mature; sync half **chess-only** |
| **Text** | 494 | compiled | 2inc | 3inc | Low | Mature; **`FontRegistry` dead** |
| **Audio** | 488 | compiled | 6f/8inc (`Mixer` ×31) | **0** | Low | Mature, **chess-only** |
| **Trace** | 227 | compiled | 2inc, **0 scopes** | 4inc, 15 scopes | None | Cleanest module |
| **Transport** | 534 | INTERFACE | 7f/14inc | 7f/12inc | **High** | **Most mature** |
| **Net** | 625 | compiled | 5f/5inc (`SendMove`) | 6f/6inc (framed `GameN`) | **High** | Contaminated; `ClockSync` dead |
| **Pairing** | 39 | INTERFACE | **0** | **0** | — | **DEAD** |
| **Platform** | 223 | STATIC (`LUR_DESKTOP AND WIN32` only) | 1inc | 1inc | Med | **Thin — window only** |
| **Render** | 1,988 | INTERFACE/STATIC | 7inc | 7f/9inc | **High** | Mature, **untested** |
| **Input** | 119 | INTERFACE | 1inc (`Input.h`) | 3inc (all `ConsoleGesture`) | Low–Med | `Input.h` trivial |
| **Hud** | 801 | compiled | 3f/4inc | 2f/3inc | Structural: **Hud→Net** | Solid, **untested** |
| **DevGui** | 524 | INTERFACE | **0** | 2f/7inc (`Numpad` ×37) | Med | Solid, **RPS-only** |

### Game-shaped leakage INTO general modules (quoted)

| Sev | Location | Evidence |
|---|---|---|
| **High** | `Render/Private/Vulkan/VulkanBackend.cpp:630` | `App.pApplicationName = "OnlyChess";` — **the shared Vulkan backend names the RPS app "OnlyChess"** |
| **High** | `Transport/.../BleProtocol.h:61-64` | `#define LUR_BLE_SERVICE_UUID "4C55524D-…"` — *"the default is chess's original value"*. The engine's default radio identity is a game's |
| **High** | `Net/.../Session.h` | `SendMove`/`SetMoveHandler` — a **chess-only** "bare 1-byte datagram is always a move" API on the engine session. RPS calls it zero times |
| **High** | `Net/.../Session.h:52-70` | `ProtocolVersion = 9` — **v6/v7/v8/v9 are ALL RPS gameplay-wire changes.** 4 of 9 engine protocol versions exist for one game; a chess-only change can't reuse a number |
| **Med** | `Net/.../Session.h:35-37` | `Game6 = 11, // the RTS aliases it to MsgCamp … Costs a wire id on purpose` |
| **Med** | `Render/.../Renderer.h:52-66` | `MaterialDesc::{Gamma,Outline,InkLo,InkHi}` + `ETextureFormat::Rg8` — *"Chess uses these to render both piece colours from one mask set"* |
| **Med** | `Render/.../Renderer.h:145-155` | `DrawInstances` — *"This is the RTS unit path"* |
| **Med** | `DevGui/.../DevTheme.h:7-9` | *"RPS's teams are cyan/yellow (#142) — the accent here is deliberately desaturated against team cyan"* |
| **Med** | `Platform/Private/Windows/Win32Window.cpp` | Auto-repeat filtering **removed** because *"The console is now the only consumer (#119)"* — engine input policy set by one game's dev UI |
| **Low** | `Sim/Fixed.h:52`, `FixedString.h:20`, `Tick.h:54` | *"grown on demand as the RPS sim's call sites need them"* |
| **Low** | `Audio/AudioDevice.h:6`, `Transport/Ble.h:25` | Seam headers name `Games/Chess/{Android,iOS}` as *the* implementers — **stale** |
| **Structural** | `Hud/CMakeLists.txt` | `lur_hud` links `lur_net`; `DebugOverlay.h`/`LinkStatusBar.h` include `Session.h` for `ELinkState` — **presentation coupled to the net session** |

### Dead / single-game code in the engine

- **`Modules/Pairing` — fully dead.** 39 LOC, no impl, no tests, **zero consumers repo-wide**. Still `add_subdirectory`'d. (#47 recommends deletion.)
- **`Net/ClockSync.h` — dead stub.** Hardcoded false/0 + `TODO(net)`. `EMsgType::ClockPing/ClockPong` reserved but never sent.
- **`Hud/LinkStatusBar` — dead.** 60 LOC compiled into every build; only self-references.
- **`Text/FontRegistry` — dead.** Both games hold a bare `Font`.
- **`Core/Hash.h` (`Fnv1a64`) — dead in production.** Only tests. *(Note: RPS hand-rolls its own `StateHash`.)*
- **`Math`: `Quat` (whole file), `Mat4::Perspective`, `Mat4::LookAt` — zero uses.** The 3D half of Math is aspirational — worth remembering given the renderer is "3D-capable by design".
- **Single-game modules:** `Sim` (RPS-only — chess has **zero** includes), `Audio` (chess-only), `DevGui` (chess: 0), `Save`'s `ISaveState`+`SyncManager` (chess-only), `Input/ConsoleGesture` (RPS-only).
- **No tests at all:** Hud (801), Render (1,988), Platform (223), Pairing (39) = **3,051 LOC untested**.

### `Modules/Platform` — what "platform abstraction" actually means today

**One header (69 LOC) + one impl (154 LOC), Windows-only, built only under `LUR_DESKTOP AND WIN32`,
consumed by exactly 2 files in the repo** (the two `Desktop/DesktopMain.cpp`).

It is a Win32 window class with a mouse→`TouchEvent` normaliser and a key queue. **It is not a
platform abstraction layer.** Missing:
- **No file I/O abstraction** — `Save::Store` calls `std::filesystem` directly; `FlightRecorder` and
  `Rps::MatchRecord` call `std::fopen` directly; save-dir discovery is each app's problem.
- **No time abstraction** — **three independent `steady_clock` readers** (`Win32Window.cpp`,
  `Trace::NowNs`, and every game main), plus a hand-rolled wall clock in `Chess/Core/MatchMeta.cpp`.
- **No thread abstraction** — raw `std::thread`/`mutex`/`atomic` in `SimRunner`, `EventInbox`,
  `Trace`, `WindowsBleTransport`.
- **No process/env/dylib abstraction** — `WindowsBleTransport` spawns a C# subprocess with raw Win32.
- **No non-Windows desktop backend; no phone path** (Android gets its window from `NativeActivity`,
  iOS from UIKit — by design, but it means Platform is bypassed on the two shipping platforms).
- **No DPI/display query, no text input, no gamepad, no clipboard.**
- The **Vulkan surface seam is NOT in Platform** — it's `Render/Platform/Windows/VulkanSurface.cpp`.
  So "platform" is split across two modules with **no shared convention**.

### Build system

`cmake/EngineFlags.cmake`: one dial `LUR_CONFIG` → six macros (`LUR_SHIPPING`, `LUR_INTERNAL`,
`LUR_ASSERTS`, `LUR_SLOW`, `LUR_TRACE`, `LUR_AGENT`), re-published as `CACHE INTERNAL` so
out-of-tree app targets re-apply them **by hand** (the 6-copy boilerplate).
**Strict flags (`-Wall -Wextra -Werror -fno-rtti`) apply only when `PROJECT_IS_TOP_LEVEL` — the
phone app builds do NOT inherit `-Werror`.** Exceptions are still on **because `Lur::Save` uses
throwing `std::filesystem`** (the review's §3.1 parable, still unpaid).

**The Games→Modules wall has NO automated lint** — it holds by convention plus the fact that a
module linking a game target would fail to configure. **The leakage is semantic, not structural.**

---

## Part 3 — Game-side shared C++: the one-way ratchet

### Size baseline, corrected

The scary raw numbers deflate a lot once cooked assets and tests are removed:

| Directory | Total | Tests | Cooked-asset headers | **Hand-written logic** |
|---|---|---|---|---|
| `Chess/Core` | 2084 | 786 | 0 | **1298** |
| `Chess/View` | 6651 | 84 | 5516 (`PieceMasks.h`, `SfxClips.h`) | **1051** |
| `RPS/Core` | 6867 | 1978 | (`Tunables.h` 1114 is a CVar table) | **4889** |
| `RPS/Net` | 4720 | **2708** | 0 | **2012** |
| `RPS/Runtime` | 741 | 208 | 0 | **533** |
| `RPS/View` | 14795 | 0 | 11584 (`IconMasks.h`) | **3211** |

"14,800 lines of RPS View" is really 3,211 lines of code. "4,700 lines of netcode in a game
folder" is 2,012 lines of netcode + 2,708 lines of tests *for* it.

### THE KEY FINDING — a different problem than the platform layer

> Almost none of it is *drifted duplication* between the two games — the two games are genuinely
> different genres and share very little logic. **The real problem is that each engine facility
> exists in exactly ONE game, so the module wall (`Modules` may not depend on `Games`) has quietly
> become a one-way ratchet: features get built in whichever game needed them first and never come
> back.**

This is the opposite failure mode from the platform layer (where the problem *is* 6 drifted copies).
Two distinct diseases needing two distinct cures:
- **Platform layer** → *deduplication* (6 copies → 1).
- **Game-side C++** → *promotion* (1 copy in the wrong place → 1 copy in the right place).

**~1,900–2,300 lines currently in game folders are engine material.**

### Engine material stranded in ONE game

| Facility | Lives in | LOC | Note |
|---|---|---|---|
| **Dev console / CVar browser** | RPS `GameView.cpp:1931-2620` | **~690** | Largest single block. Category tree from dotted names, fold/scroll/clip, keyboard cursor ≠ edit target, numpad, colour-picker v2, tooltip toaster, swatch material ring. `Modules/DevGui` exists but holds **only the leaf widgets** (479 LOC) — the assembly/layout/interaction layer never came back. Chess has none of this. |
| **Rollback / speculation / prediction** | RPS `LockstepPeer.cpp:462-571`, `SnapshotRing.h` | ~350 | Templatable over a Sim traits type |
| **Desync recovery + lost-frame repair** | RPS `LockstepPeer.cpp:690-830` | ~230 | GUID-tie-break survivor replay is genre-neutral |
| **Sim thread runner** (`SimRunner`) | RPS `Runtime` | 190 | **~170 of 190 is generic**; only a pre-match camp gate is RPS |
| **Audio triggering + variation policy** (`SfxLibrary`) | **Chess** `View` | 164 | Group/no-repeat/pitch+gain jitter is generic. RPS has no audio at all |
| **Agent remote-control parser** (`AgentControl.h`) | RPS `Core` | 152 | **Wholly game-agnostic already** — only the verb list is RPS |
| **CVar replication at a stamped tick** | RPS `LockstepPeer.cpp:572-671` | ~130 | Generic "apply a replicated value at agreed tick T" |
| Sprite batching / instanced draw | RPS `View` | ~130 | Belongs in `Modules/Render` |
| Gradient/disc mesh + `Hsv`/`Srgb`/`FlatMat` helpers | RPS `View` | ~130 | |
| **Rollback correction smoothing** | RPS `GameView.cpp:973` | ~110 | Header itself says *"This is the piece that transfers to the physics game."* Depends only on (prev pos, cur pos, serial, alive) |
| Deterministic spatial grid (counting-sort CSR, zero-alloc) | RPS `Sim.cpp:52` | ~90 | Nothing RPS about a deterministic neighbour grid |
| **GUI sub-layer ordering** (collect during world pass, flush as own passes) | RPS `GameView.cpp:1418` | ~90 | A *renderer* concept implemented in a game — fixes "loop order ≠ z-order" |
| Nearest-feasible-point projection (iterated clamp + push-out, 12×) | RPS `Snapshot.h:150` | ~60 | General 2-D constraint projection → `Modules/Math`. **Directly relevant to the physics game** |
| Camera scroll (drag, flick momentum, exp damping, clamp) | RPS `CameraScroll.h` | 53 | Entirely general; Chess has no camera |
| Snapshot mailbox (SPSC double-buffer) | RPS `Snapshot.h:273` | ~35 | Zero RPS content |
| Build-fingerprint gate | RPS | ~25 | `Lur::BuildFingerprint` already in Core |
| Alpha-step material LUT workaround | RPS, **×5 sites in one file** | ~20 | Symptom: renderer materials are immutable |
| Fixed-point constexpr builders `F()`/`FRound()` | RPS `Tunables.h:28` | ~20 | Belong in `Modules/Sim/Fixed.h` |
| Interpolation alpha (`AlphaAt`, no-extrapolation clamp) | RPS `Snapshot.h:257` | ~8 | Encodes the "never extrapolate" law |
| "Time ago" formatting | **Chess** | 10 | |
| Per-opponent enumeration / store-key sidecar | **Chess** (`OpponentRegistry`, `MatchMeta`) | 148 | Store-key enumeration is generic; RPS simply lacks the feature |

### Where BOTH games have a version — essential vs accidental

| Feature | Verdict |
|---|---|
| **Opponent selector population** (78 vs 85 LOC) | **Row model essential** (persistent async opponents vs one live peer + AI tiers); **widget-driving boilerplate accidental**. Both independently learned to re-point by *semantic* selection rather than row index. Same lesson, two implementations. |
| **Resync after reconnect** | **Same architecture, different payload.** Both are "replay the input stream, never transfer state" and justify it identically. Chess needs no chunking (~1 byte/ply); RPS needs byte-bounded chunks. Genuinely divergent code, **identical design** — and the design lives in neither module. |
| **Persistent W-L-D record** | Both independently invented "player-agnostic WinsLower/WinsHigher, orient at read time from GUID order". Layout difference is essential; a shared `Lur::Save::AgnosticTally` primitive is missing. |
| **Screen↔world transform** | Near-duplicate in concept, divergent in code — **accidental**. RPS has a clean inverse pair; Chess has a non-symmetric `ComputeLayout`+`CellTopLeft`+`SquareAt`. |
| **Safe area / layout** | **Accidental divergence** — RPS solved it (`SetInsets`), Chess is an admitted proportional stopgap (`BoardView.cpp:312`), neither is in `Modules`. |
| **RG8 shade+coverage upload loop** | **Near-duplicate** upload/material code; essential art difference (6 textures vs 1 atlas). |
| **Flight recorder** | **Two unrelated recorders, neither aware of the other.** RPS `MatchRecord` (370 LOC, line-oriented text, records *sim input + hashes* → replayable) vs `Modules/Core/FlightRecorder` (binary, records *datagrams*, used only from Chess Desktop). Both legitimate; the overlap is unmanaged. |
| **Thread-safe input inbox** | **Three near-copies of one idea**: RPS `SoloInput.h` (41), `LockstepPeer::PendingLocalEvents`, and `Modules/Transport/EventInbox.h`. |
| Move/input codec, match state machine, desync cadence, AI/bot | **Essential divergence** — genre-driven. Leave alone. |
| GUID short label | **Already resolved** — lifted to `Modules/Hud/GuidLabel.h`. The one place drift was caught. |
| Text drawing (`Hud::TextField`), gesture (`Input::ConsoleGesture`), spring (`Math/Spring`) | **Already shared, no drift.** Proof the model works. |
| Localization | **Absent in both** — all strings are inline English literals. A gap, not a duplication. |

### Why `RPS/Net` and `RPS/Runtime` sit outside the engine

The stated reason (`SessionWiring.h:25`) is correct — *"the message SET is a game concept, and Modules
must never depend on Games"* — **but the boundary is drawn too far out.** The genuinely game-typed
part is small: `StepOneTick`, `EmitAnchor`'s hash source, `SnapshotRing`'s element type, and the codec.

Everything else is genre-generic and would template cleanly over a `SimTraits{StateType, InputType,
Step, Hash}`: confirmed-frontier arithmetic, the rollback/speculate loop, the prediction seam, input
timelines, anchor cadence, the three stall bounds, the recovery/gap state machines, the resync chunk
exchange, CVar-at-a-stamped-tick replication.

> **`Modules/Net/ClockSync.h` being a 30-line stub while a full rollback netcode ships next door is
> the clearest symptom: the module that *should* hold this is unfinished, so the game folder grew
> the real thing.**

Same for `Runtime`: ~350 of its 533 lines would move into the engine under one type parameter.
`Modules/Net` today is only `Session.h/cpp` (595 LOC) + that stub.

---

## Part 4 — Issues & journals: prior decisions

### Chronology of the engine/game boundary

| When | Event | Effect |
|---|---|---|
| 2026-07-17 | **Review #1** (architecture lens, @ #38) | Named the still-governing diagnosis: **~1,620 lines of game-agnostic engine code parked inside `Games/Chess/`** — "engine backends and app scaffolding parked in the game's app folders." Produced §3.1 platform-move, §3.2 `GameHost`, §3.3 de-chess Net, §3.5 shared-first doctrine + `BleLinkController`/`IBleRadio`, §4 `IGame`. |
| 2026-07-17 | **Review #2** (Handmade lens) | Challenged the *order*, not the destination. Ruled: **`GameHost` = toolbox the game's `main` calls, not a framework that calls the game.** Plus "count the implementations". |
| 2026-07-17 | Master roadmap (now LEGACY) | Sequenced extraction *behind* game #2: … → Phase 2 platform move (#42) → Phase 4 **The Extraction** (#43 GameHost, #45 IGame) → Phase 5 BLE unification. Rule: **new code follows the doctrines immediately; retrofitting happens opportunistically or at extraction time, never as its own project.** |
| 2026-07-18 | Workbench (#50–#58, #70) | **The Windows platform layer went into `Modules/*/Platform/Windows` from day one** — "the chess mistake, not repeated". Why `Win32Window.cpp` + `Render/Platform/Windows/VulkanSurface.cpp` exist while Android/iOS equivalents don't. The desktop chess main was a **deliberate third copy-pasted main** — "Phase-4 extraction evidence, not a problem to solve now". |
| 2026-07-19 | **RPS shipped copy-first** | Chess's entire platform glue copy-pasted. *"The copy is deliberate — it's the second consumer that earns #42 with evidence."* |
| 2026-07-19 | #79 fix session | First concrete pain: *"this session hand-ported identical edits across **4 copied transport files**"* — "#42 now VERY ripe". |
| 2026-07-20 | Perf pass | #39 gained the "two dials" note: per-app CMake re-applying `LUR_*` is *"copy-paste that drifts between games — a candidate for `lur_configure_app_target()`"*. |
| 2026-07-20/21 | Devtools epic #110 | New line drawn deliberately: CVar/Console is **engine infrastructure with no per-game hook** — *"If a tool behaviour needs to change, it changes in the engine, for all games at once."* CVar `Origin` is engine-derived, never game-declared. |
| 2026-08-04 | Rollback shipped | The coordinator-lift plan, recorded on #8 and #44. |
| 2026-08-08 | Chess immediacy epic #186 | The *whole* RPS immediacy pass **hand-ported into chess**. Newest evidence of duplicated net/render/input policy. |

**Net: the evidence gate the roadmap set has been met and passed. Nothing in the extraction chain
(#42/#43/#44/#45) has actually been executed.**

### Decided rules — do NOT re-litigate

**Boundary/structure**
1. `Games/* → Modules/*` only, CMake-enforced. `Modules/*` contains no game concepts, in API *or* comments.
2. **Target end state (#39, already written):** *"a game folder holds only gameplay C++ + content; the engine holds all generic **and platform-specific** machinery."* Game shells = manifest/plist, icon, package name, ~tens of lines of glue. **← This is exactly the user's ask, already ratified.**
3. **Target layout already specified (#42):** `Modules/Transport/Platform/{Android,Ios}/`, `Modules/Render/Platform/{Android,Ios}/`, `Modules/App/Platform/{Android,Ios}/`. `Modules/Render`'s shared-backend + tiny-per-OS-seam is **the model to copy**; Windows already follows it.
4. **Shared-first doctrine (standing):** *"A platform file may contain API verbs and event forwarding — never decisions. If a line chooses (retry or not? which role? what order? how long to wait?), it is engine C++. The test: could this be unit-tested on the host against a fake? Then it must live where the host can build it."* Plus a **standing budget: platform-specific code across both OSes stays under ~600 lines total**, and every new platform file states which engine interface it implements and why the logic can't live behind it.
5. **Library, not framework.** The game owns `main`.
6. **Count the implementations.** Multi-at-runtime → interface; one-per-platform → link-time seam; one total → just code.
7. **Extract only from real duplication**; retrofit opportunistically, never as a campaign.

**Netcode/sim**
8. **`State = Replay(Inputs, Seed)` is the engine's one law** — *"live play is replay at the frontier; hitch recovery is replay from the inbox; resync is replay from chunks; the flight recorder is replay from a file; the determinism test is replay from a script. One mechanism, five costumes."*
9. Sim state POD/trivially-copyable, fixed-capacity, no floats, no heap in the tick.
10. **Renderer INTERPOLATES, never PREDICTS. Send off-grid, execute on-grid.**
11. Distributed per-entity authority; never a host peer.
12. **`IGame`'s two contract decisions are pre-made (#45):** the contract requires only `MergeIfNewer` (chess's turn-alternation becomes a *provided helper*, not an assumption); distributed authority stays a **future opt-in layer**. Don't complicate the contract for the shooter.
13. Enum slots 3–5/8–10 are generic `Game0..Game5` — the engine names no game concept.

**Build/tooling**
14. `LUR_CONFIG` is the single human-facing dial; a consumer never hardcodes `CMAKE_BUILD_TYPE`.
15. `LUR_AGENT` is a separate axis from `LUR_INTERNAL`.
16. **Devtools have no per-game hook** — behaviour changes in the engine, for all games at once.
17. BLE service UUID is per-game (`LUR_BLE_SERVICE_UUID`) — preserve through extraction.

### Open extraction chain

| # | Title | Status |
|---|---|---|
| **#39** | Epic: Engine extraction & hardening — *"from a chess app with an engine inside to an engine that shipped chess first"* | The umbrella |
| **#42** | Move the platform layer out of `Games/Chess` into engine platform modules | P1, phase-2. Work items already written: `git mv`, **dynamic JNI `RegisterNatives` in `JNI_OnLoad`**, drop `loadLibrary("onlychess")`, parameterize log tag. "VERY ripe" since 2026-07-19 |
| **#43** | Extract `Lur::App::GameHost` from the duplicated bootstrap | P1, phase-4. Header sketch exists (`Config{SaveDir, ITransport*, Log}`, `Start/Tick/OnBackground`, `OnPeerAdopted` stays game-side). Depends on #42 |
| **#44** | De-chess `Modules/Net` | **Partially done** — enum is now `Game0..Game5`. **Still open: `Session::SendMove` silently truncates >1 byte** (`Session.cpp:155-160`). Newest scope: lift the rollback coordinator |
| **#45** | Name the game contract: `IGame` + chess as reference impl + `Docs/NewGame.md` | P2, phase-4 |
| **#47** | Resolve `Modules/Pairing`, drop vestigial `EBleRole` | `IPairing` is **dead at HEAD**. Lean recommendation on record: **delete the module** |
| **#8** | Rollback → generalize to `Modules/Net` + apply to physics game | |
| **#82** | Audio: generalize into a shared engine feature | Per-piece disposition already written |
| **#49/#48/#66/#74** | Renderer resource-lifetime contract + `MaxMaterials=32`; core hardening; on-disk format version; chess record > 1 datagram can't resync | |
| **#12** | Roadmap. **Phase 5 items — BLE unification (`BleLinkController`+`IBleRadio`) and the allocation diet (`Guid` struct, fn-ptr callbacks, `std::filesystem` replacement) — are listed "to draft" and were NEVER FILED as issues.** | |

### Closed as REJECTED (don't re-propose)

- **#115 desktop CVar `--tune` panel — DROPPED.** "The Console covers tuning on both platforms." One tool, one UI. `Modules/DevConsole` was **deleted**; so was `IRenderer::SetViewportRect`.
- **Render extrapolation + local produce-ahead — built then REVERTED.** Principle recorded: *"when a change makes things worse, remove the thing you added — don't add compensating state."*
- **Snapshot resync — PARKED** (wake: cold-rejoin replay > ~2 s).
- **#59–#64 L2CAP CoC — PARKED**; **Wi-Fi hotspot handoff — REJECTED**; **`ClockSync` stays a stub** (lockstep is self-clocking).
- **Phone autoplay REMOVED**, re-scoped `LUR_AGENT`-only (#195/#196).
- **Tick-rate 10→20 Hz — deliberately skipped.**
- **GameplayId cook — deliberately NOT built** (#112); name-keyed recordings won instead.

### Measured drift at HEAD (2026-08-09)

Review #1 estimated 1,620 lines for one game. **With two games it is ~9,900 lines of per-game
platform + shell code**, and the copies have measurably drifted:

| File pair | Chess | RPS | Diff after normalizing names |
|---|---:|---:|---:|
| `BleShim.kt` | 740 | 760 | **299 — badly drifted** |
| `AndroidBleTransport.cpp` | 286 | 324 | **221** |
| `IosBleTransport.mm` | 575 | 636 | **131** |
| `WindowsBleTransport.cpp` | 165 | 167 | 10 *(not even in #42's list — a third radio copy)* |
| `AndroidVulkanSurface.cpp` | 41 | 41 | **0 — byte-identical** |
| `IosVulkanSurface.mm` | 53 | 53 | **0** |

### The third game — everything on record

- **Identity:** "crane co-op physics game" / co-op physics puzzles. Game #3 candidate since 2026-07-17.
- **Wake condition (roadmap):** *"After Phase 4: it needs the flight recorder, desync hashes, and desktop replay to make physics-determinism debugging humane — plus a deterministic 2D solver."* **Phase 4 = the GameHost/IGame extraction. The extraction is explicitly the third game's stated prerequisite.**
- **Physics decision SETTLED (2026-08-04):** hand-rolled `Fixed` physics + the shared rollback stack, **not** a Box2D fork (no snapshot API, large hidden solver state, float determinism per-toolchain fragile, forking breaks the no-libraries rule).
- **Netcode risk retired.** Remaining new risk = *authoring a stable fixed-point constraint solver*.
- **Go/no-go number:** `resim_ticks × step_cost` under **continuous two-hand manipulation** vs the frame budget, measured on a `Fixed` prototype. First thing to build (#8 item 2).
- **Why it's the adversarial case** (RPS was friendly): cost ≈ correction frequency × resim depth × step cost, and physics is worse on all three — continuous direct manipulation → frequent mispredictions; densely-coupled bodies → one corrected input moves many → large visible pops; heavier step + deeper head lead.
- **Fallback already chosen:** diegetic masking ("machines rev up", settle animation) — *prototype both*.

### Doc gaps found

- **No `Games/Chess/README.md`**, **no `Modules/*/README.md`** — the engine has no durable checked-in orientation doc. `Docs/NewGame.md` (#45) does not exist.
- `Games/Chess/iOS/README.md` is **stale** (claims "no renderer / Vulkan / MoltenVK yet").
- The two `Android/README.md` files carry **near-identical `LUR_CONFIG` ladder tables** — itself duplication.
- `Games/RocksPapersScissors/README.md` is **the model** for durable per-game orientation.
