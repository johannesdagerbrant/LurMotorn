# Research: iOS MoltenVK — breaking the stable 40fps ceiling (#103)

**Date:** 2026-08-02

## Problem Statement
iPhone 11 Pro (A13, 60Hz fixed panel, iOS 26), RPS via MoltenVK 1.4.1 on a `CAMetalLayer`, render driven by a `CADisplayLink` callback on the main thread. Stuck at a **stable exact 40fps** while the weaker Android (native Vulkan) holds 60. Measured: ~3ms real work/frame + a ~16ms drawable/vsync wait that relocates (acquire→submit→begin-frame) but never changes fps. NOT fillrate (resolution A/B flat). 2-frame Vulkan pipelining + async submit both failed to move fps.

## Axis 1 — Apple Metal / Core Animation frame pacing (agent report)

### Root-cause mechanism (high confidence, well-cited)
- **Presentation is vsync-quantized**: on a 60Hz panel a frame becomes visible only on a refresh boundary → the only clean sustained rates are 60 / 30 / 20. **40 is not a divisor of 60.**
- **40fps = a 1-refresh/2-refresh beat**: 40fps = 25ms = 1.5 refresh intervals. Produced by alternating "shown for 1 refresh (16.67ms)" and "shown for 2 refreshes (33.33ms)" → 2 frames per 3 refreshes → avg 40.0. A *stable, non-jittery* 40.0 is the fingerprint of this beat.
- **Why the beat exists**: the `CADisplayLink` callback runs on the main thread and **blocks synchronously on `[CAMetalLayer nextDrawable]`** ("if no drawable is available the calling thread is blocked until the next refresh"). Because the block is inside the callback, **frame N+1's CPU work can't start until frame N's drawable is acquired/presented — zero pipelining.**
- **Why exactly 40 not 30**: `maximumDrawableCount` defaults to **3** (triple buffering). The 3rd buffer gives just enough slack that on alternate iterations a recycled drawable is already free and the loop catches the *very next* vsync (1 refresh) instead of skipping to the 2nd → the 1-2-1-2 alternation = 40. (With no slack it'd be a clean 30; the third buffer is exactly what turns a 30-cap into a 40-beat.)
- **Why relocating the wait didn't help (matches our measurement)**: the throttle is the vsync-quantized drainage of the drawable pool, not the wall-clock position of the wait. Moving the block doesn't change how many vsyncs each frame consumes → average stays 40. Only (i) removing the serialization so CPU work pipelines, or (ii) forcing an integer cadence, changes it.
- **Smoking gun**: ~3ms real work proves we're not compute-bound; we're pinned to 40 purely because the display-link callback blocks on the drawable and never lets N+1 begin. Restore pipelining → 3ms trivially fits 60.

### Ranked fixes (agent 1)
1. **(Best) Don't block the display-link callback on the drawable; pipeline with a 3-deep in-flight semaphore.** Apple MTKView `Renderer` pattern: `DispatchSemaphore(value:3)`, `wait()` at frame top + `commandBuffer.addCompletedHandler{ signal() }`; **acquire the drawable as LATE as possible** (right before encoding the on-screen pass) and release immediately (per-frame `@autoreleasepool`); present with `present(drawable)+commit()`, NO `waitUntilScheduled/Completed` on the loop thread. Lets N+1's 3ms overlap N's present. Expected **40→solid 60**. This is the direct root-cause fix.
2. **Move rendering off the main thread / decouple present from the CADisplayLink callback.** Keep the callback tiny (timestamp+kick); encode/present on a dedicated render thread. A blocking present then stalls only that thread; frames pipeline. Expected **40→60**. (Note: sim+net already moved off vsync in #69; render/present wants the same.)
3. **(Conditional stabilizer) `presentsWithTransaction=true`** + `commit()` + `waitUntilScheduled()` + `[drawable present]`. The ebiten fix (#1196). CAVEATS: present must be on the **main thread**; must **drop maximumDrawableCount 3→2** (triple buffering + presentsWithTransaction makes nextDrawable return twice/cycle). Evidence MIXED (fixed 40fps for one reporter, not for the ProMotion variant). Try AFTER 1–2; addresses pacing not serialization. Lower confidence.

### Confirmed NON-fixes for this symptom
- **`maximumDrawableCount=3` explicitly** — already the default (range 2–3); no-op. Setting **2** alone makes it worse (nextDrawable returns nil → dropped frames), *unless* paired with presentsWithTransaction (fix 3). (Worth checking we haven't lowered it.)
- **`presentDrawable:afterMinimumDuration:`** — this ENFORCES a stable *lower* rate for apps that can't hit 16.67ms; with 3ms work it'd lock a clean **30**, not restore 60. Wrong tool.
- **`CADisplayLink.preferredFrameRateRange`/`preferredFramesPerSecond`** — panel is 60Hz fixed non-ProMotion; can't raise a 60Hz ceiling, won't break a beat that lives below 60. (Only matters on ProMotion/variable-refresh.)
- **ProMotion down-clock theories (Zed 120fps)** — need variable-refresh display; ours is fixed 60. Not our cause.
- **MTKView as a magic swap** — helps by construction (late currentDrawable, paces via preferredFramesPerSecond) but does NOT add the in-flight semaphore for you; the semaphore + late-acquire discipline is the actual cure, not the class swap.

### Key citations (agent 1)
- Apple *Metal Best Practices*: Drawables (nextDrawable blocks; acquire late/release early), Frame Rate (16.67ms; afterMinimumDuration→30 floor), Triple Buffering (3=triple,2=double).
- `CAMetalLayer.maximumDrawableCount` docs (default 3, range 2–3).
- ebiten commit "Make FPS stable by presentsWithTransaction" + issue #1196 (present-after-commit + waitUntilScheduled + drop 3→2).
- Apple Dev Forums 23798 (nextDrawable delay 60→30/40; presentsWithTransaction on main thread), 698630 (semaphore maxBuffersInFlight=3; MTKView not auto-immune), 722434 (nextDrawable 5–12ms).
- Raph Levien "Swapchains and frame pacing" (blocking acquire serializes; need ≥3 buffers; schedule to vsync). Zed "120 FPS" (buffer pool + completed-handler recycling).

### Uncertainties (agent 1)
- The exact "40 = avg of 1/2-refresh beat from 3 drawables" arithmetic is the agent's synthesis (consistent with, not verbatim in, sources); multiple sources confirm 40 is the characteristic landing point and the drivers are nextDrawable-block + full pool + overrunning callback.
- Some forum 40fps reports are OS throttling after Control Center/notifications — a *different* trigger from steady-state 40; ours matches the architectural cause (measured 21ms blocking callback).

## Axis 2 — MoltenVK internals (agent report — READ THE SOURCE, highest confidence on mechanism)

### Where MoltenVK actually blocks (source-verified, revises agents 1&3)
- **`vkAcquireNextImageKHR` never calls `nextDrawable`.** `MVKSwapchain::acquireNextImage()` just picks an available presentable image and signals your semaphore/fence. NO drawable fetch here.
- **The `CAMetalDrawable` is fetched LAZILY** in `MVKPresentableSwapchainImage::getCAMetalDrawable()` → `[layer nextDrawable]`, reached from `getMTLTexture()` — i.e. **when the swapchain image is first bound as a render target (render-pass begin / framebuffer attachment during encoding)**, and again at present. `[CAMetalLayer nextDrawable]` is the blocking call (blocks when the pool `maximumDrawableCount` default 3 is all in-flight, gated by CA vsync; ~5–12ms if acquired early).
- **So the ~16ms wait is ALWAYS `nextDrawable` blocking on the vsync-throttled pool; it RELOCATES because MoltenVK relocates the ENCODING** (the drawable is grabbed wherever encoding first needs the swapchain texture):
  - `SYNCHRONOUS_QUEUE_SUBMITS=1` (default) + `PREFILL=0` (default): whole Metal cmd buffer encoded on the submit thread inside `vkQueueSubmit` → wait sat in submit (what we saw).
  - `SYNCHRONOUS_QUEUE_SUBMITS=0`: encoding dispatched to a GCD queue → `vkQueueSubmit` ~0, stall reappears in next `vkWaitForFences`/acquire (EXACTLY what we observed — "moved the deck chair").
- Present (`presentCAMetalDrawable`) always via an `MTLCommandBuffer` `addScheduledHandler` → `[drawable present]`; toggles `displaySyncEnabledMVK` late. Source warns "the last one or two presentation completion callbacks can occasionally stall" — a late completion delays the next acquire → raw material for a beat.

### Why 40.0 (hypothesis, medium confidence) + the smoking-gun issue
- 40fps on 60Hz = 2-frames-per-3-refreshes beat (avg 1.5×16.67=25ms). Not GPU-bound (~3ms). A pipeline-depth/pool mismatch where async present + completion-callback latency adds one extra frame of drawable latency every other frame.
- **MoltenVK #1407 (the key issue):** users with **FIFO + 3 swap images** got `vkAcquireNextImageKHR` returning `0,0,1,1,2,2…` and alternating 12.6/4.1ms frames — a self-inflicted cadence artifact, wait in `vkQueueSubmit` (same as us). **"FIFO w/ 2 swap images: renders at vsync 60Hz as I would expect."** ← 2 images fixed it, 3 caused the beat.

### Ranked MoltenVK-side fixes (agent 2)
1. **(FIRST) Change swapchain image count and measure — NOT "more is better".** `MVKSwapchain.mm` sets `maximumDrawableCount = clamp(minImageCount, 2, 3)` directly. Per #1407, **try `minImageCount = 2`** (double-buffer) — gave clean 60 there; and **match frames-in-flight to the image count** (cleanest FIFO = images == frames-in-flight). **OUR BUG SMELL: we request 3 images (minImageCount+1) but run 2 frames-in-flight — exactly the mismatch that creates the beat.** Test 2img/2-in-flight vs 3img/3-in-flight; pick whichever gives 60.0.
2. **Acquire the drawable as late as possible** — but MoltenVK already does (lazy at encode); the lever is really #1 + not over-pipelining. With 3ms work, 1 frame in flight on iOS is fine and removes the beat's degrees of freedom.
3. **`SYNCHRONOUS_QUEUE_SUBMITS=0` + `PREFILL=0`** — community "best perf" combo (#581 aerofly), but only relocates the wait (we proved). Keep, rank below buffering.
4. **`CAMetalLayer.delegate` = the containing UIView** — User Guide STRONGLY recommends "to ensure correct and optimized swapchain and refresh timing." Mis-wired delegate = documented wrong-refresh-timing cause. Cheap; verify in the Swift/ObjC shim.
5. **Layer opaque + un-composited** — `isOpaque=true`, nothing (HUD, translucent view) over it (#581 Ken Thomases: non-opaque/composited layer changes present path & framerate).
6. **(Diagnostic only) `VK_PRESENT_MODE_IMMEDIATE_KHR`** → `displaySyncEnabled=NO`. If fps jumps well above 60, PROVES it's a present/vsync artifact not CPU/GPU. Don't ship (tearing); isolates cause in one line.

### Confirmed NON-fixes on iOS (agent 2, source-verified)
- **`VK_PRESENT_MODE_MAILBOX_KHR` — UNSUPPORTED by MoltenVK** (billhollings #581: needs a single-entry queue Metal doesn't expose); silently falls back to FIFO. Will NOT break the beat. (On iOS `vkGetPhysicalDeviceSurfacePresentModesKHR` shows only FIFO + IMMEDIATE.)
- **`MVK_CONFIG_PRESENT_WITH_COMMAND_BUFFER` — no longer exists** in v1.4.1 (presents always via command buffer). Don't touch.
- `MVK_CONFIG_SWAPCHAIN_MIN_MAG_FILTER_USE_NEAREST`, `DISPLAY_WATERMARK` — unrelated to pacing.
- `vkWaitForFences` timeout tweaks — don't move the cap (throttle is CA's pool, not the fence).
- `vkQueueWaitIdle` instead of fences — harmful (GPU bubble). Keep per-slot fences.

### Drawable count facts (agent 2)
- MoltenVK wires `minImageCount → maximumDrawableCount`, clamp **[2,3]** on iOS (`kMVKMin/MaxSwapchainImageCount=2/3`). **Cannot get 4+** (Metal cap). May already be getting 3 (MVKDevice L2017 sometimes sets minImageCount=maxImageCount=3 "to avoid tearing").
- Requesting MORE does not help a *pacing beat* (vs a throughput shortfall). #1407: 3 caused the beat, 2 fixed it. **Test 2 vs 3 empirically, align frames-in-flight.**

### FIFO vs MAILBOX vs IMMEDIATE on iOS (agent 2, definitive)
CA is always vsync-composited, so MoltenVK reduces present mode to ONE Metal flag: `displaySyncEnabledMVK = (mode != IMMEDIATE)`. FIFO → vsync on. MAILBOX → unsupported, == FIFO. IMMEDIATE → vsync OFF (tearing, unthrottled) — the only real toggle, diagnostic only. **No mailbox-style mode exists on iOS.** The fix must come from buffering/frames-in-flight/layer setup, not a present-mode swap.

### Best-practice 60fps MoltenVK iOS setup (agent 2)
- **Pick ONE vsync clock.** `CADisplayLink` + FIFO = TWO vsync gates that beat against each other (aerofly #581 found CVDisplayLink interfered with FIFO). Options: (i) dedicated render thread + FIFO (let MoltenVK block on the drawable), or (ii) CADisplayLink-driven + IMMEDIATE (CADisplayLink the sole pacer — but tears). **(i) is the safer default.** First cheap thing to try: **render-thread + FIFO + 2 images + matched frames-in-flight**, measure.
- **Per-frame autorelease pool** on the render loop that drains each frame — else `CAMetalDrawable`s aren't released, pool starves, `nextDrawable` waits a whole vsync (MoltenVK retains the drawable).
- Layer hygiene (delegate=UIView, isOpaque). Config `SYNCHRONOUS_QUEUE_SUBMITS=0`+`PREFILL=0`. Acquire late. Optional `presentAtTime:` (VK_GOOGLE_display_timing) to force a cadence if buffering doesn't.

### Citations (agent 2 — read source)
- MoltenVK source: `MVKImage.mm` (getCAMetalDrawable/presentCAMetalDrawable lazy drawable), `MVKSwapchain.mm` (acquireNextImage, maximumDrawableCount=clamp(minImageCount)), `MVKDevice.mm` (surface min/max ImageCount), `MVKSwapchain.h` (min/max=2/3).
- Docs: Runtime User Guide (triple-buffer, CAMetalLayer delegate), Configuration_Parameters (no PRESENT_WITH_COMMAND_BUFFER; SYNC_SUBMITS default 1, PREFILL default 0).
- Issues: **#1407 (FIFO 3-image beat, 2-image clean)**, #581 (MAILBOX unsupported, vsync clamp, render-thread advice, opaque layer), #430 (maxImageCount 2→3), #823 (present modes). Flutter/Impeller #138490 (defer nextDrawable), Apple forums 722434/780668.

### Uncertainty (agent 2)
- Exact 40.0 not pinned to one documented cause; strongly consistent with the 2/3-refresh beat + #1407 FIFO cadence artifact. Validate: does the number snap to 60.0 or 30.0 when buffering changes (confirms pacing quantization vs load).
- Most tracker repros are macOS (windowed/fullscreen nuances don't map to iOS), but the drawable model + source paths are shared, so the mechanism transfers.

## Cross-axis synthesis

### Where the three axes AGREE (high confidence)
- The stable 40.0fps is a **present/vsync PACING BEAT** (2 frames per 3 refreshes), **not** load — real work ~3ms. Confirmed by: resolution-flat, wait-relocates-not-shrinks, and all three research axes.
- The block is `[CAMetalLayer nextDrawable]` on the vsync-throttled drawable pool. It relocates because MoltenVK acquires the drawable lazily at encode-time and MoltenVK relocates encoding by config — the vsync gate is constant. → **why my 2-frame Vulkan fences + async-submit did nothing.**
- **MAILBOX is a dead end** (unsupported on MoltenVK → FIFO). Present-mode swaps won't fix it. IMMEDIATE = diagnostic only (tears).
- Native Metal backend is NOT required to hit 60; it's a future raise-with-user option.

### Where they NUANCE each other (the actionable insight)
- Agents 1&3 emphasized "add pipelining + acquire late + 3 buffers." Agent 2 (read the source + MoltenVK #1407) sharpened it: MoltenVK **already** acquires late (lazy), and **more buffers is NOT better** — **3 images caused the beat in #1407, 2 gave clean 60**, and a **frames-in-flight ≠ image-count mismatch is itself the beat trigger** ("cleanest FIFO = images == frames-in-flight").
- **Our confirmed state:** 3 swapchain images (`minImageCount+1`, clamped [2,3]) + **2 frames-in-flight** = exactly that mismatch, with 3 images = #1407's beat config. **`CAMetalLayer.delegate` is unset** (User Guide violation). These are cheap, high-probability targets.

### Option comparison
| Fix | Effort | Risk | Confidence it lifts 40→60 | Notes |
|---|---|---|---|---|
| **D. IMMEDIATE present (diagnostic)** | 1 line, iOS | none (revert) | n/a — PROVES cause | fps should shoot >60 → confirms vsync-pacing not load |
| **A. Match images↔frames-in-flight (try 2 img / 2-in-flight)** | small (image-count + maybe drop pipelining depth) | low | **High** — directly targets #1407 | #1407: 2 images = clean 60; our 3-vs-2 mismatch is the smell |
| **B. Wire CAMetalLayer.delegate=view + isOpaque** | 2 lines, iOS | low | Medium | User Guide "correct/optimized refresh timing"; currently missing |
| **C. Per-frame autorelease pool on render loop** | small, iOS | low | Medium | prevents drawable-retain pool starvation |
| **E. Decouple render from CADisplayLink (render thread + FIFO, single clock)** | LARGE, shared+iOS | med-high | High but heavy | CADisplayLink+FIFO = double vsync gate; the "proper" fix if A–C don't land |
| **F. Native Metal backend** | very large | high (breaks 1-backend rule) | Highest ceiling | future only; explicit user decision |
| ~~MAILBOX present~~ | — | — | **none** | unsupported on MoltenVK, == FIFO |
| ~~maximumDrawableCount=3 / preferredFrameRateRange / afterMinimumDuration~~ | — | — | **none/negative** | non-fixes for this symptom (agents 1&2) |

### Recommended ladder (cheapest, highest-probability first — validate on device between each)
1. **D (diagnostic):** IMMEDIATE present on iOS → confirm fps >60 (proves pacing). Revert.
2. **A+B+C (the likely cure, all cheap):** match swapchain images to frames-in-flight (test 2-img/2-in-flight, and 3-img/3-in-flight), wire the layer delegate + isOpaque, add the per-frame autorelease pool. Measure after each.
3. **E (only if 1–2 don't hit 60):** decouple render from CADisplayLink — dedicated render thread + FIFO, single vsync clock. Bigger, shared-code, needs two-phone verify.
4. **F:** native Metal backend — future, explicit user call (violates the MoltenVK-only rule).

### Housekeeping decisions surfaced
- **async-submit env var (`SYNCHRONOUS_QUEUE_SUBMITS=0`)**: only relocated the wait; community says it's the "best perf" combo but it didn't change fps. Keep or revert? (Leaning: keep IF it composes with A; else revert to reduce variables.)
- **2-frame pipelining**: correct + helps Android, but if the iOS cure is "images==frames-in-flight" with a specific count, the depth may need tuning per platform (e.g. 2 on iOS). Keep the mechanism; make depth a constant we can match to image count.

## Axis 3 — Production engines on iOS (agent report)

### What shipping engines do
- **Serious 3D engines write a NATIVE METAL backend on iOS**, MoltenVK mainly for macOS/bring-up: **Unreal** (Metal RHI), **Unity** (Metal default on Apple), **bgfx** (native Metal), **Filament** (Metal primary; Vulkan/MoltenVK secondary), **The Forge/Diligent** (native Metal). Rationale (Arseny Kapoulkine "3 Years of Metal"): layering one low-level API over another causes impedance mismatch → suboptimal perf; a Metal impl is simpler than Vulkan.
- **But MoltenVK SHIPS**: **Godot** (iOS via MoltenVK), **Dota2/Source2** (macOS via MoltenVK, ~50% faster than Apple GL). So MoltenVK is production-viable; the 40fps is NOT a MoltenVK ceiling.

### MoltenVK present mechanics (source-level)
- `VkSwapchain` wraps a `CAMetalLayer`. `VkSwapchainCreateInfo::minImageCount` is clamped and written straight into **`CAMetalLayer.maximumDrawableCount`** (`MVKSwapchain.mm::initCAMetalLayer`); docs recommend **3** for full-screen.
- **`vkAcquireNextImageKHR` ↔ `CAMetalLayer.nextDrawable`** (blocks); present always via `MTLCommandBuffer` (`presentWithCommandBuffer` obsolete/always-on). → all native Metal drawable rules apply to us through Vulkan calls.

### The anti-pattern we're hitting (agent 3)
1. iOS gives 2–3 drawables; acquire **blocks** until one is free. Acquiring EARLY then blocking burns budget (Flutter/Impeller #138490: early acquire cost ~4–5ms, cut usable render time ~8ms→<4ms). Apple: **request the drawable as LATE as possible, right before encoding the on-screen pass.** ← OUR `WaitForFrame`-at-top does the OPPOSITE.
2. Blocking that acquire/fence **inside the CADisplayLink main-thread callback serializes CPU↔GPU** → miss current vsync → present at next → 1-vsync/2-vsync beat = 40fps.
3. **No real frames-in-flight overlap.** Apple's canonical answer = `DispatchSemaphore(value:3)`; the CPU runs up to 3 frames ahead, only blocking when it laps the GPU. Vulkan equiv = N frame-sets, never stall the just-submitted frame. **NOTE:** we added 2-frame Vulkan fences but the LOOP is still one synchronous blocking CADisplayLink callback, so nothing overlaps — the fences alone don't fix it.
4. Default present doesn't coordinate with the CA transaction; `presentsWithTransaction=true` fixes ordering but is main-thread-only. MoltenVK hides this knob (a thing native Metal would give).

### Ranked fixes WITHOUT native Metal (agent 3)
1. **(Biggest) Real frames-in-flight pipelining** — 2–3 independent frame-sets; only wait on frame i's fence when the slot recycles. Vulkan form of DispatchSemaphore(3). Lets CPU build N+1 while GPU finishes N.
2. **Request 3 swapchain images and verify** `vkGetSwapchainImagesKHR` returns 3 (MoltenVK clamps minImageCount→maximumDrawableCount). 2 under FIFO starves immediately.
3. **Acquire the drawable as LATE as possible** — do all CPU update / uniform upload / offscreen passes BEFORE `vkAcquireNextImageKHR` so the drawable wait overlaps in-flight GPU work (Flutter/Impeller #138490 fix; Apple best practice). ← directly contradicts our wait-early.
4. **Get present/acquire OFF the CADisplayLink main thread** — CADisplayLink as a pure pacing tick; render on a dedicated thread (producer/consumer, Limbic pattern). Largest change; pairs with #1. (presentsWithTransaction/CAMetalDisplayLink misbehave off-main, but MoltenVK presents via plain command-buffer present, so a bg render thread is fine.)
5. **Keep FIFO; don't chase MAILBOX/IMMEDIATE** — not genuinely available on iOS (no tearing present on Metal); the lever is drawable-count + pipelining, not present mode.
6. Instrument acquire vs present vs fence (done).
- **Expectation: fixes 1+2+3 alone typically restore locked 60 when GPU work is ~3ms.** Fix 4 is headroom.

### When native Metal becomes worth it (future, raise-with-user)
Only if after 1–4 still <60 (translation wall), or you need present controls MoltenVK hides (presentsWithTransaction, CAMetalDisplayLink, ProMotion 120Hz), or to shed per-call SPIR-V→MSL CPU overhead. Would violate the "MoltenVK is the one exception" rule → explicit user decision, not silent.

### Key citations (agent 3)
- MVKSwapchain.mm (minImageCount→maximumDrawableCount, recommend 3); MoltenVK Runtime User Guide; Whats_New (present via command buffer). MoltenVK #1407 (image-count changes pacing).
- Apple triple-buffering / DispatchSemaphore(3) (forum 651581, WWDC2015 #610, Metal Best Practices: Triple Buffering).
- Flutter/Impeller #138490, #134959 (acquire late; early acquire halves frame time).
- Apple forums 698349/15102/722434 (nextDrawable blocking, 2–3 pool); 723325 + zed #53390 (presentsWithTransaction main-thread-only).
- Limbic "Multithreaded Renderer on iOS"; Apple 766889 (CAMetalDisplayLink off-thread limit).
- Kapoulkine "3 Years of Metal"; bgfx #1110; Khronos/Valve (Dota2 MoltenVK); LunarG "State of Vulkan on Apple Jan 2026"; Godot #38187/#76425 (iOS stutter class).

### Uncertainty (agent 3)
- No single published "we fixed our MoltenVK app 40→60" post; diagnosis is by inference from Apple docs + Flutter/Impeller + MoltenVK source (all corroborating). Verify our actual swapchain image count + loop structure before assuming (symptom strongly implies naive single-frame-in-flight loop).

## Axis 3-note — reconciling with our failed experiments
Both agents independently explain why my Vulkan-level 2-frame pipelining + async submit did NOT move fps: the render loop is a **single synchronous CADisplayLink callback that blocks on the drawable**, so there is never a second frame's CPU work in flight to overlap — the Vulkan fences have nothing to pipeline against. AND our `WaitForFrame`-at-top acquires the drawable EARLY (wait-early, ported from Android for input freshness), which is the iOS ANTI-pattern. The fixes that matter: acquire LATE + let the loop overlap (off-thread render or a non-blocking callback), not more fences.

## Synthesis / Option comparison (pending all axes)
