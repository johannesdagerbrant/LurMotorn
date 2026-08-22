// Tests for Lur::Sim::InputInbox — the input-thread -> sim-thread event inbox promoted out of
// Rps::SoloInputInbox and Rps::LockstepPeer::PendingLocalEvents (#201).
//
// The assertions that matter are about ORDER and about the DROP. Order, because two events on one sim
// tick (place a building, then queue units into what was just placed) only make sense in the order the
// player made them, and a partial drain that reorders the remainder is a determinism bug that shows up
// as one phone building something the other didn't. The drop, because unifying the two inboxes made the
// lockstep path BOUNDED where it used to be an unbounded vector — so a full inbox is a new failure mode
// and it must be countable rather than silent.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <thread>

#include "Lur/Sim/InputInbox.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

namespace {
struct Ev {
    int32_t Id = 0;
};
using Inbox = Lur::Sim::InputInbox<Ev, 8>;
}  // namespace

// ---- FIFO, and a partial drain keeps the remainder IN ORDER ----
// The headline test. Drain(Out, 3) on five queued events must yield 1,2,3 and leave 4,5 — not 4,5
// shuffled, and not 1,2 again.
static void TestPartialDrainPreservesOrder() {
    Inbox B;
    for (int I = 1; I <= 5; ++I) CHECK(B.Push(Ev{I}));
    CHECK(B.Size() == 5);

    Ev Out[8];
    CHECK(B.Drain(Out, 3) == 3);
    CHECK(Out[0].Id == 1 && Out[1].Id == 2 && Out[2].Id == 3);
    CHECK(B.Size() == 2);

    CHECK(B.Drain(Out, 8) == 2);
    CHECK(Out[0].Id == 4 && Out[1].Id == 5);   // the remainder, still in order
    CHECK(B.Size() == 0);
    CHECK(B.Drain(Out, 8) == 0);               // and it is genuinely empty
}

// ---- Draining more than once mid-stream still never reorders ----
// Interleaving pushes with partial drains is the real access pattern: the player keeps tapping while
// the sim thread keeps draining.
static void TestInterleavedPushAndDrainKeepsOrder() {
    Inbox B;
    Ev Out[8];
    int Next = 1, Seen = 0;
    // Drain slightly FASTER than the pushes, so the backlog never reaches the cap: this test is about
    // ordering, and a drop here would look like a reorder. The drop itself is covered separately.
    for (int Round = 0; Round < 20; ++Round) {
        for (int K = 0; K < 2; ++K) CHECK(B.Push(Ev{Next++}));
        const int N = B.Drain(Out, 3);
        for (int I = 0; I < N; ++I) {
            ++Seen;
            CHECK(Out[I].Id == Seen);   // strictly ascending with no gaps: nothing lost, nothing swapped
        }
    }
    // Whatever is still queued must continue the sequence.
    const int N = B.Drain(Out, 8);
    for (int I = 0; I < N; ++I) CHECK(Out[I].Id == ++Seen);
}

// ---- A full inbox drops the NEWEST, says so, and COUNTS it ----
// The unbounded vector this replaced could not drop, so the count is what keeps the new failure mode
// from being silent.
static void TestFullInboxDropsAndCounts() {
    Inbox B;
    for (int I = 0; I < Inbox::Capacity; ++I) CHECK(B.Push(Ev{I}));
    CHECK(B.Dropped() == 0);
    CHECK(!B.Push(Ev{999}));            // rejected, and the caller is TOLD
    CHECK(!B.Push(Ev{1000}));
    CHECK(B.Dropped() == 2);
    CHECK(B.Size() == Inbox::Capacity);

    // The OLDEST events survived — a dropped tap is better than a reordered timeline.
    Ev Out[Inbox::Capacity];
    CHECK(B.Drain(Out, Inbox::Capacity) == Inbox::Capacity);
    for (int I = 0; I < Inbox::Capacity; ++I) CHECK(Out[I].Id == I);

    // Room again, and the running total is NOT reset — a caller that logs the first drop can still
    // report the tally afterwards.
    CHECK(B.Push(Ev{7}));
    CHECK(B.Dropped() == 2);
}

