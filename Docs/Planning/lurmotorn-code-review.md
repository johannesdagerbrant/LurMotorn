# LurMotorn — Full Code Review

*Reviewed at master (latest: `#38 fix: adopt a peer on reconnect too...`), July 2026.*
*Scope: every module, the chess game (Core/View/Android/iOS), build system, tests, CI. ~8k lines of hand-written code (~21k including cooked font/art headers).*
*Lens: the stated goals — a reusable P2P engine where a game folder holds only gameplay C++ + content, the engine holds everything generic and platform-specific, chess never leaks into the engine, new games feel intuitive and fun to build, and everything is maximally lightweight ("squeeze as much game out of as few bytes as possible"). Amended lens (your follow-up): **shared-first** — bugs have proven platform-specific, so maximize shared code; games talk only to clean engine interfaces (GUI, logging, networking, rendering); each platform implements only the irreducibly platform-specific sliver, kept as small as possible.*

---

## 1. Executive summary

This is an unusually disciplined codebase for its age. The slim-bytes philosophy is real and enforced (a chess move genuinely costs 0–8 *bits*), the pure-C++/host-testable core is real, the dependency wall (`Games/* → Modules/*`, never the reverse) holds in CMake, and the comments are the best I've seen in a hobby engine — they explain *why*, cite issues, and document wire invariants. The chess core is clean, perft-tested, and correctly treats move ordering as the wire protocol.

The distance between the repo and your stated goal is concentrated in **one seam**: the platform layer. Your suspicion is correct — there is a lot of Android/iOS code inside `Games/Chess/` that is 100% game-agnostic engine machinery. But the precise diagnosis matters: it isn't *chess logic written platform-specifically* (there's none of that); it's *engine backends and app scaffolding parked in the game's app folders*. Roughly **1,600 lines** of the chess game is engine code wearing a chess badge. A second game today would copy-paste all of it.

Your shared-first instinct is not just a preference — **this review's own defect log validates it empirically**. Every defect found lives in unshared platform code: the P0 threading race is Android-only (iOS is safe *by accident*); the historical long-read offset bug (#17), the GATT-133 leak discipline, and the scan-during-connect stall were all Android-side battles; the keepalive/`ResetLink` need is iOS-side; the deprecated APIs are Kotlin-side. The shared C++ core produced zero defects beyond minor hardening. And the codebase's *best* subsystem (rendering: one 1,086-line shared backend, 94 lines of per-OS seam) versus its *worst* (BLE: the same link state machine implemented twice — 500 lines of Kotlin ∥ 427 of ObjC++) is practically a controlled experiment with a clear result. §3.5 turns this into a doctrine, with a per-subsystem scorecard and the concrete BLE restructure.

**Top findings, in priority order:**

1. **P0 — Android threading race.** BLE callbacks mutate the session and game state on Binder threads while the engine thread renders and ticks. Violates `ITransport`'s own documented contract. (§5.1)
2. **P1 — Extract the platform layer out of `Games/Chess/` — then unify it.** Moving the files into engine platform modules is step one (§3.1); step two is collapsing the twice-written BLE state machine into one shared C++ `BleLinkController` over dumb per-OS radio drivers (§3.5).
3. **P1 — De-duplicate the app bootstrap.** ~70 lines of session/persistence/hijack wiring are near-verbatim identical in `AndroidMain.cpp` and `AppMain.mm`. That wiring is the seed of the engine's missing `GameHost`. (§3.2)
4. **P1 — Remove chess concepts from `Modules/Net`.** `EMsgType::Move/Resign/DrawOffer` and `Session::SendMove`'s hardcoded 1-byte assumption are chess-shaped engine API. (§3.3)
5. **P2 — Define the game contract (`IGame`).** The pieces already exist (`ISaveState`, deterministic replay, `Session` hooks); naming the contract is what makes game #2 intuitive. (§4)
6. **P2 — Delete or implement `Modules/Pairing`.** `IPairing` is referenced by nothing; the flow it documents (peer list + 6-digit confirm) doesn't exist. Docs drift in CLAUDE.md/README matches it. (§3.4)

None of these are foundation cracks. They're the difference between "a chess app with an engine inside it" and "an engine that shipped chess first" — and the migration is mostly *moving* code, not rewriting it.

---

## 2. What the codebase gets right (keep doing this)

Grounded, not flattery:

