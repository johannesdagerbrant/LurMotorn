# CLAUDE.md

Guidance for Claude Code (and humans) working in the **LurMotorn** repo. Read this first.

## What this is

LurMotorn is a from-scratch engine for ultra-low-latency **local** multiplayer games. Two phones —
Android *or* iPhone — pair over Bluetooth Low Energy and play locally, sending the smallest possible
payload across the wire.

Two games ship from this tree, and each proves a different half of the engine. **Chess**
(`Games/Chess`) came first: turn-based, the original proving ground for the transport, the wire
codec and per-opponent persistence. **RocksPapersScissors** (`Games/RocksPapersScissors`, "RPS") is
the real-time RTS that forced the hard parts — a deterministic fixed-point sim on its own tick
thread, rollback netcode, and the dev-console/CVar tooling. Where the two disagree about how
something should work, that difference is usually the specification for an engine facility neither
one should own.

## Non-negotiable constraints

Do not violate these without explicit confirmation from the user:

- **No third-party libraries — with one sanctioned exception.** Only the unavoidable OS frameworks
  (Android SDK/NDK, Apple SDK) and a single GPU API: **Vulkan everywhere** — native on Android, via
  **MoltenVK** on iOS. MoltenVK is the *one* allowed third-party dependency, because no native GPU
  API spans both platforms. Otherwise: no ZXing/ML Kit, no networking or chess libraries, no model
  importers. If something else seems to need a library, hand-roll it or raise it with the user first.
- **Build-time asset cookers are a separate category from runtime libraries** — and one is sanctioned:
  **`msdf-atlas-gen`** (MIT). It runs OFFLINE on a dev machine / CI to bake OFL fonts into committed
  MSDF atlas headers (`scripts/gen-font.ps1` → `Modules/Text/Private/Cooked/FontAtlas_*.h`); it is
  **never linked into the app or its CMake build** — only its output ships, and text is rendered by our
  own Vulkan pipeline + a hand-written median/`fwidth` shader. This is unlike MoltenVK (a *runtime*
  dependency). It's the same category as the `images.weserv.nl` service that cooks the piece art. Fonts
  must be **OFL** (commercial-safe). Other build-time cookers still need the same "raise it first" rule.
- **No servers, ever.** Strictly local play. No backend, no internet relay — not even a seam for one.
- **Cross-platform Android <-> iPhone.** Every wire/transport/render choice must work *between* an
  iPhone and an Android phone. This is why the link is BLE and the renderer is a single Vulkan
  backend (MoltenVK on iOS).
- **Slim payload and low latency are the product.** Prefer bit-level encodings; never bloat the wire
  for convenience.

## Architecture

One shared, pure-C++ core compiles identically on host, Android (NDK), and iOS. Everything that
touches hardware gets a per-OS backend behind a common C++ interface, and those backends live in the
engine under `Modules/<Module>/Platform/{Android,Ios,Windows}/` — **never** in a game folder.

Dependency rule, enforced by CMake: `Games/*` may depend on `Modules/*`; `Modules/*` must **never**
depend on `Games/*`. That wall is what keeps the engine reusable for future games. Its cost is a
**one-way ratchet** — a facility built in whichever game needed it first cannot come back on its
own — so see the promotion rule under *Working style*.

