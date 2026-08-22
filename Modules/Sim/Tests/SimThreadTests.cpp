// Tests for Lur::Sim::SimThread — the fixed-timestep sim thread promoted out of Rps::SimRunner (#201).
//
// Almost everything here drives Service() SYNCHRONOUSLY with exact durations. That is the reason the
// loop body was split out of the thread: the three rules worth testing (bounded-but-never-discarded
// catch-up, a held clock that drops rather than banks elapsed time, input sampled by tick number) are
// all silent-desync rules, and a test that has to race a real thread to see them can only be flaky.
// A flaky test for a silent-desync rule gets muted, which is worse than not having it.
//
// One test does use the live thread, but only to check lifecycle (start/publish/stop/idempotence) —
// never a tick count.
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>

#include "Lur/Sim/SimThread.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

namespace {

constexpr int MaxEvents = 4;

// A minimal stand-in for a game sim: it accumulates the events it was handed so the tests can prove
// WHICH tick each batch landed on, and a gate flag so the pre-match hold can be exercised.
struct FakeSim {
    uint32_t Tick = 0;
    int32_t Applied = 0;     // total events stepped
    uint32_t Opened = 0;     // set to the tick the gate-opening event landed on, +1 (0 = never)
    int32_t LastTag = -1;

    void StepEvents(int32_t* Evs, int Count) {
        for (int I = 0; I < Count; ++I) {
            if (Evs[I] == 999 && Opened == 0) Opened = Tick + 1;
            LastTag = Evs[I];
            ++Applied;
        }
        ++Tick;
    }
};

// Trivially copyable, as SnapshotMailbox requires.
struct FakeSnapshot {
    uint32_t Tick = 0;
    int32_t Applied = 0;
    uint64_t StepNs = 0;
    void CaptureFrom(const FakeSim& S, uint64_t /*NowNs*/, uint64_t InStepNs) {
        Tick = S.Tick;
        Applied = S.Applied;
        StepNs = InStepNs;
    }
};

using Thread10Hz = Lur::Sim::SimThread<FakeSim, FakeSnapshot, int32_t, MaxEvents>;

// Records the tick number of every input sample, so "sampled once per tick, by tick number, in
// order" is directly observable.
struct Recorder {
    static constexpr int Cap = 64;
    uint32_t Ticks[Cap] = {};
    int N = 0;       // entries recorded in Ticks (bounded by Cap)
    int Calls = 0;   // total samples, unbounded
    int32_t Emit = -1;   // >=0: emit this one event on every sample
};

void Sample(void* Ctx, const FakeSim& /*S*/, uint32_t Tick, int32_t* Out, int Cap, int& Count) {
    Recorder* R = static_cast<Recorder*>(Ctx);
    ++R->Calls;
    if (R->N < Recorder::Cap) R->Ticks[R->N++] = Tick;
    Count = 0;
    if (R->Emit >= 0 && Cap > 0) Out[Count++] = R->Emit;
}

}  // namespace

// ---- Rule 2: the burst is BOUNDED, and nothing is discarded ----
// The headline test. A hitch worth 100 ticks must not run 100 ticks in one iteration (that blocks the
// service loop) and must not drop 92 of them (that desyncs a lockstep peer with nothing on the wire
// to blame). It must drain.
static void TestBurstIsCappedAndNothingIsDiscarded() {
    Thread10Hz T(10, /*MaxTicks*/ 8);
    Recorder R;
    Thread10Hz::Hooks H;
    H.Input = &Sample;
    H.InputCtx = &R;
    T.Configure(H);

    const uint64_t Step = 100'000'000ull;   // 10 Hz
    uint32_t Total = T.Service(Step * 100);
    CHECK(Total == 8);                      // bounded...
    CHECK(T.GetSim().Tick == 8);
    // ...and the remaining 92 drain over later iterations with ZERO further elapsed time.
    for (int I = 0; I < 200 && Total < 100; ++I) Total += T.Service(0);
    CHECK(Total == 100);
    CHECK(T.GetSim().Tick == 100);
    CHECK(T.Service(0) == 0);               // and then it stops — no overshoot
}

