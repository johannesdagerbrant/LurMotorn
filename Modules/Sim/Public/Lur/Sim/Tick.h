#pragma once
#include <cstdint>

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
        : StepNs(1'000'000'000ull / TicksPerSecond) {}

    // Feed elapsed wall-clock nanoseconds; returns how many whole sim ticks to
    // run now. Leftover time is retained for the next call (no drift).
    uint32_t Advance(uint64_t ElapsedNs) {
        AccumulatorNs += ElapsedNs;
        uint32_t Ticks = 0;
        while (AccumulatorNs >= StepNs) {
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
