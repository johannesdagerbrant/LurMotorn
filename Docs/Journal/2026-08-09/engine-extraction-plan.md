# Engine extraction — cementing two games' lessons before game #3

Frozen snapshot, 2026-08-09, against `master` @ `22468e7`. **Live status lives in the issues**
(epic **#39** and its chain); this is history — re-verify every code claim against HEAD before
acting on it. Produced by a full inventory of both games, all sixteen engine modules, every journal
batch and the whole issue tracker, followed by a decision interview.

## Why now

Review #2 (`Docs/Journal/2026-07-17/lurmotorn-review-2-handmade-lens.md` §1) set the method
explicitly: *don't pre-abstract; write game #2, then compress what actually repeats* — Muratori's
semantic compression, *"the duplication IS the specification for the engine."* The master roadmap
sequenced the extraction behind game #2 for that reason, and the RPS shells were **copy-pasted on
purpose**: *"the copy is deliberate — it's the second consumer that earns #42 with evidence."*

Two games have now been shipping in parallel for ~3 weeks. **The evidence gate has been met and
passed, and nothing in the extraction chain (#42/#43/#44/#45) has been executed.** Meanwhile the
roadmap states the extraction is the **co-op physics puzzle game's stated prerequisite**.

## What the inventory found — three diseases, not one

The instinct going in was "there is duplicated boilerplate; remove it." The measurement says there
are **three different problems that need three different cures**, and conflating them was the main
risk to any plan.

### A. The platform layer — six drifted copies

**~11,600 lines of (game × platform) code across 2 games × 3 platforms, of which ~6,000–6,500 is
mechanically duplicated.**

| Item | Copies | Normalized drift |
|---|---|---|
| Vulkan surface seam | 4 | **0 lines** — byte-identical |
| BLE radio state machine | 6 backends × 2 games ≈ 3,240 LOC | ~65% identical; the other 35% is **drift, not design** |
| `BleShim.kt` | 2 | **299 lines apart** |
| `AndroidBleTransport.cpp` | 2 | 221 |
| `IosBleTransport.mm` | 2 | 131 |
| Renderer bring-up ritual | 6 | same 5 steps everywhere |
| Elapsed-ns loop timer | 8+ | identical shape, different clock source |
| `LUR_*` macro re-application per app target | 6 | `LUR_AGENT` was silently **missing** from RPS/iOS for a while |
| `UnblockStdio()` MoltenVK guard | 2 | body **and** its 16-line postmortem comment, verbatim |
| #73 reattach-for-activation heal | 2 | ~120 lines of window-server archaeology, twice |
| AndroidManifest permission block | 2 | **0 lines** differ |

This is already costing correctness. **12+ bug fixes exist in one game and not the other, in both
directions:**

- Chess has advertise/scan **retry with backoff**; RPS does not — RPS *silently never advertises*
  if the first attempt fails.
- RPS has the #146 role-deadlock breaker, the #146 bad-device-id guard, and the #182/#163 hard
  radio restart; chess has none of them.
- Chess has the #190 priority send queue and #194's `onDestroy → ble.stop()`; RPS lacks both, so RPS
  leaks advertiser/scanner registrations across launches — the exact `ALREADY_STARTED` bug #194 fixed.
- RPS installs an engine log sink first thing; **chess never does, so `Lur::Log::*` output from
  inside the engine goes to a discarded stdout on both of chess's phone platforms** — repeating
  verbatim a bug RPS's own comment says cost a real diagnosis on 2026-07-30.
- **The same policy question is answered oppositely**: the BLE role override is `#if LUR_AGENT` in
  chess (with an explicit #196 rationale that forced state must not ship) and `#if LUR_INTERNAL` in
  RPS — so RPS ships a rig-controllable radio override in **every build a player runs**. One of
  these is wrong.

And ~40–45% of `BleShim.kt` is **decision-making logic**, not transcription: a hand-rolled
ordered-delivery send queue with token timeouts, discovery watchdogs, role escalation, reconnect
scheduling, and a `gatt.javaClass.getMethod("refresh")` private-API reflection hack. That is netcode
living in Kotlin where **no host test can reach it** — which is precisely why it drifted. The
shared-first doctrine's own test applies: *"could this logic be unit-tested on the host against a
fake? Then it must live where the host can build it."*

### B. The game side — a one-way ratchet

This one reframes the problem. **The two games barely duplicate each other's C++** — they are
genuinely different genres and share very little logic. The real finding:

> Each engine facility exists in exactly **one** game, so the module wall (`Modules` may not depend
> on `Games`) has quietly become a **one-way ratchet**: features get built in whichever game needed
> them first and never come back.

**~1,900–2,300 lines of engine material are stranded in game folders**, including:

| Facility | Stranded in | LOC |
|---|---|---|
| Dev console + CVar browser | RPS `GameView.cpp:1931-2620` | ~690 |
| Rollback coordinator + prediction | RPS `Net` | ~600 |
| Desync recovery + lost-frame repair | RPS `Net` | ~230 |
| `SimRunner` (sim thread, fixed timestep) | RPS `Runtime` | 190 (**~170 generic**) |
| Audio triggering + variation policy | **Chess** `View` | 164 |
| Agent remote-control parser | RPS `Core` | 152 (*"zero RPS in the parser"*) |
| CVar replication at a stamped tick | RPS `Net` | ~130 |
| Rollback correction smoother | RPS `View` | ~110 (*"the piece that transfers to the physics game"*) |
| Deterministic CSR spatial grid | RPS `Core` | ~90 |
| GUI sub-layer ordering | RPS `View` | ~90 |
| Nearest-feasible-point projection | RPS `Runtime` | ~60 |
| `CameraScroll` | RPS `View` | 53 |
| SPSC snapshot mailbox | RPS `Runtime` | ~35 |

`Modules/DevGui` exists and holds **only the leaf widgets** (479 LOC) — the assembly and interaction
layer never came back. `Modules/Net/ClockSync.h` being a 30-line stub while a full rollback netcode
ships next door is the sharpest symptom: **the module that should hold it is unfinished, so the game
folder grew the real thing.**

Note the deflation on the scary raw numbers: "14,800 lines of RPS View" is really 3,211 lines of
code + 11,584 of cooked glyph bytes; "4,700 lines of netcode in a game folder" is 2,012 lines of
netcode + 2,708 lines of tests *for* it.

### C. Reverse leakage — game concepts inside the engine

The mirror image, and the one that will bite game #3 by handing it wrong defaults:

| Sev | Location | Evidence |
|---|---|---|
| High | `Render/.../VulkanBackend.cpp:630` | `App.pApplicationName = "OnlyChess";` — **the shared backend names the RPS app "OnlyChess"** |
| High | `Transport/.../BleProtocol.h:61` | the engine's **default BLE service UUID is chess's** |
| High | `Net/.../Session.h` | `SendMove`/`SetMoveHandler` — a chess-only "a bare 1-byte datagram is always a move" API. RPS calls it zero times |
| High | `Net/.../Session.h:52-70` | `ProtocolVersion = 9` — **v6/v7/v8/v9 are all RPS gameplay-wire changes.** A chess-only change can no longer reuse a version number |
| Med | `Render/.../Renderer.h:52-66` | `MaterialDesc::{Gamma,Outline,InkLo,InkHi}` exists for chess's piece art |
| Med | `DevGui/.../DevTheme.h:7` | the dev palette is tuned against **RPS's team cyan** |
| Struct | `Hud/CMakeLists.txt` | `lur_hud` links `lur_net` — presentation coupled to the net session type |

Plus dead weight compiled into every build: `Modules/Pairing` (39 LOC, **zero consumers
repo-wide**), `Hud/LinkStatusBar`, `Text/FontRegistry`, `Math`'s entire 3D half
(`Quat`/`Perspective`/`LookAt`), `Net/ClockSync`. And Hud (801), Render (1,988) and Platform (223)
have **no tests at all**.

One more: **`Modules/Platform` is not a platform layer.** It is a Win32 window class used by exactly
two files, with no file, time, thread, or DPI abstraction, and it is bypassed entirely on both
shipping platforms.

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| **Entry point** | Engine owns entry, game owns tick, with **granular per-step override** | ~90% of a phone main is OS ceremony — in `RpsMain.mm`'s 1,572 lines, ~150 are game decisions. Preserves what review §8 actually defended (the game owning its **frame**) while surrendering process startup, which was never game logic |
| **Override granularity** | Per entry-step, not all-or-nothing | RPS's iOS render-thread parking becomes an override of the *background* step, not a fork of the whole main. **Guard rail: if 2 of 3 games override the same step, the default is wrong — fix the default** |
| **Scope** | Dedup + leakage fully; promote only already-generic facilities | Compression of real duplication is blessed by review §1; promoting from one sample is what it warns against |
| **Ratchet cure** | Promotion pass per milestone + `Docs/NewGame.md`. **Rejected:** CI lint, engine-first rule | The two chosen catch semantic leakage a lint cannot see |
| **BLE** | Dedup **and** move the *drifted* logic to shared C++; leave genuine API ceremony | `Modules/Transport` is the most hardened code in the repo (#83/#146/#163/#182) — bound the rewrite to exactly what is causing bugs |
| **Coordinator** | Templated ring, plain coordinator | The ring needs the concrete type for `memcpy` + `static_assert(is_trivially_copyable)` — *the contract that makes rollback work*. The bulk never touches `Sim` and compiles once |
| **Wire version** | Split engine `ProtocolVersion` / per-game `GameProtocolVersion` | 4 of 9 engine versions exist for one game today |
| **Threading** | Engine owns the topology; single-threaded is a config value | Plumbing (~30 atomics) has already drifted between RPS's *own two mains*; the iOS **4 MB pthread stack** lesson (`std::thread`'s 512 KB → `SIGBUS`) must be learned once |
| **Input** | Engine owns the full pipeline; recognizers opt-in; **multi-touch** | `TouchEvent` is bypassed on both phones; RPS's dispatch skeleton is copy-pasted **3× within RPS**; the physics game is a **two-hand manipulation game** |
| **`std::filesystem`** | Replace (~60 LOC of `fopen`/`mkdir`/`rename`), turn exceptions off | It is why exceptions are on and why the iOS floor is 13 (review §3.1). The save path is open anyway during the App work |
| **Flight recorder** | One engine framework + a 4-function game seam. **Binary + a text dump tool** | Layers become event **categories in one stream** — a datagram can be seen interleaved with the tick that consumed it. **Closes #66** for every game at once |
| **Per-opponent persistence** | Engine owns the mechanism; **the game declares the schema** | Chess: W/L/D. RPS: W/L. Physics: levels completed / gold stars. **The co-op game gets persistence despite having no win/loss concept** |
| **Audio** | Promote the generic layer (bank, variation policy, trigger path, device seam); games own classification + clips | Both RPS and the physics game will need audio, and most audio features are not game-unique. Matches #82's per-piece disposition |
| **Dead code** | **Delete all of it, even if it looks useful** | Standing rule adopted this session — see below |

### The governing rule adopted this session

> *"Get rid of all dead code. If anything looks useful get rid of it anyway. If some game needs it
> later we build it for that game first. If more games need it we consider if it deserves to get
> promoted to an engine feature."*

**Build in the game, promote on the second consumer, delete anything speculative.** This is coherent
with rejecting the engine-first rule — and it makes the **promotion pass load-bearing**, because it
deliberately creates game-first facilities and the pass is the only thing that brings them back.

## Target architecture

```
Modules/
  App/                          NEW — entry point + lifecycle
    Public/Lur/App/             IGameApp, Frame, Config, step-override table
    Platform/{Android,Ios,Windows}/   each entry written ONCE
      -> surface create, swapchain resize, log sink, UnblockStdio,
         #73 reattach heal, pause/resume, safe-area insets, save-dir discovery,
         agent-channel transport, thread topology (sim/render/mailbox)
  Transport/
    Private/BleLinkController   NEW — send queue, retry/backoff, watchdog,
                                role escalation, reconnect scheduling
    Public/.../IBleRadio.h      NEW — dumb driver seam
    Platform/{Android,Ios,Windows}/   one copy each
    Tests/FakeBleRadio          NEW — makes the policy host-testable
  Net/  RollbackCoordinator (plain) + SnapshotRing<Sim> (templated)
        engine ProtocolVersion; games carry GameProtocolVersion
  Render/Platform/{Android,Ios,Windows}/   Vulkan surface seams, one copy each
  Input/    multi-touch TouchEvent + tap/drag/pan/pinch recognizers
  Core/     unified Recorder: framing, version field, rotation, replay driver,
            peer diff, automatic engine-level events (datagrams, link, CVars)
  Save/     per-opponent store with game-declared schema; filesystem replaced
  Audio/    bank + variation policy + trigger path + per-platform device seam
  DevGui/   + the console assembly layer lifted from RPS

Games/<Name>/
  CMakeLists.txt    lur_add_game(...) — the entire app shell
  Core/ View/ Content/   gameplay + content ONLY
```

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

## Phases

`master` stays green — **both games build and run on both phones after every phase.** No big bang.

| # | Phase | Contents | Issues |
|---|---|---|---|
| **0** | Sweep & clean | Delete `Pairing`, `LinkStatusBar`, `FontRegistry`, `Math` 3D half, `ClockSync`. Resolve `Core/Hash.h`. Fix leakage: `"OnlyChess"`, default UUID, `SendMove`, stale seam comments | **#200**, #47, #44 |
| **1** | Platform layer move | `git mv` backends into `Modules/{Transport,Render,Audio}/Platform/*`; dynamic JNI `RegisterNatives`; log tag + service UUID as parameters; Vulkan seams 4→2 | #42 |
| **2** | BLE unification ⚠️ | `IBleRadio` + `BleLinkController` + `FakeBleRadio`; move send queue/retry/watchdog/role escalation/reconnect out of Kotlin+ObjC++; **reconcile the 12+ one-sided fixes**; resolve the `LUR_AGENT`/`LUR_INTERNAL` contradiction | **#197** |
| **3** | `Modules/App` | Entry points; step-override table; thread topology as config; save-dir discovery; `std::filesystem` → ~60 LOC; `-fno-exceptions`; agent channel; input pipeline + multi-touch + recognizers | #43 |
| **4** | Netcode + wire | `RollbackCoordinator` + `SnapshotRing<Sim>`; rollback/lockstep as modes; wire-version split; RPS onto the coordinator; chess evaluated on lockstep | #8, #44 |
| **5** | Promotions | Tier 1 + accepted Tier 2; unified recorder **with its dump tool in the same change**; per-opponent persistence; console → `DevGui`; selector plumbing; mutable material tints (retiring the alpha-step LUT) | **#201**, #82, #66 |
| **6** | Build scaffolding | `lur_add_game`; multi-config generator expressions so `LUR_CONFIG` drives optimization on Xcode; one CI matrix; one desktop build script; `scripts/new-game.ps1` | **#198**, tail of #89 |
| **7** | Docs + ratchet | `Docs/NewGame.md` (carrying *"the engine must not name a game"*), `Modules/*/README.md`, `Games/Chess/README.md`, refresh stale `Chess/iOS/README.md`; establish the promotion-pass ritual | #45 |

## Expected results

- **(game × platform) code drops from ~11,600 to near-zero inside game folders**; one copy per
  platform in the engine, each game's shell reduced to a `lur_add_game` block plus icon/plist data.
- **~2,200 LOC promoted** out of game folders into the engine.
- **Chess gains for free:** a dev console, the #146 role-deadlock breaker, the #182 radio restart,
  an installed engine log sink, multi-touch, the unified recorder.
- **RPS gains:** advertise/scan retry with backoff, `onDestroy → ble.stop()` (#194), persist-on-
  background, iOS touch-latency instrumentation.
- **Game #3 starts with** rollback, sim threading, multi-touch, recording + replay, per-opponent
  persistence, dev console, camera, a deterministic broadphase grid and constraint projection
  already in the engine — leaving its one genuinely new risk, *authoring a stable fixed-point
  constraint solver*, isolated exactly as intended.
- `-fno-exceptions`, no unwind tables, a possible iOS floor below 13. **#66 closed for every game.**

## Risks

| Risk | Mitigation |
|---|---|
| **Phase 2 touches the most battle-hardened code in the repo** | Bounded scope; `FakeBleRadio` host tests **before** cutover; two-phone soak gates Phase 3 |
| **Engine `ProtocolVersion` resets to a low number** — an old peer must not read it as "older" | `[engineVer][gameVer]` pair in Hello + hard refuse. **The first release after Phase 4 is a hard break for every existing install** — ship both phones together |
| **Binary recorder breaks the diff workflow** — `--recdiff` / `pullrec` are the primary desync instrument | The dump tool ships in the same change, not after |
| **Reconciling 12+ one-sided BLE fixes** — some are genuinely game-specific (chess's #190 priority queue exists because chess has a 1-byte move) | Treat each as a decision, not a merge; record the reasoning in the shared path |
| **`lur_add_game` templating is fiddly** | Android first, iOS second; keep the hand-written path alive until the template is proven |
| **Phase 3 is large** | Land per-platform (Windows → Android → iOS), old main alive until its replacement runs on device |
| **Discipline-only ratchet cure** | `NewGame.md` carries the "engine must not name a game" rule, putting the check in front of a human |

## Explicitly excluded — do not re-propose

- **Merging the recorders as two files** — superseded: they merge as one stream with distinct
  event categories.
- **Alpha-step material LUT** — a workaround for immutable materials, hand-rolled at 5 sites in one
  file. Fix mutable tints instead.
- **Localization** — zero consumers in either game; all strings are inline English literals.
- **Continuous / parameterized audio** — shape unknown until the physics game exists.
- **"Time ago" formatting** and a **long-press recognizer** — cut by the YAGNI pass (one and zero
  consumers respectively).

## Tracker reconciliation

Reviewing the open tracker against these directives found seven issues that had gone stale, plus
three that were already done. Recorded here because *why* something was closed is the part that
rots first.

**Closed as superseded by the directives**

- **#15 "Spec: BLE wire format & on-disk save record"** — three decisions invalidate it: the wire
  version splits in two; its framing rule (`length == 1 → move`) is a **chess assumption presented
  as an engine invariant** (only chess has a 1-byte move, and `SendMove` is being deleted); and its
  on-disk section is replaced by game-declared persistence schemas + the recorder's version field.
  Content moves to a checked-in `Modules/Net/README.md` in Phase 7 — it was a spec living in a
  never-closing issue, the exact anti-pattern CLAUDE.md warns about.
- **#16 "Transport & robustness follow-ups"** — 3 of 4 items dead (item 3 fixed by #40; item 2's
  files deleted by #42/#197; item 4 absorbed by #197). The survivor, iOS `VK_ERROR_DEVICE_LOST`
  non-recovery, is re-filed as **#199**.

**Absorbed — work still happens, inside a phase**

- **#14** (Android teardown leaks the renderer), **#73** (iOS relaunch black screen), **#199** (iOS
  device-lost) are **one root cause seen from three platforms: nobody owns the window/device
  lifecycle.** All three land in Phase 3. Note #73's reattach heal *already exists in both iOS
  mains* (~51 + ~73 LOC), so it may be fixed-but-unclosed — verify on device.
- **#74** (chess record > 1 datagram can't resync) — solved as a side effect of Phase 4; chess
  inherits chunked resync from the shared coordinator.
- **#49** split four ways and **retitled** to its only surviving bullet (`DrawGlyphs` silent drop).
  The Kotlin API-33 migration moved to #197 (that code is being rewritten — migrating first is
  wasted, after is moot); renderer resource-lifetime moved to Phase 5; and `NO_RESPONSE` writes are
  **likely already done** per the 2026-08-04 journal — verify before picking up.

**Notes added, not closed**

- **#9 (glTF)** — Phase 0 deletes `Quat`/`Perspective`/`LookAt`, so this now carries a prerequisite:
  re-add the 3D math *with tests* when it wakes. Recorded so nobody later reads the deletion as 3D
  being abandoned.
- **#59–#64 (L2CAP CoC)** — stays parked, but its design was drawn against today's transport shape
  and needs rebasing onto `IBleRadio`/`BleLinkController`. A stale design is worse than none.
- **#81 (RTS audio)** — sequenced behind #82; implementing it first would write a second variation
  policy that the promotion then has to reconcile. The audio *device seam* moves in Phase 1
  specifically to stop it being copied a 3rd and 4th time.

**Closed as already complete** (verified against HEAD, not inferred from commit messages)

- **#7** threefold repetition — `RepetitionKey` + `PosKeys` history + `RepetitionCount() >= 3`.
- **#67** capture trays — derived by replaying the record, nothing persisted, `Flip` honoured.
- **#78** chess audio — all four `EMoveSound` events with the documented precedence, 3-variant
  no-repeat picking for frequent events, single-clip alerts for rare ones.

## Where the per-phase detail lives

The table above is sequence and shape only. **Work items, hazards and acceptance criteria live in
the issues**, per the repo's rule that issues own everything living — a task list frozen in a
journal snapshot would rot the day implementation starts.

| Phase | Detail |
|---|---|
| 0 | **#200** (+ #47, #44) |
| 1 | #42 |
| 2 | **#197** |
| 3 | #43 |
| 4 | #8 (+ #44) |
| 5 | **#201** (+ #82, #66) |
| 6 | **#198** |
| 7 | #45 |

One hazard found while writing those up, recorded here because it is easy to hit and hard to
diagnose: **deleting `EMsgType::ClockPing`/`ClockPong` in Phase 0 renumbers every later slot** — a
silent wire break with no version guard, since Phase 0 does not touch versioning. It is deferred to
Phase 4, where the engine `ProtocolVersion` resets anyway and the break is already accounted for.
Deleting `ClockSync.h` itself is safe at any time; only the enum *values* carry the hazard.

## Provenance

Interview and per-decision records: `.claude/Documents/interviews/engine-extraction-2026-08-09/`.
Full four-part inventory (per-platform shims, engine modules, game-side logic, issues + journals):
`.claude/Documents/research/engine-extraction-inventory.md`.