// ---- Sub-tick remainder is retained: no drift ----
static void TestSubTickRemainderIsRetained() {
    Thread10Hz T(10);
    const uint64_t Step = 100'000'000ull;
    CHECK(T.Service(Step / 2) == 0);            // half a tick is not a tick
    CHECK(T.Service(Step / 2) == 1);            // the two halves complete one
    CHECK(T.GetSim().Tick == 1);
    // Ten tenths accumulate to exactly one more tick, not zero and not two.
    uint32_t N = 0;
    for (int I = 0; I < 10; ++I) N += T.Service(Step / 10);
    CHECK(N == 1);
}

// ---- Rule 4: input is sampled once per tick, BY TICK NUMBER, in order ----
static void TestInputSampledOncePerTickByTickNumber() {
    Thread10Hz T(10);
    Recorder R;
    Thread10Hz::Hooks H;
    H.Input = &Sample;
    H.InputCtx = &R;
    T.Configure(H);

    const uint64_t Step = 100'000'000ull;
    T.Service(Step * 3);
    CHECK(R.N == 3);
    for (int I = 0; I < R.N; ++I) CHECK(R.Ticks[I] == static_cast<uint32_t>(I));
    // Timing does not change the schedule: three separate iterations give ticks 3,4,5 in order.
    T.Service(Step);
    T.Service(Step);
    T.Service(Step);
    CHECK(R.N == 6);
    CHECK(R.Ticks[3] == 3 && R.Ticks[4] == 4 && R.Ticks[5] == 5);
}

// ---- Rule 3: a HELD clock drops elapsed time rather than banking it ----
// The bug this prevents: the player spends 12 s deciding where to place their opening camp, and the
// match then opens with 120 owed ticks of catch-up. Held time must vanish, not queue up.
static void TestHoldDropsElapsedTimeRatherThanBanking() {
    Thread10Hz T(10);
    Recorder R;
    Thread10Hz::Hooks H;
    H.Input = &Sample;
    H.InputCtx = &R;
    H.Held = [](void*, const FakeSim& S) { return S.Opened == 0; };
    H.Opens = [](void*, const FakeSim&, const int32_t* Evs, int Count) {
        for (int I = 0; I < Count; ++I)
            if (Evs[I] == 999) return true;
        return false;
    };
    T.Configure(H);

    const uint64_t Step = 100'000'000ull;
    // 12 seconds of thinking, in service iterations. Nothing may tick.
    for (int I = 0; I < 120; ++I) CHECK(T.Service(Step) == 0);
    CHECK(T.GetSim().Tick == 0);
    CHECK(R.Calls == 120);   // the input layer was still asked every iteration

    // Now a qualifying event arrives: exactly one tick runs, and it is tick 0.
    R.Emit = 999;
    CHECK(T.Service(Step) == 1);
    CHECK(T.GetSim().Opened == 1);   // landed on tick 0
    CHECK(T.GetSim().Tick == 1);

    // THE POINT: the 12 s of held time was dropped. One step of elapsed time now owes ONE tick, not
    // 121. A banked accumulator would fail here.
    CHECK(T.Service(Step) == 1);
    CHECK(T.GetSim().Tick == 2);
}

// ---- A non-qualifying input must not start the clock ----
static void TestUnqualifiedInputDoesNotOpenTheGate() {
    Thread10Hz T(10);
    Recorder R;
    R.Emit = 7;   // an event, but not the one that opens
    Thread10Hz::Hooks H;
    H.Input = &Sample;
    H.InputCtx = &R;
    H.Held = [](void*, const FakeSim& S) { return S.Opened == 0; };
    H.Opens = [](void*, const FakeSim&, const int32_t* Evs, int Count) {
        for (int I = 0; I < Count; ++I)
            if (Evs[I] == 999) return true;
        return false;
    };
    T.Configure(H);
    const uint64_t Step = 100'000'000ull;
    for (int I = 0; I < 20; ++I) CHECK(T.Service(Step) == 0);
    CHECK(T.GetSim().Tick == 0);
    CHECK(T.GetSim().Applied == 0);   // and the rejected batch was NOT stepped
}