// ---- Visit reads without consuming ----
// The pre-match case: look for one qualifying event among the queued ones and decide from sim state
// whether to accept it. A Drain-then-inspect would have destroyed the queue before discovering that
// nothing qualified.
static void TestVisitDoesNotConsume() {
    Inbox B;
    for (int I = 1; I <= 4; ++I) B.Push(Ev{I});
    int Sum = 0, N = 0;
    B.Visit([&](const Ev& E) { Sum += E.Id; ++N; });
    CHECK(N == 4);
    CHECK(Sum == 10);
    CHECK(B.Size() == 4);          // still all there
    B.Visit([&](const Ev&) { ++N; });
    CHECK(N == 8);                 // and visitable again
    Ev Out[8];
    CHECK(B.Drain(Out, 8) == 4);
}

// ---- Clear empties without disturbing the drop tally ----
static void TestClear() {
    Inbox B;
    for (int I = 0; I < Inbox::Capacity + 1; ++I) B.Push(Ev{I});
    CHECK(B.Dropped() == 1);
    B.Clear();
    CHECK(B.Size() == 0);
    CHECK(B.Dropped() == 1);
    Ev Out[8];
    CHECK(B.Drain(Out, 8) == 0);
    int N = 0;
    B.Visit([&](const Ev&) { ++N; });
    CHECK(N == 0);
}

// ---- Drain(Out, 0) is a no-op, not a queue-eater ----
static void TestZeroMaxDrainsNothing() {
    Inbox B;
    B.Push(Ev{1});
    Ev Out[8];
    CHECK(B.Drain(Out, 0) == 0);
    CHECK(B.Size() == 1);
}

// ---- Two threads: nothing is lost, nothing is duplicated, order is preserved ----
// The whole point of the lock. Not a timing assertion — a conservation one, which holds however the
// OS schedules the two threads.
static void TestConcurrentProducerConsumer() {
    Lur::Sim::InputInbox<Ev, 64> B;
    constexpr int Total = 20000;
    std::atomic<bool> Abort{false};
    std::thread Producer([&] {
        int I = 1;
        while (I <= Total && !Abort.load(std::memory_order_relaxed))
            if (B.Push(Ev{I})) ++I;   // retry on a full inbox so nothing is lost
    });
    int Seen = 0;
    bool Ordered = true;
    Ev Out[64];
    // BOUNDED, not `while (Seen < Total)`: an inbox that loses events would spin here forever, and a
    // sabotage pass that hangs teaches nothing — it just wedges the run.
    //
    // Bounded by TIME, not by iteration count. The first version of this bound counted total spins
    // (Total * 100), which is not a proxy for progress: on a machine where the consumer out-paces the
    // producer, most drains legitimately return nothing, so it burned through 2,000,000 empty drains
    // and gave up before the producer had finished. That passed under g++ on Windows and FAILED under
    // Apple clang on CI — a flake I introduced while fixing a hang.
    //
    // A wall-clock deadline is machine-independent in the right way: a working inbox finishes this in
    // milliseconds, and only a genuinely broken one can reach 30 seconds.
    const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (Seen < Total && std::chrono::steady_clock::now() < Deadline) {
        const int N = B.Drain(Out, 7);
        // Yield on an empty drain rather than spinning hot — it stops the consumer starving the
        // producer of a core, which is the situation that made the spin bound look plausible.
        if (N == 0) {
            std::this_thread::yield();
            continue;
        }
        for (int I = 0; I < N; ++I) {
            ++Seen;
            if (Out[I].Id != Seen) Ordered = false;
        }
    }
    Abort.store(true, std::memory_order_relaxed);
    Producer.join();
    CHECK(Seen == Total);
    CHECK(Ordered);
    CHECK(B.Size() == 0);
}

int main() {
    TestPartialDrainPreservesOrder();
    TestInterleavedPushAndDrainKeepsOrder();
    TestFullInboxDropsAndCounts();
    TestVisitDoesNotConsume();
    TestClear();
    TestZeroMaxDrainsNothing();
    TestConcurrentProducerConsumer();
    if (GFailures == 0) std::printf("input_inbox_tests: ALL PASS\n");
    else std::printf("input_inbox_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
