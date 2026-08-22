# Adding a game

Durable orientation, deliberately **here** rather than in `Docs/`. Issue #45 asked for
`Docs/NewGame.md`, but `Docs/` in this repo is frozen dated snapshots — *"no file under `Docs/` is
durable"* (CLAUDE.md). A guide to adding a game must track HEAD or it is worse than nothing, so it
lives beside the code, where it versions in diffs.

## What a game owns

Only two things, in principle: **its content, and its own rules and presentation.**

```
Games/<Game>/Core     pure C++  rules + wire codec, shared verbatim by every platform
Games/<Game>/View     rendering + input handling for that game
Games/<Game>/App      the game's answers to Lur::App::IGame  (see below)
Games/<Game>/Content  art, audio, fonts — cooked into headers by Cook/
```

Everything else — the window, the renderer, the radio, the session, persistence, the dev console, the
touch recognizers — is `Modules/*`. The dependency wall is enforced by CMake: `Games/*` may depend on
`Modules/*`, never the reverse.

## The game contract

`Lur::App::IGame` has **one** method:

```cpp
void Configure(Lur::App::GameHost& Host, Lur::App::GameHost::Hooks& Hooks) override;
```

It is called by each platform shell after `Host.Init()` and before `Host.Start()`. That window is
load-bearing rather than stylistic — see `GameHost.h`'s phase comment: the Store and device id exist
by then (so the game can hand them to its view) and the session is not live yet (so nothing can call
back into a view that has not been attached).

Inside it, state whatever your game actually has an opinion about:

* `Hooks.StateHash` — if your state has a cheap hash the session can compare (chess hashes its board).
  Leave it unset if you detect divergence yourself; RPS does, with per-tick anchors.
* `Hooks.OnLinkReady` / `Hooks.OnResync` — if a link coming up or a reconnect means something to you.
* `Host.EnableRecordSync(...)` — **only** if you keep a per-opponent record that crosses the wire.
  Chess does. RPS does not, and says so by simply not calling it.
* Attach your view to `Host.Session()`, `Host.Store()`, `Host.Sync()`, `Host.DeviceId()`.

`Chess::ChessGame` (`Games/Chess/App/`) is the reference implementation. **RPS is the other half of
the reference**: its game-side host wiring is a single `OnResync` line, which is why the contract is
one method and not a lifecycle interface. If you find yourself wanting to add a method to `IGame`,
check first whether both existing games would implement it honestly — where they disagree, that
difference is the specification for an engine facility neither should own, not a new virtual.

## The app shell

Per platform, and it is the engine's:

```cmake
lur_configure_app_target(<target>
    LOG_TAG  OnlyYourGame
    BLE_UUID 4C55524D-....)
```

That one call applies the whole `LUR_*` capability ladder, the log tag and the BLE service UUID, and
pins Xcode's optimization to `LUR_CONFIG`. **Both arguments are required and have no defaults**, on
purpose: the service UUID once defaulted to chess's, and a game that inherited it failed silently, as
two phones that never discovered each other.

## Things that will bite

* **`Modules/*` must not name your game.** `grep -rniE '\byourgame\b' Modules/` should return nothing
  but illustrative comments.
* **`build.ps1` green says nothing about the platform layer.** `Modules/*/Platform/*`, the Vulkan
  backend and every `.mm` compile only in app builds. Compile-check Android after touching them, and
  push for the macOS CI to see the `.mm` files — they are not host-buildable.
* **Build `Shipping` occasionally** (`-DLUR_CONFIG=Shipping`). Nothing in CI or `build.ps1` does, and
  it was broken for 46 commits: a capability-gated symbol touched outside its `#if` guard compiles
  fine in every other configuration.
* **Two per-app values have no default** — `LUR_LOG_TAG` and `LUR_BLE_SERVICE_UUID`. See above.
* **The console is free.** Hold a `Lur::DevGui::Console`, call `CreateResources`, forward pointer
  events to `PointerDown/Move/Up` and keys to `Key`, and draw it last. You get the CVar tree, the
  numpad, the HSV picker, dev commands, the toaster and the two-finger triple-tap without writing any
  of it. Do not hand-roll the gesture: that was per-platform-per-game once and drifted badly enough
  that one platform could not open the console at all.
