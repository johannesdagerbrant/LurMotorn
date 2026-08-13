# Interview: iOS MoltenVK FPS — breaking the 40fps ceiling (#103)

**Date:** 2026-08-02
**Status:** Complete — plan ready (see `_summary.md`)

## Context

RPS on iPhone 11 Pro (iPhone12,3, A13, **60Hz** panel, iOS 26.5) is stuck at a **stable, exact 40fps** while the *weaker* Galaxy A14 (Mali, native Vulkan) holds a clean 60fps. Same C++ core, same shared Vulkan renderer, same scene (~2.7M px both). iOS renders via **MoltenVK (static xcframework, v1.4.1)** on a `CAMetalLayer`, driven by a **CADisplayLink callback (`renderFrame`) on the main thread**.

### What today's #103 profiling established (all measured on device)
- The frame is **~3ms of real CPU+GPU work** plus one **~16ms drawable/vsync wait** that RELOCATES but never shrinks.
- TRACE scopes drilled it: `render.view` (18ms) → `rv.submit`/EndFrame (16ms) → **`es.submit` = `vkQueueSubmit` (16ms)**; `es.present` ~0, `es.endcb` ~0, draw recording (`rv.world`+`rv.gui`) ~1.8ms, GPU fence-wait `gpu.wait` ~1.3ms.
- **Two fixes attempted, neither moved fps off 40.0:**
  1. **2-frame pipelining** (per-slot cmd buffer/acquire-sem/fence + per-image present sems + images-in-flight). Result: `gpu.wait` → 0.09ms (CPU runs ahead of the fence) but `es.submit` stayed 16ms, fps 40. **Kept** (correct, validation-clean, helps Android).
  2. **`MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0`** (async submit, safe because of #1). Result: `es.submit` → 0.03ms (collapsed!) but the 16ms **moved into `rv.world`/BeginFrame** (the images-in-flight fence wait); `frame.render` (the CADisplayLink callback body) = 21.6ms; fps STILL 40. Currently on master + device; candidate to revert.
- **Invariant across EVERYTHING:** fps = exactly 40.0 (presented +80/2s) for 1-frame, 2-frame, async submit, and every render_scale (1.0/0.7/0.5x). Resolution A/B was flat → NOT fillrate.

### The core question this research must answer
Why does a 60Hz iPhone yield a **stable 40fps** (not 60, not 30) when real work is ~3ms, and the ~16ms wait is just the CPU parking on a `CAMetalDrawable`? The wait relocating without changing fps means the ceiling is NOT in the submit/fence path — it's in **how MoltenVK/CAMetalLayer/CADisplayLink pace presentation on iOS**. User's hypothesis (shared): since the weaker Android Vulkan path has NO rendering problem, this is **iOS/MoltenVK-specific, not a generic pipeline problem**.

## Themes Discovered
- **Root cause (3 axes agree):** 40.0fps = a present/vsync PACING BEAT (2 frames/3 refreshes), not load (~3ms work). The block is `[CAMetalLayer nextDrawable]` on the vsync-throttled drawable pool; it relocates with MoltenVK's encoding but the vsync gate is constant.
- **Source-verified (MoltenVK #1407):** FIFO + **3 images caused this beat, 2 gave clean 60**; a frames-in-flight ≠ image-count MISMATCH triggers it. **Our state: 3 images + 2 frames-in-flight = the mismatch.** `CAMetalLayer.delegate` unset (User Guide violation).
- **Dead ends:** MAILBOX unsupported on MoltenVK (==FIFO); maximumDrawableCount=3 is default (no-op); preferredFrameRateRange/afterMinimumDuration wrong tools; native Metal not required for 60.
- **Cheap ladder found:** IMMEDIATE-present diagnostic → match images↔frames-in-flight + wire layer delegate + autorelease pool → (only if needed) decouple render from CADisplayLink.
- See `research/ios-moltenvk-fps.md` for the full option table + citations.

## Synergies
Code state (RpsMain.mm / VulkanBackend.cpp), 2026-08-02 — the iOS-specific pacing knobs are ALL at defaults, none touched. This supports the "iOS-specific, not generic pipeline" hypothesis:
- **`CAMetalLayer.maximumDrawableCount`** — NEVER set → default 3. (Vulkan swapchain requests minImageCount+1=3 images.)
- **`CAMetalLayer.presentsWithTransaction`** — NEVER set → default false. (Known iOS 40fps-stutter knob.)
- **`CADisplayLink.preferredFramesPerSecond` / `preferredFrameRateRange`** — NEVER set → default.
- **Present mode** — hardcoded `VK_PRESENT_MODE_FIFO_KHR` in the shared backend (line 1232); no iOS override.
- **Render driver** — `renderFrame` is a **CADisplayLink callback on the MAIN thread**; it now blocks ~20ms on the drawable (frame.render=21.6ms > 16.6ms refresh → callback overruns → 40fps).
- **MoltenVK** — v1.4.1, static xcframework; only `MVK_CONFIG_LOG_LEVEL` and (today) `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=0` are set.
- Layer setup: `pixelFormat = BGRA8Unorm`, `contentsScale = UIScreen.scale`, `framebufferOnly` never set (default true).

In-flight changes (committed to master today, #103): 2-frame pipelining (VulkanBackend.cpp), TRACE sub-scopes, `rps.dev.render_scale` CVar, async-submit env var. All measured; pipelining kept, async-submit candidate to revert.

## Files Created
- `_interview-index.md` (this file)
- `../../research/ios-moltenvk-fps.md` — full research: 3 axes, option table, citations
- `approach-choice.md` — decision: render-thread decouple (option E)
- `render-thread-design.md` — concrete threading design (ownership, touch queue, lifecycle)
- `_summary.md` — final plan: phases, decisions, expected results, risks
