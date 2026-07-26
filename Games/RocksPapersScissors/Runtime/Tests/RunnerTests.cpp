// Host tests for the tick-thread + snapshot seam (#69).
//
// The SimRunner is exercised with a LIVE thread, but the correctness assertions are
// timing-independent by construction: inputs are sampled by tick number, so the
// state after N ticks is a pure function of (seed, script) regardless of how the OS
// scheduled the thread. That lets us compare the threaded run to a synchronous one
// and get a determinism proof that cannot flake.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "Lur/Sim/Random.h"
#include "Lur/Sim/Tick.h"
#include "Rps/Sim.h"
#include "Rps/SimRunner.h"
#include "Rps/Snapshot.h"

using namespace Rps;
using Lur::Sim::SplitMix64;
using Lur::Sim::TickClock;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// ---- TickClock: the lockstep path must NEVER discard a tick ----
static void TestAdvancePreservingNeverDiscards() {
    TickClock C(TickRateHz);
    const uint64_t Step = C.GetStepNs();
    // One giant jump worth 100 ticks, but capped at 8 per call.
    uint32_t Total = C.AdvancePreserving(Step * 100, 8);
    CHECK(Total == 8);  // burst is bounded...
    // ...and the remaining 92 drain over later calls with zero further elapsed time.
    for (int I = 0; I < 200 && Total < 100; ++I) Total += C.AdvancePreserving(0, 8);
    CHECK(Total == 100);  // every owed tick eventually ran — nothing discarded

    // Sub-tick remainder is retained (no drift): 100 ticks + a bit more.
    TickClock D(TickRateHz);
    uint32_t T = D.AdvancePreserving(Step / 2, 8);
    CHECK(T == 0);  // half a tick -> not yet
    T += D.AdvancePreserving(Step / 2 + Step, 8);
    CHECK(T == 2);  // the two halves completed one tick, plus one more
}

// ---- SnapshotMailbox: publish/consume without threads ----
static void TestMailboxPublishConsume() {
    SnapshotMailbox* Mb = new SnapshotMailbox();
    Snapshot Out;
    CHECK(!Mb->Consume(Out));  // nothing published yet

    Mb->Back().Tick = 1;
    Mb->Back().Count = 5;
    Mb->Publish();
    CHECK(Mb->Consume(Out));
    CHECK(Out.Tick == 1 && Out.Count == 5);

    Mb->Back().Tick = 2;  // writes the OTHER buffer (indices flipped)
    Mb->Back().Count = 9;
    Mb->Publish();
    CHECK(Mb->Consume(Out));
    CHECK(Out.Tick == 2 && Out.Count == 9);  // latest wins
    delete Mb;
}

// ---- Snapshot: interpolation alpha clamps to [0,1] (no extrapolation) ----
static bool Near(float A, float B) { return (A - B) < 0.001f && (B - A) < 0.001f; }
static void TestInterpolationAlpha() {
    Snapshot S;
    S.PublishNs = 1000;
    S.StepNs = 100;
    CHECK(Near(S.AlphaAt(1000), 0.0f));
    CHECK(Near(S.AlphaAt(1050), 0.5f));
    CHECK(Near(S.AlphaAt(1100), 1.0f));
    CHECK(Near(S.AlphaAt(5000), 1.0f));  // next tick very late -> freeze at Pos
    CHECK(Near(S.AlphaAt(900), 0.0f));   // before publish
}

// ---- Scripted EVENT source (deterministic, by tick number): #137 place + queue ----
// Both teams place a mining camp early, then queue units at it. #135: the match opens empty
// (no start-miners), so the camps land at slots 0 (team 0) / 1 (team 1) — team 0's place
// applies before team 1's in the combined batch.
static void ScriptInput(void* /*Ctx*/, const Sim&, uint32_t Tick, InputEvent* Out, int Cap,
                        int& Count) {
    Count = 0;
    if (Cap < 2) return;
    if (Tick == 3) {
        Out[Count++] = InputEvent::Place(0, UnitMiner, F(17), F(10));
        Out[Count++] = InputEvent::Place(1, UnitMiner, F(17), F(230));
    } else if (Tick == 25) {
        Out[Count++] = InputEvent::Queue(0, 0, 10);
        Out[Count++] = InputEvent::Queue(1, 1, 10);
    }
}

