# Decision: Render-thread decouple (option E)

**Chosen:** Move the iOS render loop off the CADisplayLink main-thread callback onto a **dedicated render thread**, with FIFO as the single vsync clock. Extends the #69 model (sim + net already off the vsync cadence); rendering is the last piece still on the display heartbeat.

**Explicitly NOT doing:** abandon MoltenVK, change graphics API, or write a native Metal backend. Still 100% Vulkan → MoltenVK → Metal. Only *which thread runs the loop* changes.

## Why (from research/ios-moltenvk-fps.md)
- Root cause = the CADisplayLink callback **blocks on `[CAMetalLayer nextDrawable]` on the main thread with no overlap** → 2-frames/3-refreshes beat = stable 40fps. Real work ~3ms.
- A dedicated render thread lets frame N+1's CPU work overlap frame N's drawable/vsync wait → clean 60. This is the "proper" fix and industry-standard (Unreal/Unity/etc. all run a render thread — independent of the MoltenVK-vs-Metal axis).
- Cheap fixes (image-count match, layer delegate) may also work, but the user chose the architectural fix as the target.

## Design sub-decisions (decided from research; open to override)
1. **Cadence — render thread FREE-RUNS, FIFO is the single vsync clock.** NOT CADisplayLink-signaled. Research: CADisplayLink + FIFO = two vsync gates that beat (aerofly #581). The render thread loops; MoltenVK's FIFO `nextDrawable` blocks it to the display refresh. Drop CADisplayLink for rendering. (Keep a CADisplayLink only if some non-render pacing needs it — currently nothing does.)
2. **Buffering — images == frames-in-flight, matched.** Make both a single matched constant. Start at the #1407 clean case; test 2/2 vs 3/3 on device. (We already have the per-slot pipelining machinery from today.)
3. **Per-frame autorelease pool** wrapping each render-thread iteration (drains the retained CAMetalDrawable; else the pool starves and nextDrawable waits a full vsync).
4. **Keep the IMMEDIATE-present diagnostic as Phase 0** — 1-line, ~5min, confirms the cause is vsync-pacing before we invest in the thread rework. If fps doesn't exceed 60 under IMMEDIATE, the diagnosis is wrong — stop and rethink.
5. **Housekeeping — revert the async-submit env var** (`MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`) back to default. On a render thread the submit block is off the main thread anyway, and default sync-submit is the simplest/best-understood baseline. Keep the 2-frame pipelining machinery.

## Key risks to address in the plan
- **CAMetalLayer / renderer lifecycle is main-thread (UIKit).** Init, resize, and the #73 reattach/heal all run on main. The render thread uses the renderer. Need clean ownership + a synchronized handoff (render thread owns the render loop after init; resize/reattach requests cross via a flag the render thread consumes at a safe point — mirror the existing `NeedsRecreate`).
- **Input/camera handoff.** Touch → camera is on the main thread (UIKit). The render thread reads camera Y. Cross via an atomic/snapshot (same pattern as #69's atomics + the sim→render Mailbox).
- **Shutdown/scene-inactive ordering** (#73): render thread must stop cleanly on background/teardown before the layer/renderer is destroyed.
- MoltenVK bg-render-thread is safe (presents via plain command buffer, not a CA transaction — agent 2 confirmed), so `presentsWithTransaction` (main-thread-only) is not needed and not used.
