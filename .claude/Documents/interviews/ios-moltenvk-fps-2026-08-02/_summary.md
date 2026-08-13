# Summary: iOS MoltenVK 40fps — render-thread decouple (#103)

## Problem
iPhone 11 Pro (A13, 60Hz, MoltenVK 1.4.1) renders RPS at a **stable exact 40fps** while the weaker Galaxy A14 (native Vulkan) holds 60. Root cause (research + device profiling, 3 axes + MoltenVK source): the render loop is a **CADisplayLink callback on the main thread that blocks on `[CAMetalLayer nextDrawable]`** with no frame overlap → a 2-frames/3-refreshes vsync **pacing beat** = 40.0fps. Real work is only ~3ms/frame. NOT fillrate, NOT draw count, NOT the submit path (all ruled out on device). MoltenVK acquires the drawable lazily at encode; the wait relocates with config but the vsync gate is constant. MAILBOX is unsupported on MoltenVK; native Metal not required.

## Solution
**Decouple rendering onto a dedicated render thread** (extends #69, which already moved sim+net off the vsync cadence), with **FIFO as the single vsync clock** so frame N+1's CPU work overlaps frame N's drawable wait → clean 60. Stays 100% Vulkan→MoltenVK→Metal; no API change, no native Metal backend. Details in `render-thread-design.md`; full research + citations in `../../research/ios-moltenvk-fps.md`.

## Key decisions
| Decision | Choice | Rationale |
|---|---|---|
| Fix approach | Render-thread decouple (option E) | User-chosen; proper architecture, mirrors #69; industry-standard render thread |
| Keep MoltenVK / Vulkan | Yes | Thread change only; native Metal is a future raise-with-user item, out of scope |
| Render cadence | Render thread free-runs, FIFO the single vsync clock; **remove CADisplayLink render driver** | CADisplayLink+FIFO = two vsync gates that beat (MoltenVK #581) |
| Buffering | swapchain images == frames-in-flight (matched constant); revert async-submit env var | MoltenVK #1407: 3img+mismatch caused the beat, matched gave 60; clean single-variable baseline |
| Input | Thread-safe touch-event queue; render thread owns `_View`/`_Cam`/`_DevGesture` | `_View` is shared by render + touch today; queue = same pattern as sim's `QueueLocalEvent` |
| Present timing / presentsWithTransaction | Not used | bg render thread + MoltenVK command-buffer present makes them unnecessary |

## Architecture
See `render-thread-design.md`. Threads: Main(UIKit input→queue + lifecycle) / Sim(exists) / **Render(new: owns renderer+`_View`+`_Cam`+`_DevGesture`, free-running FIFO loop, per-frame autorelease pool)**. Touch handlers push raw events; render thread drains + applies. Lifecycle (init/resize/#73-reattach/teardown) synchronized via atomics + a pause/ack handshake.

## Implementation order (each phase = its own commit; verify on device between)
0. **Diagnostic (keep despite committing to E):** iOS present mode → `IMMEDIATE` (1 line), measure. Expect fps >60 → **proves** the cause is vsync-pacing before investing in the refactor. Revert. (~1 device cycle.)
1. **Buffering baseline (shared backend):** revert async-submit env var; make swapchain image count + frames-in-flight a single **matched** constant (test 2/2, then 3/3). Measure iOS **and Android** (shared code — Android must stay 60). **Decision gate:** if this alone lands iOS 60, Phases 2–4 become an architecture/latency improvement rather than the fps fix — surface that and reconfirm appetite.
2. **Render thread core (iOS):** add `_RenderThread` mirroring `_SimThread`; move the render loop (HUD-atomics reflect + Mailbox consume + camera update + `_View.Render`) onto it; **remove the CADisplayLink render driver**; wrap each iteration in `@autoreleasepool`. Renderer owned by the render thread.
3. **Input queue (iOS):** thread-safe touch-event queue; UIKit `touchesBegan/Moved/Ended` push raw POD events; render thread drains and replays the existing `_View`/`_Cam`/`_DevGesture` input logic.
4. **Lifecycle/resize/teardown sync (iOS):** drawable-size + insets atomics; resize flag; **#73 reattach pause/ack handshake**; render-thread stop+join before layer/renderer destruction.
5. **Verify (device, two-phone):** fps→60 + smooth (no beat) via TRACE `presented` cadence; scroll feel; placement-drag + tap still work (≤1 frame added latency); dev console gesture; #73 heal after DVT relaunch; two-phone lockstep still syncs; Android still 60.

## Expected results
- iPhone locked **60fps** (from 40), smooth scroll; `presented` climbs +120/2s instead of +80.
- iOS render/input fully off the main UIKit thread (matches #69's sim/net split); cleaner latency.
- Android unchanged (verify after Phase 1 buffering).

## Risks
- **#73 reattach across threads (highest):** rebuilding UIWindow/view/layer on main while the render thread owns the renderer. Mitigate with a pause/ack handshake + careful stop-before-destroy ordering; test the DVT-relaunch heal explicitly.
- **Buffering change is shared code:** could regress Android. Mitigate: measure Android after Phase 1; keep the matched constant per-platform if needed.
- **Input latency:** hit-testing gains ≤1 frame. Acceptable; verify drag/tap feel on device.
- **Diagnosis is inference-heavy** (no single published "MoltenVK 40→60"): Phase 0 IMMEDIATE diagnostic is the guard — if it doesn't exceed 60, stop and rethink before Phases 2–4.
- **`_View` thread-safety:** everything that touches `_View` must end up on the render thread; audit for any stray main-thread `_View` access after the move.

## Status
Plan complete. No production code changed in this session. Ready to implement in a fresh session (start with Phase 0 diagnostic + Phase 1 buffering, honor the decision gate).