// ---- The seam does not corrupt determinism: live threaded run == synchronous run ----
static void TestRunnerMatchesSynchronous() {
    constexpr uint64_t Seed = 0x1357;
    SimRunner* R = new SimRunner();
    R->Start(Seed, ScriptInput, nullptr);
    // Wait for enough ticks to place + queue + produce (bounded guard so a bug can't hang CI).
    constexpr uint32_t Target = 40;
    for (int I = 0; I < 2000 && R->PublishedTick() < Target; ++I)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    R->Stop();

    const uint32_t K = R->FinalTick();  // whatever it actually reached
    const uint64_t ThreadedHash = R->FinalStateHash();
    CHECK(K >= Target);

    // Replay EXACTLY K ticks synchronously with the same script -> must match.
    static Sim Synchronous;
    Synchronous.Init(Seed);
    for (uint32_t I = 0; I < K; ++I) {
        InputEvent Evs[MaxEventsPerTick];
        int Count = 0;
        ScriptInput(nullptr, Synchronous, I, Evs, MaxEventsPerTick, Count);
        Synchronous.StepEvents(Evs, Count);
    }
    CHECK(Synchronous.StateHash() == ThreadedHash);
    delete R;
}

// ---- Smoke: the thread runs decoupled from any render loop and publishes snapshots ----
static void TestRunnerPublishesSnapshots() {
    SimRunner* R = new SimRunner();
    R->Start(0xABC, nullptr, nullptr);  // no input: #135 opens with only gold, nothing placed

    Snapshot Snap;
    bool Got = false;
    for (int I = 0; I < 2000 && R->PublishedTick() < 3; ++I) {
        R->LatestSnapshot(Snap);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Got = R->LatestSnapshot(Snap);
    R->Stop();

    CHECK(Got);
    CHECK(Snap.Count == 2);  // #146: only the two auto-placed HOME BASES — no start-miners, no camp yet
    int32_t Homes = 0;
    for (int32_t I = 0; I < Snap.Count; ++I)
        if (Snap.IsAlive(I) && Snap.IsHomeBase(I)) ++Homes;
    CHECK(Homes == 2);  // one HQ per team, present from tick 0
    CHECK(Snap.Tick >= 3);
    // Every live unit sits inside the world bounds.
    bool InBounds = true;
    for (int32_t I = 0; I < Snap.Count; ++I)
        if (Snap.IsAlive(I) &&
            (Snap.PosX[I] < Fixed{0} || Snap.PosX[I] > WorldWidth ||
             Snap.PosY[I] < Fixed{0} || Snap.PosY[I] > WorldHeight))
            InBounds = false;
    CHECK(InBounds);
    delete R;
}

// ---- #158: the placement PREVIEW must agree with the SIM, everywhere ----
// This is the test whose absence let a real bug ship. The ghost's valid/invalid blink is evaluated
// on the render thread against a Snapshot, and that used to be a hand-copied mirror of
// Sim::CanPlaceBuilding with a comment asking editors to keep both in step. Widening the mine
// clearance updated the sim only, so the ghost turned GREEN a few units from a deposit and the drop
// was then refused — the preview lied, which reads as the game being broken.
//
// Sweeping the whole map at 1-unit resolution for both teams and every buildable type is the
// assertion that matters: not "the mirror looks right" but "these two functions cannot disagree on
// any input". It still has teeth now that the mirror is gone, because the snapshot's CAPTURE of the
// CVar block could regress independently of the predicate.
static void TestPreviewAgreesWithSim() {
    static Sim S;
    S.Init(0x158);
    // A few buildings and a spent deposit so overlap, clearance and depleted-mine paths all appear.
    S.Teams[0].Gold = 100000;
    S.Teams[1].Gold = 100000;
    S.MineGold[0] = 0;   // depleted -> building over it IS allowed; the two must agree on that too
    Snapshot Snap;
    Snap.CaptureFrom(S, 0, 100'000'000ull);
    int Mismatches = 0, Accepted = 0;
    const int32_t Wi = WorldWidth.ToInt(), Hi = WorldHeight.ToInt();
    for (uint8_t Team = 0; Team < 2; ++Team)
        for (uint8_t Type = UnitMiner; Type < UnitCount; ++Type)
            for (int32_t X = 0; X <= Wi; ++X)
                for (int32_t Y = 0; Y <= Hi; ++Y) {
                    const bool Sim_ = S.CanPlaceBuilding(Team, Type, F(X), F(Y));
                    const bool Prev = Snap.CanPlaceBuilding(Team, Type, F(X), F(Y));
                    if (Sim_ != Prev) ++Mismatches;
                    if (Sim_) ++Accepted;
                }
    CHECK(Mismatches == 0);
    CHECK(Accepted > 0);   // a sweep that accepts nothing would pass vacuously
    if (Mismatches != 0) std::printf("  preview/sim disagreed on %d of the swept points\n", Mismatches);
}

int main() {
    TestAdvancePreservingNeverDiscards();
    TestMailboxPublishConsume();
    TestInterpolationAlpha();
    TestRunnerMatchesSynchronous();
    TestRunnerPublishesSnapshots();
    TestPreviewAgreesWithSim();

    if (GFailures == 0) std::printf("rps_runtime_tests: ALL PASS\n");
    else std::printf("rps_runtime_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
