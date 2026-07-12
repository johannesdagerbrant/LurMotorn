# CLAUDE.md

Guidance for Claude Code (and humans) working in the **LurMotorn** repo. Read this first.

## What this is

LurMotorn is a from-scratch engine for ultra-low-latency **local** multiplayer games. Chess
(`Games/Chess`) is the first game and the proving ground. Two phones — Android *or* iPhone — pair
over Bluetooth Low Energy and play locally, sending the smallest possible payload across the wire.

## Non-negotiable constraints

Do not violate these without explicit confirmation from the user:

- **No third-party libraries — with one sanctioned exception.** Only the unavoidable OS frameworks
  (Android SDK/NDK, Apple SDK) and a single GPU API: **Vulkan everywhere** — native on Android, via
  **MoltenVK** on iOS. MoltenVK is the *one* allowed third-party dependency, because no native GPU
  API spans both platforms. Otherwise: no ZXing/ML Kit, no networking or chess libraries, no model
  importers. If something else seems to need a library, hand-roll it or raise it with the user first.
- **No servers, ever.** Strictly local play. No backend, no internet relay — not even a seam for one.
- **Cross-platform Android <-> iPhone.** Every wire/transport/render choice must work *between* an
  iPhone and an Android phone. This is why the link is BLE and the renderer is a single Vulkan
  backend (MoltenVK on iOS).
- **Slim payload and low latency are the product.** Prefer bit-level encodings; never bloat the wire
  for convenience.

## Architecture

One shared, pure-C++ core compiles identically on host, Android (NDK), and iOS. Only three things
touch hardware and get per-OS backends behind a common C++ interface: **transport** (BLE),
**render** (Vulkan, via MoltenVK on iOS), and **input**.

Dependency rule, enforced by CMake: `Games/*` may depend on `Modules/*`; `Modules/*` must **never**
depend on `Games/*`. That wall is what keeps the engine reusable for future games.

```
Modules/Serialization  pure C++  slim-bytes codec (BitWriter/BitReader/Varint)
Modules/Sim            pure C++  deterministic fixed-point + fixed-timestep (gameplay)
Modules/Math           pure C++  vec/mat/quat for render + scene transforms (FLOAT, not sim)
Modules/Net            pure C++  session, clock-sync (rollback netcode later)
Modules/Transport      interface + BLE backend (Android JNI->Kotlin, iOS CoreBluetooth)
Modules/Pairing        interface + BLE discovery (no NFC)
Modules/Render         interface + single Vulkan backend (MoltenVK on iOS), 3D-capable
Modules/Input          per-platform touch glue
Games/Chess/Core       pure C++  rules + move codec (shared verbatim)
Games/Chess/Android    Android Studio project (Kotlin shim + C++ + Vulkan)
Games/Chess/iOS        Xcode project (Swift shim + C++ + Vulkan via MoltenVK)
```

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

### Android app

Built from `Games/Chess/Android` with Gradle (externalNativeBuild drives CMake). Needs Android
Studio + NDK. `minSdk` targets BLE + Vulkan support.

### iOS app

Built from `Games/Chess/iOS` with Xcode, linking **MoltenVK** for the Vulkan-on-Metal layer.
**Requires a Mac** — iOS cannot be built on Windows. The current dev machine is Windows 11, so the
iOS half is Mac-only (local Mac or a cloud Mac). In practice the iPhone build comes from the free
macOS CI, not a local Mac: the `ios-ipa` job produces an unsigned device `.ipa` artifact.

**Getting the `.ipa` onto the iPhone:** always download the CI artifact to
**`dist/OnlyChess-unsigned.ipa`** (overwrite it in place — `dist/` is gitignored), so the Sideloadly
flow always points at the same file:

```
gh run download <run-id> -n OnlyChess-unsigned-ipa -D dist
```

Then sideload with Sideloadly + a free Apple ID (see the `device-testing-ops` memory).

### Reading device logs (WITHOUT burning tokens on noise)

Device logs are firehoses — a raw dump is ~95% render/system spam (`BLASTBufferQueue`,
`BufferQueueProducer`, `EPDG`, per-frame `NearbyMediums`) that costs thousands of tokens and says
nothing. **Never** run an unfiltered `adb logcat -d -t N`. Always filter to our tag/prefix at the
source, and pipe through `grep` (or `Select-String`) so only relevant lines reach the model.

**Android** — everything we emit uses the log tag `OnlyChess` (native `__android_log_print` and
Kotlin `Log.i(TAG=…)` alike), so the tag filter alone kills the noise:

```
adb -s <serial> logcat -d -s OnlyChess:*
```