```
Modules/Core           pure C++  asserts, logging, CVars, hashing, build fingerprint, flight recorder
Modules/Serialization  pure C++  slim-bytes codec (BitWriter/BitReader/Varint)
Modules/Sim            pure C++  deterministic fixed-point + fixed-timestep (gameplay)
Modules/Math           pure C++  Vec/Mat4/Spring for render + scene transforms (FLOAT, not sim)
Modules/Net            pure C++  Session: handshake, framing, resync
Modules/Save           pure C++  device id, save store, per-opponent sync
Modules/Transport      ITransport + BLE: shared policy (send queue, start retry, discovery timers)
                                 over per-OS radios (Android JNI->Kotlin, iOS CoreBluetooth)
Modules/Render         IRenderer + single Vulkan backend (MoltenVK on iOS), 3D-capable
Modules/Audio          IAudioDevice + mixer + PCM codec, per-OS device backends
Modules/App            GameHost (session/persistence choreography) + per-OS entry points
Modules/Platform       desktop window + surface + input seam
Modules/Input          touch events, gesture recognizers
Modules/Text           MSDF glyph rendering + layout (atlases cooked offline)
Modules/Hud            debug overlay, dropdown, text field — game-facing HUD widgets
Modules/DevGui         dev-console widget set: category tree, colour picker, numpad, popover
Modules/Trace          scoped CPU timing

Games/<Game>/Core      pure C++  rules + codec (shared verbatim by every platform)
Games/<Game>/View      rendering + input handling for that game
Games/<Game>/Desktop   desktop harness: two instances in one process, loopback or real BLE
Games/<Game>/Android   Android Studio project (thin Kotlin activity + C++ main)
Games/<Game>/iOS       Xcode project (ObjC++ main + C++ + Vulkan via MoltenVK)
```

There is **no Swift** anywhere in the tree — the iOS side is ObjC++ (`.mm`) so it can call the C++
core directly. `Modules/Pairing` and `Modules/DevConsole` were deleted; if you find a reference to
either, it is stale.

The renderer is **3D-capable by design** (meshes + depth + camera + materials); 2D (the chess board)
is the orthographic special case via `Render/Sprite2D.h`. 3D model loading, when a game needs it,
is ours to write (no importers) — likely a glTF 2.0 / `.glb` loader.

### Authority is distributed, never hosted

LurMotorn is **symmetric peer-to-peer**: there is no host, and no peer owns the global source of
truth. Authority over a piece of state is assigned **per-entity (and per-aspect) to the peer whose
player interacts with it most tightly**, so that interaction is *locally authoritative* — instant,
never round-tripped to another peer for approval. Chess: each phone owns its own pieces. A shooter:
each phone owns *spawning* its own units (snappy spawn) but hands *movement* authority of a unit to
whoever is shooting at it, so aim is accurate ("you hit what you see"). Authority follows the
interaction whose *feel* must be protected (spawn-feel → owner; aim-feel → shooter).

This is a `Modules/Net` concern (ownership + replication) and **never** the transport. The BLE
**peripheral/central** split is a *radio mechanic only* — it confers no authority; once the link is
up it is a symmetric two-way datagram pipe. Do **not** introduce a "host" peer or a single global
authority: when a new game needs shared state, decide *per entity* which peer owns it. This
distributed-authority model is the foundation the reflex games' rollback netcode builds on.

Because play is **co-located and between two trusted people** (cheating here is like cheating at a
board game — trivial, but pointless and socially awkward in the same room), **anti-cheat is an
explicit non-goal**: each peer is trusted to report its own state, which is exactly what makes
owner-authority viable without a referee. What still matters is **consistency, not fairness** — a
dropped packet or a tie between two peers must resolve to the *same outcome on both screens*, so
contested state needs a simple **deterministic tie-break** (not cryptographic verification). And
since the players share a room, genuine disputes can be settled out-of-band, socially.

## Build and test

### Build configurations & capability macros (issue #65, Review #2 §5)

One ordinal ladder of configs (Unreal-style), each a strict **superset** of the one below, selected
with `-DLUR_CONFIG=`:

| `LUR_CONFIG` | Tooling | Asserts | Slow checks | Opt (`CMAKE_BUILD_TYPE`) |
|---|---|---|---|---|
| `Shipping` | ✗ | ✗ (quiet guards) | ✗ | `RelWithDebInfo` (-O2) |
| `Development` *(default)* | ✓ | ✓ (deafening) | ✗ | `RelWithDebInfo` (-O2 -g) |
| `Debugging` | ✓ | ✓ | ✓ | `Debug` (-O0 -g) |

