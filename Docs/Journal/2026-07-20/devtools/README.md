# Dev Tools — Agent README (console, CVars, dev-GUI, color picker)

*2026-07-20. Entry point for the **developer-tools** feature set (distinct from the RPS gameplay planning docs, which have their own README). Suggested home: `Docs/Planning/devtools/`. Grounded against the repo at `d1dbb76`.*

**What this feature set is:** an engine-owned, dev/debug-only tooling layer — an Unreal-style **console**, a **CVar** system (named, overridable, persisted, peer-synced), **dev-commands**, an in-house immediate-mode **dev-GUI layer**, a desktop **CVar editor panel**, and a **color picker**. All of it compiles out of shipping entirely; games interact only by *registering* CVars/commands, never by customizing the tools.

---

## 1. The three documents (read in this order)

1. **`dev-console-cvar-tech-spec.md`** — the main spec. Read fully first; the other two hang off it.
   - Core (§0–2): the shipping contract, the `CVar<T>` mechanism, dev-commands, console UX, input plumbing.
   - **Addendum A** — own dev-GUI layer (no IMGUI lib); the third render pass; DevTheme.
   - **Addendum B** — CVar persistence (readable per-game `cvars.cfg`, `reset`/`reset_all`).
   - **Addendum C** — bidirectional peer sync of gameplay CVars; identity, ids, conflict rule, build gate. **Read C.0–C.0.2 carefully** — that's where identity/ids/determinism live and it's the most intricate part.
   - **Addendum D** — desktop double-wide CVar editor panel.
   - **Addendum E** — command buttons, sliders, color CVars.
2. **`dev-color-picker-tech-spec.md`** — standalone spec for the color-picker widget (expands Addendum E.3). Read when building the picker; §4 (working-HSV-state) and the two-layer SV-square (§3.2) are the correctness-critical bits.
3. **`spike-phone-softkeyboard.md`** — a **timeboxed investigation** (not a feature) for phone text entry. Run this *before* attempting phone console text input; it has a hard 2-day box and a defined fallback.

*(All other `.md` in this folder tree belong to the RPS/engine planning set — see that README. This README covers only the three above.)*

## 2. Decision status — everything is final except one spike

The design is **settled**; do not reopen decisions, implement them. The load-bearing ones an implementer must honor:

- **Shipping contract:** a CVar read compiles to the raw `constexpr` default in shipping; the one `#if` lives *inside* `CVar::Get()`, never at call sites. **Build the disassembly-diff CI check FIRST**, before migrating any tunable — it's the enforcement, and the "zero overhead" claim is unproven until it exists (§1, §7).
- **Declaration:** the `LUR_CVAR(name, default, flags, category)` macro — declares the `constexpr` CVar + a dev-only registrar. No hand-written registration; no reads before `main()` (§1.1).
- **Sync boundary:** `AffectsGameplay` flag (default **false**). Only gameplay CVars sync/latch/hash; everything else is local. `CVar<float>` + `AffectsGameplay` is a **compile error** (§C.0, §1).
- **Determinism = two independent guarantees:** (1) no gameplay floats [value safety]; (2) per-tick frozen snapshot the sim reads from [timing safety] — folded into `StateHash`. Both required; neither substitutes for the other (§1 determinism).
- **Identity:** name = durable identity (source, persistence, console); **1-byte `GameplayId`** = wire encoding; the mapping is **derived by the build/cook** (lexicographic sort of gameplay-CVar names), codegen'd, never authored; 256 cap + duplicate-name = **build errors** (C.0.1, C.0.2).
- **Sync:** bidirectional tick-stamped (`MsgCvar`, dev-only opcode); full sync at match start; conflict = most-recent wall-clock edit wins, **timestamp collision → default**; enums are underlying ints (C.1–C.3).
- **Same-build gate:** exchange a build fingerprint at connect, refuse mismatch; this is the single integrity check (C.3).
- **Dev-GUI:** built in-house on `Modules/Hud` primitives + a third no-depth render pass (`BeginDevGui`); DevTheme (flat, mono, charcoal, own font, `DEV` watermark). No IMGUI lib (Addendum A).
- **Persistence:** per-game `cvars.cfg`, human-readable, name-keyed, timestamp column; separate from `Store` (Addendum B).
- **Color picker:** in-house, phased — **v1 (RGBA sliders + swatch + hex) is acceptance-blocking**, v2 (SV square + hue strip) is the UX upgrade; two-layer gradient for the SV square; keep HSV as working state (picker spec).