Two wireless transports (`<ip>:<port>` and `adb-<serial>._adb-tls-connect._tcp`) can point at the
*same* phone — confirm with `getprop ro.serialno`, then pin one via `ANDROID_SERIAL` for install so
Gradle's `installDebug` isn't ambiguous. When hunting the BLE handshake specifically, also drop the
per-frame chatter: `... -s OnlyChess:* | grep -vE 'hello: link not up|Chess core alive|Renderer'`.

**iOS** — no logcat; use `pymobiledevice3` with a process/message match, and **bound the capture with
`timeout`** (the stream never ends on its own):

```
timeout 15 python -m pymobiledevice3 syslog live -m "OnlyChess" | grep -i onlychess
```

**BLE log vocabulary to grep for** (both platforms): `BLE up` / `powered on` (radio started),
`role decided` (tie-break ran), `central: linked` / `peripheral: central linked` (handshake done),
`central attempt -> we are peripheral` (self-correction), `disconnected` / `link lost`. iOS BLE
lines carry the prefix `OnlyChess BLE:` (note the space — NOT `OnlyChess:`). Absence of `role
decided`/`linked` after `BLE up` on both phones = discovery/handshake is failing, not a crash.

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
- Keep the per-platform Kotlin/Swift shims as thin as possible — bytes and a GPU surface only. All
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
- **CodeViewer anchors are regex** — avoid parentheses in `anchor_start`/`anchor_end` strings.

## Documentation (CodeViewer sessions)

Completed phases are documented as **focused** CodeViewer walkthroughs kept in
`CodeViewerSessions/`, committed as living documentation. **One session per phase** — do not grow a
single giant session.

**Every session's `summary` (its overview / intro) must include a Lexicon** — a short glossary of
the relevant lingo and abbreviations used in that session (e.g. perft, bitboard, EP, FEN, LSB,
MoltenVK, RFCOMM), so a reader can follow the walkthrough without external lookups.

Recipe: the create tool writes `<slug>.codeviewer` into its `directory` with `base_dir: "."`. Author
it with `directory` = the repo root (so code paths validate during authoring), then move the file
into `CodeViewerSessions/` and patch `base_dir` from `"."` to `".."` so code paths still resolve to
the repo root. Use `symbol`/anchor ranges; avoid parentheses in anchors (regex).

**CodeViewer sessions are created on request, not as a mandatory pre-push gate.** The user asks for
a walkthrough when they want one; do not block pushing on authoring/reviewing a session, and do not
proactively create one unless asked. (Earlier the session was a hard pre-push gate; the user dropped
that — they'll request CodeViewer sessions when they want them.)

## Version control

**Never branch. Always commit and _push directly to `master`_.** This repo is a solo,
trunk-based workflow: no feature branches, no PRs. Commit each coherent, green change straight to
`master` and push it. (This overrides the usual "branch first on the default branch" default —
the user has explicitly chosen trunk-based here.) Keep `master` green: build + test (`build.ps1`,
and the Android/iOS builds when they're touched) before pushing.

## Working style

Favor explaining the C++/systems reasoning rather than only handing over code. When the user wants a
completed phase reviewed, they ask for a focused CodeViewer walkthrough (see Documentation).

## Current state

The shared C++ core is host-green via `build.ps1`: serialization codec, deterministic sim + math,
module interfaces, a **perft-verified** chess engine + move codec, the BLE transport contract, and
per-opponent save records (`Modules/Save`).

**Two phones play real chess over BLE, on hardware** — an iPhone and an Android phone discover,
pair, and play with no server: the move codec runs over the live link, the shared Vulkan renderer
draws the board on both (MoltenVK on iOS), and touch moves pieces. The BLE role handshake is
**in-band** (iOS can't advertise custom data): both advertise the service UUID, scan, and serve a
readable **device-id** characteristic; the central reads the peer's id and `DecideBleRole` settles
roles. A **persistent per-install device GUID** (`Modules/Save`) makes that role stable across
restarts, with deterministic fast reconnect + a net keepalive liveness timeout (#17, done).

Both apps build with no local Mac: Android via Gradle/NDK (Galaxy A14); iOS via the free macOS CI
(`.github/workflows/macos-ci.yml`), which uploads an unsigned device `.ipa` for sideloading.

**In progress:** identity-based colour + link-time record sync so a reconnected game resumes the
position and colours (#18 core landed; its net/view wiring + #19 next). Threefold-repetition (#7)
and magic bitboards (#1) are independent. See GitHub issues + memory for live status.

## Scripts

Common actions are wrapped as `.bat` entry points in `scripts/` so the workflow is consistent for
humans and future agents — prefer them over ad-hoc commands: `build.bat` (host core build + test),
`clean.bat`, `setup-android.bat` (one-time CLI-only SDK/NDK/JDK/Gradle install), `android-build.bat`,
`android-install.bat`, `logcat.bat`. See `scripts/README.md`.