`cmake/EngineFlags.cmake` derives four **capability macros** from the config —
`LUR_SHIPPING`, `LUR_INTERNAL` (dev-only tooling a player still gets: the dev console, tunable
CVars, the flight recorder), `LUR_ASSERTS` (drives `LUR_ASSERT`), `LUR_SLOW` (expensive
validation). Anything that *drives* the app rather than observing it is `LUR_AGENT` instead — see
the next section. **Gate code on the
capability, never on the config name** (`#if LUR_INTERNAL`, not `#if <config>`), so a future off-ladder
build (e.g. profiling = shipping + stats) never forces a call-site rewrite — same discipline as
Unreal's `WITH_EDITOR`/`DO_CHECK` and Casey's `HANDMADE_INTERNAL`/`HANDMADE_SLOW`.

Two rules that fall out of this and must hold: **(1)** asserts live in `Development`, not just
`Debugging` — so the *optimized* dev build (the overnight soak) still traps; `LUR_ASSERT` therefore
keys on `LUR_ASSERTS`, **decoupled from `NDEBUG`**. **(2)** `LUR_INTERNAL` code must **never** ship —
anything a player shouldn't reach (autoplayers, cheats, direct move injection) is `#if LUR_INTERNAL`,
compiled out of `Shipping`, not merely a runtime toggle. Default config is `Development`; a release
pipeline passes `-DLUR_CONFIG=Shipping`. Out-of-tree app targets (the Android/iOS mains) can't see the
engine tree's `add_compile_definitions`, so they re-apply the derived `LUR_*` cache vars to their own
target — see `Games/Chess/Android/app/src/main/cpp/CMakeLists.txt`. There is one such block **per
game per platform** and they are hand-copied, so a new `LUR_*` macro must be added to all of them;
`LUR_AGENT` was silently missing from one for a while. Unifying them is issue #198.

**Two per-app values are required compile definitions with NO default**, so a game that forgets
fails to build rather than inheriting another game's identity: `LUR_LOG_TAG` (the platform log tag)
and `LUR_BLE_SERVICE_UUID`. This is deliberate — the UUID once defaulted to chess's, so a forgetful
game inherited chess's *identity*, which fails silently as two phones that never see each other.

### `LUR_AGENT` — assistant-only instrumentation, never in a build someone plays

**Anything built so an assistant can drive or observe the app is `#if LUR_AGENT`, not
`#if LUR_INTERNAL`.** `LUR_AGENT` is a separate axis, **off in every config including
`Development`**, opted into with `-DLUR_AGENT=ON` and force-zeroed in `Shipping`.

Why it can't be `LUR_INTERNAL`: **the build a player plays IS `Development`** — that is where the dev
console and the tunable CVars live, and those exist *for the player*. So "dev-only" does not mean
"absent while playing", and gating harness scaffolding there put it in the player's hands. It bit us
concretely (playtest 2026-07-25): an agent hook that opened the console from a system property was
`LUR_INTERNAL`, a stale `setprop` was left on the device, and it then fought the player's own
two-finger gesture — the game felt remote-controlled.

What belongs behind it: **remote control of any kind** — system-property hooks, injected input,
forced state. What does *not*: desktop-only harnesses (`--aivs`, `--aidiag`, `--aibeginner`,
`--replay`) that a player never runs, and tooling meant for a human developer at the keyboard.

