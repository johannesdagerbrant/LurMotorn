# Dev-tools: progress + the console design pivot — 2026-07-21

*Frozen snapshot. Grounded against HEAD `5e83c98` (dirty, `Development`). Issues win on
anything current (#110 epic and children); read this for the rationale and the pivot.*

## What shipped today

The dev-tools epic (#110) went from spec to working, on-device, tooling:

- **#111 — CVar backend** (`Modules/Core/CVar.h`). `CVar<T>` with the zero-overhead shipping
  contract (`Get()` folds to the `constexpr` default when `LUR_SHIPPING`; the one `#if` lives
  inside `Get()`, never at call sites), a dev-only polymorphic `ICVar` registry
  (`Name/Category/AffectsGameplay/SetFromString/Reset/Overridden/ValueString/…`),
  `LUR_CVAR`/`LUR_CVAR_ENGINE`, and the disassembly-diff CI guard. All 14 boid/combat/economy
  knobs in `Rps/Tunables.h` migrated off `constexpr` onto gameplay CVars via the
  `LUR_RPS_GAMEPLAY_CVARS` X-macro (→ `CvSnapshot`/`ECvId`/`GameplayIdForName`).
- **#112 — determinism + sync.** Per-tick `CvSnapshot` latched at tick top and **folded into
  `StateHash`** (a mis-latched/mis-synced gameplay CVar surfaces as a located desync);
  `LiveCvLatch` for solo live re-latch; `MsgCvar`/`MsgCvarSync`/`MsgFingerprint` over
  `LockstepPeer` (dev-only `EMsgType` Game3/4/5), thread-safe `QueueGameplayCvar` (UI→sim),
  last-writer-by-wall-clock resolver, build-fingerprint gate. Proven in `rps_net_tests` over
  loopback; **live two-phone convergence still needs a second device to close #112's last box.**
- **#113 — dev-GUI layer (partial).** `IRenderer::BeginDevGui()` third pass + `BeginDevGuiLayer`
  shipping-guarded wrapper; `DevTheme` materials (charcoal panel / cyan accent / key face); the
  `Lur::DevGui::Numpad` widget. The full immediate-mode widget set and the platform char/text
  stream from the spec are **not** built — see the pivot below for why the console no longer
  needs them.
- **#108 / gate speedup.** Mine economy retuned (fewer clusters, 20× gold) and the match-open
  "gate" cut from ~246 s to ~10 s.
- **Persistence + on-device.** `cvars.cfg` load/save (timestamp column); the whole pipeline —
  registry → edit → latch → hash → persist — verified on the Galaxy A14 (NDK Clang) and on the
  desktop.

## The pivot: the console is ONE cross-platform tool

The 2026-07-20 specs described an **Unreal-style** console (bottom text-input + completion list +
scrollback, free-text driven) plus a separate desktop **`--tune`** editor that split a double-wide
window (game left, cvar panel right). Building toward that produced a mess: the `--tune` path
**reused the phone's cvar overlay** as the right-half panel, and desktop input was bent to fit it
(arrow-key nudges, `SetDevSplit`, `TuneMode`). That reactive morphing is the "frankenstein" we
tore out today.

**New, settled direction:**

- **The Console is one tool with one UI on both platforms** — the cvar-browser overlay we built:
  every `AffectsGameplay` CVar shown upfront, grouped into collapsible **category dropdowns**
  (`[+]`/`[-]` headers), each row a **name column + right-aligned boxed value column + `R` reset
  button** (reset is accent when overridden, dim at default). Pointer-driven: tap a header to
  fold, tap a row to select + open the **numpad** for a precise value, Enter commits (persist +,
  in a live match, peer sync). No free-text field, no Unreal-style completion list.
  - **Phone:** opens on a **two-finger triple-tap**; top-right **X** closes. (adb can't inject
    multitouch — the gesture is verified by hand.)
  - **Desktop:** opens on the **§ key** (physical scancode `0x29`, the key left of `1`,
    layout-independent — Swedish §, US backtick; mirrors the F1 overlay-toggle pattern in
    `Win32Window`); the mouse drives the identical overlay (click = `DevTap`); X or § closes.
  - Verified on both: the Galaxy and the desktop show pixel-identical layout.
- **`--tune` is deleted** — the flag, the double-wide window, `SetDevSplit`/`SetTuneMode`,
  `GameW*0.5`, the arrow-nudge block, and `DevSelectMove`/`DevAdjustSelected`. `GameView` no
  longer knows about splits.
- **`ConsoleModel`** (`Modules/DevConsole`: completion, MRU, command/CVar dispatch, scrollback)
  is **built and unit-tested but not wired to a front-end.** It's kept for a *possible* future
  desktop free-text console and for the CVarEditor's completion — not deleted, but not on the
  critical path.

### #118 soft-keyboard spike → NO-GO

Raising the OS soft keyboard on a `NativeActivity` failed (`showSoftInput` never served the view;
`addContentView` black-screened). Decision: **the numpad is the phone text-entry answer** — no OS
keyboard. This is exactly the spec's allowed fallback, so nothing is blocked. #118 closed.

## The second tool: CVarEditor (was "tune panel"), desktop-only, not yet built

The two-tool split still holds, but the second tool is renamed and re-scoped. The **CVarEditor**
is a **desktop-only** GUI editor (it wants screen real estate phones don't have) that works like a
traditional Windows editor: category tree, click/keyboard-select a row → highlight, edit by typing
a value or ←/→ nudge (Shift = bigger). It is **its own tool**, decoupled from the console — not a
reused overlay. It's the one dev-tool that genuinely needs the platform **char/text input stream**
from #113 (free-text typing), so that stream is resurrected *for the CVarEditor*, not the console.
Tomorrow's starting point (#115, rewritten).

## Divergences from the 2026-07-20 specs (issues are authoritative)

| Spec (2026-07-20) | As-built / new direction |
|---|---|
| Unreal-style console (text-input + completion + scrollback) | Cross-platform cvar-browser overlay; no free-text field |
| Phone console opens on **three-finger** triple-tap | **Two-finger** triple-tap |
| Desktop `--tune` double-wide split panel | Deleted; desktop drives the same console via § |
| Char/text input stream needed for the console | Console needs none; stream deferred to the CVarEditor |
| Dev-commands wired into the console | `ConsoleModel` built, not yet wired to any front-end |

## Next

1. **CVarEditor** (#115, rewritten) — desktop-only editor tool; needs the #113 char stream.
2. **Two-phone sync convergence** — close #112's last acceptance box on a real device pair.
3. **Dev-commands + `ConsoleModel` wiring** — decide whether the desktop grows a free-text
   console or the CVarEditor absorbs completion; until then #114's command layer is deferred.
