# 2026-08-02 — iOS MoltenVK 40fps: diagnosis, research & plan (#103 / #183)

Frozen snapshot of the investigation into the iPhone's stable 40fps render ceiling (RPS, #103) and the research-and-plan session that chose the fix. **Live status lives in the issues** — this batch is history; re-verify code claims against HEAD before acting.

- **[ios-40fps-measurements.md](ios-40fps-measurements.md)** — the device-measured evidence (TRACE tables from every experiment: baseline split, resolution A/B, render.view sub-scopes, EndFrame split, 2-frame pipelining, async submit). The raw "data we gathered."
- **[ios-40fps-research.md](ios-40fps-research.md)** — full research: 3 axes (Core Animation frame pacing, MoltenVK source internals, production engines), option comparison table, all citations. Root cause: single-threaded CADisplayLink callback blocking on `nextDrawable` → 2-frames/3-refreshes vsync beat.
- **[ios-40fps-render-thread-design.md](ios-40fps-render-thread-design.md)** — concrete threading design for the chosen fix (render-thread decouple): thread ownership, the touch-event queue, lifecycle/resize/#73-reattach synchronization.
- **[ios-40fps-plan.md](ios-40fps-plan.md)** — the plan: decision table, phased implementation order (Phase 0 diagnostic → Phase 5 verify), expected results, risks.

## Live trackers (authoritative for current state)
- **#103** — the original finding + the full diagnosis comment thread.
- **#183** — the render-thread-decouple implementation tracker (phase checklist, starting-state, risks). Deferred to a fresh implementation session as of this snapshot.

## One-line conclusion
The 40fps is a MoltenVK/iOS **presentation pacing beat**, not a render-cost problem (real work ~3ms; the weaker Android native-Vulkan path is fine) — the fix is to move rendering off the blocking CADisplayLink main-thread callback onto a dedicated render thread with FIFO as the single vsync clock, staying 100% MoltenVK/Vulkan.
