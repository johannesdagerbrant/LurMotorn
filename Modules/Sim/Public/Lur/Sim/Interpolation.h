#pragma once
// Fixed-timestep render interpolation: where between the last two sim ticks are we right now?
//
// Promoted out of Rps::Snapshot::AlphaAt (#201). It is eight lines, and it is here because of the ONE
// thing it does at the boundaries rather than the arithmetic in the middle.
//
// ---- THE CLAMP IS THE LAW ----
// The result is clamped to [0, 1]. It NEVER exceeds 1, which means the renderer holds at the newest
// tick when the sim is late instead of extrapolating past it. That is not a rounding convenience; it
// is the repo's standing rule (CLAUDE.md: "The renderer INTERPOLATES, never PREDICTS"), and
// render-side extrapolation has been built and reverted TWICE because it overshoots at every velocity
// discontinuity. At the netcode's ceiling — where the sim deliberately stops — this clamp is exactly
// what makes the picture freeze gracefully rather than smear forward into a guess.
//
// So any future game gets the law by using the function, and a game that wants to break it has to
// write its own and say so.
#include <cstdint>

namespace Lur::Sim {

// Alpha for rendering at NowNs, given the wall time the newest tick was published and the step
// length. 0 = show the previous tick, 1 = show the newest. Clamped: see above.
// StepNs == 0 (nothing published yet) yields 0.
constexpr float InterpAlpha(uint64_t NowNs, uint64_t PublishNs, uint64_t StepNs) {
    if (StepNs == 0 || NowNs <= PublishNs) return 0.0f;
    const uint64_t D = NowNs - PublishNs;
    if (D >= StepNs) return 1.0f;
    return static_cast<float>(D) / static_cast<float>(StepNs);
}

}  // namespace Lur::Sim
