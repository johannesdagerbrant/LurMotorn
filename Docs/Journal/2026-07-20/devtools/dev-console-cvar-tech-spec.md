# Tech Spec — Dev Console, CVars & Dev-Commands (+ optional IMGUI dev panels)

*2026-07-20. Status: draft for review. Grounded in the repo at `d1dbb76`: `cmake/EngineFlags.cmake` (the `LUR_CONFIG` → `LUR_SHIPPING`/`LUR_INTERNAL`/`LUR_ASSERTS`/`LUR_SLOW` derivation), `Modules/Hud` (`TextField`, `DebugOverlay`, `Dropdown` — MSDF over `IRenderer`), `Modules/Platform/Window` (`TakeKeys()` VK stream + `TakeTouches()`), and `Rps/Tunables.h` (the `constexpr` table CVars shadow).*

## 0. Goals & non-negotiables

- An Unreal-style drop-down **console**: a text field along the screen bottom; type CVar assignments or dev-commands; a most-recently-used completion list renders **above** the field (newest at the bottom); arrow keys scroll/highlight (overwriting the field), arrowing back to the bottom restores the in-progress input; Enter executes. On phones, list rows are tapped.
- **CVars**: named overrides of default values, registered in engine modules (engine-related) or games (game-specific). Types v1: **bool, int, float, enum, Fixed** (per decision).
- **Dev-commands**: named functions with args that run game/engine logic (restart match, delete opponent history, spawn units…).
- **The shipping contract (the heart of this spec):**
  1. Shipping builds compile CVar reads to **the raw `constexpr` default** — no registry, no lookup, no branch, no wrapper cost. Verified by disassembly, not hope.
  2. **No double-branching**: call sites never write `#if SHIPPING … #else …`. One expression, authored once, reads the same in every config; only what it *compiles to* differs.
  3. ImGui and any dev-tool code are **absent** from shipping — not compiled, not linked, unreachable. A shipping build is unaware they exist.
  4. Dev ergonomics and readability must not regress to buy (1).
  5. **Engine-owned, uniform across games:** the console, dev-GUI layer, CVar/command systems, persistence, sync, and the desktop panel are engine tools that are **identical in every game**. A game cannot customize, restyle, or extend them; it influences them *only* by registering CVars/commands, which changes their contents (variables, categories), never their behaviour, layout, or code. (Elaborated in Addendum D.0.)

## 1. The CVar mechanism (this decides everything else)

The chosen shape (decision: "a CVar class whose inline functions return the `constexpr` default in shipping, else do override logic"). Realized so all four contract points hold simultaneously:

```cpp
// Modules/Core/Public/Lur/Core/CVar.h
namespace Lur::Core {

// T ∈ { bool, int32_t, float, Fixed, and enums (stored as their underlying int) }.
template <class T>
class CVar {
public:
    // Trivial constexpr ctor: stores only Default_ (+ dev-only metadata under the #if).
    // NOT consteval — the object must be usable as a normal value; registration is a
    // SEPARATE dev-only static (see the LUR_CVAR macro, §1.1), never a ctor side effect,
    // so there is no "constexpr ctor that also runs registration code" contradiction.
    constexpr CVar(const char* Name, T Default, CVarFlags Flags = {}, const char* Category = "");

    // THE hot path. One expression at every call site, all configs:
    //   if (Rps::CvAiAggro.Get()) ...
    //   x += Rps::CvMinerSpeed.Get();
    inline T Get() const noexcept {
#if LUR_SHIPPING
        return Default_;                 // ← raw constexpr value; entire class melts away
#else
        return Value_;                   // ← current (default unless overridden this session)
#endif
    }
    inline operator T() const noexcept { return Get(); }   // ergonomic: use it like the value

#if !LUR_SHIPPING
    void   SetFromString(const char*);   // console/registry entry point
    void   Reset();                      // back to Default_
    T      Default() const { return Default_; }
    bool   Overridden() const { return Value_ != Default_; }
    // name/help/type introspection for completion + panels
#endif

private:
    T Default_;
#if !LUR_SHIPPING
    T Value_ = Default_;
    // + intrusive registry linkage (see §1.1), all dev-only
#endif
};
}
```

Why this satisfies the contract:

- **Zero shipping cost (point 1):** `Get()` returns `Default_`, a member initialized from a `constexpr`. With the CVar object itself declared `constexpr`/`inline constexpr` at namespace scope, the optimizer treats `CvFoo.Get()` as the literal — identical codegen to reading the old `constexpr`. **Acceptance is a disassembly diff** (§7): the shipping `.o` for a hot sim function must be instruction-identical before/after CVar-izing a variable. **Structural condition:** in shipping the object must have **zero members other than `Default_`** (all registry/metadata members live under `#if !LUR_SHIPPING`), so it is a pure value the optimizer folds; if any non-`Default_` member survived into shipping it would become a real static with a guard variable and the identical-codegen claim would break. The disassembly-diff CI (§7) is the enforcement and MUST exist before the first migration.
- **Single authored branch (point 2):** the `#if` lives **once, inside `Get()`**, not at call sites. Game/sim code always writes `CvMinerSpeed.Get()` (or just `CvMinerSpeed` via `operator T`). The two-worlds split is entirely inside the class.
- **No abstraction tax in readability (point 4):** call sites read like a named constant. Migrating a tunable is: `constexpr Fixed MinerSpeed = F(6,10);` → `inline constexpr CVar<Fixed> CvMinerSpeed{"miner.speed", F(6,10)};` and then `.Get()` at uses. Diff-small, greppable, no indirection to trace.

**Determinism (two independent guarantees, both required):** a CVar read inside the **deterministic sim** must be identical on both peers in *value* and *timing*, or lockstep desyncs. These are separate problems with separate fixes:

