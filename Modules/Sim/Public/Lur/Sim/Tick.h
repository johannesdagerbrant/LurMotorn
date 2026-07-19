#pragma once
#include <cstdint>

#include "Lur/Core/Assert.h"

namespace Lur::Sim {

// Fixed-timestep accumulator.
//
// The simulation advances in discrete, equal ticks (e.g. 120 Hz) regardless of
// the display's frame rate; rendering interpolates between ticks for smooth
// motion. A fixed timestep is the other half of determinism (with Fixed): given
// the same inputs and the same number of ticks, every device reaches the same
// state — the precondition for rollback netcode.
//
// Chess uses this trivially (it ticks only when a move is made), but the contract
// is identical to what a reflex game needs.
class TickClock {
public:
    explicit TickClock(uint32_t TicksPerSecond)
        : StepNs(1'000'000'000ull / (TicksPerSecond ? TicksPerSecond : 1)) {
        LUR_ASSERT_MSG(TicksPerSecond > 0, "TickClock: TicksPerSecond must be > 0");
    }

    // The most ticks Advance() will return in one call. A long pause (debugger,
    // backgrounded app) would otherwise return thousands of catch-up ticks in a
    // burst — a stutter for chess, a spiral of death for a reflex game. We cap the
    // burst and DISCARD the backlog beyond it. Both peers clamp identically, so this
    // stays deterministic (it changes the tick COUNT after a stall, never a tick's
    // computation). Sized generously above any real frame hitch.
    static constexpr uint32_t MaxCatchup = 8;

    // Feed elapsed wall-clock nanoseconds; returns how many whole sim ticks to
    // run now. Leftover sub-tick time is retained for interpolation (no drift).
    uint32_t Advance(uint64_t ElapsedNs) {
        AccumulatorNs += ElapsedNs;
        uint32_t Ticks = 0;
        while (AccumulatorNs >= StepNs) {
            AccumulatorNs -= StepNs;
            if (++Ticks >= MaxCatchup) {
                AccumulatorNs %= StepNs;  // drop the backlog, keep the sub-tick remainder
                break;
            }
        }
        return Ticks;
    }

    // Non-discarding advance for the LOCKSTEP SIM path. Returns up to MaxThisCall
    // whole ticks and KEEPS the remainder — including any backlog it couldn't return
    // this call — in the accumulator, so the sim eventually runs EVERY tick, spread
    // across service iterations after a hitch. This is the deliberate counterpart to
    // Advance(): Advance() DISCARDS backlog beyond MaxCatchup because it paces
    // RENDERING (a dropped frame is fine and a burst is a stutter); the lockstep sim
    // must never drop a tick — dropping on one peer only is a desync machine (RTS
    // design doc §3). MaxThisCall bounds the per-call BURST (so one Advance after a
    // long pause can't block the loop), never the total: the rest drains next call.
    uint32_t AdvancePreserving(uint64_t ElapsedNs, uint32_t MaxThisCall) {
        AccumulatorNs += ElapsedNs;
        uint32_t Ticks = 0;
        while (AccumulatorNs >= StepNs && Ticks < MaxThisCall) {
            AccumulatorNs -= StepNs;
            ++Ticks;
        }
        return Ticks;
    }

    // Fraction [0,1) into the next tick — the alpha for render interpolation.
    float GetInterpolationAlpha() const {
        return static_cast<float>(AccumulatorNs) / static_cast<float>(StepNs);
    }

    uint64_t GetStepNs() const { return StepNs; }

private:
    uint64_t StepNs;
    uint64_t AccumulatorNs = 0;
};

} // namespace Lur::Sim