**Capture is not remote control (revised 2026-07-26, #156).** The RPS match flight recorder used to
be the headline example of `LUR_AGENT` — it writes files while someone is playing. That was the wrong
call in practice: capture only pays off when it is already running, so requiring a special build
meant the interesting match was always the one that wasn't recorded. It is now `LUR_INTERNAL`, **on
by default in dev builds**, with `rps.dev.flight_recorder` as a visible console checkbox. The line
that actually matters is *who is driving*: code that acts on the player's behalf without them asking
is `LUR_AGENT`; code that merely *observes* a session the player is driving can be `LUR_INTERNAL`,
provided the off switch is **discoverable in-product** (a console row, not a hidden system property)
and it still compiles out of `Shipping`. A hidden switch is what made the setprop hook dangerous, not
the fact that it had one.

Two rules for handing a build over: build it **without** `-DLUR_AGENT` so the code is *absent*
rather than idle, and **leave no device state behind** — clear properties and capture files, because
"inert by default" is not the same as "not there". Configuring with the flag on prints a CMake
warning naming the hazard.

**The handover happens WHEN THE USER ASKS FOR IT — not at the end of every task, issue or phase.**
The two rules above govern *how* to hand over, never *how often*. Leaving agent builds installed
between pieces of work is expected and fine: during development the phones are dev hardware, and
tearing the rig down after each issue costs a rebuild, a CI round-trip and two installs — which was
slowing real work down (said 2026-08-11). So keep the agent builds on the devices across a work
session, and do the close-out only on an explicit "we're done" / "give me the handover". The hazard
the rules exist for is a *player* holding a remote-controllable build; that risk arrives at handover
time, not at the end of an issue.

**The harness that lives behind it (RPS).** `Rps/AgentControl.h` defines one command grammar —
`<seq> <verb> [args]`, verbs `place queue stress corrupt droptx console gesture killown linked` — so an
assistant can drive both phones with no hands. It exists because the interesting failures aren't
reachable by tapping: a BLE link won't drop one frame on request, a deterministic sim won't diverge on
request, drag-to-place *snaps to the nearest valid square* so a placement can never land on an exact
occupied coordinate, and **iOS has no touch injection at all**. Channels differ because the idioms do —
Android polls the system property `debug.lur.agent.cmd`, iOS reads `Documents/agent.cmd` (which the dev
rig can already push into). Both are *level-triggered*, so **the sequence number must strictly
increase**; that's what makes re-reading idempotent. The iOS build comes from a **manual-dispatch-only**
CI job (`gh workflow run "macOS CI" -f agent=true`) — never an ordinary push, or the artifact would sit
next to the player one. The command PARSER is compiled always (it can't drive anything, and that keeps
its grammar in the host suite); the channel and every effect are `#if LUR_AGENT`.

**`linked` goes FIRST in any two-phone scenario (#170).** The app *opens in a solo AI match*, so a
`place` sent before the phone has crossed over lands in the **solo** sim — accepted, logged, and useless,
while the other phone waits forever for a camp that went elsewhere. `linked` takes the selector's manual
route, which is exempt from the `!HasMinerCamp(0)` gate the auto-switch carries; the auto-switch itself
refuses forever once *anything* has put a camp in the solo sim, so a single mistimed `place` used to cost
a relaunch. The flag latches, so sending `linked` before the link is up is fine. Both mains now log an
**error** when a `place`/`queue` is about to land in solo while a peer is ready — if you see that line,
the command did nothing useful.

Three traps worth knowing before using it. The RPS sim loop `continue`s for solo (**the mode the app opens
in**), for a decided match and for the post-match hold — so anything that must always run, agent poll and
diagnostics included, belongs at the **top** of the loop, not the bottom. `WorldHeight` is **240**:
team 0's opening camp is `(17, 16)`, team 1's mirror is `(17, 224)`, and a placement outside a team's
frontier is rejected **silently** — gold simply doesn't drop. And the two mains route local input from
*separate* copies of the same decision (`RouteLocalEvent` vs `placeLocal:`), which have already drifted
once — change one, change both.

**`LUR_CONFIG` is the single dial — it drives optimization too, not just the macros (issue #89).**
`EngineFlags.cmake` derives `CMAKE_BUILD_TYPE` from it (the *Opt* column above), so
`Development`/`Shipping` are **optimized** and only `Debugging` is `-O0`. Never hardcode
`-DCMAKE_BUILD_TYPE` in a build driver (that was the old bug: every driver forced `Debug`/`-O0`, so no
build was ever actually optimized — the phone *and* the desktop ran `-O0`). Drivers pass `LUR_CONFIG`
and, if they want a fast **compile** loop at the cost of speed (the host unit tests), `-DLUR_FAST=ON`
to pin `-O0` regardless. Coupling is via `CMAKE_BUILD_TYPE`, so it binds only single-config generators
(Ninja: host + Android); multi-config (Xcode/iOS, VS) picks opt per-build — keep the Xcode scheme in
sync with `LUR_CONFIG` by hand.

### Shared C++ core (do this for any core change)

```
powershell -ExecutionPolicy Bypass -File build.ps1
```

`build.ps1` configures (Ninja), builds, and runs the codec tests in one shot. The host toolchain is
**VS-free**, installed via winget — standalone CMake + Ninja + MinGW-w64 g++
(`BrechtSanders.WinLibs.POSIX.UCRT`); no Visual Studio. Production compilers are the Android NDK's
Clang and Apple's Clang, so the host compiler is only for the unit tests — a *different* compiler
here is a feature (extra portability coverage). The core is host-buildable on purpose — a fast,
always-green correctness loop before touching the apps.

### Desktop harnesses (the fast loop)

Each game has a `Desktop/` target that runs **two instances in one process** over loopback or a real
BLE radio — the correctness and balance loop, and the only place a change compiles fast enough to
iterate on. `scripts/desktop-build.ps1` (chess) and `scripts/rps-desktop-build.ps1` (RPS); during
playtest tuning build these rather than the full `build.ps1` suite. Kill the old exe before
relinking.

### Android app

Built from `Games/<Game>/Android` with Gradle (externalNativeBuild drives CMake). Needs Android
Studio + NDK. `minSdk` targets BLE + Vulkan support. Note `scripts/android-build.bat` is
**chess-only**; RPS builds via its own Gradle wrapper. An Android build doubles as the compile check
for every platform file the host suite never touches — see the trap below.

### iOS app

Built from `Games/<Game>/iOS` with Xcode, linking **MoltenVK** for the Vulkan-on-Metal layer.
**Requires a Mac** — iOS cannot be built on Windows. The current dev machine is Windows 11, so the
iOS half is Mac-only (local Mac or a cloud Mac). In practice the iPhone build comes from the free
macOS CI, not a local Mac: the `ios-ipa` job produces an unsigned device `.ipa` artifact.

**Getting the `.ipa` onto the iPhone — HEADLESS:** download the CI artifact to
**`dist/OnlyChess-unsigned.ipa`** (overwrite in place — `dist/` is gitignored), then let the rig
sign+install with zero interaction (zsign + the persisted free dev cert + the newest provisioning
profile re-dumped from the device):

```
gh run download <run-id> -n OnlyChess-unsigned-ipa -D dist
powershell -File Tools\DeviceRig\device-rig.ps1 -Action install -Peer ios
```

The only remaining ritual is the **weekly** profile renewal (free Apple accounts get 7-day
profiles): one Sideloadly run with the Apple ID, then the rig is headless again. See
`Tools/DeviceRig/README.md`.

### Reading device logs (WITHOUT burning tokens on noise)

Device logs are firehoses — a raw dump is ~95% render/system spam (`BLASTBufferQueue`,
`BufferQueueProducer`, `EPDG`, per-frame `NearbyMediums`) that costs thousands of tokens and says
nothing. **Never** run an unfiltered `adb logcat -d -t N`. Always filter to our tag/prefix at the
source, and pipe through `grep` (or `Select-String`) so only relevant lines reach the model.

**Android** — everything we emit uses that app's `LUR_LOG_TAG` (native `__android_log_print` and
Kotlin `Log.i(TAG=…)` alike), so the tag filter alone kills the noise. The tag is **`OnlyChess` for
chess and `OnlyRps` for RPS** — grepping the wrong one reads as a dead app:

```
adb -s <serial> logcat -d -s OnlyChess:*      # or OnlyRps:*
```

Two wireless transports (`<ip>:<port>` and `adb-<serial>._adb-tls-connect._tcp`) can point at the
*same* phone — confirm with `getprop ro.serialno`, then pin one via `ANDROID_SERIAL` for install so
Gradle's `installDebug` isn't ambiguous. When hunting the BLE handshake specifically, also drop the
per-frame chatter: `... -s OnlyChess:* | grep -vE 'hello: link not up|Chess core alive|Renderer'`.

**iOS** — no logcat; use `pymobiledevice3` and **bound the capture with `timeout`** (the stream
never ends on its own). Filter with **`-pn <process>`** and write with **`-o <file>`**, then grep the
file:

```
timeout 30 python -m pymobiledevice3 syslog live -pn OnlyChess -o ios.txt >/dev/null 2>&1
grep -aE "BLE|linked" ios.txt | sed 's/\x1b\[[0-9;]*m//g'
```

**Do NOT pipe the stream into `grep > file`.** It silently drops most lines — grep block-buffers and
`timeout` kills the pipeline before it flushes. That cost hours on 2026-08-01: 6–10 lines captured
where ~25 were due, so a working agent-control channel (and its startup banner) read as dead. Piping
to the *terminal* is fine; it's the redirect that loses data. Sanity-check any empty result against
the expected cadence — the RPS diagnostic line prints every 2 s, so a 30 s capture owes you ~15.
(`-m/--match` filters too; the older note that it doesn't work on iOS 26 applied to a different form.)

**BLE log vocabulary to grep for** (both platforms): `BLE up` / `powered on` (radio started),
`role decided` (tie-break ran), `central: linked` / `peripheral: central linked` (handshake done),
`central attempt -> we are peripheral` (self-correction), `disconnected` / `link lost`. iOS BLE
lines carry the prefix `<LogTag> BLE:` — `OnlyChess BLE:`, note the **space**, NOT `OnlyChess:`.
Absence of `role decided`/`linked` after `BLE up` on both phones = discovery/handshake is failing,
not a crash.

**A locked phone fakes a half-open link.** A locked Galaxy stalls the app loop, so the peer sees a
connected-but-silent link — indistinguishable from a real radio fault, and it will burn the hard
restart ladder. The tell is a wake-up burst of `hello RECV` sharing one millisecond. **Screenshot
the phone before believing any BLE verdict**; injected input cannot unlock it.

## Folder layout & file naming (Unreal-style)

- Per module: public headers in `Public/Lur/<Module>/`, private sources/headers in `Private/`.
  The game mirrors this: `Games/Chess/Core/Public/Chess/`, `Games/Chess/Core/Private/`.
- **All folders and files are PascalCase.** Headers use `.h` (not `.hpp`); files are PascalCase
  (`BitWriter.h`). The `Public/Lur/<Module>/` sub-path mirrors the namespace and keeps includes
  collision-proof: `#include "Lur/Serialization/BitWriter.h"`.
- Header-only modules are CMake `INTERFACE` libraries exposing `Public/`; compiled modules add
  `Public/` (PUBLIC) and `Private/` (PRIVATE) and expose a `lur::<name>` alias target.

## Naming convention

The rule: **our code is PascalCase; anything `snake_case`/lowercase is `std`/C/legacy.** Casing
alone tells you the origin (`Lur::Serialization::BitWriter` vs `std::vector`).

- **Namespaces:** PascalCase — `Lur::Serialization`, `Lur::Render`, `Chess`.
- **Types:** PascalCase. Interfaces get an `I` prefix (`IRenderer`, `ITransport`); enums get an
  `E` prefix (`EColor`, `EPieceType`, `EMoveFlag`).
- **Methods, free functions, locals, params, members:** PascalCase (`WriteBits`, `SideToMove`,
  `Count`). **No type-encoding prefixes** — no `b` for bools, no Hungarian. A bool is just `Ok`,
  `Ready`, `Connected`, with `Is…()` getters.
- **Getters** use `Get…()`/`Is…()` so they don't clash with same-named members.
- **Constants:** plain PascalCase, no `k` prefix (`MaxMoves`, `ProtocolVersion`, `NoSquare`).
- **Unscoped bitmask enum values** carry the concept prefix to avoid namespace collisions
  (`MoveFlagDoublePush`, `CastleWhiteKing`).
- Keep the per-platform Kotlin/ObjC++ shims as thin as possible — bytes and a GPU surface only. All
  real logic lives in C++.
- Fixed-capacity containers in the hot path (e.g. `Chess::MoveList`), not heap allocation.

## Critical gotchas

- **Move ordering IS the wire protocol.** `Chess::GenerateLegalMoves` must produce an identical
  order on both phones — the codec transmits only an index into that list. Changing the order is a
  breaking wire change: bump `Lur::Net::ProtocolVersion`.
- **Determinism is load-bearing.** `Modules/Sim` uses fixed-point (`Fixed`) and a fixed timestep
  (`TickClock`) so both devices simulate bit-for-bit identically — the precondition for the reflex
  games' future rollback netcode. Do not put floats into simulation state. (`Modules/Math` floats
  are for *rendering* only — never gameplay sim.)
- **Renderer targets the Vulkan portability subset** (so MoltenVK can run it on iOS): triangle-list
  meshes, vertex/fragment/compute shaders, standard formats only. No geometry/tessellation shaders,
  no wide lines, no triangle fans. None of those are needed for 2D or typical 3D models.
- **BLE is the only cross-platform link.** Do not reach for Bluetooth Classic/RFCOMM (iOS-locked) or
  NFC peer handover (unavailable to iOS apps). Wi-Fi Aware (iOS 26+) is a possible *future* faster
  transport but isn't production-ready cross-vendor yet — keep it behind the `ITransport` seam.
- **`build.ps1` green says nothing about the platform layer.** `Modules/*/Platform/*`,
  `Modules/Render/Private/VulkanBackend.cpp` and every `.mm` compile only in app builds. After
  touching any of them, compile-check Android (`./gradlew assembleDebug`) — and if the code is
  behind `#if LUR_AGENT`, with `-PlurAgent=ON`, because a check that doesn't compile the code under
  test is not a check.
- **The renderer INTERPOLATES, never PREDICTS.** Unit motion is `mix(Prev, Pos, alpha)` over the
  sim's Verlet integration. Render-side extrapolation was built and reverted twice: it overshoots
  at every velocity discontinuity. Input immediacy is won in the sim/net path, never in the view.
- **CodeViewer anchors are regex** — avoid parentheses in `anchor_start`/`anchor_end` strings.

## Planning & project state

Two homes, split by whether the content **moves**:

- **GitHub issues own everything living** — WORK (tasks, bugs, epics, roadmap, sequencing, priority,
  current state) *and* the design rationale that evolves with the code (specs, wire formats, the
  "why"). Start at the roadmap tracker issue **#12**. File or update an issue for any plan or
  decision that must stay current; don't record planning or current-state in `CLAUDE.md` (keep this
  file to durable, always-true guidance).

  **"Phase N" is ambiguous — always say which ladder.** Two unrelated numbering schemes are both
  live: the **roadmap** phases (the `phase-0 … phase-5` labels, tracker #12) and the **engine
  extraction** phases 0–7 (epic #39). Extraction Phase 2 is BLE unification; roadmap Phase 2 is
  "RTS on phones". They are not the same thing and never were.
- **`Docs/Journal/<YYYY-MM-DD>/` holds frozen snapshots** — every `.md`/`.html` under `Docs/` is a
  timestamped artifact capturing the thinking against the repo *at that date* (a design synthesis, a
  review, an execution plan, a decision sheet). A batch is **never amended in place**: new thinking
  lands as a new dated batch, or as an issue update — so a snapshot always reads as what was true when
  it was written. A batch may carry its own `README.md` manifest; there is no evergreen top-level
  index (that would rot). No file under `Docs/` is durable.

Precedence: a snapshot is history, so **the issue always wins** on anything current — sequencing,
priority, state, *or* design. Read a journal batch for rationale and context; act from the issues. And
re-verify every code claim in an old snapshot against HEAD before acting on it (paths and symbols
drift).

**Never mirror another issue's open/closed status into prose — point to a live query instead.** A
tracker/orientation issue that hand-lists "what's open" becomes a second copy of state with nothing
forcing it current, and it *will* rot (issue #80 did: on 2026-08-02 it still listed four
already-closed issues as open, and got quoted as current). "What's open for X" is a label query
(`gh issue list --state open --label <x>`), not a typed list. Durable per-game orientation (layout,
build/run, device ops, hard rules) belongs in a checked-in `README.md` beside the code — it versions
in diffs and can't drift from HEAD — **not** in a long-lived issue, which has no diff review and
never closes.

## Documentation (CodeViewer sessions)

Walkthroughs live in `CodeViewerSessions/`. The authoring recipe (the `base_dir` relocation and the
anchor rules) is the `codeviewer-session` skill — invoke it when writing one.

## Version control

**Never branch — commit straight to `master`.** This repo is a solo, trunk-based workflow: no
feature branches, no PRs. (This overrides the usual "branch first on the default branch" default.)
**Push only when the user asks** — commit freely as coherent, green changes land, but leave pushing
`master` to an explicit request. **Exception (pre-authorised):** you MAY push without asking when a
change legitimately needs verifying on the iPhone — the iOS `.ipa` is built by CI only on push, so a
push to get a fresh `.ipa` for a real device test is fine. Keep `master` green: build + test
(`build.ps1`, and the Android/iOS builds when they're touched) before committing.

## Working style

Favor explaining the C++/systems reasoning rather than only handing over code. When the user wants a
completed phase reviewed, they ask for a focused CodeViewer walkthrough (see Documentation).

### Where code lives — the three rules that decide it

- **Platform files hold API verbs; engine C++ holds decisions.** If a line *chooses* — retry or
  not? which role? what order? how long to wait? — it belongs in engine C++, not in Kotlin or
  ObjC++. The test: *could this be unit-tested on the host against a fake?* If yes and it isn't
  there, it is in the wrong place. This is not style: ~40% of the Kotlin BLE shim was decision
  logic no host test could reach, and that fraction is exactly where 299 lines of drift between the
  two games accumulated.
- **Build it in the game that needs it; promote to the engine on the SECOND consumer.** Speculative
  engine facilities with one user are dead code. The corollary is that the **promotion pass is
  load-bearing** — the `Modules`↛`Games` wall means nothing comes back on its own.
- **Delete dead code, even when it looks useful.** Git remembers. A tested module that nothing
  calls is worse than absent: it reads as covered.

### Evidence

**A success-shaped signal is not evidence.** This codebase's failures are overwhelmingly silent and
they point somewhere other than their cause — a MATCH END tally that printed while nothing
persisted; a test green on a count the bug itself produced; three logged radio restarts against a
transport with no restart implementation; "BLE is unstable" that was the lock screen. Before
believing a green result, confirm the check could actually have failed. When you cannot verify
something, say so plainly rather than reporting the nearest thing you did verify.

## Scripts

Common actions are wrapped as `.bat` entry points in `scripts/` so the workflow is consistent for
humans and future agents — prefer them over ad-hoc commands. See `scripts/README.md` for the list.

## Content pipeline: Tools sanitize → Cook builds

Two distinct, game-agnostic stages turn source content into what the app embeds (no runtime
image/font decoder ever ships):

- **Tools** (`Tools/`) **sanitize** raw content into cook-acceptable formats — e.g.
  `Tools/ImageConvert` normalises an arbitrary image into a 2-channel (RG8) or 4-channel
  (RGBA8) PNG. Hand-run while authoring; never linked into the app. (`Tools/` also holds
  debugging instruments like the BLE dev rig.)
- **Cook** (`Cook/`) turns that content into **built data** (embedded byte-array headers). It's
  a build-activated process (`build.ps1` runs `Cook/Cook.ps1`) — NOT a hand-run tool — and it's
  **reference-driven**: gameplay code declares each content dependency inline with a
  `// LUR_COOK <format> src=… out=…` marker (src paths are partial, relative to that game's
  `Content/`), and the cook derives *what* to cook and *how* (the format) from those markers.
  Incremental via a `// cook-source-hash:` stamp; cooked outputs are committed, so a clean build
  with unchanged content needs no cook tools. See `Cook/README.md`.

The font/shader cookers (`scripts/gen-font.ps1`, `scripts/gen-shaders.ps1`) predate this and are
not yet folded into the reference-driven driver. Keep the split clean: sanitizing belongs in
`Tools/`; content→data cooking belongs in `Cook/`.