// ---- A Held hook with no Opens hook can never open (and must not tick) ----
static void TestHeldWithoutOpensNeverTicks() {
    Thread10Hz T(10);
    Thread10Hz::Hooks H;
    H.Held = [](void*, const FakeSim& S) { return S.Opened == 0; };
    T.Configure(H);
    CHECK(T.Service(100'000'000ull * 50) == 0);
    CHECK(T.GetSim().Tick == 0);
}

// ---- No hooks at all: the sim still advances, with empty input ----
static void TestNoHooksStillTicks() {
    Thread10Hz T(10);
    CHECK(T.Service(100'000'000ull * 3) == 3);
    CHECK(T.GetSim().Tick == 3);
    CHECK(T.GetSim().Applied == 0);
}

// ---- Every tick publishes, and the counter matches the mailbox ----
static void TestEveryTickPublishes() {
    Thread10Hz T(10);
    FakeSnapshot Snap;
    CHECK(!T.LatestSnapshot(Snap));            // nothing published before the first tick
    T.Service(100'000'000ull * 5);
    CHECK(T.PublishedTick() == 5);
    CHECK(T.LatestSnapshot(Snap));
    CHECK(Snap.Tick == 5);                     // the mailbox holds the newest, not the oldest
    CHECK(Snap.StepNs == 100'000'000ull);      // and carries the step for the interpolator
    // Consume is a read of the LATEST, not a queue pop: a render thread that draws twice between
    // ticks must get the same frame again rather than nothing.
    FakeSnapshot Again;
    CHECK(T.LatestSnapshot(Again));
    CHECK(Again.Tick == 5);
    // Intermediate ticks are OVERWRITTEN, not queued — the consumer always sees the newest.
    T.Service(100'000'000ull * 3);
    CHECK(T.LatestSnapshot(Snap));
    CHECK(Snap.Tick == 8);
}

// ---- Lifecycle, with a LIVE thread: publishes before the first tick, and Stop is idempotent ----
// Deliberately asserts nothing about tick COUNTS — that would be racing the scheduler.
static void TestThreadLifecycle() {
    Thread10Hz T(10);
    Recorder R;
    Thread10Hz::Hooks H;
    H.Input = &Sample;
    H.InputCtx = &R;
    T.Start(H);
    // The initial publish happens on the CALLER before the thread spawns, so a consumer always has a
    // frame to draw. Without it the first Consume fails and the first frame has nothing to show.
    FakeSnapshot Snap;
    CHECK(T.LatestSnapshot(Snap));
    CHECK(Snap.Tick == 0);
    CHECK(T.IsRunning());
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    T.Stop();
    CHECK(!T.IsRunning());
    T.Stop();                                  // idempotent
    T.Stop();
    CHECK(T.FinalTick() >= 1);                 // 250 ms at 10 Hz owes at least one tick
    CHECK(T.FinalTick() == T.PublishedTick());
}

// ---- Stop without Start must not join an unstarted thread ----
static void TestStopWithoutStart() {
    Thread10Hz T(10);
    T.Stop();
    CHECK(!T.IsRunning());
}

int main() {
    TestBurstIsCappedAndNothingIsDiscarded();
    TestSubTickRemainderIsRetained();
    TestInputSampledOncePerTickByTickNumber();
    TestHoldDropsElapsedTimeRatherThanBanking();
    TestUnqualifiedInputDoesNotOpenTheGate();
    TestHeldWithoutOpensNeverTicks();
    TestNoHooksStillTicks();
    TestEveryTickPublishes();
    TestThreadLifecycle();
    TestStopWithoutStart();
    if (GFailures == 0) std::printf("sim_thread_tests: ALL PASS\n");
    else std::printf("sim_thread_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
