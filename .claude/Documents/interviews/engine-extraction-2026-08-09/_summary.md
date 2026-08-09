# Summary: Engine Extraction — cementing two games' lessons into LurMotorn

**Date:** 2026-08-09 · `master` @ 22468e7 · Status: **plan complete, not started**

## Problem

Two games are near feature-complete — Chess (turn-based, degenerate) and RocksPapersScissors
(real-time, rollback netcode). The third, a **co-op physics puzzle game**, is next, and the roadmap
already names this extraction as its prerequisite. Goal: game folders should hold **only unique
content and gameplay code**; zero platform-specific code; gameplay built by *using engine libraries
and extending small seams*. Highest-value target: **multi-axis exponential boilerplate** — anything
reimplemented per game × per platform.

The precondition the Handmade review set (§1: *"write game #2 first; the duplication IS the
specification"*) has been **met and passed**. Nothing in the extraction chain (#42/#43/#44/#45) has
been executed.

## Diagnosis — three diseases, three cures

| | Disease | Evidence | Cure |
|---|---|---|---|
| **A** | Platform layer: **6 drifted copies** | ~11,600 LOC of (game × platform); ~6,000–6,500 mechanically duplicated. `AndroidVulkanSurface.cpp` byte-identical; `BleShim.kt` **299 lines apart**; **12+ one-sided bug fixes in both directions**; the *same* policy question answered oppositely (`LUR_AGENT` in chess, `LUR_INTERNAL` in RPS) so RPS ships a rig-controllable BLE role override in every playable build | Deduplicate |
| **B** | Game-side: a **one-way ratchet** | The two games barely duplicate each other — different genres. Each engine facility exists in **exactly one** game and never comes back. ~1,900–2,300 LOC stranded (dev console ~690 in RPS while `Modules/DevGui` holds only leaf widgets; `SfxLibrary` in chess while RPS has no audio) | Promote, tiered by conflict risk |
| **C** | **Reverse leakage** — game concepts in the engine | `pApplicationName = "OnlyChess"` in the shared Vulkan backend *shipping inside RPS*; the engine's default BLE UUID **is chess's**; `Session::SendMove` a chess-only API RPS never calls; **4 of 9 `ProtocolVersion` bumps are RPS features** | Clean now — game #3 would inherit the wrong defaults |

## Key Decisions

| Decision | Choice | Rationale |
|---|---|---|
| **Entry point** | Engine owns entry, game owns tick, **granular per-step override** | ~90% of a phone main is OS ceremony (in `RpsMain.mm`'s 1,572 lines, ~150 are game decisions). Preserves what review §8 defended — the game owning its *frame* — while surrendering process startup, which was never game logic |
| **Override granularity** | Per entry-step, not all-or-nothing | RPS's iOS render-thread parking becomes an override of the *background* step, not a fork of the whole main. **Guard rail: if 2 of 3 games override the same step, the default is wrong — fix the default** |
| **Scope** | Dedup + leakage fully; promote only already-generic facilities | Compression of real duplication is what review §1 blesses; promoting from one sample is what it warns against |
| **Ratchet cure** | Promotion pass per milestone + `Docs/NewGame.md` | The only cures that catch semantic leakage. **Rejected: CI lint, engine-first rule** |
| **BLE** | Dedup + move the *drifted* logic to C++; leave API ceremony | `Modules/Transport` is the most hardened code in the repo — bound the rewrite to exactly what's causing bugs |
| **Coordinator** | Templated ring, plain coordinator | The ring needs the concrete type for `memcpy` + `static_assert(is_trivially_copyable)` — *the contract that makes rollback work*. Everything else compiles once |
| **Wire version** | Split engine / per-game | 4 of 9 engine versions exist for one game today |
| **Threading** | Engine owns topology; single-threaded is a config | Plumbing has already drifted between RPS's *own two mains*; the iOS 4 MB-pthread-stack lesson must be learned once |
| **Input** | Engine owns full pipeline; recognizers opt-in; **multi-touch** | `TouchEvent` is bypassed on both phones; RPS's dispatch skeleton is copy-pasted 3× *within RPS*; physics is a **two-hand manipulation game** |
| **`std::filesystem`** | Replace (~60 LOC), turn exceptions off | It is why exceptions are on and why the iOS floor is 13. The save path is open anyway |
| **Flight recorder** | Unified engine framework + 4-function game seam, **binary + dump tool** | *(User proposal — supersedes an earlier "don't merge")*. Layers become event **categories in one stream**, so a datagram can be seen interleaved with the tick that consumed it. Closes **#66** for every game |
| **Per-opponent persistence** | Engine owns mechanism, **game declares the schema** | *(User reframing)*. Chess: W/L/D. RPS: W/L. Physics: levels completed / gold stars. **The co-op game gets persistence despite having no win/loss concept** |
| **Dead code** | **Delete all of it**, even if it looks useful | User doctrine: build in the game, promote on the second consumer |
| **Build scaffolding** | Full `lur_add_game` template | A new game supplies name, package, UUID, icon — nothing else |

## Architecture

```
Modules/
  App/                          NEW — the entry-point + lifecycle layer
    Public/Lur/App/             IGameApp, Frame, Config, step-override table
    Platform/Android/           android_main            (written ONCE)
    Platform/Ios/               UIApplicationMain + VC   (written ONCE)
    Platform/Windows/           WinMain                  (written ONCE)
      -> owns: surface create, swapchain resize, log sink, UnblockStdio,
         #73 reattach heal, pause/resume, safe-area insets, save-dir discovery,
         agent-channel transport, thread topology (sim/render/mailbox)
  Transport/
    Private/BleLinkController   NEW — shared policy: send queue, retry/backoff,
                                watchdog, role escalation, reconnect scheduling
    Public/Lur/Transport/IBleRadio.h   NEW — dumb driver seam
    Platform/{Android,Ios,Windows}/    one copy each
    Tests/FakeBleRadio          NEW — makes the above host-testable
  Net/
    RollbackCoordinator         plain class, compiled once
    SnapshotRing<Sim>           templated only where the type is required
    ProtocolVersion             engine-only; games carry GameProtocolVersion
  Render/Platform/{Android,Ios,Windows}/   Vulkan surface seams, one copy each
  Input/                        multi-touch TouchEvent + recognizer library
  Core/Recorder                 unified: framing, version field, rotation,
                                replay driver, peer diff, engine-level events
  Save/                         per-opponent store, game-declared schema;
                                std::filesystem replaced
  DevGui/                       + the console assembly layer from RPS

Games/<Name>/
  CMakeLists.txt                lur_add_game(...) — the entire app shell
  Core/  View/  Content/        gameplay + content ONLY
```

### The `lur_add_game` shell

```cmake
lur_add_game(physics
    NAME     "OnlyPhysics"
    PACKAGE  com.lurmotorn.onlyphysics
    LOG_TAG  OnlyPhysics
    BLE_UUID 4C55524D-...-5068797300
    ICON     Content/Icon.svg
    SOURCES  Core/ View/)
# derives: android shell, ios shell, desktop target, LUR_* macros, CI matrix entry
```

Feasible because the iOS build **already generates its `.xcodeproj` from CMake** — there is no
hand-maintained Xcode project to fight.

## Implementation Order

**Rule throughout: `master` stays green — both games build *and run on both phones* after every
phase.** No phase lands as a big bang.

**Phase 0 — Sweep and clean** *(cheap, shrinks the surface before anything moves)*
Delete `Modules/Pairing`, `Hud/LinkStatusBar`, `Text/FontRegistry`, `Math::Quat` +
`Mat4::Perspective` + `LookAt`, `Net/ClockSync` (frees 2 wire slots). Resolve `Core/Hash.h` —
use `Fnv1a64` in the hashing path or delete it. Fix leakage: `"OnlyChess"` → app-supplied name,
BLE UUID default removed, `Session::SendMove`/`SetMoveHandler` deleted, stale seam comments.
*Files: `Modules/{Pairing,Hud,Text,Math,Net,Render,Transport}`*

**Phase 1 — Platform layer move (#42)**
`git mv` the platform backends into `Modules/{Transport,Render}/Platform/{Android,Ios}`. Dynamic JNI
`RegisterNatives` in `JNI_OnLoad`; drop `loadLibrary("onlychess")`; log tag and service UUID become
app-supplied parameters. Vulkan surface seams collapse 4 → 2 (**provably 0-diff**).
*Kills: ~2,000 duplicated lines. Lowest risk, highest certainty.*

**Phase 2 — BLE unification** ⚠️ *highest product risk*
`IBleRadio` + `BleLinkController` + `FakeBleRadio` host tests. Move send queue, retry/backoff,
watchdog, role escalation, reconnect scheduling out of Kotlin/ObjC++. **Reconcile the 12+ one-sided
fixes** — chess's advertise/scan backoff, RPS's #146 breaker and #182 restart, chess's #190 priority
queue — into one path, and resolve the `LUR_AGENT` vs `LUR_INTERNAL` role-override contradiction.
*Two-phone soak required before proceeding.*

**Phase 3 — `Modules/App`** *(the big one)*
Entry points per platform; the step-override table; thread topology as config; save-dir discovery;
`std::filesystem` → ~60 lines of `fopen`/`mkdir`/`rename`; `-fno-exceptions`; agent-channel
transport; input pipeline with multi-touch + tap/drag/pan/pinch recognizers.
*Absorbs: #73 reattach ×2, `UnblockStdio` ×2, pause/resume, safe area, ~30-atomic mailbox ×2.*

**Phase 4 — Netcode + wire**
`RollbackCoordinator` (plain) + `SnapshotRing<Sim>` (templated) lifted from `Rps/LockstepPeer`.
Rollback and lockstep as **modes**. Split `ProtocolVersion` / `GameProtocolVersion`. Point RPS at
the coordinator; evaluate chess on lockstep mode.
*Unblocks the physics game.*

**Phase 5 — Promotions**
Tier 1 (minus cuts) + accepted Tier 2. Unified recorder + **binary format and its dump tool
together** — the dump tool ships *with* the format change, since `--recdiff` and
`device-rig pullrec` are the primary desync instrument. Per-opponent persistence with game-declared
schema. Dev console assembly → `DevGui`. Selector plumbing. Mutable material tints in `Render`
(which retires the alpha-step LUT rather than promoting it).

**Phase 6 — Build scaffolding**
`lur_add_game`; multi-config generator expressions so `LUR_CONFIG` drives optimization on Xcode too;
one CI matrix; one parameterized desktop build script; `scripts/new-game.ps1`.

**Phase 7 — Docs + the ratchet**
`Docs/NewGame.md` (carrying an explicit *"the engine must not name a game"* rule), `Modules/*/README.md`,
`Games/Chess/README.md`, refresh the stale `Games/Chess/iOS/README.md`. Establish the
**promotion-pass ritual** per milestone.

## Expected Results

- **(game × platform) code: ~11,600 → near-zero in game folders.** One copy per platform in the
  engine; each game's shell becomes a `lur_add_game` block plus icon/plist data.
- **~2,200 LOC promoted** out of game folders into the engine.
- Chess gains, for free: a dev console, the #146 role-deadlock breaker, the #182 radio restart, an
  installed engine log sink (currently its `Lur::Log::*` output goes to a **discarded stdout** on
  both phones), multi-touch, and the unified recorder.
- RPS gains: chess's advertise/scan retry-with-backoff (it currently **never advertises if the first
  attempt fails**), `onDestroy → ble.stop()` (#194's fix, still missing), persist-on-background, and
  iOS touch-latency instrumentation.
- Game #3 starts with rollback, sim threading, multi-touch, recording+replay, per-opponent
  persistence, console, camera, broadphase grid and constraint projection already in the engine.
- `-fno-exceptions`; no unwind tables; possible iOS floor below 13.
- **#66 closed** (on-disk format version) for every game at once.

## Risks

| Risk | Mitigation |
|---|---|
| **BLE rewrite breaks the product's core** — most battle-hardened code, 4 hardening issues live there | Bounded scope (only drifted logic moves); `FakeBleRadio` host tests *before* the cutover; two-phone soak gates Phase 3 |
| **Engine `ProtocolVersion` resets to a low number** — a peer on an old build must not read it as "older" | `[engineVer][gameVer]` pair in Hello + hard refuse. **The first release after the split is a hard break for every existing install** — ship both phones together |
| **Binary recorder breaks the diff workflow** — `--recdiff`/`pullrec` are the primary desync instrument | Dump tool ships in the same change, not after |
| **Reconciling 12+ one-sided BLE fixes** — some are genuinely game-specific (chess's #190 priority queue exists because chess has a 1-byte move) | Treat each as a decision, not a merge; record why in the shared path |
| **`lur_add_game` is ambitious** — Gradle + Xcode templating is fiddly | iOS already generates its project from CMake; do Android first, iOS second, and keep the hand-written path working until the template is proven |
| **Phase 3 is large and touches everything** | Land it per-platform (Windows → Android → iOS), keeping the old main alive until its replacement runs on device |
| **Discipline-only ratchet cure** (CI lint and engine-first were rejected) | `NewGame.md` carries the "engine must not name a game" rule so the check lands in front of a human |
| ~~One-sample audio promotion~~ — **withdrawn.** User: *"both RPS and the physics game need audio eventually, and the majority of audio features will not be unique to any of the games."* The second consumer is known, not hypothetical | Not a risk. Promote the generic audio layer (bank, variation policy, triggering); games own only classification + clips — matching #82's existing per-piece disposition |

## YAGNI cuts applied

- **"Time ago" formatting** — stays in chess (10 LOC, one consumer).
- **Long-press recognizer** — not built (zero consumers). Tap/drag/pan/pinch only.
- *Kept despite one consumer, by explicit choice:* gradient/disc mesh builders, SFX variation picker.

## Explicitly excluded (do not re-propose)

Merging the recorders as *two files* (superseded — they merge as one stream, distinct categories);
alpha-step material LUT (fix mutable tints instead); localization (zero consumers);
continuous/parameterized audio (wait for game #3).
