#pragma once
// The dedicated fixed-timestep sim thread: it advances a deterministic sim at a fixed rate,
// DECOUPLED from render/present/vsync, and publishes one snapshot per tick to a consumer thread.
//
// Promoted out of Rps::SimRunner (#201). The class is ~30 lines of thread plumbing around four
// hard-won rules, and the rules are the reason it belongs in the engine:
//
// ---- 1. Service rate is NOT tick rate ----
// The loop runs at ~1 kHz and asks the clock how many ticks are OWED. That is what makes an inbound
// datagram serviceable in ~1 ms instead of waiting up to a rendered frame (~16 ms of vsync) — the
// latency #69 measured. The transport pump belongs at this service point, not on the render thread.
//
// ---- 2. The catch-up burst is BOUNDED, never DISCARDED ----
// After a hitch (debugger, backgrounded app) the clock may owe hundreds of ticks. Running them all in
// one iteration blocks the loop; skipping them desyncs a lockstep peer instantly. So the burst is
// capped per iteration and the remainder stays in TickClock's accumulator, draining over the next
// iterations. Every owed tick eventually runs.
//
// ---- 3. A HELD clock DROPS elapsed time rather than banking it ----
// The subtle one. A pre-match hold (waiting for the player's opening placement) must not accumulate
// the wait, or the match opens with a catch-up burst of however many seconds the player spent
// deciding. So while the gate is closed the clock is not advanced AT ALL. The hold is expressed as
// two caller predicates — Held (are we waiting?) and Opens (does this input batch release us?) —
// because *what* qualifies is game rules, while "drop, don't bank" is engine law.
//
// ---- 4. Input is sampled BY TICK NUMBER, on the sim thread ----
// Which is what makes a threaded run bit-identical to a synchronous one: the state after N ticks is a
// pure function of (seed, input schedule) regardless of how the OS scheduled the thread. This class
// decides only WHEN ticks run; it never influences WHAT they compute.
//
// ---- Why Service() is public ----
// The loop body is a pure function of an elapsed duration, deliberately separated from the thread, so
// all four rules above are testable SYNCHRONOUSLY with exact durations and no sleeping. A test that
// has to race a real thread to observe a cap can only ever be flaky, and a flaky test for rule 2 is
// worse than none — it would be muted, and rule 2 is a silent-desync rule.
//
// ---- Requirements on the type parameters ----
//   TSim      : uint32_t Tick;  void StepEvents(TInput*, int Count)
//               (the caller Init()s it BEFORE Start — see the note on Start)
//   TSnapshot : trivially copyable; void CaptureFrom(const TSim&, uint64_t NowNs, uint64_t StepNs)
//   TInput    : trivially copyable event POD
//
// NOTE ON LINKAGE: this header spawns a std::thread, so a target that INSTANTIATES it must link
// Threads::Threads. lur::sim deliberately does not force that on its many header-only consumers.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "Lur/Sim/SnapshotMailbox.h"
#include "Lur/Sim/Tick.h"

namespace Lur::Sim {

// Monotonic nanoseconds. steady_clock is the right source — never goes backwards, unaffected by
// wall-clock adjustments. Scheduling only; never simulation state.
inline uint64_t MonotonicNs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count());
}

template <class TSim, class TSnapshot, class TInput, int MaxEventsPerTick>
class SimThread {
public:
    // Sampled ON THE SIM THREAD at the start of each tick to get that tick's input events. Fn-ptr +
    // ctx (not std::function) to stay allocation-free — the callee writes up to Cap events into Out
    // and sets Count. Real play drains the input layer; tests return a scripted schedule by tick
    // index; a solo AI reads the (const) sim to decide its events. The sim is const-ref because an
    // AI's whole input IS the game state — read here, before StepEvents, so it is race-free.
    using InputFn = void (*)(void* Ctx, const TSim& S, uint32_t Tick, TInput* Out, int Cap,
                             int& Count);
    // True while the clock must be held (see rule 3).
    using HeldFn = bool (*)(void* Ctx, const TSim& S);
    // True when this input batch is allowed to release the hold. The whole batch is then stepped as
    // tick 0 — so a player's opening move and an AI's own first move land together, exactly as two
    // lockstep peers begin.
    using OpensFn = bool (*)(void* Ctx, const TSim& S, const TInput* Evs, int Count);

    struct Hooks {
        InputFn Input = nullptr;
        void* InputCtx = nullptr;
        HeldFn Held = nullptr;    // null => never held
        OpensFn Opens = nullptr;  // required if Held is set, else the gate could never open
        void* GateCtx = nullptr;
    };

    // TickRateHz is the sim rate; MaxTicks caps the per-iteration catch-up burst (rule 2).
    explicit SimThread(uint32_t TickRateHz, uint32_t MaxTicks = 8)
        : Clock(TickRateHz), MaxTicksPerService(MaxTicks) {}

