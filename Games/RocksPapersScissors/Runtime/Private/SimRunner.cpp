#include "Rps/SimRunner.h"

#include <chrono>

namespace Rps {
namespace {

// Monotonic nanoseconds since an arbitrary (process-stable) epoch. steady_clock is
// the right source — never goes backwards, unaffected by wall-clock adjustments.
uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

}  // namespace

void SimRunner::Start(uint64_t Seed, InputFn InInput, void* InCtx, uint32_t StressPerTeam) {
    Input = InInput;
    Ctx = InCtx;
    TheSim.Init(Seed);
#if LUR_INTERNAL
    if (StressPerTeam > 0) TheSim.StressFill(static_cast<int32_t>(StressPerTeam));
#else
    (void)StressPerTeam;
#endif

    // Publish tick 0 immediately so the render thread has a frame before the first
    // sim step lands.
    Mailbox.Back().CaptureFrom(TheSim, NowNs(), Clock.GetStepNs());
    Mailbox.Publish();
    PublishedTickCounter.store(TheSim.Tick, std::memory_order_release);

    Running.store(true, std::memory_order_release);
    Thread = std::thread([this] { ThreadMain(); });
}

void SimRunner::Stop() {
    if (!Running.exchange(false, std::memory_order_acq_rel)) return;  // idempotent
    if (Thread.joinable()) Thread.join();
}

void SimRunner::ThreadMain() {
    uint64_t Last = NowNs();
    while (Running.load(std::memory_order_acquire)) {
        const uint64_t Now = NowNs();
        const uint64_t Elapsed = Now - Last;
        Last = Now;

        // (slice 1/2: pump the transport here — this is the ~1 kHz service point that
        //  collapses the local polling latency #69 measured. Slice 0 has no transport.)

        const uint32_t Owed = Clock.AdvancePreserving(Elapsed, MaxTicksPerService);
        for (uint32_t K = 0; K < Owed; ++K) {
            uint8_t M0 = 0, M1 = 0;
            if (Input) Input(Ctx, TheSim.Tick, M0, M1);  // sample by tick number (deterministic)
            TheSim.Step(M0, M1);

            // Publish this tick. CaptureFrom (the heavy copy) runs UNLOCKED into the
            // back buffer; Publish() only flips indices under a short lock.
            Mailbox.Back().CaptureFrom(TheSim, NowNs(), Clock.GetStepNs());
            Mailbox.Publish();
            PublishedTickCounter.store(TheSim.Tick, std::memory_order_release);
        }

        // High service rate (~1 kHz), independent of vsync. The accumulator absorbs
        // the OS sleep granularity, so tick TIMING stays correct even if the sleep
        // overshoots. Slice 2 replaces this sleep with a transport wait/pump.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}  // namespace Rps
