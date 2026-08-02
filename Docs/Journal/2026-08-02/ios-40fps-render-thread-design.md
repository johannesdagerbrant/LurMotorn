# Render-thread decouple — concrete design (iOS)

## Thread ownership after the change
Mirrors #69 (sim/net already off the vsync cadence). Three threads:

| Thread | Owns | Does |
|---|---|---|
| **Main (UIKit)** | UIWindow/UIView/`CAMetalLayer`, lifecycle | Touch events → push raw event to a queue; #73 window-heal; publish drawable-size + safe-area insets + layer-ready/resize/teardown handshake to render thread. **No rendering, no `_View` access.** |
| **Sim** (exists) | `_Lp`/`_SoloSim`, `_Session`, scores | ~500Hz sim + net pump; publishes Snapshot via `_Mailbox` + score/state atomics (unchanged) |
| **Render** (NEW) | `_Renderer`, `_View` (GameView), `_Cam`, `_DevGesture`, `_Snap` | Free-running loop: drain touch queue → apply input; reflect sim atomics into `_View`; consume Mailbox; update camera; `_View.Render` (WaitForFrame/Begin/draw/End — FIFO blocks HERE, off main) |

**Single vsync clock:** the render thread free-runs; MoltenVK FIFO `nextDrawable` throttles it to the refresh. **The CADisplayLink render driver is removed** (no more double vsync gate). Each render iteration is wrapped in its own `@autoreleasepool` (drains the retained CAMetalDrawable).

## The touch-event queue (core of the refactor)
Today the UIKit touch handlers (`touchesBegan/Moved/Ended`, main thread) directly call `_View.PlateAt / BeginPlaceDrag / ResolvePlacement / UpdatePlaceDrag / PressProductionButton / DevScroll / DevTap / OnTap / EndPlaceDrag`, `_Cam.Begin/Move/End`, and `_DevGesture.*`. All of that state now lives on the render thread, so:

- **Main handlers become thin:** package each touch into a POD `{ phase (Began/Moved/Ended/Cancelled), x, y (already ×contentsScale), pointerCount, ns }` and push to a thread-safe queue. Nothing else.
- **Render thread drains the queue** at the top of each frame and replays the EXACT existing logic (same `_View`/`_Cam`/`_DevGesture` calls, in order) — now single-threaded on the render thread. `placeLocal:`/`RouteLocalEvent` already enqueues to the sim thread (thread-safe) and is called from the render thread unchanged.
- Queue impl (YAGNI): a mutex-guarded small vector with swap-on-drain, or a fixed ring. Touch rate is low and drained once/frame — no lock-free MPSC needed.
- **Cost:** placement-drag / tap hit-testing gains ≤1 frame (~16ms) of latency (was synchronous in the handler). Acceptable; same model the sim input already uses.

## Lifecycle / resize / teardown (highest risk)
- **Renderer owned by the render thread.** Main creates the `CAMetalLayer` + view (UIKit), then signals "layer ready"; the render thread does `_Renderer->Init(layer)` + `_View.CreateResources` and runs the loop. (Or init on main once, then hand the loop to the thread — decide at impl; owning all Vulkan on the render thread is cleaner.)
- **Resize:** main sets `layer.drawableSize` + an atomic `resizeRequested`; render thread calls `Resize()` (→ existing `NeedsRecreate`) at a safe point. `render_scale` CVar path folds in here.
- **#73 reattach/heal (main rebuilds UIWindow/view/layer + renderer Shutdown/Init):** needs a **pause/ack handshake** — main signals pause, render thread parks at a safe point and acks, main rebuilds + republishes the layer, render thread re-inits and resumes. This is the gnarliest piece and the main source of risk.
- **Shutdown/background:** stop+join the render thread BEFORE destroying the layer/renderer (mirror `_SimRunning`/`_SimThread` join in `dealloc`).
- Safe-area insets: main reads `self.view.safeAreaInsets` (main-only) → publishes to atomics; render thread consumes for `_View.SetInsets`/camera clamp.

## What stays / what's cut (YAGNI)
- Keep: 2-frame pipelining machinery, TRACE scopes, `render_scale` CVar.
- One render thread only — no job system / thread pool.
- Simple mutex queue — no lock-free structure.
- No present-timing extension (VK_GOOGLE_display_timing), no `presentsWithTransaction` (bg thread + MoltenVK command-buffer present makes it unnecessary).
- Buffering: images == frames-in-flight as a matched constant (shared backend); revert async-submit env var.
