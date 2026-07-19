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

// ---- Scripted input source (deterministic, by tick number) ----
struct Script {
    int N = 0;
    uint8_t M0[4096] = {};
    uint8_t M1[4096] = {};
};
static void ScriptInput(void* Ctx, uint32_t Tick, uint8_t& M0, uint8_t& M1) {
    const Script* S = static_cast<const Script*>(Ctx);
    if (static_cast<int>(Tick) < S->N) { M0 = S->M0[Tick]; M1 = S->M1[Tick]; }
    else { M0 = 0; M1 = 0; }
}

// ---- The seam does not corrupt determinism: live threaded run == synchronous run ----
static void TestRunnerMatchesSynchronous() {
    constexpr uint64_t Seed = 0x1357;
    static Script Sc;
    Sc.N = 4096;
    SplitMix64 Rng(0xBEEF);
    for (int I = 0; I < Sc.N; ++I) {
        Sc.M0[I] = static_cast<uint8_t>(Rng.NextBounded(16)) | 0x2;
        Sc.M1[I] = static_cast<uint8_t>(Rng.NextBounded(16)) | 0x4;
    }

    SimRunner* R = new SimRunner();
    R->Start(Seed, ScriptInput, &Sc);
    // Wait for a modest number of ticks (bounded wall-clock guard so a bug can't hang CI).
    constexpr uint32_t Target = 15;  // ~1.5 s at 10 Hz
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
        uint8_t M0, M1;
        ScriptInput(&Sc, I, M0, M1);
        Synchronous.Step(M0, M1);
    }
    CHECK(Synchronous.StateHash() == ThreadedHash);
    delete R;
}

// ---- Smoke: the thread runs decoupled from any render loop and publishes snapshots ----
static void TestRunnerPublishesSnapshots() {
    SimRunner* R = new SimRunner();
    R->Start(0xABC, nullptr, nullptr);  // no input: the 3 starting workers just gather

    Snapshot Snap;
    bool Got = false;
    for (int I = 0; I < 2000 && R->PublishedTick() < 3; ++I) {
        R->LatestSnapshot(Snap);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    Got = R->LatestSnapshot(Snap);
    R->Stop();

    CHECK(Got);
    CHECK(Snap.Count == StartLumberjacks * 2);  // 6 workers, none built yet this early
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

int main() {
    TestAdvancePreservingNeverDiscards();
    TestMailboxPublishConsume();
    TestInterpolationAlpha();
    TestRunnerMatchesSynchronous();
    TestRunnerPublishesSnapshots();

    if (GFailures == 0) std::printf("rps_runtime_tests: ALL PASS\n");
    else std::printf("rps_runtime_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
