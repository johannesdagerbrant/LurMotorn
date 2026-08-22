// Tests for Lur::Sim::SnapshotRing — the rollback restore buffer promoted out of Rps::SnapshotRing
// (#201).
//
// The assertion that carries the class is TestEvictedTickReturnsNullNotStaleState. Modular indexing
// means an evicted tick's SLOT is still populated with a perfectly valid-looking state — just the
// wrong one. Handing that back would restore the sim to a tick nobody asked for, and the symptom is a
// divergence with nothing on the wire to blame. The tick tag is the whole defence.
#include <cstdio>
#include <cstdint>

#include "Lur/Sim/SnapshotRing.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

namespace {
// Stands in for a sim state: a payload plus the tick it belongs to, so a wrong-slot answer is visible.
struct State {
    uint32_t Tick = 0;
    int32_t Payload = 0;
};
using Ring = Lur::Sim::SnapshotRing<State>;
}  // namespace

// ---- Round trip: what went in at T comes back for T ----
static void TestRoundTrip() {
    Ring R(18);
    for (uint32_t T = 0; T < 18; ++T) R.Save(T, State{T, static_cast<int32_t>(T) * 7});
    for (uint32_t T = 0; T < 18; ++T) {
        const State* S = R.Get(T);
        CHECK(S != nullptr);
        if (S != nullptr) {
            CHECK(S->Tick == T);
            CHECK(S->Payload == static_cast<int32_t>(T) * 7);
        }
    }
    CHECK(R.Capacity() == 18);
}

// ---- THE ONE THAT MATTERS: an evicted tick returns NULL, never the slot's current occupant ----
static void TestEvictedTickReturnsNullNotStaleState() {
    constexpr uint32_t Cap = 8;
    Ring R(Cap);
    for (uint32_t T = 0; T < Cap; ++T) R.Save(T, State{T, static_cast<int32_t>(T)});
    // Saving tick 8 reuses slot 0, evicting tick 0.
    R.Save(Cap, State{Cap, 100});
    CHECK(R.Get(0) == nullptr);          // NOT a pointer to tick 8's state
    CHECK(R.Get(Cap) != nullptr);
    if (R.Get(Cap) != nullptr) CHECK(R.Get(Cap)->Tick == Cap);
    // Everything still inside the window survives.
    for (uint32_t T = 1; T < Cap; ++T) {
        const State* S = R.Get(T);
        CHECK(S != nullptr);
        if (S != nullptr) CHECK(S->Tick == T);
    }
    // A tick that shares a slot with a held tick but is far in the future is also absent.
    CHECK(R.Get(Cap * 5) == nullptr);
}

// ---- A never-saved ring answers null for everything, including tick 0 ----
// Tick 0 is the trap: a zero-initialised tag would make slot 0 look like a valid tick-0 snapshot.
static void TestEmptyRingHasNoTickZero() {
    Ring R(4);
    CHECK(R.Get(0) == nullptr);
    for (uint32_t T = 0; T < 20; ++T) CHECK(R.Get(T) == nullptr);
    R.Save(0, State{0, 42});
    CHECK(R.Get(0) != nullptr);
}

// ---- Clear forgets everything, including tick 0 ----
static void TestClear() {
    Ring R(4);
    for (uint32_t T = 0; T < 4; ++T) R.Save(T, State{T, 1});
    R.Clear();
    for (uint32_t T = 0; T < 4; ++T) CHECK(R.Get(T) == nullptr);
    R.Save(2, State{2, 9});
    CHECK(R.Get(2) != nullptr);
    CHECK(R.Get(0) == nullptr);
}

// ---- Re-saving the same tick overwrites in place ----
// The rollback loop re-simulates a tick it already stored; the newer state must win.
static void TestResaveSameTickOverwrites() {
    Ring R(4);
    R.Save(3, State{3, 1});
    R.Save(3, State{3, 2});
    const State* S = R.Get(3);
    CHECK(S != nullptr);
    if (S != nullptr) CHECK(S->Payload == 2);
}

// ---- Zero capacity is clamped to 1 rather than dividing by zero ----
static void TestZeroCapacityIsClamped() {
    Ring R(0);
    CHECK(R.Capacity() == 1);
    R.Save(7, State{7, 5});
    CHECK(R.Get(7) != nullptr);
    CHECK(R.Get(6) == nullptr);
    R.Save(8, State{8, 6});
    CHECK(R.Get(7) == nullptr);   // one slot: 8 evicted 7
}

// ---- A sliding window: only the last Capacity ticks are answerable ----
// This is what sizing the ring means in practice, so it is worth stating as a test rather than a
// comment: the horizon the netcode can roll back to IS the capacity.
static void TestSlidingWindowDepth() {
    constexpr uint32_t Cap = 6;
    Ring R(Cap);
    for (uint32_t T = 0; T < 100; ++T) R.Save(T, State{T, static_cast<int32_t>(T)});
    for (uint32_t T = 94; T < 100; ++T) CHECK(R.Get(T) != nullptr);
    for (uint32_t T = 0; T < 94; ++T) CHECK(R.Get(T) == nullptr);
}

int main() {
    TestRoundTrip();
    TestEvictedTickReturnsNullNotStaleState();
    TestEmptyRingHasNoTickZero();
    TestClear();
    TestResaveSameTickOverwrites();
    TestZeroCapacityIsClamped();
    TestSlidingWindowDepth();
    if (GFailures == 0) std::printf("snapshot_ring_tests: ALL PASS\n");
    else std::printf("snapshot_ring_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