- **The slim-bytes chain is genuinely elegant.** `BitsForIndex` → `EncodeMove` (index into the deterministic legal list) → `Session::SendMove` (bare 1-byte datagram, no type tag, disambiguated by length) → resync as `EncodeGame` (~1 byte/ply). A forced move costs **zero bits**. This is the product thesis expressed in code, end to end.
- **Determinism is treated as load-bearing**, not aspirational: move *ordering* documented as protocol in `Board.h` and `MoveGen.cpp`, `ProtocolVersion` bumps recorded with reasons (v2/v3/v4 changelog in `Session.h`), `Lur::Sim::Fixed` + `TickClock` already in place for the rollback future, float use explicitly fenced to rendering.
- **The record design is smart.** `ChessRecord` is player-agnostic (WinsLower/WinsHigher anchored to GUID order) so both phones store byte-identical blobs and `MergeIfNewer` is order-independent and monotonic. Colour = GUID order + match parity, decoupled from the radio role. The "impossible tie" defensive branch in `MergeIfNewer` even picks a deterministic winner.
- **`Modules/Save` is a model module.** Tiny surface (`Store`, `ISaveState`, `SyncManager`), atomic temp-file-rename writes, reversible `%XX` key escaping so keys can't escape the directory, deliberate Net-decoupling to avoid a dependency cycle, host-tested.
- **The Vulkan backend is minimal in the right way.** Push-constants-only (no descriptor churn for uniforms), one descriptor set per material texture, FIFO present (right for a board game's battery), portability-subset discipline for MoltenVK, dynamic viewport/scissor, embedded SPIR-V. 1,086 lines for a working cross-platform renderer is lean. It is also the proof-of-concept for the shared-first doctrine: one shared implementation, 41+53 lines of platform seam — §3.5 asks every other subsystem to look like this.
- **The BLE role tie-break is well-engineered.** In-band device-id read (forced by iOS's advertising limits), `DecideBleRole` as the single shared source of truth called from Kotlin *via JNI* rather than reimplemented, persistent GUID for stable roles across restarts, cached-peer one-sided reconnect to kill the mutual-connect collision, keepalive-driven `ResetLink()` for the iOS-peripheral silent-death case, GATT long-read offset handling. The hard-won 133/collision lessons are encoded in comments *and* watchdogs.
- **Comments are documentation.** Nearly every public header teaches the design. The GATT UUIDs spell "LURMOTORN Transp/Datagr/Nonce" in ASCII — memorable *and* documented. CLAUDE.md's log-vocabulary section ("grep for `role decided`…") is operational gold.
- **Test culture:** perft for move-gen, codec round-trips, session handshake/keepalive over `LoopbackTransport`, save/merge monotonicity, font layout — all host-runnable in one `build.ps1`, on a *different* compiler than production (correctly framed as portability coverage).

---

## 3. Architecture: the engine/game seam

### 3.1 The confirmed suspicion: engine code living in `Games/Chess/`

Inventory of what sits in the game today but contains **zero chess**:

| File (in `Games/Chess/...`) | Lines | What it actually is | Where it belongs |
|---|---:|---|---|
| `Android/.../kotlin/.../BleShim.kt` | 500 | The entire Android BLE radio: advertise/scan/GATT server+client, role tie-break, reconnect watchdogs, MTU | Engine platform layer |
| `iOS/Sources/IosBleTransport.mm` | 427 | The entire CoreBluetooth radio (mirror of the above) | Engine platform layer |
| `Android/.../cpp/AndroidBleTransport.cpp` | 190 | JNI bridge: `ITransport` ⇄ BleShim, device-id JNI, role JNI | Engine platform layer |
| `Android/.../cpp/AndroidVulkanSurface.cpp` | 41 | The Android half of `Lur::Render::Vk::PlatformSurface` | Engine platform layer |
| `iOS/Sources/IosVulkanSurface.mm` | 53 | The iOS/MoltenVK half of the same seam | Engine platform layer |
| `Android/.../OnlyChessActivity.kt` | 46 | NativeActivity subclass + BLE runtime permissions | Engine app-shell template |
| `AndroidMain.cpp` / `AppMain.mm` | 157 / 209 | App loop + renderer/transport creation + **the game wiring** (§3.2) | Loop/wiring → engine `GameHost`; only the last ~15 lines are chess |

That's ~1,620 lines — more than the chess rules themselves (Core is ~800). The tell is in the module docs: `Modules/Render`'s CMake correctly says "the per-OS surface seam is implemented by each app," and `Ble.h` says `CreateBleTransport` is "implemented separately in Games/Chess/Android and Games/Chess/iOS." The *interfaces* were placed in the engine (correct), but the *implementations* were parked in the first game. CLAUDE.md's architecture table even describes Transport as "interface + BLE backend" as if the backend were in the module — the docs already believe the code lives where it should.

Consequences beyond duplication: the Kotlin package is `com.lurmotorn.onlychess` and the JNI symbols are `Java_com_lurmotorn_onlychess_BleShim_*` — so the JNI bridge is **name-coupled to the chess app's package**. A second game with its own package can't even reuse the bridge without editing symbol names. The log tag `"OnlyChess"` is likewise baked into engine-grade code.

**Recommended target shape** (names to taste):

```
Modules/
  Transport/
    Public/Lur/Transport/...            (as today)
    Platform/Android/                   BleShim.kt + LurBleJni.cpp   ← from Games/Chess
    Platform/Ios/BleTransport.mm                                     ← from Games/Chess
  Render/
    Private/Vulkan/VulkanBackend.cpp    (already here — the model to copy)
    Platform/Android/VulkanSurface.cpp                               ← from Games/Chess
    Platform/Ios/VulkanSurface.mm                                    ← from Games/Chess
  App/                                  NEW: the game-agnostic bootstrap (§3.2)
    Public/Lur/App/GameHost.h
    Platform/Android/  (android_main loop, NativeActivity/permissions template)
    Platform/Ios/      (UIKit shim, CADisplayLink loop template)
Games/Chess/
  Core/  View/  Content/                (unchanged — this part is already right)
  Android/  iOS/                        thin per-game shells: manifest/plist, icon,
                                        package name, ~30 lines instantiating GameHost
```

Two mechanical notes for the move:
- **JNI naming:** either registre the natives dynamically (`RegisterNatives` in `JNI_OnLoad`) so the Kotlin package stops mattering, or move `BleShim` to a fixed engine package (`com.lurmotorn.engine`) that every game app includes as a module. The dynamic-registration route is the cleaner one — it also removes the `System.loadLibrary("onlychess")` name coupling (pass the lib name in, or standardize it).
- **Log tag:** make it a parameter the app supplies once (it already is for `Session::SetLogger`; extend the pattern to the Kotlin/ObjC sides) so the `OnlyChess` string leaves engine code.

This migration is almost entirely `git mv` + include/package fixes. Nothing about the BLE logic itself needs to change to become engine code — it already is engine code. (§3.5 then goes further: after the move, the BLE *logic* should exist once, in shared C++, with the Kotlin/ObjC files reduced to radio drivers.)

### 3.2 The duplicated bootstrap → the missing `GameHost`

`AndroidMain.cpp` and `AppMain.mm` contain the same ~70-line block, comment-for-comment: create `Store` → `LoadOrCreateDeviceId` → `SyncManager` → `SetOnMatchEnd`(persist+log) → create transport → wire loggers → `SendRecord` lambda (hijack-guarded `Sync` send) → `OnLive` lambda → `SetReadyHandler`/`SetResyncHandler`/`SetHandler(Sync)` → `Session.Start`. Plus both duplicate persist-on-background and the per-frame `Session.Tick()` + `View.Render()` cadence.

This block is the **engine's session/persistence choreography**, not chess. Duplicated, it's already drifted in small ways (heap vs stack ownership, capture styles) and every future game would fork it again. Extract it:

```cpp
// Modules/App/Public/Lur/App/GameHost.h — sketch
namespace Lur::App {
// Owns the cross-cutting plumbing every LurMotorn game needs:
// Store + DeviceId + SyncManager + Session, wired with the adopt/sync flow.
// The game supplies its ISaveState and reacts to lifecycle hooks.
class GameHost {
public:
    struct Config {
        std::string SaveDir;                       // platform supplies (filesDir / App Support)
        Lur::Transport::ITransport* Transport;     // platform supplies
        std::function<void(const char*)> Log;      // platform supplies
    };
    void Start(const Config&, Lur::Save::ISaveState& State);
    void Tick();                                    // per frame: session tick (+ future clock sync)
    void OnBackground();                            // persist
    // Hooks the game implements/uses:
    //   OnPeerAdopted(peerGuid) -> bool   (the hijack rule lives with the game/view)
    //   Session(), Sync(), Store(), DeviceId() accessors for the game's wiring.
};
}
```

After this, `AndroidMain.cpp` shrinks to: platform loop + `GameHost` + `BoardView` glue (~50 lines), and `AppMain.mm` likewise. The hijack rule itself (`OnPeerLinked`) correctly stays game-side — which game to adopt is a *game* policy — but the plumbing that invokes it becomes engine.

### 3.3 Chess inside the engine (the reverse leak)

The rule "the engine never implements anything chess-specific" has three violations, all in `Modules/Net`:

1. **`EMsgType::Move`, `Resign`, `DrawOffer`** (`Session.h`). `Resign` and `DrawOffer` are board-game concepts the engine has opinions about; nothing consumes them yet (no handlers registered anywhere). `Move` is borderline-generic but pairs with (2).
2. **`Session::SendMove` hardcodes a one-byte payload**: `const uint8_t Byte = Size >= 1 ? Data[0] : 0; Transport->Send(&Byte, 1);` — it silently **truncates anything larger to its first byte**. That's the chess move-index size baked into the engine API, and a landmine for the first game whose per-turn input exceeds 8 bits: it would compile, run, and corrupt the wire. The length-disambiguation trick (1-byte datagram = move, framed ≥2 bytes) is clever and worth keeping, but it should be expressed generically: e.g. `Session::SendBare(payload, size)` with the invariant "bare datagrams are anything shorter than 2 bytes… " — or better, reserve N *low* type values as "bare sizes" and let the game register what a bare datagram means. Minimal fix now: assert/`Logf` on `Size > 1` instead of silently truncating, and rename to reflect "1-byte fast path."
3. **Comments name chess** throughout `Session.h`/`Transport.h` ("chess: a legal-move index, ~4-6 bits", "lets the same chess code run over BLE"). Cosmetic, but for a general engine, recast chess as *an example* ("e.g. a turn-based game's move index") so the API reads as the general case.

Also engine-adjacent: `EMsgType::Sync`'s dispatch works, but `MaxMsgTypes = 8`'s comment says "covers EMsgType 0..6" while `Sync = 7` — stale comment on a wire-adjacent constant; worth fixing precisely because your comments are usually authoritative.

Everything else honours the wall admirably — `Dropdown` is genuinely generic (the chess selector maps onto label/sublabel/dot/ring), `MaterialDesc`'s ink-band/gamma knobs are engine-generic with chess as a documented *user*, `LinkStatusBar` reads `ELinkState` not chess.

### 3.4 Dead and drifted pieces

- **`Modules/Pairing` is unused.** Nothing implements or calls `IPairing`; the flow it documents (peer list, user pick, 6-digit confirmation code) doesn't exist — pairing today is the automatic tie-break inside the BLE backends. Either delete the module (my lean: yes, until a real "choose among N peers" feature arrives — YAGNI, and the file misleads a new-game author into wiring against a phantom) or rewrite its doc to describe the *actual* auto-pairing and keep it as the future seam. README/CLAUDE.md list it as a real layer; both should match reality.
- **`EBleRole` parameter is vestigial.** Both apps pass a hardcoded role (`Central` on Android, `Peripheral` on iOS) and both backends **ignore it** (`CreateBleTransport(EBleRole /*Role*/)`) — the real role is decided in-band. The parameter documents a design that no longer exists; drop it (`CreateBleTransport()`), and delete the misleading hardcoded arguments from both mains.
- **Docs drift:** CLAUDE.md's table says Transport/Render/Input backends live in the modules; Transport's don't (§3.1). `Modules/Input` is one struct — no per-platform glue module actually exists (the apps translate events inline). Fine, but the docs oversell it.
- `.claude/Documents/` + `CodeViewerSessions/` are a nice practice (design interviews, walkthroughs) — just note they'll drift the same way; the glyph-renderer research doc describes decisions now implemented, worth a "status: shipped" stamp.

### 3.5 Shared-first: one implementation per subsystem, thin platform drivers

*(Folded in at your request: bugs have proven platform-specific; maximize shared code; games interact only with engine interfaces for GUI, logging, networking, rendering; each platform extends only what is irreducibly platform-specific.)*

The defect audit backs the instinct (see the executive summary), so state it as an enforceable rule rather than a preference:

> **A platform file may contain API verbs and event forwarding — never decisions.** If a line *chooses* (retry or not? which role? what order? how long to wait?), it is engine C++. The test: could this logic be unit-tested on the host against a fake? Then it must live where the host can build it.

**Scorecard today:**

| Subsystem | Shared implementation | Per-platform code | Verdict |
|---|---|---|---|
| Rendering | `VulkanBackend.cpp` (1,086) + shaders | 41 (Android) + 53 (iOS) surface-seam lines | ✅ **the model** |
| Persistence | all of `Modules/Save` (std::filesystem) | a directory string | ✅ |
| GUI widgets / text | `Hud` + `Text`, pure C++ over `IRenderer` | — | ✅ |
| Game logic & view | `Chess/Core` + `Chess/View`, shared verbatim | — | ✅ |
| Link *policy* (roles, reconnect, watchdogs) | only `DecideBleRole` (via JNI) | **written twice**: `BleShim.kt` (500) ∥ `IosBleTransport.mm` (427) | ❌ worst offender |
| Logging | — | four ad-hoc seams across nine files: two `LOGI` macro sets, `Vk::PlatformLog`, per-object `SetLogger` lambdas; tag hardcoded | ❌ scattered |
| Input | `Lur::Input::TouchEvent` — **defined, instantiated nowhere** (verified by grep) | inline translation in both mains straight to `OnTap(x,y)` | ⚠️ interface bypassed |
| App loop & wiring | — (→ `GameHost`, §3.2) | duplicated mains | ❌ (§3.2) |

**The BLE unification — the big win.** Today the *same* state machine — advertise+scan, connect-out, read peer id, role verdict, the drop-and-serve peripheral dance, the cached-peer one-sided reconnect, the connect watchdog, the rescan backoff, keepalive-driven reset — exists once in Kotlin and once in Objective-C++. Only `DecideBleRole` was correctly shared (called from Kotlin via JNI — the right instinct that should have been the pattern for the whole machine). The two copies already show drift: timers are `Handler.postDelayed` on Android and ad-hoc on iOS; the iOS file's header still says "NOT yet run on hardware" while commit #38 was an on-device reconnect fix — stale in exactly the way parallel implementations always go stale. And every hard-won lesson (long-read offset serving, GATT-133 close discipline, scan-during-connect stall) had to be discovered and encoded *per platform*.

Restructure:

```cpp
// Modules/Transport — sketch
// The irreducible platform sliver: dumb radio verbs + event forwarding. NO policy.
class IBleRadio {
public:
    // Commands (engine thread → radio):
    virtual void StartAdvertising() = 0;   virtual void StopAdvertising() = 0;
    virtual void StartScanning()    = 0;   virtual void StopScanning()    = 0;
    virtual void Connect(PeerRef)   = 0;   virtual void Disconnect()      = 0;
    virtual void ReadPeerDeviceId() = 0;   virtual void EnableNotifications() = 0;
    virtual void SendDatagram(const uint8_t*, size_t) = 0;
    // Events, queued onto the engine thread (the §5.1 inbox IS this queue):
    //   PeerFound(ref) · Connected · Disconnected · DeviceIdRead(bytes)
    //   NotifyReady · DatagramReceived(bytes) · Failure(code)
};

// Shared C++, ticked by the engine (real time, per §5.2). Owns ALL policy:
// role tie-break, defer-to-peripheral dance, cached-role fast reconnect,
// connect watchdog + rescan backoff, keepalive ResetLink. Exposes ITransport.
class BleLinkController final : public ITransport { /* ~350 lines, host-tested */ };
```

What this buys, concretely:

- The collision/reconnect scenarios that today need **two physical phones** to reproduce become host unit tests against a `FakeBleRadio`: both sides discover simultaneously → assert exactly one canonical link forms; kill the central silently → assert reset fires at the timeout; cached-role restart → assert no mutual connect. The engine's hardest-won bug class gains a regression suite.
- Watchdogs move from `Handler.postDelayed` into controller ticks — one timing model, and it lands on the real-time `Tick(ElapsedNs)` fix (§5.2) for free.
- LOC math: 1,117 lines of parallel policy become ~350 shared + ~150 Kotlin driver + ~180 ObjC driver — less total code, and the 350 lines that matter gain tests.

What legitimately stays per-platform (design `IBleRadio` around this union): the permission flow (Activity-bound), Android's GATT-server long-read `offset` serving, CoreBluetooth manager lifecycle quirks, API-level guards, and thread-marshalling into the event queue. Everything else crosses the seam.

**Logging: one seam.** Replace the four ad-hoc paths with a single engine-wide sink: `Lur::Log::Init(Sink, Tag)` called once by the platform shell; `Lur::Log::Info/Error(fmt, …)` everywhere else (existing `SetLogger` lambdas forward into it during migration). ~30 shared lines + a 5-line sink per platform — and the `OnlyChess` tag becomes an app-supplied string, closing that coupling from §3.1 for every module at once.

**Input: use the interface that already exists.** `Lur::Input::TouchEvent` (phase, pixel coords, `TimeNs`, `PointerId`) is documented as the platforms' normalization target — and is instantiated nowhere; both mains translate native events inline and call `OnTap(x, y)`, discarding phase, timestamp, and pointer id. Route platforms → a `TouchEvent` queue → the game consumes one stream. This isn't gold-plating: `TimeNs` is the exact field the rollback plan needs for input-to-tick alignment, and `PointerId` matters the moment any game wants a second finger. While at it, give the platform surface a metrics struct (size, scale, **safe-area insets**) — `BoardView` currently hacks the notch with a documented "stopgap" proportional inset, which is a platform capability leaking into game code, exactly what this doctrine forbids.

**Enforcement.** Adopt a standing budget: platform-specific code across *both* OSes stays under ~600 lines total (the post-unification reality), and every new platform file must answer, in its header comment, *which engine interface it implements and why the logic can't live behind it* — the same discipline your module headers already model everywhere else.

Sequencing vs §3.1: do the mechanical move first (cheap, zero logic risk, unblocks `GameHost`), then this unification as its own change with on-device verification — but build the P0 threading fix (§5.1) as the controller's event queue from day one so none of it is throwaway.

---

## 4. The game contract — making game #2 intuitive and fun

Today, to build game #2 you would: copy two app projects (~1.6k lines), rename JNI symbols, re-derive the session wiring, and discover by reading `BoardView.cpp` which powers exist. After §3's moves, the remaining gap is that **the engine never names what a game *is***. The parts all exist:

- state that persists + syncs → `Lur::Save::ISaveState` ✔ (already game-agnostic)
- deterministic evolution from an input stream → chess does it by convention (`ApplyMove` + `RebuildBoard`)
- input encode/decode → `MoveCodec` by convention
- presentation + touch → `BoardView` by convention

Name the convention:

```cpp
// Modules/Sim (or a new Modules/Game) — sketch
namespace Lur::Game {
class IGame : public Lur::Save::ISaveState {
public:
    // Deterministic core (both peers run this identically):
    virtual void Reset() = 0;
    virtual bool ApplyInput(const uint8_t* Data, std::size_t Size) = 0; // decode + step; false = protocol error
    virtual void EncodeInput(/*game-defined input*/ ...) = 0;           // via BitWriter, game-typed in practice
    virtual uint64_t StateHash() const = 0;                             // desync detection (cheap: even FNV of a canonical serialize)
    // Presentation (screen-side, non-deterministic):
    virtual void Render(Lur::Render::IRenderer*, float W, float H) = 0;
    virtual void OnTap(float X, float Y, float W, float H) = 0;
};
}
```

Then `GameHost` (§3.2) drives *any* `IGame`: bare datagrams → `ApplyInput`, `Sync` → `MergeIfNewer`, background → `Persist`, and — later — `StateHash` exchanged piggyback on keepalives gives every game desync detection for free. Chess becomes the reference implementation (`ChessMatchState` + `BoardView` already fit the shape almost exactly), and the README's promise becomes checkable: *a game folder = an `IGame` + content.*

Two contract decisions to make deliberately now, because chess made them implicitly:

1. **Turn model.** Chess's offline-move trick ("you can only ever be one move ahead") and the resync-by-history-exchange both lean on strict turn alternation. A general `IGame` shouldn't inherit that silently — make resync semantics part of the contract (e.g. `MergeIfNewer` is the only requirement; turn-based games get the history trick as a provided helper).
2. **Authority.** CLAUDE.md's distributed-authority essay (per-entity ownership, no host) is the most forward-looking design writing in the repo — but no code expresses it yet. When `Modules/Net` grows ownership/replication, keep it as a *layer a game opts into*, with lockstep (`IGame` above) remaining the cheap blessed path; don't let the shooter's needs complicate chess's.

Finally, the discoverability half of "fun": after the moves, a `Docs/NewGame.md` that walks `IGame` + `GameHost` + the module map would be ~1 page. Right now that knowledge lives only in `BoardView.cpp`'s comments.

---

## 5. Correctness findings

### 5.1 P0 — Android delivers BLE data on the wrong thread

`ITransport::SetReceiver` documents: *"Invoked on the engine thread."* `Session` documents: *"Not thread-safe: drive it from one thread."* The Android backend violates both:

- `BluetoothGattServerCallback.onCharacteristicWriteRequest` / `BluetoothGattCallback.onCharacteristicChanged` fire on **Binder threads**, and call `nativeOnReceived` → `g_Transport.ReceiverFn` → `Session::OnDatagram` **directly** (`BleShim.kt:345,487`; `AndroidBleTransport.cpp: nativeOnReceived`).
- Meanwhile `android_main`'s native thread runs `Session.Tick()` and `View.Render()` concurrently.

So a peer's move mutates `Session` state (`Ready`, `PeerGuid`, `SinceRecvTicks`), then `ChessMatchState` (`Rec.Moves.push_back`, `Position`) and `BoardView` (`Selected`, `ItemsDirty`) on a Binder thread **while the engine thread reads the same objects to render** — a genuine data race (a `std::vector` reallocation mid-`Render` can crash; torn `Board` reads draw garbage; `Connected`/`Ready` are plain bools). Even `nativeOnConnected/Disconnected` flip non-atomic state cross-thread. It will mostly *appear* to work because payloads are tiny and rare — which is exactly the kind of race that ships.

iOS is safe *by accident*: both managers use `queue:nil` → main queue, and `CADisplayLink` renders on the main run loop, so everything is single-threaded there. That asymmetry (and the fact that the contract is documented but unenforced) is the trap.

**Fix (engine-side, so every backend gets it for free):** add a tiny inbox to `Modules/Transport` — a mutex-guarded (or SPSC ring) `std::vector<Datagram>` the radio thread pushes into and the engine thread drains at the top of `Session::Tick()`. E.g. a `ThreadedTransportAdapter : ITransport` that wraps any raw backend, or bake the queue into the Android backend's `nativeOnReceived`/`nativeOnConnected` and drain via a `Pump()` the host calls. Given moves are ≤64 bytes, a fixed ring of 32×64B slots does it with zero steady-state allocation. Also make `Connected` atomic (or carry connect/disconnect through the same queue so ordering with data is preserved — the queue route is strictly better). One more reason to prefer the queue: it is exactly the event inbox the shared `BleLinkController` (§3.5) needs — build the P0 fix as that controller's front door and none of it is throwaway.

### 5.2 P2 — `Session` timing is frame-rate–denominated

`HelloResendTicks/KeepaliveTicks/LinkTimeoutTicks` assume `Tick()` ≈ 60 Hz. On a 120 Hz phone (both your targets can render >60) the keepalive halves to ~0.5 s and the link timeout to ~2.5 s; if the app throttles (background/battery) the timeout stretches arbitrarily — and iOS `CADisplayLink` pauses in background, so a backgrounded iPhone stops sending keepalives entirely and gets reset by the peer. Pass elapsed nanoseconds into `Tick(uint64_t ElapsedNs)` (the platforms already have timestamps; `TickClock` shows the pattern) and denominate the constants in ms. Small change, removes a whole class of "works on my phone" drift.

### 5.3 P2 — `Varint::ReadVarUint` shift UB on hostile input

`Value |= Group << Shift;` with `Shift += GroupBits` unbounded: a crafted/corrupt stream of continuation bits pushes `Shift` past 63 → undefined behaviour on the shift (the `!R.IsOk()` break only triggers at buffer end, which can be >16 groups away). One line: `if (Shift >= 64) { /* poison: treat as corrupt */ break; }` (and ideally surface a failure the way `BitReader::IsOk` does). You decode peer-supplied bytes with this family; the reader's own sticky-Ok design shows you already care about hostile input — extend it here.

### 5.4 P3 — small sharp edges

- **`EncodeMove` silently encodes index 0 when the move isn't in `Legal`** (`MoveCodec.cpp`): a caller bug becomes "the peer sees your first legal move" — maximally confusing to debug. Assert (or encode an out-of-range index so the peer *rejects* it).
- **`MoveList::Add` has no bounds check.** 256 genuinely bounds legal chess, but `AddCastling` et al. write through `Count++` unchecked — one future off-by-one becomes stack smash. A debug-only assert costs nothing in release.
- **`TickClock::Advance` has no spiral-of-death clamp**: a long pause (debugger, background) returns thousands of ticks. Chess ticks trivially today; clamp before a reflex game inherits it (`Ticks = min(Ticks, MaxCatchup)`).
- **`Fixed` division by zero / overflow**: `operator/` will hardware-trap on `O.Raw == 0`; `FromInt` shifts into the sign bit for |v| ≥ 32768 silently. Fine while unused; document ranges or saturate before rollback games build on it. (Also: no `Sqrt/Sin/Lerp` yet — expected, just noting the module is a seed, not a toolkit.)
- **`LoopbackTransport::Send` is synchronously re-entrant**: a receiver that replies inside the callback recurses A→B→A. `NetTests` currently avoids it; a comment ("do not send from within a receiver in tests") or a tiny pending-queue would prevent a future test heisenbug.
- **`Session::OnHello` identical-GUID stall**: `if (LocalGuid == PeerGuid) return;` waits forever with no signal. Can only happen via a cloned save dir, but it's silent — log it at least; you already log every other handshake edge.
- **Kotlin BLE API deprecations**: `characteristic.value =` + `writeCharacteristic(ch)` and `notifyCharacteristicChanged(dev, ch, false)` are deprecated from API 33 in favour of the byte-array overloads. Works today; migrating also removes a real (if unlikely at 1:1 rates) race where `.value` is a shared mutable field.
- **`BoardView::ApplyRemoteMove` turn-check ordering** is subtly load-bearing: it decodes against *our* current legal list, then rejects if it's our turn. Out-of-order/duplicate delivery (BLE notifications are ordered, so today: fine) would decode against the wrong list. When transports diversify (your CoC/Wi-Fi plans), add a ply counter to bare moves or sequence at the session level — worth a `// NOTE` now so the assumption is visible.

### 5.5 Not bugs, verified fine (so you don't re-audit)

`Store::ListKeys` `%XX` decode bounds are correct; long-read offset handling in `BleShim` is correct (and the comment explains the iOS interop failure it fixed); `ChessRecord::Read` validates the move stream *before* mutating (strong exception-ish guarantee); `MergeIfNewer` tie-break is symmetric-safe; `Board::MakeMove` castling-rights mask table covers capture-on-rook-square via `ClearMask[To]`; `DecodeGame` rejects both reader underrun and illegal indices; halfmove 100 (claimable 50-move) in `Result()` vs 150 (automatic 75-move) in `DetectResult()` is FIDE-correct and the *deterministic* one is the networked path — nice catch in the design.

---

## 6. Performance & the bytes budget

**Wire:** already near-optimal for chess. A live move = 1 datagram byte; the true cost is GATT overhead, and there you have one lever left: `send()` uses `WRITE_TYPE_DEFAULT` (acknowledged) for the central→peripheral direction — `WRITE_TYPE_NO_RESPONSE` drops a round-trip per move and is the standard choice for a datagram channel with an app-level liveness net (you have keepalives). The MTU request of 247 is correct and future-proof. `EncodeGame`'s fixed 16-bit ply count could be a varint (a fresh-game sync currently spends 16 bits saying "zero"), but that's 1–2 bytes on a rare resync — take it only if it's free during another codec touch, and remember it's a `ProtocolVersion` bump.

**Simulation:** copy-make legality filtering (copy `Board` ≈ 128 B + re-`MakeMove` per pseudo-move, and `Result()` regenerates all moves) is the documented "correctness first" choice and the right one — worst case is ~10⁴ simple ops per applied move, invisible at chess cadence even on the A14's G80. The header already marks magic bitboards as "the spot to optimize later"; agreed, and *later* means "when a game runs movegen inside a per-tick loop," not before. One real double-work nit: `ChessMatchState::ApplyMove` → `DetectResult()` generates legal moves, and the very next `BoardView::Render` with a selection generates them again — a one-position `MoveList` cache on the state would halve it, but measure first; it's likely nanoseconds that don't justify the cache-invalidation surface.

**Serialization:** `WriteBits/ReadBits` loop bit-by-bit. For ≤8-bit chess payloads, irrelevant; for `EncodeGame` on a 300-ply resync it's ~3k iterations — still nothing. Flag it only because `Modules/Serialization` will sit under a future rollback input stream at 120 Hz; a word-wise fast path is a contained, test-protected optimization when that day comes.

**Render:** the design choices are the *lightweight-smart* ones — push constants for all per-draw data (zero uniform-buffer management), lazily bound pipelines, per-frame text arena with cursor reset, FIFO. Three scalability notes, none urgent:
- **Single frame in flight** (`vkWaitForFences` on the sole fence each `BeginFrame`) serializes CPU and GPU. Correct and simplest; for chess, ideal. A reflex game will want 2 frames in flight + per-frame command buffers/semaphores — design the upgrade as a constant (`MaxFramesInFlight = 1`) now so the code shape anticipates it.
- **No resource destruction API**: `CreateMesh/LoadTexture/CreateMaterial` only append; `Dropdown` caches per-colour meshes forever; `MaxMaterials = 32` with chess using ~20 already. One long session of the *engine* is fine; a game that creates materials per level will hit the pool. Either add `Destroy*` or document "resources are create-once for app lifetime" as a contract in `Renderer.h` (honestly, for this engine's philosophy, the documented contract may be the better answer).
- **Text arena vs. layout cap mismatch**: `LayoutResult::MaxGlyphs = 512` per field but the frame arena holds 4096 vertices ≈ 1024 glyphs total; several full fields in one frame silently drop later `DrawGlyphs` (the guard logs nothing). Add a one-line log on drop — silent visual truncation is a nasty debug.

**View-layer I/O:** `RebuildItems()` reads every opponent's record **and** a `MatchMeta` sidecar file per opponent from disk, on link-state change and after every move (`StampMove` sets `ItemsDirty`). With tens of opponents that's dozens of file reads on the render thread per move — imperceptible today, and the dirty-flag gating is the right structure; just keep the *reads-per-rebuild* linear in opponents (it is) and resist calling it per-frame. `StampMove` also does two writes (meta + record) per move — fine on flash, worth knowing.

**JNI churn:** one `NewByteArray` + copy per send/receive. At chess rates: irrelevant. At a future 60 Hz input stream: recycle a direct `ByteBuffer`. Note filed for the CoC work, where it actually matters.

---

## 7. Elegance & consistency

- **Naming convention adherence is near-perfect** (PascalCase everywhere, `I`/`E` prefixes, no Hungarian, casing-tells-origin). The single systemic deviation: **`Games/Chess/View` uses namespace `Chess` with include root `Chess/View/`**, while modules pair `Lur::X` with `Lur/X/`. Consistent enough; just codify "game namespaces are the game name" in CLAUDE.md so game #2 doesn't guess.
- `BoardView` is drifting toward a god-view: rendering + touch + opponent selector + persistence orchestration + hijack policy (~615 lines with its header). It's still coherent, but the selector (RebuildItems/SwitchActive/StampMove and their state) is a separable `OpponentPanel` — do it opportunistically when the `IGame` refactor touches this file anyway, not as its own project.
- The `AndroidMain` "smoke test" block (start-position movegen log) predates the real game running on-device; it's now dead weight in both mains — delete with the `GameHost` extraction.
- `Session::Send`'s `Frame[1 + 64]` implicit 64-byte payload cap silently drops oversized `Sync` payloads… except `Sync` is the one message that grows (3 + ~1 byte/ply → overflows the cap around ply ~61 of a single match!). **Check this one immediately** — a long in-progress game may currently fail to resync, silently (`Send` returns void). At minimum log the drop; correctly, route `Sync` around the fixed frame or size the buffer from a named constant shared with the record format. *(Filed here rather than §5 because I can't run two phones to confirm, but reading the code, a >61-ply live match's `SendRecord` is dropped.)*
- Scripts + CI are a strength: one-command host loop, CLI-only Android setup, unsigned-IPA pipeline for Mac-less iOS dev, log-filter discipline written down. The `build.ps1`-on-MinGW "different compiler as portability coverage" framing is exactly right.

---

## 8. Prioritized roadmap

| P | Item | Size | Where |
|---|---|---|---|
| **P0** | Marshal Android BLE callbacks to the engine thread (transport inbox drained in `Tick`) | S–M | §5.1 |
| **P0** | Verify & fix the `Session::Send` 64-byte cap vs. growing `Sync` payloads | S | §7 |
| **P1** | Move BLE backends + Vulkan surface seams + app shells into engine platform modules; dynamic JNI registration; parameterize log tag | M (mostly `git mv`) | §3.1 |
| **P1** | Unify the BLE link logic: shared `BleLinkController` (C++) + per-OS `IBleRadio` drivers; host-test the collision/reconnect scenarios against a fake radio | M–L | §3.5 |
| **P1** | Extract `GameHost` from the duplicated main-file wiring | M | §3.2 |
| **P1** | De-chess `Modules/Net`: drop `Resign/DrawOffer`, generalize/guard `SendMove` truncation, fix `MaxMsgTypes` comment | S | §3.3 |
| **P2** | Name the game contract (`IGame`), port chess onto it, write `Docs/NewGame.md` | M | §4 |
| **P2** | Real-time-denominate `Session` timing | S | §5.2 |
| **P2** | Delete-or-document `Modules/Pairing`; drop vestigial `EBleRole` param; sync CLAUDE.md/README to reality | S | §3.4 |
| **P2** | One `Lur::Log` seam: engine-wide logging interface, per-platform sink installed once, tag supplied by the app | S | §3.5 |
| **P2** | Wire input through `Lur::Input::TouchEvent` (currently unused) and expose window metrics/safe-area insets via the platform surface | S–M | §3.5 |
| **P3** | Varint shift guard; `EncodeMove`/`MoveList` asserts; `TickClock` clamp; Loopback note; identical-GUID log | S | §5.3–5.4 |
| **P3** | `WRITE_TYPE_NO_RESPONSE`; Kotlin BLE API modernization; glyph-drop log; renderer resource-lifetime contract | S | §6 |
| Later | Magic bitboards, word-wise bitstream, ≥2 frames in flight, ClockSync implementation — each gated on the first game that needs it | — | §6 |

**Bottom line:** the foundation — determinism discipline, the bytes philosophy, the module wall, the test loop — is exactly what your goals need, and the chess core is quality work. The whole gap to "game folder = gameplay + content" lives in one move-and-unify-the-platform-layer refactor plus naming the game contract, and the worst actual defect (the Android threading race) is a contained fix the engine should own once for every future backend. The shared-first doctrine (§3.5) is the standing rule that keeps it fixed: platform files hold verbs, engine C++ holds decisions, and the defect log shows why. Do P0 and the P1 moves, adopt the ~600-line platform budget, and game #2 becomes the ~week-of-gameplay-code experience you're aiming for.
