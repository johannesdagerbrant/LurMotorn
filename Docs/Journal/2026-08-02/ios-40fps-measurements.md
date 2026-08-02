# iOS 40fps — device measurement record (#103)

**Frozen snapshot, 2026-08-02.** All numbers measured on hardware; this consolidates the device data gathered across the #103 diagnosis (also in the issue's comment thread). Live status: **#103** (diagnosis) / **#183** (implementation plan). Re-verify any code path against HEAD before acting — paths/symbols drift.

## Rig
- **Device:** iPhone 11 Pro (iPhone12,3, A13, **60Hz non-ProMotion** panel), iOS 26.5.2.
- **Renderer:** shared Vulkan backend via **MoltenVK 1.4.1 (static xcframework)** on a `CAMetalLayer`; render driven by a **CADisplayLink callback on the main thread**. Present mode `VK_PRESENT_MODE_FIFO_KHR`.
- **Comparison:** Galaxy A14 (SM-A145R, Mali, **native Vulkan**) holds a clean 60fps on the same C++ core + scene (~2.7M px both).
- **Method:** solo/pre-match (empty field + HUD = the fixed cost floor; issue notes cost is flat vs unit count). TRACE line (avg/max ms) emitted every 2s by the sim thread from the global `Lur::Trace` registry; fps from `HEARTBEAT presented=` delta (a `presented` step of +80 per 2s = 40.0 fps; +120 = 60fps). Capture via `pymobiledevice3 syslog live -pn OnlyRps -o <file>`.
- **TRACE scopes added for this investigation** (all `LUR_TRACE`-gated, no-op in Shipping): `gpu.wait` (WaitForFrame fence+acquire), `frame.render` (whole CADisplayLink callback body), `render.view` (`GameView::Render` = BeginFrame→draws→EndFrame); sub-scopes `rv.world`/`rv.gui`/`rv.submit` (render-pass phases in GameView); `es.endcb`/`es.submit`/`es.present` (inside `VulkanBackend::EndFrame`). Commits: `60643b1`, `e7ab46c`, `d4afb0c`.

## Experiment ladder and results (ms = avg per 2s window)

### 1. Baseline TRACE split — native resolution, FIFO, 1 frame in flight
```
gpu.wait     ≈ 1.34    (GPU fence+acquire idle — small)
render.view  ≈ 18.0    (BeginFrame→record→EndFrame, CPU) — the whole frame
frame.render ≈ 18.0
fps          = 40.0    (presented +80/2s over 28s)
```
→ GPU is not the limiter; ~18ms is CPU-side in the render/submit path. NOT fillrate.

### 2. Resolution A/B — `rps.dev.render_scale` sweep (via LUR_AGENT autopilot)
| render_scale | drawable    | fps  | gpu.wait | render.view |
|--------------|-------------|------|----------|-------------|
| 1.0×         | 1125×2436   | 40.0 | 1.32     | 18.0        |
| 0.7×         | 787×1705    | 40.0 | 0.90     | 18.4        |
| 0.5×         | 562×1218    | 40.0 | 0.68     | 18.7        |
→ **fps dead flat at 40 across ¼-pixel-count range.** gpu.wait scales a little (real but tiny GPU fill); render.view flat. Categorically **not fillrate/overdraw**. (Also verified the `render_scale` swapchain-recreate path works live: drawable resized with no hitch.)

### 3. `render.view` sub-scopes — where the 18ms lives
```
rv.world  ≈ 0.95   (BeginFrame + field/grid/units/text RECORDING)
rv.gui    ≈ 0.87   (BeginGui + HUD + dev overlay RECORDING)
rv.submit ≈ 16.2   (EndFrame)                                  ← ~90% of the frame
```
→ Draw *recording* is only ~1.8ms. NOT a draw-call-count problem. It's all in EndFrame.

### 4. `EndFrame` split — which call holds the 16ms
```
es.endcb  ≈ 0.003  (vkCmdEndRenderPass + vkEndCommandBuffer)
es.submit ≈ 16.1   (vkQueueSubmit)                             ← ALL of it
es.present≈ 0.10   (vkQueuePresentKHR)
```
→ The entire frame is one `vkQueueSubmit`, rock-stable at ~one 60Hz refresh (16.0–16.3 avg, ~29–30 max = occasional 2-refresh). NOT the present call.

### 5. Fix attempt A — 2 frames in flight (Vulkan-level pipelining, `03377bb`)
```
                 1-frame → 2-frame
gpu.wait          1.32   → 0.09     (fence wait eliminated — CPU no longer waits on prev fence)
es.submit        16.1    → 16.2     (UNCHANGED)
fps              40.0    → 40.0     (UNCHANGED)
```
→ More frames-in-flight at the fence level did nothing; the block is inside MoltenVK's `vkQueueSubmit`, not the Vulkan fence graph. (Pipelining kept — correct, validation-clean, helps Android.)

### 6. Fix attempt B — async submit `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` (`df11dce`)
```
gpu.wait     ≈ 0.09
rv.world     ≈ 20.6    ← the ~16ms wait LANDED HERE (BeginFrame / images-in-flight wait)
es.submit    ≈ 0.03    ← collapsed
es.present   ≈ 0.03
frame.render ≈ 21.6    ← the CADisplayLink callback body now overruns the 16.6ms refresh
fps          = 40.0    (UNCHANGED)
```
→ Async submit collapsed `es.submit` but the ~16ms `nextDrawable`/vsync wait simply **relocated** into BeginFrame. **The same wait has now been relocated three times (acquire → submit → begin-frame) and fps stayed 40.0 every time.**

## What the data proves (feeds the root cause in #103/#183)
- Real per-frame work ≈ **3ms** (es.submit 0.03 + rv.gui 0.9 + rv.submit 0.07 + gpu.wait 0.09 in exp 6); the other ~16–18ms is one CPU block on the drawable/vsync.
- The block is `[CAMetalLayer nextDrawable]` on the vsync-throttled drawable pool; MoltenVK acquires the drawable lazily at encode, so it **relocates** with config but the vsync gate is constant → moving it never changes fps.
- **fps = exactly 40.0 across ALL variations** (1/2 frames-in-flight, sync/async submit, render_scale 1.0/0.7/0.5). A stable 40 on a 60Hz panel = a 2-frames-per-3-refreshes vsync **pacing beat**, caused by the single-threaded CADisplayLink callback blocking on the drawable with no frame overlap (default 3-drawable pool turns a 30-cap into the 40-beat). See `ios-40fps-research.md` for the mechanism + citations, and MoltenVK #1407 for the source-confirmed FIFO image-count trigger.

## Config confirmed in code (2026-08-02 HEAD)
- Swapchain requests `minImageCount+1` → **3 images**; renderer runs **2 frames in flight** → the #1407 mismatch.
- `CAMetalLayer.delegate` **unset**; `presentsWithTransaction`, `preferredFrameRateRange`, `maximumDrawableCount` **all unset** (defaults).
