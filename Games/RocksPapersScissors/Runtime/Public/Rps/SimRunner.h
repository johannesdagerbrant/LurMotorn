#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

#include "Lur/Sim/Tick.h"
#include "Rps/Sim.h"
#include "Rps/Snapshot.h"
#include "Rps/Tunables.h"

namespace Rps {

// The dedicated sim(+transport) thread — #69's loop shape, built here from the first
// line rather than retrofitted. It runs the fixed-timestep sim tick DECOUPLED from
// render/present/vsync: an inbound datagram (once transport is wired in slice 2) is
// serviced in ~1 ms instead of waiting up to a rendered frame (~16 ms vsync). Slice 0
// has no transport, so the thread just advances the sim at TickRateHz and publishes a
// Snapshot per tick; the high-rate service loop and the transport-pump seam are in
// place for slice 1/2 to fill.
//
// Determinism note: this class decides only WHEN ticks run (wall-clock driven); each
// tick's computation is deterministic, and inputs are sampled by TICK NUMBER via the
// InputFn, so the produced state after N ticks is independent of timing. That is what
// makes it lockstep-ready and unit-testable against a synchronous run.
class SimRunner {
public:
    // Sampled ON THE SIM THREAD at the start of each tick to get that tick's inputs.
    // Fn-ptr + ctx (not std::function) to stay allocation-free. Real play reads
    // atomics set by the input layer; tests return a scripted schedule by tick index.
    using InputFn = void (*)(void* Ctx, uint32_t Tick, uint8_t& Mask0, uint8_t& Mask1);

    ~SimRunner() { Stop(); }

    // Spawn the sim thread. Init(Seed) runs on the caller before the thread starts.
    // StressPerTeam > 0 (LUR_INTERNAL) bulk-spawns that many soldiers per side first —
    // the #75 stress scene (tick budget + one-draw render at the raised cap).
    void Start(uint64_t Seed, InputFn Input, void* Ctx, uint32_t StressPerTeam = 0);

    // Signal the thread to finish the current iteration and join. Idempotent.
    void Stop();

    // --- Consumer (render thread), safe while running ---
    bool LatestSnapshot(Snapshot& Out) const { return Mailbox.Consume(Out); }
    uint32_t PublishedTick() const { return PublishedTickCounter.load(std::memory_order_acquire); }

    // --- Post-Stop() accessors (thread joined; no other thread touches TheSim) ---
    uint64_t FinalStateHash() const { return TheSim.StateHash(); }
    uint32_t FinalTick() const { return TheSim.Tick; }

private:
    void ThreadMain();

    // Cap on ticks run per service iteration: bounds the catch-up BURST after a hitch
    // (debugger, backgrounded window) so one iteration can't block the loop; the rest
    // stays in TickClock's accumulator and drains over the next iterations. Never a
    // discard — the lockstep sim runs every tick (design doc §3).
    static constexpr uint32_t MaxTicksPerService = 8;

    Sim TheSim;
    SnapshotMailbox Mailbox;
    Lur::Sim::TickClock Clock{TickRateHz};

    std::thread Thread;
    std::atomic<bool> Running{false};
    std::atomic<uint32_t> PublishedTickCounter{0};

    InputFn Input = nullptr;
    void* Ctx = nullptr;
};

} // namespace Rps
