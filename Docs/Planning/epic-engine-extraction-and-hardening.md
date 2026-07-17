<!--
  Each block delimited by "===== ISSUE =====" is one GitHub issue.
  First line of each block is "Title:"; everything after is the issue body (Markdown).
  Example (repo root):
    gh issue create --title "<title>" --body-file body.md --label <labels>
  Create the Epic first, then paste its number into the child issues' "Part of #NN" lines.
  Source: the full code review (lurmotorn-code-review.md), July 2026, at master #38.
-->

===== ISSUE =====
Title: Epic: Engine extraction & hardening — from "chess app with an engine inside" to "engine that shipped chess first"

## Goal

Close the gap between the repo and its stated goal: **a game folder holds only gameplay C++ + content; the engine holds all generic and platform-specific machinery; the engine never knows chess.** Today ~1,620 lines of game-agnostic engine code (both BLE radios, the Vulkan surface seams, the app shells, the session/persistence wiring) live inside `Games/Chess/`, and `Modules/Net` carries chess-shaped API. Two correctness defects found in the review are folded in as the P0 head of this epic.

Full analysis: `lurmotorn-code-review.md` (§ references below point into it).

## Sequencing

- **P0 first, independent of everything:** #NN (Android threading race), #NN (Sync payload vs 64-byte frame cap). Both are shippable fixes today.
- **Then the extraction chain, in order:** #NN (platform layer → engine) → #NN (GameHost) → #NN (de-chess Net) → #NN (IGame contract). Each builds on the previous one's layout.
- **P2/P3 items** (#NN timing, #NN Pairing/docs, #NN core hardening, #NN wire & render polish) are independent and can interleave.

## Child issues

- [ ] #NN — **P0** Transport: marshal Android BLE callbacks onto the engine thread
- [ ] #NN — **P0** Net: `Session::Send` 64-byte frame cap silently drops long-game Sync payloads
- [ ] #NN — **P1** Move the platform layer out of `Games/Chess/` into engine platform modules
- [ ] #NN — **P1** Extract `Lur::App::GameHost` from the duplicated Android/iOS bootstrap
- [ ] #NN — **P1** De-chess `Modules/Net` (message types, `SendMove`, comments)
- [ ] #NN — **P2** Name the game contract: `IGame`, chess as reference implementation, `Docs/NewGame.md`
- [ ] #NN — **P2** Denominate `Session` timing in real time, not frames
- [ ] #NN — **P2** Resolve `Modules/Pairing` (delete or document reality) + kill vestigial `EBleRole` + doc sync
- [ ] #NN — **P3** Core hardening batch (Varint shift guard, codec/list asserts, TickClock clamp, misc)
- [ ] #NN — **P3** Wire & render polish batch (NO_RESPONSE writes, Kotlin BLE API 33, glyph-drop log, resource-lifetime contract)

## Definition of done

- No data races on the Android path; the `ITransport` "receiver fires on the engine thread" contract is enforced by construction.
- A >61-ply in-progress match resyncs correctly (regression-tested via Loopback).
- `Games/Chess/Android` and `Games/Chess/iOS` contain only per-game shell files (manifest/plist, icon, package name, ~tens of lines of glue).
- `Modules/*` contains no chess concepts, in API or comments-as-API.
- `Docs/NewGame.md` exists and is accurate: a new game = an `IGame` + content + a thin shell.


===== ISSUE =====
Title: P0 — Transport: marshal Android BLE callbacks onto the engine thread

Part of #NN (epic). Independent — fix immediately. Review §5.1.

## Problem

`ITransport::SetReceiver` documents *"invoked on the engine thread"* and `Lur::Net::Session` documents *"not thread-safe: drive it from one thread."* The Android backend violates both:

- `BluetoothGattServerCallback.onCharacteristicWriteRequest` and `BluetoothGattCallback.onCharacteristicChanged` fire on **Binder threads** and call `nativeOnReceived` directly (`BleShim.kt` ~lines 345 and 487).
- `AndroidBleTransport.cpp`'s `nativeOnReceived` invokes `g_Transport.ReceiverFn` synchronously → `Session::OnDatagram` → registered handlers → `BoardView::ApplyRemoteMove` → `ChessMatchState::ApplyMove` (`Rec.Moves.push_back`, `Position` mutation).
- Meanwhile `android_main`'s native thread concurrently runs `Session.Tick()` and `View.Render()` reading the same state.

Result: a real data race — a `std::vector` reallocation mid-render can crash; torn `Board` reads render garbage; `Connected`/`Ready` are plain (non-atomic) bools flipped cross-thread via `nativeOnConnected/Disconnected`. It mostly *appears* to work because payloads are tiny and rare.

iOS is safe **by accident**: both CB managers use `queue:nil` (main queue) and `CADisplayLink` renders on the main run loop — single-threaded end to end.

## Fix (engine-side, so every future backend inherits it)

Add a small inbox to `Modules/Transport` that radio threads push into and the engine thread drains:

- A `ThreadedTransportAdapter : ITransport` wrapping any raw backend, or the queue baked into the Android backend — either way the drain happens on the engine thread (e.g. a `Pump()` the host calls at the top of each frame, or drained inside `Session::Tick()`).
- Carry **connect/disconnect through the same queue** as data, so ordering between link-state changes and datagrams is preserved (strictly better than making `Connected` atomic on its own).
- Zero steady-state allocation: payloads are ≤64 B today; a fixed ring (e.g. 32 × 64 B slots) under a mutex or SPSC discipline is plenty. Overflow policy: drop-oldest + log (should never fire at chess rates).
- Have iOS route through the same adapter for uniformity, even though it is currently single-threaded — that removes the "safe by accident" asymmetry and makes the contract enforced-by-construction on both platforms.

## Acceptance criteria

- [ ] All `ReceiverFn` invocations happen on the engine thread on Android (verified by thread-id logging in a debug build).
- [ ] Connect/disconnect and datagrams are delivered in arrival order through one path.
- [ ] No new steady-state allocation on the receive path.
- [ ] `Transport.h` contract comment updated to name the adapter as the mechanism.
- [ ] Loopback tests still pass; on-device smoke: rapid move exchange while rendering shows no tearing/crash.


===== ISSUE =====
Title: P0 — Net: Session::Send's 64-byte frame cap silently drops long-game Sync payloads

Part of #NN (epic). Independent — verify immediately. Review §7.

## Problem

`Session::Send` frames into a fixed `uint8_t Frame[1 + 64]` and **returns silently** when `Size > 64` ("guard, don't truncate the wire"). But `EMsgType::Sync` carries a serialized `ChessRecord`: 3 tally bytes + `EncodeGame` (16-bit ply count + ~1 byte/ply). That crosses 64 bytes at roughly **ply ~61 of a single in-progress match** — at which point `SendRecord` in both app mains silently does nothing, and a reconnect mid-long-game never resyncs.

`Send` returns `void`, so nothing upstream can even notice.

## Fix

1. **First, add the failing test** (host, via `LoopbackTransport`): play/construct a >61-ply record, run the link-establishment sync flow, assert the peer adopted it. This should fail today.
2. Then pick the mechanism:
   - Minimal: size the frame from a named constant shared with the record format (e.g. `MaxFramedPayload` sized for `3 + 2 + MaxStoredPlies`), and make `Send` return `bool` + `Logf` on drop so overflow is never silent again.
   - Cleaner long-term: framed messages that can grow (Sync) get a size-prefixed path rather than a stack frame — but do not redesign framing inside a P0; the named-constant fix is enough.
3. Wire note: this is a local buffer, **not** a wire-format change — no `ProtocolVersion` bump for the minimal fix.

Also fold in: the `MaxMsgTypes = 8` comment says "covers EMsgType 0..6" but `Sync = 7`; correct the comment while in the file (it's a wire-adjacent constant and the comments are otherwise authoritative).

## Acceptance criteria

- [ ] New Loopback regression test: >61-ply record syncs on link establishment.
- [ ] `Send` can no longer fail silently (bool return and/or log).
- [ ] 75-move-rule worst case (≥150 plies) fits the new bound.
- [ ] `MaxMsgTypes` comment corrected.


===== ISSUE =====
Title: P1 — Move the platform layer out of Games/Chess into engine platform modules

Part of #NN (epic). Do before the GameHost extraction. Review §3.1.

## Problem

~1,620 lines of game-agnostic engine code live in the chess game's app folders:

| File (in `Games/Chess/...`) | Lines | Actually is |
|---|---:|---|
| `Android/.../BleShim.kt` | 500 | Entire Android BLE radio (advertise/scan/GATT/roles/watchdogs) |
| `iOS/Sources/IosBleTransport.mm` | 427 | Entire CoreBluetooth radio |
| `Android/.../AndroidBleTransport.cpp` | 190 | JNI bridge `ITransport` ⇄ BleShim + device-id/role JNI |
| `Android/.../AndroidVulkanSurface.cpp` | 41 | Android half of `Lur::Render::Vk::PlatformSurface` |
| `iOS/Sources/IosVulkanSurface.mm` | 53 | iOS/MoltenVK half of the same seam |
| `Android/.../OnlyChessActivity.kt` | 46 | NativeActivity + BLE permission flow |
| `AndroidMain.cpp` / `AppMain.mm` | 157/209 | App loop + wiring (moves in the GameHost issue) |

Coupling artifacts that block reuse as-is: JNI symbols are baked to the chess package (`Java_com_lurmotorn_onlychess_BleShim_*`), `System.loadLibrary("onlychess")` hardcodes the lib name, and the `"OnlyChess"` log tag is embedded in engine-grade code. `Modules/Render` already models the correct pattern (shared `VulkanBackend.cpp` in the module, compiled only for device builds, tiny per-OS seam) — Transport should match it, and the seam implementations should live with their modules.

## Target layout

```
Modules/Transport/Platform/Android/   BleShim.kt + LurBleJni.cpp      (from Games/Chess)
Modules/Transport/Platform/Ios/       BleTransport.mm                  (from Games/Chess)
Modules/Render/Platform/Android/      VulkanSurface.cpp                (from Games/Chess)
Modules/Render/Platform/Ios/          VulkanSurface.mm                 (from Games/Chess)
Modules/App/Platform/Android|Ios/     activity/permission + UIKit loop templates (shells)
Games/Chess/Android|iOS/              per-game shell only: manifest/plist, icon, package,
                                      gradle/xcode config, ~tens of lines of glue
```

## Work items

- `git mv` the files; fix includes/gradle source sets/iOS CMake `SOURCES` accordingly (Android app gradle points its Kotlin source set + `externalNativeBuild` at the module paths; iOS CMake adds the module `.mm` files).
- **Dynamic JNI registration**: replace the package-named `Java_...` exports with `RegisterNatives` in `JNI_OnLoad`, so the Kotlin class can live in a fixed engine package (`com.lurmotorn.engine`) regardless of the app's package. Pass or standardize the native lib name (drop the hardcoded `loadLibrary("onlychess")`).
- **Parameterize the log tag** through the existing logger seams (`Session::SetLogger` pattern) on the Kotlin and ObjC sides; the `OnlyChess` string moves to the chess shell.
- Keep behavior identical — this issue is movement + decoupling, zero logic change (the threading fix is #NN and can land before or after; coordinate merge order).
- Update the CLAUDE.md architecture table, which already *describes* this layout ("Transport: interface + BLE backend") — after this issue the docs become true.

## Acceptance criteria

- [ ] `Games/Chess/Android` and `Games/Chess/iOS` contain no `.kt`/`.mm`/`.cpp` implementing transport or render seams.
- [ ] JNI binds via `RegisterNatives`; no `Java_com_lurmotorn_onlychess_*` symbols remain.
- [ ] Both apps build and link; BLE handshake works on the real pair.
- [ ] Grep for `OnlyChess` in `Modules/` returns nothing.
- [ ] CLAUDE.md/README architecture sections match the new layout.


===== ISSUE =====
Title: P1 — Extract Lur::App::GameHost from the duplicated Android/iOS bootstrap

Part of #NN (epic). Depends on the platform-extraction issue (lands in `Modules/App`). Review §3.2.

## Problem

`AndroidMain.cpp` and `AppMain.mm` contain the same ~70-line block, comment-for-comment: `Store` → `LoadOrCreateDeviceId` → `SyncManager` → `SetOnMatchEnd`(persist+log) → transport creation → loggers → `SendRecord` (hijack-guarded `Sync` send) → `OnLive` → `SetReadyHandler`/`SetResyncHandler`/`SetHandler(Sync)` → `Session.Start` — plus duplicated persist-on-background and the per-frame `Session.Tick()` cadence. It has already drifted in small ways (heap vs stack ownership, capture styles). This block is the engine's session/persistence choreography, not chess; every future game would fork it again.

## Design

```cpp
// Modules/App/Public/Lur/App/GameHost.h — sketch
namespace Lur::App {
class GameHost {
public:
    struct Config {
        std::string SaveDir;                    // platform supplies (filesDir / App Support)
        Lur::Transport::ITransport* Transport;  // platform supplies
        std::function<void(const char*)> Log;   // platform supplies (tag lives app-side)
    };
    void Start(const Config&, Lur::Save::ISaveState& State);
    void Tick();            // per frame: session tick (+ future clock sync)
    void OnBackground();    // persist
    // Accessors the game's glue uses: Session(), Sync(), Store(), DeviceId().
    // Peer-adoption hook: the game supplies OnPeerAdopted(peerGuid) -> bool
    // (the hijack rule is game policy and stays game-side; the host only invokes it
    //  from both ReadyHandler and ResyncHandler and sends the record iff adopted).
};
}
```

## Work items

- Implement `GameHost` in `Modules/App` (pure C++, host-testable against `LoopbackTransport` — the ready/resync/adopt/sync choreography gets its first unit test in the bargain).
- Shrink both mains to platform loop + `GameHost` + `BoardView` glue (~50 lines each).
- Delete the stale start-position "smoke test" movegen block from both mains while there.
- Match-end logging moves behind the host's log seam so the duplicated WLD log lines unify.

## Acceptance criteria

- [ ] The ready/resync/adopt/record-sync flow exists exactly once, in `Modules/App`, with a Loopback unit test covering: initial link adopt, reconnect re-adopt, non-adopt (different active opponent) sends nothing.
- [ ] Both mains contain no `Session`/`SyncManager` wiring beyond constructing `GameHost`.
- [ ] Behavior on device is unchanged (handshake, sync-on-link, persist-on-background, match-end persist).


===== ISSUE =====
Title: P1 — De-chess Modules/Net: message types, SendMove's 1-byte assumption, comments

Part of #NN (epic). Best after GameHost (touches the same call sites). Review §3.3.

## Problem

The engine's Net module carries chess-shaped API:

1. `EMsgType::Resign` and `EMsgType::DrawOffer` are board-game concepts; **no handler registers for either anywhere** (verified by grep). `EMsgType::Move = 3` is also unreferenced in code — live moves have been *bare* 1-byte datagrams since protocol v4 (`SetMoveHandler`); only a stale comment in `BoardView.h` still claims "peer moves arrive as EMsgType::Move".
2. `Session::SendMove` hardcodes the chess payload size: `const uint8_t Byte = Size >= 1 ? Data[0] : 0; Transport->Send(&Byte, 1);` — **silently truncating any payload >1 byte to its first byte**. The first game whose per-turn input exceeds 8 bits would compile, run, and corrupt the wire.
3. Comments across `Session.h`/`Transport.h` present chess as *the* case rather than an example.

## Fix

- **Enum values:** prefer **reserving** over renumbering — mark 3/4/5 as `Reserved3/4/5` (or comment-only reservation) so remaining values keep their numbers and **no `ProtocolVersion` bump is needed**. (Alternative: renumber compactly + bump to v5; only worth it bundled with another wire change.) Game-level actions like resign/draw belong to the game's own payload space, not the engine enum.
- **`SendMove` → generalize the fast path:** keep the elegant length-disambiguation invariant (bare datagram <2 bytes = game input; framed ≥2 bytes = typed message) but express it generically — e.g. `SendBare(const uint8_t*, size_t)` that **asserts/logs and refuses** when the payload can't stay bare, instead of truncating. Document the invariant where `SetMoveHandler` (→ `SetBareHandler`) is declared. Wire format unchanged.
- **Comments:** recast chess mentions in `Modules/*` as examples ("e.g. a turn-based game's move index"); fix the stale `BoardView.h` comment.

## Acceptance criteria

- [ ] No chess-named concepts in `Modules/Net` API or enum.
- [ ] Oversized bare sends fail loudly (assert in debug, logged refusal in release) — never truncate.
- [ ] Wire compatibility preserved (paired old/new builds still interoperate; no version bump), or a deliberate v5 with changelog if renumbering was chosen.
- [ ] NetTests cover the bare-vs-framed disambiguation and the oversized-bare refusal.


===== ISSUE =====
Title: P2 — Name the game contract: IGame, chess as the reference implementation, Docs/NewGame.md

Part of #NN (epic). Depends on GameHost + de-chessed Net. Review §4.

## Problem

The engine never names what a game *is*. The pieces all exist — `ISaveState` (persist+sync), deterministic evolution from an input stream (chess's `ApplyMove` + `RebuildBoard`, by convention), input encode/decode (`MoveCodec`, by convention), presentation/touch (`BoardView`, by convention) — but a new-game author discovers them only by reading `BoardView.cpp`. Naming the contract is what makes game #2 intuitive.

## Design (sketch — refine in implementation)

```cpp
namespace Lur::Game {
class IGame : public Lur::Save::ISaveState {
public:
    virtual void Reset() = 0;
    virtual bool ApplyInput(const uint8_t* Data, std::size_t Size) = 0; // decode + step; false = protocol error
    virtual uint64_t StateHash() const = 0;                             // desync detection (FNV of canonical serialize is fine)
    // Presentation (screen-side, non-deterministic):
    virtual void Render(Lur::Render::IRenderer*, float W, float H) = 0;
    virtual void OnTap(float X, float Y, float W, float H) = 0;
};
}
```

`GameHost` drives any `IGame`: bare datagrams → `ApplyInput`, `Sync` → `MergeIfNewer`, background → `Persist`; later, `StateHash` piggybacked on keepalives gives every game desync detection for free (follow-up, not this issue).

## Two contract decisions to make explicitly (chess made them implicitly)

- **Turn model:** chess's offline-move allowance and resync-by-history-exchange lean on strict alternation. The contract should require only `MergeIfNewer`; the turn-based history trick becomes a provided helper, not an assumption.
- **Authority:** CLAUDE.md's per-entity distributed-authority design stays a *future opt-in layer*; lockstep `IGame` remains the cheap blessed path. Don't complicate the contract for the shooter yet.

## Work items

- Introduce `IGame` (in `Modules/Sim` or a new `Modules/Game` — decide and document).
- Port chess: `ChessMatchState` + `BoardView` become the reference `IGame` (largely renaming/adapting; the shapes already fit).
- Write `Docs/NewGame.md` (~1 page): the contract, the module map, the shell checklist. Content moves out of `BoardView.cpp` comments into the doc.

## Acceptance criteria

- [ ] Chess runs unchanged through `GameHost` driving an `IGame`.
- [ ] The bare-input and Sync paths reach the game only via the contract (no chess types in host wiring).
- [ ] `Docs/NewGame.md` exists; a dry-run outline of a second trivial game (e.g. tic-tac-toe) fits it without new engine work.


===== ISSUE =====
Title: P2 — Denominate Session timing in real time, not frames

Part of #NN (epic). Independent. Review §5.2.

## Problem

`HelloResendTicks / KeepaliveTicks / LinkTimeoutTicks` assume `Tick()` ≈ 60 Hz. On a 120 Hz phone the keepalive halves (~0.5 s) and the timeout drops to ~2.5 s; under throttling the timeout stretches arbitrarily. Worse: iOS `CADisplayLink` **pauses in background**, so a backgrounded iPhone stops keepaliving entirely and the peer resets a healthy link.

## Fix

- `Session::Tick(uint64_t ElapsedNs)`; both platforms already have timestamps (`TickClock` shows the pattern). Constants become ms: `HelloResendMs=500`, `KeepaliveMs=1000`, `LinkTimeoutMs=5000`.
- Note in the header that a paused loop (iOS background) means no keepalives by design; the peer-side timeout + reconnect flow is the recovery path — make that explicit rather than accidental.

## Acceptance criteria

- [ ] Timing behavior identical at 60/120 Hz and under a simulated stall (unit test with synthetic ElapsedNs).
- [ ] NetTests updated from tick counts to time.


===== ISSUE =====
Title: P2 — Resolve Modules/Pairing, drop vestigial EBleRole parameter, sync docs to reality

Part of #NN (epic). Independent; cheap. Review §3.4.

## Problems

1. **`Modules/Pairing` is dead.** Nothing implements or calls `IPairing`; the documented flow (peer list, user pick, 6-digit confirm code) doesn't exist — pairing is the automatic in-band tie-break in the BLE backends. The header misleads a new-game author into wiring against a phantom.
2. **`EBleRole` is vestigial.** Both apps pass hardcoded roles (`Central` on Android, `Peripheral` on iOS) and both backends ignore the parameter (`CreateBleTransport(EBleRole /*Role*/)`); the real role is decided in-band by `DecideBleRole`.
3. **Docs drift:** CLAUDE.md's table claims transport backends live in the module (true only after the extraction issue); `Modules/Input` is one struct, not the described "per-platform touch glue"; README lists Pairing as a real layer.

## Fix

- Pairing: **delete the module** (lean recommendation — YAGNI until a real "choose among N peers" feature exists), or, if kept, rewrite its doc to describe the actual auto-pairing and mark it as the future seam for user-facing selection. Decide once, in this issue.
- `CreateBleTransport()` loses the parameter; remove the misleading hardcoded role arguments from both mains; `EBleRole` stays only where the tie-break genuinely uses it (`DecideBleRole`'s return).
- Update CLAUDE.md/README to the post-extraction layout; stamp the `.claude/Documents` glyph-renderer research as shipped.

## Acceptance criteria

- [ ] `grep -rn IPairing` returns nothing (or the module's doc matches reality, if kept).
- [ ] No call site passes a BLE role to transport creation.
- [ ] CLAUDE.md/README architecture sections verified against the tree.


===== ISSUE =====
Title: P3 — Core hardening batch: Varint shift guard, codec/list asserts, TickClock clamp, misc

Part of #NN (epic). Independent micro-fixes; one PR. Review §5.3–5.4.

- **`Varint::ReadVarUint` shift UB on hostile input:** `Group << Shift` with unbounded `Shift` — a crafted continuation-bit stream pushes `Shift` past 63 → UB before the buffer-end break triggers. Guard `if (Shift >= 64) break;` and surface failure consistently with `BitReader::IsOk` (this decodes peer-supplied bytes).
- **`EncodeMove` silently encodes index 0 when the move isn't in `Legal`:** a caller bug becomes "peer sees your first legal move." Assert in debug; consider encoding an out-of-range index so the peer *rejects* instead.
- **`MoveList::Add` unbounded `Count++`:** 256 bounds legal chess, but add a debug-only assert so a future off-by-one can't stack-smash.
- **`TickClock::Advance` spiral-of-death clamp:** a long pause returns thousands of ticks; clamp (`Ticks = min(Ticks, MaxCatchup)`) before a reflex game inherits the loop.
- **`Fixed` sharp edges:** `operator/` traps on zero; `FromInt` overflows silently for |v| ≥ 32768. Document ranges (or saturate) in the header — it's the future rollback substrate.
- **`LoopbackTransport` synchronous re-entrancy:** a receiver replying inside its callback recurses A→B→A. Add a header note ("don't send from within a receiver") or a tiny pending-queue.
- **`Session::OnHello` identical-GUID silent stall:** `if (LocalGuid == PeerGuid) return;` waits forever with no log. Log it — every other handshake edge logs.

Acceptance: each item lands with a unit test where testable (varint hostile stream, encode-missing-move, tick clamp), plus green `build.ps1`.


===== ISSUE =====
Title: P3 — Wire & render polish batch: NO_RESPONSE writes, Kotlin BLE API 33 migration, glyph-drop log, renderer resource-lifetime contract

Part of #NN (epic). Independent; one PR. Review §6.

- **Central→peripheral writes use `WRITE_TYPE_DEFAULT` (acknowledged):** switch the datagram characteristic to `WRITE_TYPE_NO_RESPONSE` — drops a round-trip per move; app-level keepalives already provide the liveness net. (Add `PROPERTY_WRITE_NO_RESPONSE` server-side alongside the existing property.)
- **Kotlin BLE deprecations (API 33):** migrate `characteristic.value = x; writeCharacteristic(ch)` → `writeCharacteristic(ch, bytes, writeType)` and `notifyCharacteristicChanged(dev, ch, false)` → the byte-array overload. Also removes the shared-mutable `.value` field as a latent race.
- **`DrawGlyphs` silent drop:** when the per-frame text arena (4096 verts ≈ 1024 glyphs) overflows across multiple fields (each field caps at `LayoutResult::MaxGlyphs = 512`), later calls are dropped with no signal. One log line on drop — silent visual truncation is a nasty debug.
- **Renderer resource lifetime:** `CreateMesh/LoadTexture/CreateMaterial` only append; `MaxMaterials = 32` with chess already using ~20; `Dropdown` caches per-colour meshes forever. Decide and **document the contract in `Renderer.h`** — either "resources are create-once for app lifetime" (fits the engine's philosophy) or add `Destroy*`. Note the headroom number either way so game authors budget.

Acceptance: real-pair smoke test after the BLE changes (handshake + moves both directions); text overflow log verified by a synthetic overdraw; `Renderer.h` states the lifetime rule.