**The one open item:** phone soft-keyboard text entry — status is "prove on hardware, then spec." Everything else is decided. See the spike doc; its fallback (phone console = tap-to-select only) means this never blocks anything, and desktop text entry doesn't depend on it.

## 3. How an agent should work from these

1. **Backend first, tools second.** The high-value, low-risk core is: `CVar<T>` + `LUR_CVAR` macro + registry + `FromString<T>` + per-game `cvars.cfg` load/save + the disassembly-diff CI check. This alone enables text-file tuning with hot-reload — a large chunk of the value before any console/GUI exists. Land it, migrate the boid + economy tunables (the first `AffectsGameplay` set), confirm the CI check passes.
2. **Then the console + dev-GUI layer** (Addendum A + §4), then the desktop panel (D), then sliders/buttons/color (E), then the picker (its spec, v1 then v2).
3. **Sync (Addendum C) lands with or just after the console**, since editing a gameplay CVar mid-match needs it; the per-tick latch (§1) and the build-fingerprint gate (C.3) are prerequisites for any gameplay-CVar editing to be determinism-safe.
4. **Run the soft-keyboard spike** only when phone text entry is actually wanted; until then phones use tap-to-select.
5. **Re-verify against HEAD.** Specs cite `Renderer.h`, `DesktopMain.cpp`, `LockstepPeer`, the `gen-font.ps1` cook, `Modules/Hud` widgets at `d1dbb76` — treat every code reference as a hint, confirm against current code before implementing.

## 4. Key repo anchors the specs lean on (verify these still hold)

- `Render::Color { float R,G,B,A }` + `MaterialDesc::Tint/Outline` — the color-CVar + picker target.
- `IRenderer`: `BeginFrame`/`BeginGui`/`EndFrame`, no-depth ortho overlay, `DrawInstances` (per-quad tint), `DrawGlyphs` (per-vertex color — the gradient path). Dev-GUI adds `BeginDevGui` as a third pass.
- `Modules/Hud`: `TextField`, `Dropdown`, `DebugOverlay` — the console/panel primitives.
- `cmake/EngineFlags.cmake`: `LUR_CONFIG` → `LUR_SHIPPING`/`LUR_INTERNAL`/`LUR_ASSERTS` — the compile-out mechanism.
- `scripts/gen-font.ps1` → `Private/Cooked/*.h` — the **cook/codegen precedent** the `GameplayId` table follows.
- `DesktopMain.cpp`: `main(argc,argv)` `--flag` loop, `--winw/--winh`, `Window::Create(title,w,h,x,y)` — where the `--tune` double-wide panel and any dev flags hook in.
- `LockstepPeer`: tick-stamped masks, `MsgAnchor` on `EMsgType::Game1`, seed-based `Init` — where `MsgCvar`/`MsgCvarSync` and the build-fingerprint handshake slot in.
- Platform `Window::TakeKeys()` (VK stream) — **insufficient for text**; the specs add a UTF-8 char stream + key down/up/repeat (§4.1). The soft-keyboard spike is the phone half of that.

## 5. Build/CI additions these introduce

- **Disassembly-diff CI** (shipping zero-overhead proof) — build before migrating CVars.
- **No-double-branch lint** — forbid `LUR_SHIPPING`/`LUR_INTERNAL` `#if` at CVar call sites in sim/gameplay code.
- **CVar id cook step** (codegen `name↔GameplayId`, lexicographic; 256/duplicate = build error) — follows `gen-font.ps1`.
- **Shipping-absence check** — `Modules/DevGui`/`DevConsole` produce no symbols, no dev font, in a `LUR_SHIPPING` link.
- **Determinism soak** — desktop `--auto` with a scripted CVar-edit schedule over loopback → hash-identical; a deliberately un-latched/un-synced gameplay CVar must *fail* it (proves the guards).