1. **Value safety — no floats.** `CVar<float>` tagged `AffectsGameplay` is a **compile error** (a `static_assert` in the `AffectsGameplay` registration path rejects `T=float`). Floats round differently across ARM/x86 → cross-platform divergence. AffectsGameplay CVars are restricted to **bool / int32 / enum / Fixed**. Non-gameplay `CVar<float>` (render/HUD/camera) is fine — it never touches the hash.
2. **Timing safety — per-tick latching (decided: Option A).** Even with deterministic `Fixed`, if the sim reads a CVar mid-tick while an override is being applied, one peer can read the old value at one phase and the other the new value at another phase of the *same* tick → divergence on a value both agree on. Fix: **at the top of each tick, all `AffectsGameplay` CVar values are copied into a per-tick frozen snapshot; the sim reads only from that snapshot for the whole tick.** Overrides (from the Addendum C sync, applied at `applyTick`) mutate the live values, but the running tick sees the frozen set — so both peers see the *old* value for all of tick N and the *new* value for all of tick N+1. The float-ban does **not** solve this (it's about representation, not read-ordering); latching does.
   - **Bonus:** the snapshot is a small POD (few trivially-copyable values) → **fold it into `StateHash`**. A mis-synced or mis-latched gameplay CVar then surfaces as an immediate, located desync alarm instead of silent drift.
   - **Ergonomic split (the one real rule this imposes):** **outside** the deterministic sim (render, HUD, tools, console), call `CvFoo.Get()` freely. **Inside** the sim, read from the latched snapshot (e.g. `Tick.Cv.MinerSpeed`), never `CvMinerSpeed.Get()` directly. This is deliberate and visible — it marks "this is a synced gameplay value, constant for the tick" in the code, which is a feature, not a wart.

Non-gameplay CVars (rendering, HUD, camera, debug) are free to change live and locally; they're never latched, never synced, never hashed.

### 1.1 Registration via the `LUR_CVAR` macro (structural, can't-forget)

CVars are declared through a macro so registration is guaranteed and the shipping shape is structural, not optimizer-dependent:

```cpp
// Dev/debug:
//   LUR_CVAR(CvMinerSpeed, "rps.miner.speed", F(6,10), CVarFlags::AffectsGameplay, "Economy")
// expands to:
//   inline constexpr CVar<Fixed> CvMinerSpeed{"rps.miner.speed", F(6,10), CVarFlags::AffectsGameplay, "Economy"};
//   #if !LUR_SHIPPING
//   static const CVarRegistrar CvMinerSpeed_reg{CvMinerSpeed};  // ctor inserts into the registry
//   #endif
//
// Shipping: the macro expands to ONLY the `inline constexpr CVar<...>` line — the registrar
// vanishes, leaving bare constexpr data (satisfies \u00a71's structural condition).
```

- **Why a macro:** it makes the shipping-codegen guarantee *structural* (the CVar object is always trivially `constexpr`; the registrar is a wholly separate dev-only static) and makes "forgot to register" impossible (you can't declare a CVar without the macro registering it). This is the decided approach (B).
- **The registrar is a separate static, not a ctor side effect** — so there is no `constexpr`-ctor-with-runtime-work contradiction. The CVar value can be constant-initialized; only the dev-only registrar is dynamically initialized.
- **Static-init-order rule (must-state):** the registry's list head is a **function-local `static` (Meyers singleton)** so it's constructed on first use, regardless of registrar init order — registration across TUs is order-independent (a registry only needs the *set*, not an order). **Hard rule: no CVar value may be read before `main()` begins.** Reads-before-main would touch the value during static init where ordering is unspecified; forbid it (a debug `LUR_ASSERT(g_MainEntered)` in `Get()`'s dev path can enforce it cheaply). CVar *values* themselves are `constexpr` so reading is always well-defined value-wise; the rule is about the registry/override state, which is dev-only.
- In shipping the registry, the list, `CVarRegistrar`, and every registration are inside `#if !LUR_SHIPPING` — the CVar is pure data. Name/category/help strings are dev-only metadata (kept, they're tiny `constexpr`).

### 1.2 Types

`CVar<bool>`, `CVar<int32_t>`, `CVar<float>`, `CVar<Lur::Sim::Fixed>`, and enums via `CVar<EnumT>` (stored as underlying int, parsed by name or ordinal). Parsing: `SetFromString` dispatches on `T` (a small `FromString<T>` in Core). `Fixed` parses a decimal into Q16.16 exactly (reuse the `F(num,den)` rational path).

## 2. Dev-commands

Parallel system, fully dev-only (`#if !LUR_SHIPPING`, no shipping shape needed — commands don't exist in shipping at all):

```cpp
// Lur::Core::DevCommand — registered with a name, help, arg spec, and a handler.
// void Handler(const DevArgs& args, DevOutput& out);
```

- Registered in engine modules (`net.reset_link`, `save.wipe`) or games (`rps.restart`, `rps.spawn <team> <type> <count>`, `rps.delete_opponent_history`).
- Registration mirrors CVars: an intrusive dev-only registry, iterable for completion.
- A command that mutates **sim state** (restart, spawn) is a **sim command** and routes through §3 so both peers stay in lockstep. A command that touches only **local/non-sim** state (toggle a debug draw, dump a log, wipe *local* save history when disconnected) executes locally and immediately.
- `save.wipe` / `delete_opponent_history`: these mutate persisted state, not tick state. Spec them as **local, and only sane while not in a live match** (a guard + a printed refusal otherwise) — deleting history mid-match under lockstep is undefined; do it at the menu.

## 3. Determinism & lockstep (decision: route commands through the input stream)

The wire today is the tick-stamped **press mask** + framed control messages (`LockstepPeer`). Dev-commands and sim-CVar overrides do **not** fit the press-mask budget and must not pollute the shipping wire. Design:

- A new **`EMsgKind::DevCommand`** framed message, compiled **only in `LUR_INTERNAL`** builds. Payload: `[tick][opcode][args]`. Both peers enqueue it and apply it deterministically at the stamped tick — same as a normal input — so the sim stays identical and the flight-recorder captures it.
- **A shipping peer never emits or accepts this opcode** (the kind isn't compiled in; an unknown framed kind is already rejected). So the shipping wire and shipping determinism are untouched; two dev peers can drive each other's sims for testing.
- **Sim-CVar override** = a specific dev-command (`cvar.set <name> <value>` where the CVar is sim-tagged) carried the same way, applied at a tick boundary on both peers. Non-sim CVar sets are local-only and skip the wire entirely.
- **Single-peer / desktop / disconnected**: commands apply locally at the next tick (no peer to sync with). The routing is "through the tick pipeline," which degrades cleanly to local when there's no link.
- CVars are tagged sim-visible vs local at registration (`CVarFlags::Sim`), so the console knows whether a set goes local or over the wire. Getting this tag wrong on a sim CVar is a desync bug — so: **default to Sim (safe: routed/deferred) and opt into Local explicitly.**

## 4. Console UX & implementation

Built on the **existing `Modules/Hud` stack** (`TextField` + `Dropdown`-style list + `DebugOverlay` patterns) over `IRenderer` — **no new dependency**, consistent with the engine's own-your-stack ethos. Lives in a new `Modules/DevConsole` (or `Modules/Hud/DevConsole`), entirely `#if !LUR_INTERNAL`-guarded → absent in shipping.

- **Layout:** input `TextField` pinned to the screen bottom (full width, one line, blinking caret); completion list stacked **above** it, most-recently-used order, **newest at the bottom** (nearest the field), so the freshest match sits right above the caret. Semi-transparent backing quad; drop-down animation optional (cosmetic, later).
- **Completion:** as the user types, filter CVars ∪ commands by prefix/substring; rank by MRU (a dev-only recency list), newest-used at the bottom. Show name + current value (CVars) or arg hint (commands).
- **Keyboard nav:** ↑/↓ move the highlight through the list; highlighting **overwrites** the field with that entry; arrowing **back down past the last row** restores the *original in-progress text* captured at the last keystroke (so browsing is non-destructive). Enter executes the field's content. Esc closes. Tab = complete-to-highlighted.
- **Execution:** parse the line → CVar assignment (`name value` or `name` to print current) or command (`name args`) → dispatch (local now, or over §3 wire if sim-tagged) → append result/echo to a small scrollback above the list (dev-only ring buffer).
- **Toggle key (desktop):** the key left of `1` on a Swedish layout (`§`/`½`). This is **VK_OEM_5 / scancode 0x29** on Windows. Bind by **scancode** (layout-independent) in `Win32Window`'s key handler, and expose the bind as a CVar (`console.toggle_key`) so it's rebindable. The console **consumes** its key (doesn't leak the toggle char into the field). `Window::TakeKeys()` currently streams VKs; the console needs **key-down with modifiers + a char/text stream** for editing — see §4.1.

### 4.1 Input plumbing gap (must-fix for text editing)

`TakeKeys()` returns a `vector<uint32_t>` of VK codes on key-down only — enough to detect the toggle and arrows, **not** enough for text entry (no WM_CHAR/Unicode, no key-repeat, no backspace semantics beyond the VK). Spec addition:

- Extend the platform `Window` with a **text/char event stream** (Win32: `WM_CHAR` → UTF-8 codepoints) and key **down/up + repeat** with a modifier mask, drained like touches. Keep it minimal and engine-level in `Modules/Platform`.
- The console consumes char events for editing and VK/arrow events for nav; when the console is **closed**, these are ignored by it and flow to the game as today (the game still only reads taps/its own keys). No gameplay input path changes.

### 4.2 Phone support — open gesture decided; soft-keyboard still a spike

- **Open gesture: a three-finger triple-tap** (decided). Dev-build only (`#if !LUR_SHIPPING`) — the recognizer isn't compiled into shipping, so there's no hidden gesture in players' hands. Chosen because it's essentially impossible to trigger accidentally during normal one/two-finger play, needs no on-screen chrome, and is symmetric on Android and iOS. Implementation: the platform layer already drains touches with `PointerId`s; the dev layer watches for three simultaneous pointers cycling down/up three times within a short window (tunable via a local CVar, e.g. `console.open_taps`, `console.open_window_ms`). It's consumed by the dev layer and never reaches the game.
- **Soft-keyboard activation: still its own investigation spike** (unchanged). Android needs `InputMethodManager.showSoftInput` on the `NativeActivity`/view with the IME text routed into the console field — likely a small Kotlin shim exposing typed text over JNI, mirroring the BLE shim pattern; iOS needs a hidden first-responder (`UITextField`) feeding the console. Flagged as a task to scope before implementation, not hand-waved.
- Phone completion is **tap-to-select** (no arrow keys), which the `SelectList` already supports.

## 5. Where things live (module placement)

- `Modules/Core`: `CVar<T>`, `CVarRegistry`, `DevCommand`/registry, `FromString<T>`. (Core because both engine and games register; the shipping shape is header-only + `constexpr`.)
- `Modules/DevConsole` (dev-only): the console widget, completion, scrollback, input handling. Depends on Hud/Text/Render/Platform. **Not built at all in shipping** (CMake excludes the target when `LUR_SHIPPING`).
- `Modules/Net` (dev-only additions): the `DevCommand` framed message kind + apply hook, `#if LUR_INTERNAL`.
- Engine CVars/commands: in their owning module (`net.*`, `render.*`, `save.*`).
- Game CVars/commands: in the game (`Rps/DevVars.h`, `Rps/DevCommands.cpp`), registered at startup. `Tunables.h` values migrate to `CVar` opportunistically (start with the ones you actually tweak while balancing — the boid/economy knobs are prime candidates and dovetail with #84).

## 6. IMGUI — the optional second surface (recommendation)

The console itself needs **no** IMGUI lib (§4). But the brief wants IMGUI for richer dev tools (a CVar browser with sliders, live graphs, entity inspectors, the balance-matrix dashboard), where an immediate-mode lib genuinely beats hand-rolled widgets.

**Recommendation: microui, not Dear ImGui.**
- microui is ~1 C file, no dependencies, immediate-mode, and emits a simple command list you render yourself — you'd bridge it to `IRenderer`/MSDF in an afternoon, and it fits the engine's zero-dependency, own-your-stack ethos. Small enough to vendor and actually read.
- Dear ImGui is the industry default and far more capable, **but** it's large, wants its own font atlas + a render/input backend (you'd write or port an `imgui_impl` for your bespoke Vulkan `VulkanBackend`, plus MoltenVK/Android surfaces), and pulls a lot of surface into your tree. Its power (docking, tables, plots) is more than dev panels for a two-button RTS need.
- **Verdict:** vendor microui behind `#if LUR_INTERNAL` in a `Modules/DevUi`; render via `IRenderer`. Keep it *entirely* separate from game GUI (which stays on `Hud`). If a future tool genuinely needs Dear ImGui's heft, revisit — the console and CVar systems don't depend on this choice, so it's reversible.
- **Either way:** the IMGUI lib, its backend, and every panel are `#if LUR_INTERNAL` and excluded from the shipping build at the CMake target level. Shipping links nothing.

*(If you'd rather ship the console first and defer any IMGUI lib, everything in §§1–5 stands alone; §6 is independently schedulable.)*

## 7. Testing & acceptance

- **Shipping zero-overhead proof (the headline acceptance):** pick a hot sim function using a CVar-migrated tunable; compile `LUR_CONFIG=Shipping`; diff the emitted assembly against the pre-migration `constexpr` version — **must be identical** (or provably equivalent). Automate as a CI check on one representative TU.
- **No-double-branch lint:** grep/CI guard that forbids `LUR_SHIPPING`/`LUR_INTERNAL` `#if` inside sim/gameplay call sites touching CVars (the split belongs in `CVar::Get`). 
- **Determinism:** desktop `--auto` soak with a scripted `cvar.set`/command schedule applied via the §3 tick pipeline on two loopback instances → hash-identical. A sim-CVar changed *without* routing (deliberately, in a test) must **fail** the equivalence test — proving the guard works.
- **Registry/parse:** unit tests for `FromString<T>` across bool/int/float/Fixed/enum, round-trip and malformed input (assert-loud on bad, per house policy).
- **Console UX:** headless-ish tests for completion ranking (MRU order, newest-at-bottom), arrow-restore-original-input, and execute dispatch (local vs wire tag). 
- **Shipping absence:** a build assertion / CMake check that `Modules/DevConsole` and `Modules/DevUi` produce no objects and no symbols in a `LUR_SHIPPING` link.

## 8. Open questions for the owner

**All resolved as of 2026-07-20.** Recorded here for provenance:

1. **Phone open-gesture** — ✅ three-finger triple-tap, dev-build only (§4.2). Soft-keyboard activation remains a scoped **investigation spike** (Android IME-over-JNI shim; iOS hidden first-responder) — the one piece of implementation work still to be designed, tracked as its own task.
2. **CVar name strings in shipping** — keep them (tiny, harmless `constexpr`) (§1.1).
3. **Sim-CVar change timing** — apply at the stamped `applyTick` on both peers via the sync message (§C), i.e. live, not match-boundary-only.
4. **microui vs Dear ImGui** — ✅ neither; build our own dev-GUI layer (Addendum A).
5. **First CVar migration scope** — the boid + economy knobs (they're about to be heavily tuned for #84).
6. **Sim-CVar propagation model** — ✅ bidirectional peer sync, not host-authoritative (Addendum C).
7. **Persistence file scope** — ✅ per-game `cvars.cfg` (C.4).
8–10. **Conflict resolver** — ✅ last-writer by wall-clock; timestamp collision → compile-time default; enums are underlying ints (Addendum C, C.2).

Remaining implementation work to scope (not a design question): the **phone soft-keyboard spike**.

---

# Addendum A — Own Dev-GUI Layer (no IMGUI lib)

*2026-07-20, supersedes §6's "vendor microui" recommendation and refines §4. Decision: build our own immediate-mode dev-GUI as a distinct rendering layer in front of both the world and the game-GUI layer, code- and visually-separated from shipping game GUI. Grounded in the `IRenderer` layer model at `d1dbb76`.*

## A.1 Three layers, one new pass

The renderer already composes in painter's order with an engine-owned ortho GUI camera and **no depth** (`Renderer.h`: `BeginFrame` → world; `BeginGui` → ortho overlay). The dev GUI is a **third pass** after the game's GUI:

```
BeginFrame(worldCam)   → world/units          (game)
BeginGui()             → game HUD/GUI          (game — Hud widgets, ships)
BeginDevGui()          → DEV layer             (engine, LUR_INTERNAL only) ← NEW
EndFrame()
```

- **New `IRenderer::BeginDevGui()`**, default no-op (like `BeginGui`), same engine-sized ortho, depth off → the dev layer always paints over everything with zero ordering work. In `LUR_SHIPPING` the call is compiled out at every call site (see A.5), so the pass literally does not occur.
- The host main calls it, not the game: the frame composition (`DesktopMain.cpp`, `RpsMain.cpp`) drives `DevGui::Render()` between `View.Render(...)`'s GUI and `EndFrame()`. Games never invoke dev GUI; they don't know it exists.

## A.2 The dev-GUI module (immediate-mode, on our own primitives)

New `Modules/DevGui` (entire target excluded from shipping via CMake). Immediate-mode, retained-free, built on `IRenderer` + `Modules/Text` MSDF + the existing `Hud` quad/glyph patterns — **no third-party lib**.

- A tiny `DevGuiContext`: `Begin(frameInputs) … widgets … End()`. Per-frame arena for vertices (mirrors `DrawGlyphs`/`DrawInstances` transient model); zero retained state beyond a small persistent block (open flags, MRU list, scrollback ring, per-widget hot/active ids).
- Widget vocabulary v1 (only what the console + first panels need): `Panel`, `Label`, `TextInput` (the console field), `SelectList` (the completion list; keyboard + tap), `Button`, `Checkbox` (bool CVar), `SliderInt/Float/Fixed` (numeric CVars), `EnumCombo`. All draw via `DrawGlyphs` + tinted `DrawInstances`/quads.
- Immediate-mode id/hot/active handling is standard (hash of call-site/label → id; track hot under cursor, active on press). Input comes from the A.4 stream; when the dev layer is open it **captures** input (game sees none); when closed it consumes nothing.
- The **console** (§4) becomes the first `DevGui` client: a bottom `TextInput` + a `SelectList` above it, MRU-ordered newest-at-bottom, arrow/tap nav with original-input restore, Enter to dispatch. Scrollback is a `DevGui` panel. Nothing about the console's behaviour changes from §4 — only its widgets now come from `DevGui` instead of raw `Hud`.

## A.3 The standardized dev look (visually unmistakable)

A single `DevTheme` constant block in `Modules/DevGui`, deliberately **anti-game**: nothing in the dev layer should ever be mistaken for shippable UI.

- **Aesthetic:** flat, high-contrast, monospaced. Dark charcoal translucent panels (e.g. `#141414` @ ~85%), thin 1px cyan/lime accent borders, a single accent hue for hot/active, mono font at one or two fixed sizes. No rounded corners, no gradients, no game palette — a "debug instrument" look distinct from RPS's warm board GUI.
- **Dedicated font:** ship a compact monospace MSDF as `DevGui`'s own cooked font (separate from game fonts) so dev text is instantly recognizable and never inherits a game's typeface.
- **Persistent affordances:** a thin top strip or watermark (`DEV` / config + build hash) visible whenever any dev surface is up, so a screenshot can never be mistaken for shipping. The existing `DebugOverlay` (fps/link/tick) folds into this layer and adopts the theme.
- **Standardization:** all dev tools (console, CVar browser, future graphs/inspectors) must use `DevGui` widgets + `DevTheme` — no bespoke per-tool styling. This *is* "a clean standardized dev look," enforced by there being one widget/theme source.

## A.4 Input (extends §4.1)

Same platform additions §4.1 requires (UTF-8 char stream + key down/up/repeat + modifiers, drained like touches). Routing rule: **dev layer first**. Each frame, if any dev surface is open, `DevGui::Begin` consumes the char/key/tap stream and the game receives an empty input set; if closed, dev consumes only its global toggles (console key, a future dev-menu chord) and passes everything else through unchanged. This keeps the "game input path never changes" guarantee — the dev layer is a front-of-everything input sink exactly as it's a front-of-everything render layer.

## A.5 Shipping separation (code + build)

- **Code:** `Modules/DevGui` and `Modules/DevConsole` are separate modules under `LUR_INTERNAL`. Game GUI (`Modules/Hud`) and dev GUI never share types; a game cannot accidentally draw dev widgets (different namespace, different module, not linked in shipping). Naming keeps them distinct (`Lur::DevGui::*` vs `Lur::Hud::*`).
- **The new renderer hook:** `BeginDevGui()` is declared unconditionally (default no-op keeps the vtable stable) but **every call is wrapped** so it vanishes in shipping without a call-site branch:
  ```cpp
  // Lur/Render/DevGui.h (engine)
  inline void BeginDevGuiLayer(IRenderer* R) {
  #if !LUR_SHIPPING
      R->BeginDevGui();
  #endif
  }
  ```
  Host mains call `BeginDevGuiLayer(R)` + `DevGui::Render()` guarded by `#if !LUR_SHIPPING` in one place (the frame composer), so shipping contains no dev-layer submission at all. (Matches the §0 contract: no double-branch in game/sim code; the one `#if` lives in the engine wrapper + the host composer.)
- **Assets:** the dev mono font and theme live in `DevGui` and are not cooked into shipping builds.
- **Acceptance (adds to §7):** a shipping link contains **no** `DevGui`/`DevConsole` symbols and **no** dev font asset; the frame composer in shipping issues exactly the world + game-GUI passes (verified by a render-call trace or a build assertion).

## A.6 Consequences for the rest of the spec

- §6 (microui/Dear ImGui) is **withdrawn** — we build our own; no third-party dev-UI dependency enters the tree. Open question #4 is resolved (answer: neither).
- The console (§4) is unchanged in behaviour; its widgets now come from `DevGui`. 
- Phone open-gesture + soft-keyboard (§4.2) still an open spike — unaffected by this decision (the dev layer renders identically on phones; only input origin differs).
- Everything CVar/command (§§1–3, 5) is unchanged and remains independent of the GUI choice.

---

# Addendum B — CVar Persistence (readable, separate from game save)

*2026-07-20. Adds session-persistent CVar overrides. Grounded in `Modules/Save/Store` (the byte-blob game persistence this deliberately does NOT reuse) at `d1dbb76`.*

## B.1 Behaviour

- An override set via the console **persists across sessions** — it reloads next launch and stays until reset.
- Typing **`reset`** as the value after a CVar name (`miner.speed reset`) restores the `constexpr` default **and removes the persisted override** for that CVar.
- A **`cvar.reset_all`** command (a.k.a. `ResetAllCvars`) clears *every* persisted override and restores all defaults.
- Persistence file is **separate from the shippable game save**, and **human-readable** — designed to be read and hand-edited, not bit-packed like `Store` blobs.

## B.2 Storage format & location — NOT the Store

`Store` is byte-blob, atomic-rename, sanitized-key persistence for shippable data (device id, opponent records). CVar overrides are a **developer artifact**: they want to be legible, diffable, and editable in a text editor. So they get their own file, written directly (not through `Store`):

- **Format:** a flat text file, one override per line, **keyed by the CVar's name (its identity, C.0.1)**: `name = value`, `#` comments, blank lines ignored. A line whose name no longer resolves (renamed/removed CVar) is warned-and-dropped on load (C.0.1). Values in the same human syntax the console accepts (`true/false`, ints, decimals for float/Fixed, enum by name). Stable-sorted by name on write so diffs are clean.
  ```
  # LurMotorn dev cvar overrides — safe to hand-edit; delete a line to un-override.
  render.debug_bars   = true
  rps.miner.speed     = 0.70
  rps.counter_mult    = 2.5
  ```
- **File:** a **per-game** `cvars.cfg` (decided, C.4) in a dev config dir (e.g. `<saveDir>/dev/cvars.cfg`, where `<saveDir>` is the app-supplied path from `RpsMain`/`DesktopMain` — already per-game). A plain file the dev layer owns, never a `Store` key. Writing still uses a temp-file-then-rename for crash safety (borrow the technique, not the class).
- **Entirely `#if !LUR_SHIPPING`.** Shipping has no persistence file, no loader, no writer — CVars are pure `constexpr` (§1), so there is nothing to persist and no code to do it.

## B.3 Load timing & the determinism split (critical)

Persisted overrides interact with the lockstep rule from §3 (sim CVars must be identical on both peers, or desync). The load path therefore respects the same sim/local tag:

- **Local CVars** (render, HUD, camera, debug draw — never hashed): loaded and applied **at startup**, unconditionally. A designer's persisted `render.debug_bars = true` just takes effect. No determinism concern.
- **Sim CVars** (feed the deterministic tick): a persisted override is **read at startup but NOT silently applied to the live sim**, because two devices with different `cvars.cfg` files would boot with different sim constants and desync with no wire change to blame — the worst class of bug. Instead:
  - On the **host/authoritative** side, a persisted sim override becomes a `cvar.set` **dev-command routed through the tick stream** (§3) at match start, so **both peers apply it at the same tick** and stay identical.
  - Solo/desktop/disconnected: applied locally at startup (no peer to diverge from).
- This means a designer tuning sim values on one device and connecting to a peer with defaults gets a **consistent** match (the host's persisted sim values propagate as commands), never a silent desync. See open question B.5.

## B.4 Implementation notes

- Loader/writer live in `Modules/DevConsole` (or a small `Lur::Core` dev-only `CVarConfig`), `#if !LUR_SHIPPING`. Parsing reuses `FromString<T>` (§1.2); unknown names in the file are **warned and skipped** (a CVar may have been renamed/removed — don't hard-fail a hand-edited file, but log loudly per house policy).
- Write triggers: on each successful console `set` (and on `reset`/`reset_all`), rewrite the file from the current override set (small; whole-file rewrite is simplest and keeps it sorted/clean). No need to persist on every frame — only on change.
- `reset <name>`: drop the override in memory (`CVar::Reset()`), remove its line, rewrite. `cvar.reset_all`: clear all overrides, delete the file (or write an empty header). Both are dev-only; both print what they cleared to the scrollback.
- Override precedence at boot: `constexpr` default ← persisted file (local applied now; sim staged for match-start routing) ← live console edits this session.

## B.5 Open questions (add to §8)

6. **Persisted sim-CVar propagation** (B.3): confirm the model — host's persisted sim overrides propagate to the peer as start-of-match commands (recommended, keeps lockstep and lets one designer drive), *or* require both peers to carry the same `cvars.cfg` (simpler, brittle), *or* forbid persisted overrides on sim CVars entirely (safest, least useful)? Recommendation: **propagate from host**.
7. **Per-game vs shared file:** one `cvars.cfg` for all games, or per-game (engine CVars shared, game CVars namespaced)? Names are already namespaced (`rps.*`, `render.*`), so a single file works; per-game files are cleaner if two games define clashing local tweaks. Recommendation: **single file**, namespaced keys.

---

# Addendum C — Bidirectional CVar Sync (supersedes B.3's host model)

*2026-07-20. Decision: CVar overrides sync symmetrically, exactly like a unit spawn — send the value + the tick it changed; full CVar sync at match start; if both peers override the same CVar, **the most recent edit (by wall-clock) wins, and if the timestamps collide the CVar reverts to its compile-time default**. Grounded in `LockstepPeer` at `d1dbb76`: tick-stamped masks, `MsgAnchor` on `EMsgType::Game1`, per-10-tick anchoring, seed-based `Init`.*

This replaces the host-authoritative model in **B.3 / B.5-Q6**. CVar changes are now peer inputs, not host broadcasts.

## C.0 `AffectsGameplay` — the explicit sync boundary
Every CVar carries an **`AffectsGameplay`** flag (set at registration; part of `CVarFlags`). It declares a *property* of the CVar — does changing it alter the game both players see — and participation in this whole message system is the **implication** of that property, not the thing you toggle:

- **`AffectsGameplay = true`** — and *only* true — for CVars that affect **gameplay determinism / the networked simulation** (anything read inside the deterministic tick: unit stats, economy, combat, targeting, movement). These sync (C.1) and are resolved across peers (C.2).
- **`AffectsGameplay = false`** (the default; also note `CVar<float>` may never be `AffectsGameplay` — compile error, §1 determinism rule 1) for everything else — audio, rendering, camera, HUD, debug draw. These are **entirely ignored by the CVar message system**: never sent, never accepted, never resolved. They stay purely local visual/UX preferences and may change live on one device freely (they never touch the state hash).
- **Registration must set it explicitly for gameplay CVars**; the safe default is `false` (a CVar you forget to flag simply won't sync — the failure mode is "my tweak didn't reach the peer," a visible annoyance, **not** a silent desync). This inverts the earlier "default to Sim" note in §3 — `AffectsGameplay` makes the safe default *false*, because the flag is now an affirmative, reviewable declaration rather than an inferred tag. A CI/review check can assert that every CVar read inside sim code is declared `AffectsGameplay`.
- This flag **supersedes** the informal "sim-tagged vs local" language in §1/§3: wherever earlier sections say "sim CVar," read "`AffectsGameplay` CVar."

## C.0.1 CVar identity: name for humans & persistence, 1-byte id on the wire

Two identity schemes, each chosen for its medium's lifetime:

- **Name string — the durable, human, cross-version identity.** Used in source declarations, the persistence file, and the console. Survives app updates: an override persists as long as a CVar with that name still exists.
- **1-byte `GameplayId` (`u8`) — the ephemeral within-build wire identity.** Only `AffectsGameplay` CVars get one (they're the only CVars that ever travel — realistically dozens, capped at 256). The wire (`MsgCvar`/`MsgCvarSync`) references a gameplay CVar by this single byte instead of its name: minimal BLE traffic, and both phones map the id to the same CVar because the build-version gate (C.3) forces identical builds. The id is **not** source-position/registration-order; it is **derived by the build** from the lexicographically-sorted set of gameplay-CVar names and compiled in (C.0.2), so it's consistent for the whole run and identical across the same build. Developers never author it.

- **Persistence is keyed by name, never by id** (Addendum B): each override line is `rps.miner.speed = ...`. This is what lets tuning survive version updates — even a rebuild that renumbers `GameplayId`s (by adding/reordering CVars) doesn't disturb the file, because the file never mentions ids. On load, each persisted name resolves to its current CVar (and thus its current id); a name that no longer exists is warned-and-dropped.
- **The wire is keyed by `GameplayId` (1 byte), never by name:** the full string never crosses BLE.
- **Cap:** `AffectsGameplay` CVars are capped at **256** (`u8`); the cook enforces this (and rejects duplicate names) as a **build error** (C.0.2), so overflow never reaches a device. *Non-*gameplay CVars (render/audio/HUD — the bulk) are **uncapped** and get no id; only the small synced set consumes the 256 space.
- **Startup resolution step:** the registry loads the build-generated name↔id table (C.0.2); persistence (names) and the wire (ids) both resolve through it. No runtime id assignment.
- **Renaming a CVar obsoletes its old overrides, and that is fine (decided).** A rename is simply a new identity: the old name no longer resolves, so any persisted override under the old name is **ignored on load with a dev warning and dropped on next rewrite** (the CVar reverts to its default until re-tuned under the new name). No migration, no aliasing — renames are cheap and self-cleaning. Document this so a rename during balancing doesn't surprise ("my tweak vanished" = "I renamed the CVar").
- **Uniqueness:** names must be unique within the registry; the `LUR_CVAR` macro path can `LUR_ASSERT` on duplicate registration in dev (a name collision is a bug, caught at startup).
- These messages are rare dev-only traffic (never shipping, never the hot press-mask path); the 1-byte id keeps even a full match-start sync tiny over BLE.

## C.0.2 `GameplayId` is assigned by the build, never authored

Developers **never write, pick, or think about ids** — they only ever declare a CVar with a name (`LUR_CVAR(...)`). The `GameplayId` mapping is derived by the **build/cook step** and compiled in as generated code. This removes the one manual, drift-prone step: there is no id to forget, collide, or keep in sync by hand.

**Derivation rule (decided):** the cook collects every `AffectsGameplay` CVar **by name**, sorts the names **lexicographically**, and assigns `GameplayId = index in that sorted order` (0..N-1). The id is therefore a **pure function of the set of gameplay-CVar names** — independent of source file layout, declaration order, discovery order, or which module a CVar lives in. Reordering code never changes an id; only adding/removing/renaming a gameplay CVar does, and only for names at/after the change's alphabetical position — which disturbs nothing durable (the wire is ephemeral-per-build; persistence is name-keyed, C.0.1). Because the phone and desktop builds run the same cook over the same source, they produce the same sorted set → the same ids; the build-version gate (C.3) already guarantees same source, so agreement is automatic.

**Where it lives (decided): codegen, compiled in.** Mirrors the existing cook precedent in the repo (`scripts/gen-font.ps1` → cooked `Private/Cooked/*.h` embedded and checked in). A cook script/step scans the registered `AffectsGameplay` CVars and emits a generated table — `name → GameplayId` (and the reverse) — that the build compiles into the registry. Startup does **not** assign ids at runtime; it loads the generated table. (A cheap dev-only `LUR_ASSERT` may still verify the loaded table covers exactly the registered gameplay set, catching a stale-generated-file mistake.)

**Build-time validation (errors, not runtime asserts):** the cook fails the **build** if the gameplay set exceeds **256** (`u8` overflow) or contains a **duplicate name** — so both are caught at cook time and never reach a device. This is strictly better than the earlier startup assert: the overflow/duplicate checks move left into the build.

**Discovery mechanism (implementation note):** the cook needs to enumerate the `AffectsGameplay` CVars. Options for the scanner, in rough order of robustness: (a) the `LUR_CVAR` macro emits a marker into a dedicated linker section the cook reads from a throwaway host build; (b) a small host-side program that links the registry and dumps it (reuses the runtime registry — most reliable, no parsing); (c) textual scan of `LUR_CVAR(...)` invocations (simplest, but macro-fragile). **Recommend (b)** — a tiny host tool that links the same registry code and prints the sorted gameplay set, feeding codegen — because it can't disagree with what the code actually registers. This introduces a cook/codegen step to the RPS build if one isn't already wired for CVars (the font cook shows the pattern to follow).

## C.1 Live change = a tick-stamped CVar input

A **`AffectsGameplay`** CVar override made in the console mid-match is broadcast like any input (a non-sync CVar edit does nothing on the wire):

- New dev-only message **`MsgCvar`** (next free `EMsgType::GameN` after `MsgAnchor`; compiled only under `LUR_INTERNAL`, so shipping neither sends nor accepts it — determinism/wire untouched, per §3).
- Payload: `[applyTick][gameplayId:u8][editWallClockMs:u64][value]` (value width by CVar type: bool/int/Fixed; **enums travel as their underlying int**; **no float** — AffectsGameplay floats are a compile error, §1). **`gameplayId` is the CVar's 1-byte within-build wire id** (C.0.1). `editWallClockMs` is the wall-clock timestamp of the edit (the resolver key, C.2) — **metadata for conflict resolution only, never fed to the sim**, so it can't perturb determinism (C.2). Both peers map `gameplayId` to the same CVar because the build-version gate (C.3) forces identical builds.
- Both peers apply the resolved override to the sim **at `applyTick`** (a few ticks ahead, like inputs, so it lands before either simulates that tick) → identical sim state. The change enters the flight recorder for free.
- **Non-`AffectsGameplay` CVars never hit the wire** — not hashed, not synced; a purely local preference (render bars, camera, audio).

## C.2 Conflict resolver: last-writer-wins (wall-clock); collision → default

Every override carries the **date+wall-clock time it was set** (`editWallClockMs`). When two overrides of the same CVar meet (live, or merged at match start), resolution is:

1. **Most recent edit wins** — larger `editWallClockMs`.
2. **Exact timestamp collision** → **fall back to the CVar's compile-time default** (drop both overrides). Rationale: identical timestamps mean the system genuinely cannot order the two edits, so rather than invent a winner it resolves to the one value both peers unambiguously agree on — the `constexpr` default. Clean, symmetric, and self-announcing (a colliding tweak visibly snaps to default, signalling "set it again").

- **Determinism safety (the important part):** the timestamp is a *selection key*, not a simulation input. Both peers exchange the same `(timestamp, value)` pairs, run the same comparison, and reach the same outcome — a winning value **or** the default on collision — then only that resolved value enters the sim at `applyTick`. Clock skew can't desync the sim; at worst wrong clocks pick a surprising winner. **Note the one real subtlety:** for the collision→default rule to be deterministic, both peers must see the *same pair of timestamps colliding* — which holds because match-start resolution compares the same exchanged triples on both sides, and a live `MsgCvar` carries its own timestamp both peers read identically. (Millisecond collisions across two independent devices are astronomically rare in practice; the rule exists for totality and for the same-device persisted-vs-live case.)
- **Types:** **Enum = its underlying int** everywhere — value and transport; no special case. **bool:** timestamp settles who set it; a timestamp collision reverts to default like any other value, per rule 2.
- **Re-tuning is symmetric:** the later edit wins outright, whoever makes it — lowering a value from one side just works.
- Each peer stores, per overridden `AffectsGameplay` CVar, the tuple `(value, editWallClockMs)` — in the persistent file too (Addendum B gains a timestamp column, C.4).

## C.3 Full sync at match start

At match start (the existing seed/ready handshake, `LockstepPeer::Init`), before tick 0:

- Each peer sends its **entire set of `AffectsGameplay` CVar overrides** (the persisted set from Addendum B + any set pre-match): a `MsgCvarSync` list of `[gameplayId:u8, editWallClockMs, value]` triples (C.0.1). **No CVar-registry hash is needed** — see the build-version gate below, which subsumes it.
- **Build-version gate (decided: version-only, minimalist).** The lockstep contract already requires identical builds. Enforce it **once, at connect**, before tick 0: both peers exchange a **build fingerprint** (git commit hash + dirty-tree flag, baked in at compile time) and **refuse to connect on mismatch** with a clear dev error. Because identical builds are then guaranteed, everything downstream is automatically consistent: CVar **`GameplayId`s** map to the same CVar on both peers (C.0.1), the wire `ProtocolVersion`, and the sim itself. So the wire keys CVars by a 1-byte **`GameplayId`**, and the build gate makes id agreement free. This is the single authoritative integrity check; nothing redundant layered on top (if the fingerprint ever proves too coarse, the fix is a better fingerprint, not a second checker).
  - **Build the fingerprint to capture same-commit-different-flags:** include the commit hash, a dirty flag, and ideally a config/flag hash, so a phone built with one flag set and the desktop rig with another from the same commit still differ and refuse. (This is why version-only is safe here — the fingerprint is the source of truth.)
- Both peers merge the two override sets with the C.2 resolver **per CVar** (most-recent-edit wins; timestamp-collision → compile-time default), arriving at an identical resolved map, applied before tick 0. Symmetric: no host, both compute the same result from the same triples.
- Registry-hash mismatch → refuse the match with a clear dev error (different builds; the whole lockstep contract already requires same-build, this just names the failure).
- A CVar overridden on **neither** side stays its `constexpr` default (not sent).

## C.4 Interaction with persistence (Addendum B)

- Persistence (B) is unchanged as the *local* store; the sync layer is how those local values reach the peer. A persisted sim override is part of the match-start `MsgCvarSync`.
- **B's file format gains a timestamp column** so last-writer survives restarts: `name = value  @ <iso8601-or-ms>`. A hand-edited line with no timestamp is treated as "epoch 0" (oldest — loses any conflict to a real edit) and re-stamped on next write; document this so hand-edits behave predictably. `reset`/`reset_all` clear the tuple (value + timestamp) as before.
- B.3's "sim overrides staged, not applied at startup" now resolves cleanly: they're applied at **match start via the sync + resolver**, on both peers, deterministically. Solo/disconnected: the local persisted set applies directly (no peer, no resolve).
- B.5-Q6 is **answered** (bidirectional, last-writer-wins with collision→default, not host-authoritative). **B.5-Q7 is answered: per-game persistence files.** Each game gets its own `cvars.cfg` in its own dev config dir (engine CVars registered by a game are written into that game's file). Rationale: two games can define local (non-`AffectsGameplay`) tweaks without clashing, a game's tuning travels with that game, and there's no shared-file coupling between unrelated titles. Keys stay namespaced regardless (`rps.*`, `render.*`), so an engine CVar simply appears in whichever game's file it was overridden from.

## C.5 Open questions (supersede/extend §8)

Conflict resolution is final: bidirectional tick-stamped sync; most-recent wall-clock edit wins; **timestamp collision → revert to compile-time default**; enums are underlying ints.
- The only residual note (not a blocker): resolution assumes roughly-sane device clocks (C.2) — deterministic regardless, but "most recent" means "by device clock." Acceptable for a dev tool.

---

# Addendum D — Desktop CVar Editor Panel (desktop dev/debug builds only)

*2026-07-20. A desktop-only tuning surface: a `--tune`-style flag doubles the window width and fills the right half with a hierarchical CVar editor. Purely a desktop dev/debug convenience — phones and shipping are entirely unaffected. Grounded in `DesktopMain.cpp` at `d1dbb76` (existing `main(argc,argv)` `--flag` loop, `--winw/--winh`, `Window::Create(title,w,h,x,y)`) and the CVar registry (Addendum §1.1).*

## D.0 Engine-owned, never game-customized (governing principle)

This panel — and the console, the dev-GUI layer, the CVar/command systems, persistence, and sync — are **engine tools, identical across every game built on the engine.** A game **cannot customize, restyle, extend, or re-lay-out** any of them. The *only* way a game influences these tools is by **registering CVars and commands**: those registrations change the tool's *contents* (which variables appear, under which categories) but never its *behaviour, layout, or code*.

Concretely, this means:

- The panel's tree structure, widgets, theme, key/gesture bindings, sync rules, and file formats live wholly in engine modules (`Lur::Core`, `Lur::DevGui`, `Lur::DevConsole`, the desktop platform layer). No game code path feeds into them beyond `CVar`/`DevCommand` registration.
- A game registering `rps.counter_mult` under category `"Combat"` makes that row appear under `Game ▸ Combat`. A different game registering `foo.gravity` under `"Physics"` makes *that* appear. The tool is the same tool; the two just enumerate different registries. This is the "logically game-agnostic, contents-vary-by-game" model.
- There is **no per-game hook** to change how the tool looks or works — deliberately. If a tool behaviour needs to change, it changes in the engine, for all games at once.

The rest of this addendum is written to honour D.0 (see the origin-derivation note in D.3 and the registration note in D.4, both corrected so nothing is game-supplied except the CVar declarations themselves).

## D.1 Scope & flag

- **Desktop only, dev/debug only.** Lives behind `#if !LUR_SHIPPING` *and* in the desktop app/`Modules/Platform` desktop path — it never compiles for Android/iOS or shipping. (The dev-GUI layer it draws on, Addendum A, is already dev-only.)
- **New cmdline arg** (joins the existing loop, e.g. `--tune` or `--cvars`): when present, the window is created **twice as wide** as normal — `Create(title, kWinW * 2, kWinH, …)` — and split 50/50: **left half = the game** (rendered exactly as today into a left viewport of width `kWinW`), **right half = the CVar editor panel**.
- Absent (default): unchanged single-width behaviour. Compatible with the existing modes — most useful with `--solo` (one peer + the panel), and usable in two-window lockstep (each window can carry its own panel, or only one; keep it simple — panel per window that passes the flag).
- The width doubling is a **desktop window/layout** concern only; nothing about the game's own rendering, camera, or the sim changes. The left viewport keeps the current aspect/size so the game looks identical to a normal dev window.

## D.2 Layout: two viewports in a double-wide window

- Left `[0, kWinW)`: the game. `GameView::Render` already takes explicit `(WidthPx, HeightPx)`; pass the left half's dimensions and offset its ortho/scissor to that sub-rect (the renderer already supports dynamic viewport/scissor). No game code changes — the host just tells it a smaller width and a left origin.
- Right `[kWinW, 2*kWinW)`: the CVar editor, drawn in the **dev-GUI layer** (Addendum A) with `DevTheme` — so it's visually unmistakable as a dev surface, consistent with the console. Its own scissor to the right sub-rect.
- A thin divider line (DevTheme accent) between the two.

## D.3 The hierarchical CVar menu

A collapsible tree, built from the registry via immediate-mode `DevGui` widgets:

```
▼ Engine
   ▼ Render
        render.debug_bars          [ true      ]
        render.msaa                [ 4         ]
   ▼ Net
        net.keepalive_ms           [ 1000      ]
   ▶ Save
▼ Game
   ▼ Economy
        rps.mine.capacity          [ 300       ]
        rps.miner.cost             [ 30        ]
   ▼ Combat
        rps.counter_mult           [ 2.5       ]
   ▼ Flocking
        rps.boid.sep_radius        [ 12.0      ]
        ...
```

- **Top level = origin: `Engine` vs `Game`.** Per D.0 this must be determined by the **engine**, not declared by a game (a game can't be trusted to, and shouldn't have to, label itself). Mechanism: CVars registered by engine modules (`Lur::*`) are `Engine`; everything else is `Game`. Cleanest implementation that needs no game cooperation: the **engine modules** register with an internal `ECVarOrigin::Engine` marker (they're engine code, so this is engine-owned), and the registry treats any CVar lacking that marker as `Game`. So "Engine" is an allow-list the engine maintains for itself; a game simply registers a CVar normally and it lands under `Game` automatically. No game-supplied origin field.
- **Second level = `Category`** — a **`const char* Category` member on the CVar**, supplied in the CVar declaration (e.g. `CVar<Fixed> CvSepRadius{"rps.boid.sep_radius", F(12), CVarFlags::AffectsGameplay, "Flocking"}`). This is *data the game declares about its own CVar*, not customization of the tool — the panel renders whatever categories happen to exist, identically for every game. CVars sharing a `Category` string group under one header, within their origin; uncategorised → an `Uncategorised` bucket. Engine CVars carry categories the same way (declared in engine code).
- **Rows: name left, value textfield right.** Each row is `Label(name)` + a `TextInput` pre-filled with the current value. The textfield reuses the **console's editing widget** (same `DevGui::TextInput`, same `FromString<T>` parse on commit). Editing + Enter (or focus-loss commit) applies the override exactly as a console `set` would — including, for `AffectsGameplay` CVars, routing through the tick-stamped sync (Addendum C) so a value tuned in the panel during a live loopback match syncs to the peer. Non-`AffectsGameplay` edits apply locally.
- **All categories open/closeable**: each `Engine`/`Game` node and each `Category` node is a collapsible header (click to toggle; ▶/▼). Default all-open (it's a tuning panel; you want everything visible). Open/closed state is dev-UI-local session state (optionally persisted as a local, non-`AffectsGameplay` preference later — not v1).

## D.4 CVar class additions (small, dev-only where possible)

- **`Category` (`const char*`)**: a new member + declaration parameter, supplied where each CVar is declared (game CVars in game code, engine CVars in engine code). It's per-CVar data, not tool customization (D.0). In shipping the CVar is still a `constexpr`-returning value type (§1); `Category`/`Name`/origin are dev-only introspection metadata — keep them inside the same `#if !LUR_SHIPPING` block as the rest of the registry linkage so shipping stays pure data (consistent with §1.1's decision to strip introspection from shipping).
- **Origin (Engine/Game)**: **not a game-set field** (D.0). Derived by the engine — engine-module registrations carry an internal Engine marker; anything else is Game. A game never labels its origin; it just registers.
- Both are metadata for the editor + console grouping; neither affects `Get()` codegen or the shipping build, and neither lets a game alter the tool itself — only what populates it.

## D.5 Reuse & consistency

- The panel is **not** a new widget system — it's `DevGui` (Addendum A) + `DevTheme`, the same primitives as the console, so it inherits the dev look for free and adds no new dependency.
- Editing goes through the **same commit path** as the console (`FromString<T>` → set → persist (Addendum B) → sync if `AffectsGameplay` (Addendum C)). The panel and console are two views onto one CVar system; a value changed in one reflects in the other (both read live `CVar::Get()` / override state).
- Editor and console can be open together; both are dev-GUI-layer surfaces.

## D.6 Acceptance

- [ ] `--tune` (name TBD) opens a double-wide desktop window; game renders correctly in the left half (identical to a normal dev window), panel in the right.
- [ ] Without the flag, window size and behaviour are exactly as today.
- [ ] Tree shows `Engine`/`Game` at top, `Category` beneath, every registered CVar as name + value field; all nodes collapse/expand; default all-open.
- [ ] Editing a field commits via the console's parse/apply path; an `AffectsGameplay` edit during a `--solo`-vs-peer or two-window session syncs to the peer (Addendum C) and persists (Addendum B); a non-gameplay edit applies locally only.
- [ ] Entire feature absent from Android/iOS and shipping builds (no symbols, no window-doubling code).

## D.7 Open question (add to §8)

11. **Flag name:** `--tune`, `--cvars`, or `--panel`? (Recommend `--tune`.) And should the panel width equal the game width exactly (true "two phone displays" — the stated design) or be independently sizable? Recommend: equal halves as specified, `--winw` still governs the game half.

---

# Addendum E — Commands-as-buttons, sliders, and color CVars

*2026-07-20. Panel/console richness: dev-commands appear as buttons in the settings menu (so they need a `Category` too); CVars with a declared min/max render as sliders; a color CVar type renders as a color picker and has dedicated console syntax. Honours D.0 — all engine-owned, populated by registrations. Grounded in `Render::Color { float R,G,B,A }` and `MaterialDesc::Tint/Outline` at `d1dbb76`.*

## E.1 Dev-commands in the panel (buttons)

- `DevCommand` gains a **`const char* Category`** member (declared where the command is registered, like a CVar's — per-command data, not tool customization).
- The settings tree (Addendum D.3) interleaves commands with CVars under the same `Engine`/`Game` → `Category` hierarchy: a command renders as a **button** (label = command name; DevTheme styling) placed in its category. Clicking runs it — the exact same dispatch as typing it in the console, including sim-command routing through the tick stream (Addendum C) where applicable.
- Commands **with arguments**: v1 renders the button plus inline `TextInput`(s) for the args (or a single arg string field), committed on click. Zero-arg commands (`rps.restart`, `net.reset_link`) are a bare button. Keep arg UI minimal; the console remains the power path for complex args.
- Origin (Engine/Game) for commands is engine-derived, same rule as CVars (D.3): engine-registered → Engine, else Game. No game-supplied origin.

## E.2 Sliders from min/max metadata

- A CVar may **optionally** declare a **min and max** (new optional fields in the declaration, numeric types only — int/float/Fixed). Declaring them is per-CVar data (D.0-safe).
- **Widget selection is automatic and engine-owned:**
  - min/max present → **slider** (with the numeric value shown/editable alongside, so you can still type an exact value; clamped to range on commit).
  - no range → the default **text field** (Addendum D.3).
- Range is **advisory UI only**, not a hard invariant on the value: typing out-of-range in the console still works (with a warning) unless we later add a `Clamped` flag — but the slider itself respects the declared bounds. (Decide `Clamped` in review; default: slider clamps, console warns-but-allows.)
- Sliders are `DevGui` widgets (Addendum A) in `DevTheme`. For `AffectsGameplay` CVars, dragging commits through the sync/persist path like any edit — live-tunable during a loopback match.
- **Determinism note:** a slider on an `AffectsGameplay` Fixed/int CVar must commit **discrete** values through the tick-stamped sync, not a continuous stream per drag-pixel (that would flood the wire and the flight recorder). Commit on drag-release, or throttle to a sensible step; the console/panel share one commit path so this is one place to get right. (The value still enters the sim via the per-tick latch, §1 determinism rule 2 — so even a mid-drag commit is applied cleanly at a tick boundary, never mid-Step.)

## E.3 Color CVar type + color picker

- **New CVar type `CVar<Color>`** using the existing `Render::Color { float R,G,B,A }` — so color CVars feed `MaterialDesc::Tint`/`Outline` and the RTS silhouette-tint path directly. Colors are the intended tool for tuning visuals (unit tints, HUD accents, DevTheme itself could even be CVar-driven later).
- **Almost always `AffectsGameplay = false`** — colors are rendering, not simulation, so they don't sync and don't touch the hash. (If a game ever made a color gameplay-relevant, the flag is available, but the default and expectation is local visual tuning.)
- **Panel: a color picker widget — built in-house**, specified in full in its own document: **`dev-color-picker-tech-spec.md`**. Summary: dev-only, on `DevGui`/`DevTheme` (no IMGUI lib), rendered via the per-vertex `DrawGlyphs` path for gradients + flat quads for handles. **Phased** — v1 (RGBA sliders + swatch + hex field, ~1 day, acceptance-blocking) unblocks the whole `CVar<Color>` feature; v2 (SV square + hue strip + alpha, the UX-friendly target) is a tracked follow-up. The `CVar<Color>` type and the `.r/.g/.b/.a` console syntax work regardless of the picker's phase. See that spec for HSV math, the drag model, and the lossy-edge working-state correctness that makes or breaks the feel.
- **Console syntax** for `CVar<Color>`:
  - **All four channels, space-separated:** `theme.accent 0.1 0.9 0.9 1.0` (R G B A, floats 0..1; document whether 0..255 ints are also accepted — recommend 0..1 floats canonical, ints optional).
  - **Single channel via `.r/.g/.b/.a` suffix on the name:** `theme.accent.g 0.5` sets only the green channel, leaving the others. The four suffixes `.r .g .b .a` are the alternatives. Parsing: the console strips a trailing `.<channel>` from the CVar name, resolves the base `CVar<Color>`, and applies the single component. Completion (Addendum §4) offers both the base name and the four `.channel` forms.
  - Reading `theme.accent` (no value) prints all four; `theme.accent.b` prints just that channel.
- `FromString` gains a `Color` path (4-float parse) and the console gains the `.channel` sub-target parse (a small, color-specific extension to the line parser).

## E.4 CVar/command class additions (summary; all dev-only metadata)

| Field | On | Purpose | Shipping |
|---|---|---|---|
| `Category` (`const char*`) | CVar **and** DevCommand | tree grouping under origin | dev-only metadata; not compiled into shipping value reads |
| min / max (optional, numeric) | CVar | advisory range → slider widget | dev-only; no effect on `Get()` |
| `Color` value type | `CVar<Color>` | color picker + `.channel` console syntax | ships as `constexpr Color` value like any CVar (§1) — the *picker/parse* is dev-only |

All consistent with §1 (shipping reads compile to the raw `constexpr`; metadata and widgets are `#if !LUR_SHIPPING`) and D.0 (engine-owned tool; declarations populate it).

## E.5 Acceptance (extends D.6)

- [ ] Commands appear as buttons in their `Engine`/`Game` → `Category` node; click runs them (same path as console); zero-arg and simple-arg forms both work.
- [ ] A numeric CVar with declared min/max renders as a slider (value still typeable); without, a text field; `AffectsGameplay` slider edits commit discretely through sync/persist.
- [ ] `CVar<Color>` renders a color picker (v1: RGBA sliders + swatch + hex field, acceptance-blocking); edits update the bound color live and persist locally. SV-square/hue-strip picker (v2) tracked separately as a UX upgrade.
- [ ] Console: `name r g b a` sets all channels; `name.r 0.5` sets one; reading prints channel(s); completion offers base + `.r/.g/.b/.a`.
- [ ] Entire feature set absent from shipping (no widgets, no parse extensions, values are plain `constexpr`).