    ~SimThread() { Stop(); }

    SimThread(const SimThread&) = delete;
    SimThread& operator=(const SimThread&) = delete;

    // The sim is owned here so the snapshot copy is local, but it is INITIALISED BY THE CALLER
    // before Start: seeding, and any game-specific pre-fill (a stress scene, a debug flag), are the
    // game's business and must happen while no thread is reading.
    TSim& GetSim() { return TheSim; }
    const TSim& GetSim() const { return TheSim; }

    // Install the hooks without spawning anything. Use this to drive the sim synchronously through
    // Service() — which is how the rules above are tested, and a legitimate way to run headless.
    void Configure(const Hooks& H) { Hook = H; }

    // Configure, publish the current state, then spawn the thread. The initial publish is not a
    // nicety: the consumer thread needs a frame to draw before the first tick lands, and without it
    // the first Consume fails and the first frame has nothing to show.
    void Start(const Hooks& H) {
        Configure(H);
        PublishCurrent();
        Running.store(true, std::memory_order_release);
        Thread = std::thread([this] { ThreadMain(); });
    }

    // Signal the thread to finish its current iteration and join. Idempotent, and safe if never
    // started.
    void Stop() {
        if (!Running.exchange(false, std::memory_order_acq_rel)) return;
        if (Thread.joinable()) Thread.join();
    }

    bool IsRunning() const { return Running.load(std::memory_order_acquire); }

    // ---- The loop body. Advance by ElapsedNs; returns the number of ticks actually stepped. ----
    // Call this directly (without Start) to drive the sim synchronously — that is how the rules above
    // are tested, and it is also a legitimate way to run headless.
    uint32_t Service(uint64_t ElapsedNs) {
        // Rule 3: while held, the clock is not touched, so the wait is DROPPED rather than banked.
        // Each iteration still asks for a batch, but applies it only if it qualifies — an invalid
        // input must not start the clock.
        if (Hook.Held != nullptr && Hook.Held(Hook.GateCtx, TheSim)) {
            TInput Evs[MaxEventsPerTick];
            int Count = 0;
            if (Hook.Input != nullptr)
                Hook.Input(Hook.InputCtx, TheSim, TheSim.Tick, Evs, MaxEventsPerTick, Count);
            if (Hook.Opens == nullptr || !Hook.Opens(Hook.GateCtx, TheSim, Evs, Count)) return 0;
            TheSim.StepEvents(Evs, Count);
            PublishCurrent();
            return 1;
        }
        // Rule 2: bounded burst, zero discard — the remainder stays in the accumulator.
        const uint32_t Owed = Clock.AdvancePreserving(ElapsedNs, MaxTicksPerService);
        for (uint32_t K = 0; K < Owed; ++K) {
            TInput Evs[MaxEventsPerTick];
            int Count = 0;
            if (Hook.Input != nullptr)  // rule 4: by TICK NUMBER, not by wall clock
                Hook.Input(Hook.InputCtx, TheSim, TheSim.Tick, Evs, MaxEventsPerTick, Count);
            TheSim.StepEvents(Evs, Count);
            PublishCurrent();
        }
        return Owed;
    }

    // ---- Consumer side (render thread), safe while running ----
    bool LatestSnapshot(TSnapshot& Out) const { return Mailbox.Consume(Out); }
    uint32_t PublishedTick() const { return PublishedTickCounter.load(std::memory_order_acquire); }

    // ---- Post-Stop accessors (thread joined; no other thread touches the sim) ----
    uint32_t FinalTick() const { return TheSim.Tick; }

private:
    // CaptureFrom (the heavy copy) runs UNLOCKED into the back buffer; Publish() only flips indices
    // under a short lock. The tick counter is stored with release AFTER the flip, so a consumer that
    // sees tick N is guaranteed to be able to consume it.
    void PublishCurrent() {
        Mailbox.Back().CaptureFrom(TheSim, MonotonicNs(), Clock.GetStepNs());
        Mailbox.Publish();
        PublishedTickCounter.store(TheSim.Tick, std::memory_order_release);
    }

    void ThreadMain() {
        uint64_t Last = MonotonicNs();
        while (Running.load(std::memory_order_acquire)) {
            const uint64_t Now = MonotonicNs();
            const uint64_t Elapsed = Now - Last;
            Last = Now;
            Service(Elapsed);
            // ~1 kHz, independent of vsync (rule 1). The accumulator absorbs the OS sleep
            // granularity, so tick TIMING stays correct even when the sleep overshoots. A transport
            // pump replaces this sleep with a bounded wait.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    TSim TheSim;
    SnapshotMailbox<TSnapshot> Mailbox;
    TickClock Clock;
    uint32_t MaxTicksPerService;

    std::thread Thread;
    std::atomic<bool> Running{false};
    std::atomic<uint32_t> PublishedTickCounter{0};

    Hooks Hook;
};

}  // namespace Lur::Sim
