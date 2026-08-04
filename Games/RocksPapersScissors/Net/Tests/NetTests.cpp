// Host tests for the RPS lockstep netcode. Starts with the event codec: round-trip,
// the byte budget (a press/watermark is 1 byte before framing), and fuzz-safety
// (hostile bytes never crash the decoder). Chess's fuzz_tests as the pattern.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "Lur/Core/BuildFingerprint.h"
#include "Lur/Net/Session.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Loopback.h"
#include "Rps/EventCodec.h"
#include "Rps/LockstepPeer.h"
#include "Rps/MatchRecord.h"   // #159: two peers recording one linked match, then compared
#include "Rps/SnapshotRing.h"  // rollback Phase 1 scaffolding (snapshot ring + peer predictor)
#include "Rps/SessionWiring.h" // the mains' Session->LockstepPeer routing table, shared verbatim

using namespace Rps;
using Lur::Serialization::BitReader;
using Lur::Serialization::BitWriter;
using Lur::Sim::SplitMix64;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// ---- #137: input-EVENT batch codec — round-trip place + queue events, and the empty batch ----
static void TestEventBatchRoundTrip() {
    InputEvent In[] = {
        InputEvent::Place(0, UnitMiner, F(17), F(10)),
        InputEvent::Queue(0, 6, 20),
        InputEvent::Place(1, UnitScissor, F(8), F(230)),
        InputEvent::Queue(1, 7, 1),
    };
    BitWriter W;
    EncodeEventBatch(W, In, 4);
    const std::vector<uint8_t>& Bytes = W.Finish();
    BitReader R(Bytes.data(), Bytes.size());
    InputEvent Out[MaxEventsPerTick];
    const int N = DecodeEventBatch(R, Out, MaxEventsPerTick);
    CHECK(N == 4);
    for (int I = 0; I < 4; ++I) {
        CHECK(Out[I].Kind == In[I].Kind && Out[I].Team == In[I].Team);
        CHECK(Out[I].X == In[I].X && Out[I].Y == In[I].Y);
        if (In[I].Kind == EventPlaceBuilding) CHECK(Out[I].Type == In[I].Type);
    }
    // The empty batch (the common idle tick) round-trips to zero and is tiny.
    BitWriter We;
    EncodeEventBatch(We, nullptr, 0);
    CHECK(We.Finish().size() == 1);  // just the varint 0 count
    BitReader Re(We.Finish().data(), We.Finish().size());
    InputEvent None[MaxEventsPerTick];
    CHECK(DecodeEventBatch(Re, None, MaxEventsPerTick) == 0);
}

// ---- #137: the batch decoder is TOTAL on hostile bytes (never traps, honours the cap) ----
static void TestEventBatchFuzz() {
    SplitMix64 Rng(0xB47C4);
    for (int Iter = 0; Iter < 20000; ++Iter) {
        uint8_t Buf[16];
        const int N = 1 + static_cast<int>(Rng.NextBounded(16));
        for (int I = 0; I < N; ++I) Buf[I] = static_cast<uint8_t>(Rng.NextBounded(256));
        BitReader R(Buf, static_cast<size_t>(N));
        InputEvent Out[MaxEventsPerTick];
        const int Got = DecodeEventBatch(R, Out, MaxEventsPerTick);
        CHECK(Got <= MaxEventsPerTick);  // never overruns the buffer
    }
    CHECK(true);  // no crash/hang == pass
}

// #137: a deterministic per-tick input schedule for the lockstep tests — each peer places a
// mining camp early (funded by the CvStartingGold Init default) then queues units at it, so the
// tests exercise real place/queue EVENTS over the wire and stay reproducible. Team 0 builds in
// the bottom band, team 1 in the top. #135: the match opens empty (no start-miners), so the camp
// lands at slot 0 (team 0) / 1 (team 1) — the combined batch applies team 0's place before team 1's.

// ONE definition of the tests' opening-camp spot, because it has to satisfy several map rules at
// once and a scattered magic coordinate silently rots when any of them moves (#157 did exactly
// that — the old (17, 14) fell inside the widened mine clearance and every match-start test failed
// at once, looking like a lockstep bug rather than a stale coordinate). It must be:
//   * >= Cv.MineClearance (7) from every live mine. X=17 sits midway between the mine columns at
//     14 and 20, so dx=3 and the end mine row at Y=9 forces dy >= sqrt(49-9) = 6.33 -> Y >= 15.4.
//   * >= 2 x footprint from the HQ, which auto-places at InitialFrontier x 3/4 = 26.25.
//   * inside the team's opening frontier (<= 35 from its own end).
// Y=16 clears all three with margin; mirrored for team 1.
static const Fixed CampTestX = F(17);
static Fixed CampTestY(uint8_t Team) { return Team == 0 ? F(16) : F(WorldHeight.ToInt() - 16); }
// Slot 2/3 is each team's OPENING CAMP, not 0/1: #146 auto-places the two home bases at slots 0/1
// before tick 0, and tick 0's two camps land next. This said 0/1 (a comment from before the home bases
// existed), and ApplyQueue REJECTS the home base — it produces nothing — so every queue event these
// lockstep tests "exercised over the wire" was a decoded no-op. The codec coverage was real; the
// gameplay-effect coverage was not, which is worth more here: an event that changes nothing cannot
// expose a misaligned or dropped input stream.
static void DriveInput(LockstepPeer& P, uint8_t Team, int TickIdx) {
    if (TickIdx == 3)
        P.QueueLocalEvent(InputEvent::Place(Team, UnitMiner, CampTestX, CampTestY(Team)));
    else if (TickIdx == 15)
        P.QueueLocalEvent(InputEvent::Queue(Team, Team == 0 ? 2 : 3, 5));
}

// Replay a recorded/reassembled EVENT stream (combined batch per tick) into a fresh sim -> hash.
static uint64_t ReplayHashEvents(uint64_t Seed, const std::vector<std::vector<InputEvent>>& Rec) {
    static Sim S;
    S.Init(Seed);
    for (const std::vector<InputEvent>& Batch : Rec)
        S.StepEvents(Batch.data(), static_cast<int32_t>(Batch.size()));
    return S.StateHash();
}

// ---- Lockstep harness: a QUEUED link (models the real deferred delivery / Pump, and
// avoids the synchronous re-entrancy hazard a naive loopback has). ----
struct Outbox {
    std::vector<std::pair<Lur::Net::EMsgType, std::vector<uint8_t>>> Q;
};
static void Enqueue(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* Data, std::size_t N) {
    static_cast<Outbox*>(Ctx)->Q.emplace_back(Type, std::vector<uint8_t>(Data, Data + N));
}
static void Deliver(Outbox& From, LockstepPeer& To) {
    for (auto& M : From.Q) To.OnMessage(M.first, M.second.data(), M.second.size());
    From.Q.clear();
}
static constexpr uint64_t OneTickNs = 100'000'000ull;  // 10 Hz

// Drive both peers to the SAME head so raw head-state hashes are comparable. Under rollback the two
// run on independent wall clocks and their speculative heads can sit a tick apart (most visibly after
// a staggered recovery reseed) even though their CONFIRMED timelines already agree — lockstep gave
// equal heads for free, rollback needs this. Ticks whichever peer is behind (speculating the idle
// peer forward) until the heads meet, delivering both ways each step.
static void SettleUntilEqual(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb) {
    for (int I = 0; I < 64 && A.ExecTick() != B.ExecTick(); ++I) {
        if (A.ExecTick() <= B.ExecTick()) A.Tick(OneTickNs);
        if (B.ExecTick() <= A.ExecTick()) B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
}

// A link that holds each datagram for `Lag` steps before releasing it — models a peer whose frames
// arrive `Lag` ticks late. This is the harness that separates rollback from lockstep: under lag,
// lockstep STALLS at the ceiling (it cannot run a tick until the peer's real input is in hand), while
// rollback speculates across the gap (predicting the peer idle) and rolls back only when a delivered
// frame contradicts the guess. Absorb() takes a peer's outbox; Release() delivers everything now due.
struct LaggyLink {
    explicit LaggyLink(int LagSteps) : Lag(LagSteps) {}
    int Lag;
    int Clock = 0;
    struct Item { int ReleaseAt; Lur::Net::EMsgType Type; std::vector<uint8_t> Data; };
    std::vector<Item> Q;
    void Absorb(Outbox& From) {
        for (auto& M : From.Q) Q.push_back({Clock + Lag, M.first, M.second});
        From.Q.clear();
    }
    void Release(LockstepPeer& To) {
        std::vector<Item> Keep;
        for (auto& It : Q) {
            if (It.ReleaseAt <= Clock) To.OnMessage(It.Type, It.Data.data(), It.Data.size());
            else Keep.push_back(std::move(It));
        }
        Q.swap(Keep);
    }
    void Tick() { ++Clock; }
    bool Empty() const { return Q.empty(); }
    // Deliver every still-held frame at once, ignoring the lag — used to drain the in-flight tail
    // before a no-lag settle so both peers reach a common CONFIRMED frontier to compare.
    void Flush(LockstepPeer& To) {
        for (auto& It : Q) To.OnMessage(It.Type, It.Data.data(), It.Data.size());
        Q.clear();
    }
};

// #139: drive both peers through the pre-match placement handshake — each places its mining camp
// (its "ready"), the camps are exchanged, and the match starts from tick 0 with both camps in.
// Tests that don't otherwise place a camp call this so the clock actually starts.
static void PlaceCampsAndStart(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb) {
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 4 && !(A.MatchStarted() && B.MatchStarted()); ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
}

// ---- Two peers, random inputs, stay bit-identical in lockstep with zero desyncs ----
static void TestLockstepStaysInSync() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1234, 0, Enqueue, &Qa);
    B.Init(0x1234, 1, Enqueue, &Qb);

    for (int I = 0; I < 300; ++I) {
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);  // A's inputs+anchors -> B
        Deliver(Qb, A);  // B's inputs+anchors -> A
        CHECK(!A.Desynced() && !B.Desynced());
    }
    // A few settle rounds (no new input) so both drain to the same frontier.
    for (int I = 0; I < 4; ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(A.ExecTick() > 250);                 // the match actually progressed
    CHECK(A.ExecTick() == B.ExecTick());       // both at the same tick
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());  // bit-identical state
}

// ---- #139: the match-start ready gate — neither clock advances until BOTH camps are placed ----
static void TestLockstepReadyGate() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x135, 0, Enqueue, &Qa);
    B.Init(0x135, 1, Enqueue, &Qb);

    // Only A readies (places its camp): the match must NOT start; both clocks hold at tick 0.
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));  // clear of the home base (#146)
    for (int I = 0; I < 10; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(!A.MatchStarted() && !B.MatchStarted());
    CHECK(A.ExecTick() == 0 && B.ExecTick() == 0);

    // B readies too -> both camps in -> the match starts and runs bit-identical.
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));  // clear of the home base (#146)
    for (int I = 0; I < 40; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    for (int I = 0; I < 4; ++I) {  // settle to a common frontier (B readied a half-step ahead)
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.MatchStarted() && B.MatchStarted());
    CHECK(A.ExecTick() > 0 && A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    // Both camps landed in the tick-0 state: one alive miner building per team.
    int Camps0 = 0, Camps1 = 0;
    const Sim& S = A.GetSim();
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J) && S.IsBuilding(J) && S.Type[J] == UnitMiner) ++(S.Team[J] == 0 ? Camps0 : Camps1);
    CHECK(Camps0 >= 1 && Camps1 >= 1);
}

// ================================================================================================
// Rollback Phase 1 scaffolding (the responsiveness experiment, Docs/Journal/2026-08-03).
// The snapshot ring, the peer predictor, and ConfirmedTick() are landed and proven BEFORE Phase 2
// touches LockstepPeer's execution model. These tests are the "exercised by NetTests + the
// two-window loopback" the plan asks for; none of them change or depend on execution behaviour.
// ================================================================================================

// ---- The snapshot ring is a memcpy round-trip: what goes in comes back bit-identical, and stored
// snapshots are INDEPENDENT copies (mutating the source Sim after Save doesn't touch the stored one).
static void TestSnapshotRingRoundTrip() {
    SnapshotRing Ring(RollbackHorizon + 1);
    CHECK(Ring.Capacity() == RollbackHorizon + 1);
    CHECK(Ring.Get(0) == nullptr);  // nothing stored yet -> null, not a zeroed Sim

    // A Sim advanced a few ticks so the snapshot carries real, non-default state.
    auto Src = std::make_unique<Sim>();
    Src->Init(0x5A0F);
    for (int I = 0; I < 5; ++I) Src->StepEvents(nullptr, 0);
    const uint32_t T = Src->Tick;
    const uint64_t H = Src->StateHash();
    Ring.Save(T, *Src);

    const Sim* Got = Ring.Get(T);
    CHECK(Got != nullptr);
    CHECK(Got->StateHash() == H);        // memcpy preserved the state exactly
    CHECK(Ring.Get(T + 1) == nullptr);   // a tick we never saved is absent

    // The stored snapshot is a copy: advancing the source must not change what the ring holds.
    for (int I = 0; I < 3; ++I) Src->StepEvents(nullptr, 0);
    CHECK(Src->StateHash() != H);        // the source really moved on
    CHECK(Ring.Get(T)->StateHash() == H);  // ...but the snapshot is still the frozen state

    Ring.Clear();
    CHECK(Ring.Get(T) == nullptr);       // Clear forgets everything, keeps the allocation
}

// ---- Eviction is implicit in the modular indexing: saving Capacity+k ticks keeps the LAST Capacity
// and drops the older ones, and Get's tick-tag check returns null for an evicted tick rather than the
// stale Sim now occupying its slot. ----
static void TestSnapshotRingEviction() {
    const uint32_t Cap = 4;
    SnapshotRing Ring(Cap);
    auto S = std::make_unique<Sim>();
    S->Init(0xE71C);
    // Save ticks 0..Cap+1 (Cap+2 saves). Each carries a DISTINCT state so a wrong slot is detectable.
    std::vector<uint64_t> HashAt;
    for (uint32_t T = 0; T <= Cap + 1; ++T) {
        S->StepEvents(nullptr, 0);       // advance so each saved state differs
        HashAt.push_back(S->StateHash());
        Ring.Save(T, *S);
    }
    // The two oldest (ticks 0,1) were evicted by the two newest saves reusing their slots.
    CHECK(Ring.Get(0) == nullptr);
    CHECK(Ring.Get(1) == nullptr);
    // The last Capacity ticks (2..Cap+1) are live and carry their own recorded state — proving the
    // tag check hands back the RIGHT tick, not just some non-null Sim.
    for (uint32_t T = 2; T <= Cap + 1; ++T) {
        const Sim* G = Ring.Get(T);
        CHECK(G != nullptr);
        CHECK(G != nullptr && G->StateHash() == HashAt[T]);
    }
}

// ---- Exercise the ring against REAL match states from the two-window loopback: snapshot every
// executed tick, then confirm the last-horizon window restores bit-identically and older ticks have
// been retired. This is the buffer Phase 2's roll-back-and-resim will restore from. ----
static void TestSnapshotRingCapturesLoopbackStates() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x210B, 0, Enqueue, &Qa);
    B.Init(0x210B, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    SnapshotRing Ring(RollbackHorizon + 1);
    std::vector<uint64_t> HashByTick;  // index = exec tick -> recorded StateHash
    for (int I = 0; I < 80; ++I) {
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
        const Sim& S = A.GetSim();
        const uint32_t T = S.Tick;
        if (T >= HashByTick.size()) HashByTick.resize(T + 1, 0);
        HashByTick[T] = S.StateHash();
        Ring.Save(T, S);
    }
    const uint32_t Head = A.GetSim().Tick;
    CHECK(Head > RollbackHorizon + 2);  // enough ran that eviction actually happened

    // The last-horizon window restores exactly; a heap copy stands in for Phase 2's "restore into the
    // live sim" so we prove the stored bytes are a usable Sim, not just intact in place.
    int Restored = 0;
    for (uint32_t T = Head; T + Ring.Capacity() > Head; --T) {  // T in (Head-Capacity, Head]
        const Sim* G = Ring.Get(T);
        CHECK(G != nullptr);
        if (G) {
            auto Copy = std::make_unique<Sim>(*G);
            CHECK(Copy->StateHash() == HashByTick[T]);
            ++Restored;
        }
        if (T == 0) break;
    }
    CHECK(Restored == static_cast<int>(Ring.Capacity()));  // a full horizon of restorable snapshots
    // ...and the tick just past the window has been evicted (proves the depth is bounded, not growing).
    if (Head >= Ring.Capacity()) CHECK(Ring.Get(Head - Ring.Capacity()) == nullptr);
}

// ---- The peer predictor's contract: "no events this tick" is the empty batch, and it CLEARS whatever
// it is handed (Phase 2 reuses one scratch vector across ticks, so the clear is load-bearing). ----
static void TestPredictPeerBatchIsEmpty() {
    std::vector<InputEvent> Batch;
    Batch.push_back(InputEvent::Place(0, UnitMiner, F(17), F(16)));  // stale content from a prior tick
    Batch.push_back(InputEvent::Queue(1, 3, 5));
    PredictPeerBatch(Batch);
    CHECK(Batch.empty());  // the prediction is genuinely no input
}

// ---- ConfirmedTick(): the frontier both peers agree on. It starts at the delay pre-seed, advances
// monotonically as real frames stream both ways, stays symmetric once the peers settle, and never
// runs ahead of executed state in the steady loopback. ----
static void TestConfirmedTickTracksBothTimelines() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0xC0F1, 0, Enqueue, &Qa);
    B.Init(0xC0F1, 1, Enqueue, &Qb);
    // Fresh match under rollback: both input timelines start EMPTY (no delay pre-seed), so nothing is
    // confirmed yet — the frontier is -1. (Tick 0's camps are seeded only once both peers ready.)
    CHECK(A.ConfirmedTick() == -1);
    CHECK(B.ConfirmedTick() == -1);

    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    int64_t PrevA = A.ConfirmedTick();
    for (int I = 0; I < 120; ++I) {
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
        const int64_t CurA = A.ConfirmedTick();
        CHECK(CurA >= PrevA);                                   // monotonic (no resync -> never rewinds)
        // Execution never outruns confirmed input — that IS lockstep's guarantee (a tick runs only
        // once both its inputs are known). The confirmed frontier LEADS exec here, because in the
        // W+Delay model the input timelines are produced/scheduled Delay ticks ahead of the execution
        // ceiling; so the real bound is confirmed >= exec-1, never confirmed <= exec.
        CHECK(CurA >= static_cast<int64_t>(A.ExecTick()) - 1);
        PrevA = CurA;
    }
    for (int I = 0; I < 4; ++I) {  // settle to a common frontier
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.ConfirmedTick() > 100);                    // it genuinely advanced with the match
    CHECK(A.ConfirmedTick() == B.ConfirmedTick());     // both peers agree on the confirmed frontier
}

// ================================================================================================
// Rollback Phase 2 BEHAVIOUR (the responsiveness experiment). These are the test-first spec for the
// execution-model replacement: local input applies at the head with NO scheduling delay, execution
// speculates the peer forward under lag instead of stalling, a delivered frame that contradicts the
// prediction rolls back and both peers re-converge bit-identically, and runaway speculation is capped
// at the rollback horizon. They FAIL against the old W+Delay lockstep and pass once Phase 2 lands.
// ================================================================================================

// ---- The headline: a local action executes at the tick it was issued, not InputDelayTicks later. ----
static void TestRollbackLocalInputHasNoSchedulingDelay() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x0011, 0, Enqueue, &Qa);
    B.Init(0x0011, 1, Enqueue, &Qb);
    A.SetRecording(true);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());
    for (int I = 0; I < 6; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }

    const uint32_t IssueHead = A.ExecTick();  // the tick the local action will apply AT under rollback
    A.QueueLocalEvent(InputEvent::Queue(0, 2, 1));  // queue one unit at team 0's own camp (slot 2)
    A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);

    int FoundTick = -1;
    const std::vector<std::vector<InputEvent>>& Rec = A.RecordedEvents();
    for (std::size_t T = 0; T < Rec.size(); ++T)
        for (const InputEvent& E : Rec[T])
            if (E.Kind == EventQueueUnits && E.Team == 0 && E.X == 2) FoundTick = static_cast<int>(T);
    CHECK(FoundTick >= 0);                              // it executed…
    CHECK(FoundTick == static_cast<int>(IssueHead));   // …AT the head it was issued on — zero delay
}

// ---- Under peer lag, the local head keeps pace with the wall clock (speculates) instead of stalling. ----
static void TestRollbackAdvancesUnderPeerLag() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1A66, 0, Enqueue, &Qa);
    B.Init(0x1A66, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    LaggyLink AtoB(3), BtoA(3);  // 3-tick one-way lag each direction; neither peer issues input
    const int Steps = 40;
    for (int I = 0; I < Steps; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        AtoB.Absorb(Qa); BtoA.Absorb(Qb);
        AtoB.Release(B); BtoA.Release(A);
        AtoB.Tick(); BtoA.Tick();
    }
    // Rollback keeps A's head near its wall clock despite B's frames lagging 3 ticks — lockstep would
    // sit ~3 behind. (Started with WallTicks 1 at tick 0, so the head is ~Steps.)
    CHECK(A.ExecTick() >= static_cast<uint32_t>(Steps) - 2);
    CHECK(static_cast<int64_t>(A.ExecTick()) - A.ConfirmedTick() >= 1);  // there is live speculation
    CHECK(A.Rollbacks() == 0);  // both idle -> the "peer idle" prediction was right every tick

    AtoB.Flush(B); BtoA.Flush(A);  // deliver the in-flight tail, then settle with no lag
    for (int I = 0; I < 8; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    CHECK(!A.Desynced() && !B.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());  // confirmed state converges
}

// ---- A delivered frame that contradicts the "peer idle" prediction rolls back and re-converges. ----
static void TestRollbackCorrectsMispredictionAndConverges() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x0155, 0, Enqueue, &Qa);
    B.Init(0x0155, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    LaggyLink AtoB(2), BtoA(2);
    const int Steps = 120;
    for (int I = 0; I < Steps; ++I) {
        // B issues real production at irregular ticks — A speculated those ticks as "B idle", so each
        // one that lands late is a misprediction A must roll back and re-simulate.
        if (I % 9 == 4) B.QueueLocalEvent(InputEvent::Queue(1, 3, 2));
        if (I % 13 == 2) A.QueueLocalEvent(InputEvent::Queue(0, 2, 2));
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        AtoB.Absorb(Qa); BtoA.Absorb(Qb);
        AtoB.Release(B); BtoA.Release(A);
        AtoB.Tick(); BtoA.Tick();
    }
    CHECK(A.Rollbacks() > 0);          // B's late real input really did force corrections on A
    CHECK(A.ResimTicks() > 0);

    AtoB.Flush(B); BtoA.Flush(A);
    for (int I = 0; I < 12; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    CHECK(!A.Desynced() && !B.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());  // deterministic convergence after rollbacks
}

// ---- Speculation is capped at the rollback horizon: a silent peer can't make the head run away. ----
static void TestRollbackSpeculationCappedAtHorizon() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x4021, 0, Enqueue, &Qa);
    B.Init(0x4021, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    for (int I = 0; I < 5; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }

    // B goes silent (nothing delivered either way). A keeps ticking well past the horizon, but must
    // NOT speculate more than RollbackHorizon ticks beyond the confirmed frontier — and must NOT yet
    // draw (the #162 ceiling bound is seconds away, far beyond this many ticks).
    for (int I = 0; I < static_cast<int>(RollbackHorizon) + 25; ++I) {
        A.Tick(OneTickNs);
        Qa.Q.clear();  // A's frames go nowhere; B sends nothing back
    }
    CHECK(A.GetSim().Result == ResultOngoing);
    CHECK(static_cast<int64_t>(A.ExecTick()) - A.ConfirmedTick() <= static_cast<int64_t>(RollbackHorizon) + 1);
}

// ---- Send-on-tap: a waiting input is produced + sent WITHOUT waiting for the wall-tick boundary ----
// The sim thread calls Tick() every ~2 ms but a wall tick elapses only every 100 ms. This drives Tick()
// with SUB-TICK elapsed times (no wall tick), and asserts a queued input still goes out immediately —
// the latency win — while never running more than InputLeadTicks ahead of the wall clock, and staying
// deterministic.
static void TestSendOnTapProducesBeforeWallTick() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x5EAD, 0, Enqueue, &Qa);
    B.Init(0x5EAD, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    for (int I = 0; I < 3; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    const uint32_t HeadBefore = A.ExecTick();
    Qa.Q.clear();  // ignore prior traffic; measure only what the tap produces

    // Tap, then tick a SUB-tick slice (10 ms << 100 ms): no wall tick elapses, but send-on-tap must
    // still produce + send the frame this call.
    A.QueueLocalEvent(InputEvent::Queue(0, 2, 1));
    A.Tick(OneTickNs / 10);
    int Inputs = 0; for (auto& M : Qa.Q) if (M.first == MsgInput) ++Inputs;
    CHECK(Inputs == 1);                        // it hit the wire immediately, not at the 100 ms boundary
    CHECK(A.ExecTick() == HeadBefore + 1);     // and executed locally now (the head advanced one tick)

    // No runaway: further sub-tick calls with nothing pending must NOT produce ahead again — the lead
    // is capped at InputLeadTicks (=1 here), paid back at the next real boundary.
    A.Tick(OneTickNs / 10); A.Tick(OneTickNs / 10);
    CHECK(A.ExecTick() == HeadBefore + 1);

    // Deliver + settle: the peer got it, the sims agree, and the event actually took.
    Deliver(Qa, B);
    for (int I = 0; I < 6; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    SettleUntilEqual(A, B, Qa, Qb);
    CHECK(!A.Desynced() && !B.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
}

#if LUR_INTERNAL
// ---- #112: a gameplay-CVar override on ONE peer syncs to the other, applies at the same
// stamped tick, and keeps both bit-identical — AND actually changes the match. ----
static void TestLockstepCvarSyncStaysIdentical() {
    auto RunMatch = [](bool Tweak) {
        Outbox Qa, Qb;
        LockstepPeer A, B;
        A.Init(0xC0DE, 0, Enqueue, &Qa);
        B.Init(0xC0DE, 1, Enqueue, &Qb);
        for (int I = 0; I < 200; ++I) {
            // At tick ~20, A doubles the miner speed. Miners exist from the start + the placed
            // camp, so the tweak measurably shifts mining/deposit timing -> state diverges. The
            // input schedule is identical to the untweaked run — only the CVar differs.
            if (Tweak && I == 20)
                A.SetGameplayCvar(CvIdMinerSpeed, F(8, 10).Raw, /*wallMs*/ 1000);
            DriveInput(A, 0, I);
            DriveInput(B, 1, I);
            A.Tick(OneTickNs);
            B.Tick(OneTickNs);
            Deliver(Qa, B);
            Deliver(Qb, A);
            CHECK(!A.Desynced() && !B.Desynced());  // the synced override never desyncs
        }
        for (int I = 0; I < 6; ++I) {  // settle
            A.Tick(OneTickNs);
            B.Tick(OneTickNs);
            Deliver(Qa, B);
            Deliver(Qb, A);
        }
        CHECK(A.ExecTick() == B.ExecTick());
        CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());  // both applied it at the same tick
        return A.GetSim().StateHash();
    };

    const uint64_t Tweaked = RunMatch(true);
    const uint64_t Baseline = RunMatch(false);
    CHECK(Tweaked != Baseline);  // the synced knob genuinely altered the simulation
}

// ---- #112: match-start MsgCvarSync merges both peers' pre-match override sets with the
// last-writer-wall-clock resolver (timestamp collision -> compile-time default). ----
static void TestCvarSyncMatchStartMerge() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x5EED, 0, Enqueue, &Qa);
    B.Init(0x5EED, 1, Enqueue, &Qb);

    // A: WSeek=3 @t=200 (newer). B: WSeek=1 @t=100 (older, loses) + WCohSame=0.5 @t=50
    // (only B has it, survives). Plus a COLLISION on WAlign: same t=70, different values
    // -> must revert to the compile-time default on both peers.
    A.SeedGameplayCvar(CvIdWSeek,    Fixed::FromInt(3).Raw,  200);
    A.SeedGameplayCvar(CvIdWAlign,   Fixed::FromInt(2).Raw,  70);
    B.SeedGameplayCvar(CvIdWSeek,    Fixed::FromInt(1).Raw,  100);
    B.SeedGameplayCvar(CvIdWCohSame, Fixed{Fixed::One / 2}.Raw, 50);
    B.SeedGameplayCvar(CvIdWAlign,   Fixed::FromInt(5).Raw,  70);   // collides with A's WAlign@70

    A.SendCvarSync();
    B.SendCvarSync();
    Deliver(Qa, B);   // A's set -> B merges
    Deliver(Qb, A);   // B's set -> A merges
    // Both converged to the identical resolved set BEFORE tick 0.
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    CHECK(A.GetSim().Cv.WSeek == Fixed::FromInt(3));            // newer edit won
    CHECK(A.GetSim().Cv.WCohSame == Fixed{Fixed::One / 2});     // only-one-side override survives
    CHECK(A.GetSim().Cv.WAlign == CvWAlign.Get());             // collision -> compile-time default
    CHECK(B.GetSim().Cv.WSeek == A.GetSim().Cv.WSeek);
    CHECK(B.GetSim().Cv.WAlign == A.GetSim().Cv.WAlign);

    // And they stay bit-identical once the match runs on the merged set.
    for (int I = 0; I < 120; ++I) {
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
}

// ---- #147: the peers boot with DIFFERENT persisted cvars (the on-device case: the Android had
// a stale rps-cvars.cfg, the iPhone none). The merged set must land on both peers AND re-derive
// the Cv-DEPENDENT initial state Sim::Init bakes (frontier high-water, opening gold, home-base Y)
// — assigning Cv alone left those at each peer's pre-sync value and desynced at the first anchor
// with no units on the field. Also pins the baseline: overlaying the merged set must start from
// the COMPILE-TIME defaults, not from whatever this peer's globals happen to hold. ----
static void TestCvarSyncRederivesInitState() {
    const int32_t GoldDefault = CvStartingGold.Default();
    const Fixed   FrontDefault = CvInitialFrontier.Default();
    const Fixed   AlignDefault = CvWAlign.Default();
    CHECK(GoldDefault != 400 && FrontDefault != F(60));  // the test values must actually differ

    Outbox Qa, Qb;
    LockstepPeer A, B;
    // Peer A boots with the persisted overrides live in the globals — exactly what
    // LoadCVarConfig leaves behind — so its Sim::Init bakes Gold/Frontier/HQ-Y from THEM.
    CvStartingGold.Set(400);
    CvInitialFrontier.Set(F(60));
    CvWAlign.Set(F(2));
    A.Init(0xF00D, 0, Enqueue, &Qa);
    A.SeedGameplayCvar(CvIdStartingGold,    400,        500);
    A.SeedGameplayCvar(CvIdInitialFrontier, F(60).Raw,  500);
    A.SeedGameplayCvar(CvIdWAlign,          F(2).Raw,   70);
    // Peer B boots clean (no cvars.cfg) — the globals are back at the compile-time defaults.
    CvStartingGold.Set(GoldDefault);
    CvInitialFrontier.Set(FrontDefault);
    CvWAlign.Set(F(5));                     // B has its OWN WAlign, same wall-clock -> tie
    B.Init(0xF00D, 1, Enqueue, &Qb);
    B.SeedGameplayCvar(CvIdWAlign, F(5).Raw, 70);
    CvWAlign.Set(AlignDefault);             // and B's global is reverted before the sync lands

    // Before the sync, the two peers genuinely disagree — that IS the bug being fixed.
    CHECK(A.GetSim().StateHash() != B.GetSim().StateHash());

    A.SendCvarSync();
    B.SendCvarSync();
    Deliver(Qa, B);
    Deliver(Qb, A);

    // Both converged on the merged Cv...
    CHECK(A.GetSim().Cv.StartingGold == 400    && B.GetSim().Cv.StartingGold == 400);
    CHECK(A.GetSim().Cv.InitialFrontier == F(60) && B.GetSim().Cv.InitialFrontier == F(60));
    // ...including the wall-clock TIE, which resolves to the compile-time default on BOTH — the
    // baseline check: overlaying onto local globals would have left A on 2 and B on 5.
    CHECK(A.GetSim().Cv.WAlign == AlignDefault && B.GetSim().Cv.WAlign == AlignDefault);
    // ...and the Init-DERIVED state was rebuilt from it on both peers (the #147 fix).
    CHECK(A.GetSim().Teams[0].Gold == 400 && B.GetSim().Teams[0].Gold == 400);
    CHECK(A.GetSim().FrontierT0 == F(60) && B.GetSim().FrontierT0 == F(60));
    CHECK(A.GetSim().FrontierT1 == WorldHeight - F(60) && B.GetSim().FrontierT1 == A.GetSim().FrontierT1);
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());  // covers the home base's Y too

    // And a real match on the merged set stays bit-identical through several anchors (tick 10 is
    // where the on-device desync fired).
    for (int I = 0; I < 60; ++I) {
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.ExecTick() > 10);  // the anchor that fired on-device was actually crossed
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());

    CvStartingGold.Set(GoldDefault);        // leave the globals as we found them
    CvInitialFrontier.Set(FrontDefault);
    CvWAlign.Set(AlignDefault);
}

// ---- #147: the peer's MsgCvarSync ARRIVES BEFORE our own Lp.Init. Not hypothetical — it is
// what happens on iOS every time (one renderFrame pumps the session inbox, delivering the sync,
// and only afterwards reaches the "ready -> Lp.Init" branch), and it silently defeated the fix
// on hardware: the merged set sat in ActiveCvars while Init re-latched Cv from the local globals.
// Caught by a two-phone run showing Android gold=400 / iPhone gold=190. ----
static void TestCvarSyncArrivingBeforeInit() {
    const int32_t GoldDefault = CvStartingGold.Default();
    const Fixed   FrontDefault = CvInitialFrontier.Default();

    Outbox Qa, Qb;
    LockstepPeer A, B;
    // A boots with the persisted overrides live in its globals and sends its set.
    CvStartingGold.Set(400);
    CvInitialFrontier.Set(F(60));
    A.Init(0xAB1E, 0, Enqueue, &Qa);
    A.SeedGameplayCvar(CvIdStartingGold,    400,       900);
    A.SeedGameplayCvar(CvIdInitialFrontier, F(60).Raw, 900);
    A.SendCvarSync();
    // B is clean, and receives A's sync while its OWN Lp is still uninitialised...
    CvStartingGold.Set(GoldDefault);
    CvInitialFrontier.Set(FrontDefault);
    Deliver(Qa, B);
    // ...and only THEN enters the match. Init must not discard what the sync already merged.
    B.Init(0xAB1E, 1, Enqueue, &Qb);
    B.SendCvarSync();
    Deliver(Qb, A);

    CHECK(B.GetSim().Cv.StartingGold == 400);          // the value the peer sent, not our default
    CHECK(B.GetSim().Teams[0].Gold == 400);            // ...and the Init-DERIVED state with it
    CHECK(B.GetSim().Cv.InitialFrontier == F(60));
    CHECK(B.GetSim().FrontierT0 == F(60));
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());

    for (int I = 0; I < 40; ++I) {                     // and it holds through several anchors
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());

    CvStartingGold.Set(GoldDefault);
    CvInitialFrontier.Set(FrontDefault);
}

// ---- #147: a RESYNC rebuild must keep the merged cvar set. RebuildFromHistory re-created the sim
// with Sim::Init, re-latching the LOCAL globals, so a rejoining peer replayed the whole match on a
// DIFFERENT Cv than the survivor and desynced immediately. The easiest of the three fresh-sim paths
// to miss, because it only fires after a link blip. Modelled on TestLockstepColdRejoinResync: the
// rejoiner must be genuinely BEHIND, or the marker branch keeps its own history and never rebuilds.
static void TestCvarSyncSurvivesResync() {
    const int32_t GoldDefault = CvStartingGold.Default();
    const Fixed   FrontDefault = CvInitialFrontier.Default();

    Outbox Qa, Qb;
    LockstepPeer A, B;
    CvStartingGold.Set(400);                       // A boots with the persisted overrides live
    CvInitialFrontier.Set(F(60));
    A.Init(0x5E51, 0, Enqueue, &Qa);
    CvStartingGold.Set(GoldDefault);               // B boots clean (no cvars.cfg)
    CvInitialFrontier.Set(FrontDefault);
    B.Init(0x5E51, 1, Enqueue, &Qb);
    A.SeedGameplayCvar(CvIdStartingGold,    400,       700);
    A.SeedGameplayCvar(CvIdInitialFrontier, F(60).Raw, 700);
    A.SendCvarSync();
    B.SendCvarSync();
    Deliver(Qa, B);
    Deliver(Qb, A);
    CHECK(B.GetSim().Teams[0].Gold == 400);        // converged before the blip

    for (int I = 0; I < 40; ++I) {                 // play a while
        DriveInput(A, 0, I); DriveInput(B, 1, I);
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
    }
    const uint32_t Frontier = A.ExecTick();
    const uint64_t StateAtF = A.GetSim().StateHash();
    CHECK(Frontier > 20);

    // B relaunches FRESH and rejoins: it must rebuild the whole match off A's history. Its own
    // globals are the defaults, so a rebuild that latches them replays on gold=190/frontier=45.
    Outbox Qb2;
    LockstepPeer B2;
    B2.Init(0x5E51, 1, Enqueue, &Qb2);
    B2.SeedGameplayCvar(CvIdStartingGold,    400,       700);   // the sync it re-does on reconnect
    B2.SeedGameplayCvar(CvIdInitialFrontier, F(60).Raw, 700);
    A.BeginResync();
    B2.BeginResync();
    Deliver(Qa, B2);   // A's history + frontier marker -> B2 rebuilds from it
    Deliver(Qb2, A);   // B2's empty history -> A keeps its own (it is ahead)

    CHECK(!A.AwaitingResync() && !B2.AwaitingResync());
    CHECK(B2.ExecTick() == Frontier);
    CHECK(B2.GetSim().Cv.StartingGold == 400);      // the rebuild kept the MERGED set...
    CHECK(B2.GetSim().Cv.InitialFrontier == F(60));
    CHECK(B2.GetSim().StateHash() == StateAtF);      // ...so the replay lands bit-identical

    for (int I = 0; I < 40; ++I) {                  // and live lockstep resumes cleanly
        DriveInput(A, 0, I); DriveInput(B2, 1, I);
        A.Tick(OneTickNs); B2.Tick(OneTickNs);
        Deliver(Qa, B2); Deliver(Qb2, A);
        CHECK(!A.Desynced() && !B2.Desynced());
    }
    CHECK(A.GetSim().StateHash() == B2.GetSim().StateHash());

    CvStartingGold.Set(GoldDefault);
    CvInitialFrontier.Set(FrontDefault);
}

// ---- #169: the peer that joins LATE must still receive the incumbent's tunables ----
// The hardware failure, exactly: SendCvarSync was called once per peer, by the main, next to
// Lp.Init. When one phone's app restarts and the other keeps running, the rejoiner Inits and offers
// its (empty) set, and the incumbent NEVER re-offers — its Init already happened and the
// `!Started && PeerReady` gate that calls SendCvarSync cannot fire again. So the rejoiner simulates
// on compile-time defaults while the incumbent simulates on its persisted overrides, and every match
// desyncs at the first anchor until someone restarts the other phone too.
//
// Measured 2026-08-01 by diffing both phones' .rec files: whichever phone launched LAST ran the whole
// session un-merged (miner 600 / gold 800 against the Galaxy's 400 / 750) across four consecutive
// matches. It hid because it is ASYMMETRIC and silent — the incumbent is never wrong (it merged its
// own set when it seeded it) and the rejoiner's empty set merges into the incumbent as a no-op, so
// both peers report a clean exchange and only the pre-tick-0 state differs.
//
// Note what this test does NOT do: it never seeds the rejoiner with the overrides. The pre-existing
// #147 rejoin test does (`the sync it re-does on reconnect`), and that assumption is what let the bug
// through — the rejoiner has nothing, and the incumbent's re-offer is the only thing that can save it.
static void TestLateJoinerReceivesIncumbentCvars() {
    const int32_t GoldDefault = CvStartingGold.Default();
    Outbox Qa, Qb;
    LockstepPeer A, B;

    // A is the incumbent, launched first, carrying a persisted override. Its Init-time offer goes
    // out while nobody is listening — the other phone's app is not up yet.
    A.Init(0x169A, 0, Enqueue, &Qa);
    A.SeedGameplayCvar(CvIdStartingGold, 400, 700);
    A.SendCvarSync();
    Qa.Q.clear();                       // ...into the void: the peer was not there to hear it
    CHECK(A.GetSim().Teams[0].Gold == 400);

    // B launches now, with NO persisted tunables, and both mains call BeginResync on entering the
    // match (#148). That is the only event either side has to reconcile on.
    B.Init(0x169A, 1, Enqueue, &Qb);
    CHECK(B.GetSim().Teams[0].Gold == GoldDefault);   // B starts wrong, by construction
    B.SendCvarSync();                                 // B's own Init-time offer (empty)
    A.BeginResync();
    B.BeginResync();
    Deliver(Qb, A);
    Deliver(Qa, B);

    // The fix: A re-offered, so B is on A's tunables BEFORE tick 0 — not merely equal, but equal to
    // the value the incumbent was already simulating on.
    CHECK(B.GetSim().Teams[0].Gold == 400);
    CHECK(A.GetSim().Cv.StartingGold == B.GetSim().Cv.StartingGold);
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());

    // ...and the match then runs desync-free, which is the property the hardware pair could not hold
    // for ten ticks.
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());
    for (int I = 0; I < 120; ++I) {
        DriveInput(A, 0, I); DriveInput(B, 1, I);
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    CvStartingGold.Set(GoldDefault);
}

// ---- #169: a cvar sync that arrives MID-MATCH must be ignored, not applied ----
// The whole-set exchange carries no apply tick (unlike MsgCvar, which is stamped a few ticks ahead
// precisely so both peers move together). Merging one mid-match moves TheSim.Cv the instant the
// datagram lands — on whatever tick each peer happens to be on — so honouring a late sync would
// CAUSE a desync rather than prevent one. Pre-tick-0 was always the only safe window; re-offering on
// every resync (above) made a late arrival reachable, so the rule has to be enforced rather than
// assumed. The cost of dropping it is nil: every offer precedes its sender's camp, and no match can
// start until both camps are in.
static void TestMidMatchCvarSyncIsIgnored() {
    const int32_t GoldDefault = CvStartingGold.Default();
    Outbox Qa, Qb, Qlate;
    LockstepPeer A, B, Late;
    A.Init(0x169B, 0, Enqueue, &Qa);
    B.Init(0x169B, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());
    for (int I = 0; I < 30; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
    }
    const uint64_t Before = A.GetSim().StateHash();
    const int32_t  GoldBefore = A.GetSim().Cv.StartingGold;

    // A third peer's full-set offer lands on A alone, twenty ticks into the match.
    Late.Init(0x169B, 1, Enqueue, &Qlate);
    Late.SeedGameplayCvar(CvIdStartingGold, 12345, 900);
    Late.SendCvarSync();
    Deliver(Qlate, A);

    CHECK(A.GetSim().Cv.StartingGold == GoldBefore);   // Cv did not move under the running match...
    CHECK(A.GetSim().StateHash() == Before);
    for (int I = 0; I < 60; ++I) {                     // ...so the pair stays in lockstep
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    CvStartingGold.Set(GoldDefault);
}

// ---- #148: restarting one app must not wedge the peer. The survivor takes Session's reconnect
// edge and offers its history; the RESTARTED app never takes that edge (it connects before the
// Hello handshake finishes), so it sent no frontier marker — and LockstepPeer::Tick early-returns
// while Awaiting with nothing to clear it, so the survivor froze permanently. Reported from a live
// two-phone session: "still paused on the phone that kept running the app." ----
static void TestResyncStallCannotWedgeSurvivor() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x5A1D, 0, Enqueue, &Qa);
    B.Init(0x5A1D, 1, Enqueue, &Qb);
    for (int I = 0; I < 40; ++I) {   // play a while
        DriveInput(A, 0, I); DriveInput(B, 1, I);
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
    }
    const uint32_t Before = A.ExecTick();
    CHECK(Before > 20);

    // A is the survivor: it takes the reconnect edge and offers its history. The peer NEVER
    // answers (the restarted app can't), so A must not hold forever.
    A.BeginResync();
    Qa.Q.clear();                       // nothing delivered to B; nobody will send a marker back
    CHECK(A.AwaitingResync());
    for (int I = 0; I < 5; ++I) { A.Tick(OneTickNs); }
    CHECK(A.AwaitingResync());          // still holding — the bound is seconds, not ticks
    CHECK(A.ExecTick() == Before);       // genuinely frozen while it waits

    // Past the stall bound it resumes on its own state instead of wedging.
    A.Tick(LockstepPeer::ResyncStallTimeoutNs);
    CHECK(!A.AwaitingResync());
    for (int I = 0; I < 20; ++I) { DriveInput(A, 0, I); A.Tick(OneTickNs); }
    CHECK(A.ExecTick() > Before);        // the match is running again
}

// ---- #148: the peer that is AHEAD re-offers its history when it sees a marker behind its own
// frontier. Without that, a rejoiner whose Lp.Init wiped the first offer (arrival-before-Init, the
// same hazard as #147) would never be handed the history again and would sit at tick 0. ----
static void TestResyncReoffersToBehindPeer() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x5A1E, 0, Enqueue, &Qa);
    B.Init(0x5A1E, 1, Enqueue, &Qb);
    for (int I = 0; I < 40; ++I) {
        DriveInput(A, 0, I); DriveInput(B, 1, I);
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
    }
    const uint32_t Frontier = A.ExecTick();
    const uint64_t StateAtF = A.GetSim().StateHash();

    // The survivor offers first, and that offer is LOST (this is the Init-wipe on the newcomer).
    A.BeginResync();
    Qa.Q.clear();

    // The newcomer relaunches, enters the match, and reconciles (the new post-Init BeginResync).
    Outbox Qb2;
    LockstepPeer B2;
    B2.Init(0x5A1E, 1, Enqueue, &Qb2);
    B2.BeginResync();                   // sends an empty history + marker F=0
    Deliver(Qb2, A);                    // A sees a marker BEHIND it -> must re-offer
    CHECK(!A.AwaitingResync());          // A reconciled and resumed immediately
    Deliver(Qa, B2);                    // the re-offer reaches B2
    CHECK(!B2.AwaitingResync());
    CHECK(B2.ExecTick() == Frontier);            // caught up off the re-offer
    CHECK(B2.GetSim().StateHash() == StateAtF);  // bit-identical to the survivor

    for (int I = 0; I < 30; ++I) {       // and live lockstep resumes
        DriveInput(A, 0, I); DriveInput(B2, 1, I);
        A.Tick(OneTickNs); B2.Tick(OneTickNs);
        Deliver(Qa, B2); Deliver(Qb2, A);
        CHECK(!A.Desynced() && !B2.Desynced());
    }
    CHECK(A.GetSim().StateHash() == B2.GetSim().StateHash());
}

// ---- #112: build-fingerprint gate — identical builds pass, a mismatch is refused ----
static void TestBuildFingerprintGate() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1, 0, Enqueue, &Qa);
    B.Init(0x1, 1, Enqueue, &Qb);
    A.SendFingerprint();
    B.SendFingerprint();
    Deliver(Qa, B);
    Deliver(Qb, A);
    CHECK(!A.BuildMismatch() && !B.BuildMismatch());  // same process = same LUR_BUILD_FP -> ok

    // A peer reporting a different fingerprint is refused (mid-match draw avoided).
    const char Fake[] = "deadbeefcafe-dirty+Shipping";
    A.OnMessage(MsgFingerprint, reinterpret_cast<const uint8_t*>(Fake), sizeof(Fake) - 1);
    CHECK(A.BuildMismatch());
}

// #112: the gate must ACT, not just report. It used to set a flag whose only readers were two diag
// log lines, so the "refuse the match" its own handler comment promised did not exist — a mismatched
// pair played on and surfaced as an unexplained mid-match desync instead.
static void TestBuildFingerprintRefusesMatchStart() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x7, 0, Enqueue, &Qa);
    B.Init(0x7, 1, Enqueue, &Qb);

    const char Fake[] = "0000000000ff-clean+Development";
    A.OnMessage(MsgFingerprint, reinterpret_cast<const uint8_t*>(Fake), sizeof(Fake) - 1);
    CHECK(A.BuildMismatch());

    // Both camps placed: everything EXCEPT the build agrees, so this would have started before.
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 8; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(!A.MatchStarted());   // refused, before tick 0
    CHECK(A.ExecTick() == 0);   // and it really never ticked

    // B heard no bad fingerprint, so B is free to start — the refusal is per-peer evidence, not a
    // global halt. (On hardware the other phone refuses too, once its own fingerprint arrives.)
    CHECK(!B.BuildMismatch());
}

// #166: a fingerprint that lands BEFORE this side's Lp.Init must survive it. Init used to clear the
// verdict blind, and MsgFingerprint routinely arrives first (the normal iOS order: one renderFrame
// pumps the session inbox, and only afterwards reaches the session-ready -> Init branch). So which
// peer noticed a mismatch depended on init order, and the same mismatched pair read badbuild=1 on
// one phone and 0 on the other — logged as unexplained on 2026-07-30, reproduced 2026-08-01.
static void TestFingerprintSurvivesLaterInit() {
    Outbox Qa;
    LockstepPeer A;
    const char Fake[] = "feedface1234-clean+Development";

    // Arrives BEFORE Init — the exact ordering that used to lose it.
    A.OnMessage(MsgFingerprint, reinterpret_cast<const uint8_t*>(Fake), sizeof(Fake) - 1);
    CHECK(A.BuildMismatch());
    A.Init(0x9, 0, Enqueue, &Qa);
    CHECK(A.BuildMismatch());   // re-derived from the remembered string, not cleared

    // And the clearing behaviour it replaced still works: a peer rebuilt to match retires the
    // verdict on EVIDENCE (its next fingerprint), which is what d591539 was protecting.
    const char* Real = Lur::BuildFingerprint();
    A.OnMessage(MsgFingerprint, reinterpret_cast<const uint8_t*>(Real),
                static_cast<uint32_t>(std::strlen(Real)));
    CHECK(!A.BuildMismatch());
    A.Init(0xA, 0, Enqueue, &Qa);
    CHECK(!A.BuildMismatch());  // a matching peer stays clean across Init too
}

// #112 acceptance: the determinism soak. Editing a gameplay CVar mid-match must land on the SAME
// tick on both peers and leave the sims bit-identical — that is the whole point of the per-tick
// latch plus MsgCvar, and the StateHash fold is what makes a mis-latch visible instead of silent.
// A scripted schedule rather than one edit: the failure mode that matters is an edit applied a tick
// apart, which a single well-timed set can hide.
static void TestCvarEditSoakStaysHashIdentical() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0xD0FA, 0, Enqueue, &Qa);
    B.Init(0xD0FA, 1, Enqueue, &Qb);
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 8 && !(A.MatchStarted() && B.MatchStarted()); ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs);
        Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.MatchStarted() && B.MatchStarted());

    // Edits from BOTH sides, interleaved, on knobs that actually move the sim.
    struct Edit { int AtTick; bool FromA; uint8_t Id; int32_t Raw; };
    const Edit Schedule[] = {
        { 12, true,  CvIdMinerSpeed,   F(3).Raw   },
        { 20, false, CvIdRockCost,     55         },
        { 28, true,  CvIdRockHp,       140        },
        { 36, false, CvIdMinerSpeed,   F(2).Raw   },  // same knob, other peer, later stamp
        { 44, true,  CvIdCounterMultiplier, 3     },
        { 52, false, CvIdPaperHp,      90         },
    };
    std::size_t Next = 0;
    uint64_t WallMs = 1000;

    for (int I = 0; I < 200; ++I) {
        while (Next < sizeof(Schedule) / sizeof(Schedule[0]) &&
               Schedule[Next].AtTick == I) {
            const Edit& E = Schedule[Next];
            (E.FromA ? A : B).SetGameplayCvar(E.Id, E.Raw, WallMs);
            WallMs += 100;   // strictly increasing, so last-writer-wins has a defined answer
            ++Next;
        }
        DriveInput(A, 0, I); DriveInput(B, 1, I);
        A.Tick(OneTickNs);  B.Tick(OneTickNs);
        Deliver(Qa, B);     Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(Next == sizeof(Schedule) / sizeof(Schedule[0]));  // the whole schedule really ran
    CHECK(A.ExecTick() > 100);
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    // The edits actually took (otherwise the hashes would match trivially, proving nothing).
    CHECK(A.GetSim().Cv.MinerSpeed == F(2));
    CHECK(A.GetSim().Cv.RockCost == 55);
    CHECK(B.GetSim().Cv.MinerSpeed == F(2));
    CHECK(B.GetSim().Cv.RockCost == 55);
}

// ---- #170/#169: a resync must RE-OFFER both pre-match agreements, not just the input history ----
// Both the build fingerprint and the cvar set were sent exactly once, by the main, next to Lp.Init.
// So the peer that Inits LAST receives neither: the incumbent's Init is long past and nothing makes
// it send again. For the tunables that meant simulating a different game (#169); for the fingerprint
// it means `badbuild=0` — a CLEAN reading — on a pair genuinely built from two different commits.
// Caught on hardware 2026-08-01 immediately after the cvar fix, by watching badbuild flip 1 -> 0 when
// only the Galaxy restarted. A gate that reports 0 when it cannot know is worse than no gate, because
// badbuild=0 is exactly what you check before trusting a two-phone result.
//
// Asserted as EMISSION rather than end-to-end, deliberately: two peers in one test process share a
// LUR_BUILD_FP, so a delivered fingerprint and a missing one are indistinguishable by BuildMismatch().
// What the fix changes is that the datagrams go out at all.
static void TestResyncReoffersFingerprintAndTunables() {
    Outbox Qa;
    LockstepPeer A;
    A.Init(0x170F, 0, Enqueue, &Qa);
    Qa.Q.clear();                       // drop Init-time traffic; we want what BeginResync alone sends
    A.BeginResync();
    int Fp = 0, Cv = 0, Resync = 0;
    for (const auto& M : Qa.Q) {
        if (M.first == MsgFingerprint) ++Fp;
        else if (M.first == MsgCvarSync) ++Cv;
        else if (M.first == MsgResyncChunk) ++Resync;
    }
    CHECK(Fp == 1);        // the build gate is re-offered...
    CHECK(Cv == 1);        // ...and so is the tunable set (#169)
    CHECK(Resync > 0);     // ...without displacing what BeginResync already did (#148)
}
#endif

#if LUR_INTERNAL
// ---- #159: the "no recording open" sentinel must never be a real match index ----
// A main opens its linked recording on the match-started EDGE, keyed on MatchIndex(). iOS held that
// key as a zero-initialised Obj-C ivar, so "unset" and "match 0" were the same value: the edge never
// fired for the first match and the iPhone recorded NOTHING until a post-match restart bumped the
// index. The pair captured one side of the first linked match — and comparing two peers is the whole
// point of #159, so the first match is the one you least want half of. Found on hardware 2026-07-31
// by pulling the Galaxy's .rec and finding no iPhone counterpart.
//
// The sentinel is shared now, and this pins the property that makes it safe: no match a session can
// actually reach may equal it. Cheap, and it fails on the host rather than on two phones.
static void TestNoRecMatchIdxIsNotAReachableMatchIndex();  // defined below, next to the restart helpers

// ---- #159: two peers recording ONE linked match must produce comparable files ----
// The point of recording both sides is to diff them, so the property that matters is not "a file
// exists" but "the two files agree tick for tick". If they can drift for benign reasons the diff is
// useless as evidence, which is why this asserts equality of both the event stream and the hashes
// rather than just checking the recorder wrote something.
static void TestLinkedRecordingsMatchAcrossPeers() {
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "lur-rps-recdiff";
    std::error_code Ec;
    std::filesystem::create_directories(Dir, Ec);
    const std::string Pa = (Dir / "peer-a.rec").string(), Pb = (Dir / "peer-b.rec").string();

    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x9159, 0, Enqueue, &Qa);
    B.Init(0x9159, 1, Enqueue, &Qb);
    MatchRecorder Ra, Rb;
    // The sink is the shipping wiring the phone mains use — same lambda shape, same 10-tick hash
    // cadence — so this covers the path a real capture takes and not a test-only shortcut.
    struct Ctx { MatchRecorder* R; };
    Ctx Ca{&Ra}, Cb{&Rb};
    auto Sink = [](void* C, uint32_t Tick, const InputEvent* Batch, int Count, uint64_t Hash) {
        MatchRecorder* R = static_cast<Ctx*>(C)->R;
        R->Events(Tick, Batch, Count);
        if (Tick % 10 == 0) R->Hash(Tick, Hash);
    };
    A.SetTickSink(Sink, &Ca);
    B.SetTickSink(Sink, &Cb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    // This driver DELIBERATELY re-places the opening camp at its exact coordinates (I % 19 == 5) as
    // well as at distinct spots. That collision is what #160 was: the receiver classified a produced
    // batch as the pre-match camp re-send by reading its payload, dropped it, and skewed the peer's
    // stream for the rest of the match. The camp exchange has its own message type now, so a produced
    // tick can no longer be mistaken for one — and this comparison is what proves it, since it was
    // this comparison that found the bug.
    auto Drive = [](LockstepPeer& P, uint8_t Team, int I) {
        if (I % 17 == 3) P.QueueLocalEvent(InputEvent::Queue(Team, Team == 0 ? 0 : 1, 5));
        if (I % 23 == 7)
            P.QueueLocalEvent(
                InputEvent::Place(Team, UnitMiner, F(19 + (I % 5) * 3), CampTestY(Team)));
        if (I % 19 == 5)
            P.QueueLocalEvent(InputEvent::Place(Team, UnitMiner, CampTestX, CampTestY(Team)));
    };
    CHECK(Ra.Begin(Pa.c_str(), A.GetSim(), /*tier*/ -1, /*human*/ 0));
    CHECK(Rb.Begin(Pb.c_str(), B.GetSim(), /*tier*/ -1, /*human*/ 1));
    for (int I = 0; I < 120; ++I) {
        Drive(A, 0, I);
        Drive(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    for (int I = 0; I < 4; ++I) {   // settle so both drain to the same frontier
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(!A.Desynced() && !B.Desynced());
    Ra.End(A.GetSim());
    Rb.End(B.GetSim());

    const MatchRecording La = LoadMatchRecording(Pa.c_str()), Lb = LoadMatchRecording(Pb.c_str());
    CHECK(La.Ok && Lb.Ok);
    CHECK(!La.Events.empty());     // a recording of nothing would pass every check below
    CHECK(!La.Hashes.empty());
    // TICK-EXACT equality, which is the property a diff needs to be usable as evidence: both peers
    // execute the same combined batch on the same tick, so the files must agree element for element.
    // Anything weaker (same events, ±1 tick) would let a real one-tick skew hide — and a skew is a
    // latent desync generator, since any event whose effect depends on WHEN it lands then applies a
    // tick apart on the two sims.
    CHECK(La.Events.size() == Lb.Events.size());
    bool EventsMatch = La.Events.size() == Lb.Events.size();
    for (std::size_t I = 0; EventsMatch && I < La.Events.size(); ++I)
        EventsMatch = La.Events[I].Tick == Lb.Events[I].Tick &&
                      La.Events[I].Event.Team == Lb.Events[I].Event.Team &&
                      La.Events[I].Event.Kind == Lb.Events[I].Event.Kind &&
                      La.Events[I].Event.Type == Lb.Events[I].Event.Type &&
                      La.Events[I].Event.X == Lb.Events[I].Event.X &&
                      La.Events[I].Event.Y == Lb.Events[I].Event.Y;
    CHECK(EventsMatch);
    CHECK(La.Hashes.size() == Lb.Hashes.size());
    bool HashesMatch = La.Hashes.size() == Lb.Hashes.size();
    for (std::size_t I = 0; HashesMatch && I < La.Hashes.size(); ++I)
        HashesMatch = La.Hashes[I].Tick == Lb.Hashes[I].Tick && La.Hashes[I].Hash == Lb.Hashes[I].Hash;
    CHECK(HashesMatch);
    // Hashes land on the ANCHOR cadence, which is what makes them align across peers without any
    // interpolation — if this drifts, two files can be internally fine yet impossible to compare.
    for (const RecordedHash& H : La.Hashes) CHECK(H.Tick % 10 == 0);
    // And the header records each peer's OWN side, which is what orients the pair for a reader.
    CHECK(La.HumanTeam == 0 && Lb.HumanTeam == 1);
    CHECK(La.Seed == Lb.Seed);
}

// #112: recordings key their CVar lines by NAME (format v3), not by the X-list ordinal. The ordinal
// is declaration order, so inserting one knob slid every later id and replayed the previous day's
// captures with values shifted along the list — mine rows of "5/35" nobody set. A name cannot slide.
static void TestRecordingCvarsAreNameKeyedAndSurviveReorder() {
    const std::filesystem::path Dir = std::filesystem::temp_directory_path() / "lur-rps-recname";
    std::error_code Ec;
    std::filesystem::create_directories(Dir, Ec);
    const std::string P = (Dir / "named.rec").string();

    // A sim carrying NON-DEFAULT tunables — defaults would round-trip even if the key were ignored.
    Sim S;
    S.Init(0xC0FFEE);
    S.Cv.MinerSpeed = F(3);
    S.Cv.RockCost = 77;
    S.Cv.PaperHp = 133;

    MatchRecorder R;
    CHECK(R.Begin(P.c_str(), S, /*Tier*/ -1, /*HumanTeam*/ 0));
    R.End(S);

    const MatchRecording L = LoadMatchRecording(P.c_str());
    CHECK(L.Ok);
    CHECK(L.Version == 3);
    CHECK(L.Cv.MinerSpeed == F(3));
    CHECK(L.Cv.RockCost == 77);
    CHECK(L.Cv.PaperHp == 133);
    CHECK(L.UnknownCvs == 0);

    // The file really is name-keyed: every cv line names a registered gameplay CVar, and the ids
    // appear nowhere. Reading the text is the only way to prove the KEY changed rather than just
    // the values surviving.
    {
        std::FILE* F2 = std::fopen(P.c_str(), "r");
        CHECK(F2 != nullptr);
        char Line[256];
        int CvLines = 0, Named = 0;
        while (F2 && std::fgets(Line, sizeof(Line), F2) != nullptr) {
            if (std::strncmp(Line, "cv ", 3) != 0) continue;
            ++CvLines;
            if (std::strncmp(Line, "cv rps.", 7) == 0) ++Named;
        }
        if (F2) std::fclose(F2);
        CHECK(CvLines == static_cast<int>(CvIdCount));
        CHECK(Named == CvLines);   // not one ordinal left
    }

    // A name this build does not register is SKIPPED, leaving the default — never applied to
    // whatever now occupies that slot, which is the corruption the name key exists to prevent.
    {
        const std::string P2 = (Dir / "stale.rec").string();
        std::FILE* W = std::fopen(P2.c_str(), "w");
        CHECK(W != nullptr);
        if (W) {
            std::fprintf(W, "rec 3\nfp whatever\nseed c0ffee\ntier -1 human 0\n");
            std::fprintf(W, "cv rps.knob.that.was.deleted 999\n");
            std::fprintf(W, "cv %s 42\n", GameplayNameForId(CvIdRockCost));
            std::fprintf(W, "end -1 0\n");
            std::fclose(W);
        }
        const MatchRecording St = LoadMatchRecording(P2.c_str());
        CHECK(St.Ok);
        CHECK(St.UnknownCvs == 1);                 // reported, not silently swallowed
        CHECK(St.Cv.RockCost == 42);               // the knob that still exists still applies
        CHECK(St.Cv.MinerSpeed == DefaultCvs().MinerSpeed);  // the stale one touched nothing
    }
}
#endif  // LUR_INTERNAL (MatchRecorder is dev tooling)

// Deliver everything EXCEPT the Nth MsgInput frame — a lost produced tick inside a link that is
// otherwise working perfectly, which is what #163 observed on hardware (iPhone->Galaxy notifications
// stopped while writes kept landing, and separately an input the iPhone executed at tick 4528 was
// simply absent from the Galaxy's stream with no transport complaint at all).
static void DeliverDroppingNthInput(Outbox& From, LockstepPeer& To, int NthInput) {
    int Seen = 0;
    for (auto& M : From.Q) {
        if (M.first == MsgInput && Seen++ == NthInput) continue;  // the frame that never arrives
        To.OnMessage(M.first, M.second.data(), M.second.size());
    }
    From.Q.clear();
}

// Deliver every MsgInput frame TWICE — the opposite fault to the one above, and the one actually
// caught on hardware 2026-08-01: the Android central had established TWO GATT connections to the same
// iPhone peripheral and subscribed to notifications on both, so every produced frame the peripheral
// notified arrived at the app twice. Writes (central->peripheral) go out from one place and were
// clean, which is why the damage was one-directional in exactly the direction #163 documents.
static void DeliverDuplicatingInputs(Outbox& From, LockstepPeer& To) {
    for (auto& M : From.Q) {
        To.OnMessage(M.first, M.second.data(), M.second.size());
        if (M.first == MsgInput) To.OnMessage(M.first, M.second.data(), M.second.size());
    }
    From.Q.clear();
}

// ---- #163: a DUPLICATED input frame is not a gap, and must not be re-buffered ----
// The sequence is one byte, so "one frame arriving twice" and "255 frames missing" are the same
// unsigned delta. Before the signed reading, a duplicate-delivery link reported an INPUT GAP on every
// tick of a 20-minute match, spent the whole recovery budget in the first three ticks (after which the
// repair path was a silent no-op), and then appended each duplicate to PeerEvents — skewing the peer's
// timeline by one index per frame. That skew is INVISIBLE while batches are empty, which is why it
// survived a clean-looking soak; it diverges the sims the moment a duplicated frame carries an event.
static void TestDuplicateInputFrameIsNotAGap() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1633, 0, Enqueue, &Qa);
    B.Init(0x1633, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    // Every one of A's produced frames reaches B twice; B's reach A once (the half-open shape).
    for (int I = 0; I < 12; ++I) {
        DriveInput(A, 0, I);   // real events, so a skewed stream cannot hide in empty batches
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        DeliverDuplicatingInputs(Qa, B);
        Deliver(Qb, A);
    }
    for (int I = 0; I < 4; ++I) {  // settle
        A.Tick(OneTickNs); B.Tick(OneTickNs); DeliverDuplicatingInputs(Qa, B); Deliver(Qb, A);
    }

    CHECK(B.DuplicateFrames() > 0);   // it noticed, and named the fault as duplication
    CHECK(B.InputGaps() == 0);        // and NOT as loss — the two are opposite faults
    CHECK(A.InputGaps() == 0);        // the clean direction stays clean
    // The duplicates were discarded rather than buffered, so the timelines never skewed: no repair was
    // needed, no budget spent, and the sims agree. This is the assertion the old code could not pass.
    CHECK(B.RecoveryAttempts() == 0);
    CHECK(B.GapRecoveries() == 0);
    CHECK(!A.Desynced() && !B.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
}

// ---- #172/#159: a LONG two-peer soak under continuous duplicate delivery — the host analog of the
// hardware determinism soak ----
// The 07-30 divergence's demonstrated mechanism (#159) is a duplicated frame silently skewing the
// peer timeline: harmless while the duplicated batches are empty, fatal the instant one carries a
// real event, and therefore surfacing at an arbitrary later tick unrelated to when the fault began.
// TestDuplicateInputFrameIsNotAGap proves the signed-sequence discard over 16 ticks; this proves it
// holds over THOUSANDS, with real events landing at irregular, seeded intervals so a one-index skew
// cannot hide in a run of empty batches (which is exactly how the hardware skew stayed invisible for
// ~13 minutes). Every A->B frame is delivered twice for the whole run — the half-open duplicate
// shape #163 found on hardware. If the discard ever regresses to buffering the second copy, the two
// sims diverge here within a few hundred ticks, long before a 20-minute hardware run would show it.
static void TestTwoPeerDuplicateSoakStaysIdentical() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x5017A, 0, Enqueue, &Qa);
    B.Init(0x5017A, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    SplitMix64 Rng(0x9E3779B97F4A7C15ull);
    constexpr int Ticks = 2500;   // ~250 anchors; well past the point a skew would surface if broken
    for (int I = 0; I < Ticks; ++I) {
        // Real production orders at irregular, seeded ticks on BOTH sides. A duplicated Queue event
        // applied twice would enqueue a phantom unit and diverge the hash — so this is precisely the
        // input that makes a silent skew observable. Type/count mirror DriveInput; count kept low so
        // the field (and this test's runtime) stays bounded as units meet and cull.
        if (Rng.NextBounded(12) == 0) A.QueueLocalEvent(InputEvent::Queue(0, 2, 2));
        if (Rng.NextBounded(12) == 0) B.QueueLocalEvent(InputEvent::Queue(1, 3, 2));
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        DeliverDuplicatingInputs(Qa, B);   // every A frame reaches B twice (the half-open shape)
        Deliver(Qb, A);                    // B -> A stays clean, so the fault is one-directional
        if ((I % 500) == 0) CHECK(!A.Desynced() && !B.Desynced());
    }
    for (int I = 0; I < 8; ++I) {   // settle: drain both to a common frontier
        A.Tick(OneTickNs); B.Tick(OneTickNs); DeliverDuplicatingInputs(Qa, B); Deliver(Qb, A);
    }

    CHECK(B.DuplicateFrames() > Ticks / 2);   // duplication really was sustained for the whole run
    CHECK(B.InputGaps() == 0);                // and never once misread as a loss (the 255-missing bug)
    CHECK(A.InputGaps() == 0);
    CHECK(B.RecoveryAttempts() == 0 && B.GapRecoveries() == 0);  // no budget spent on a phantom fault
    CHECK(!A.Desynced() && !B.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.ExecTick() > Ticks - 100);        // the match ran the full distance, not a stalled stub
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());  // bit-identical after thousands of ticks
}

// ---- #167: repairing a lost frame must not spend the DESYNC budget ----
// MaxDesyncRecoveries is what finally declares a draw. The gap path shares the request machinery but
// not the meaning: it is caused by the radio, it is repaired before the hole executes, and an ordinary
// app restart takes it too — so a match containing one restart used to start its next real repair
// already an attempt down, and enough of them ended a healthy match as a draw.
static void TestInputGapDoesNotSpendTheDesyncBudget() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1671, 0, Enqueue, &Qa);
    B.Init(0x1671, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);

    for (int I = 0; I < 3; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    A.Tick(OneTickNs); B.Tick(OneTickNs);
    DeliverDroppingNthInput(Qa, B, 0);          // lose one produced frame
    Deliver(Qb, A);
    A.Tick(OneTickNs); B.Tick(OneTickNs);
    Deliver(Qa, B);                              // the next frame exposes it
    Deliver(Qb, A);

    CHECK(B.InputGaps() == 1);
    CHECK(B.GapRecoveries() == 1);        // charged to the gap budget...
    CHECK(B.RecoveryAttempts() == 0);     // ...and NOT to the one that ends the match in a draw
}

// ---- #163: a lost input frame must be DETECTED AND LOCATED, not silently absorbed ----
// The link carried `recv msg type=N size=M` per frame and nothing tied a frame to a TICK, so a
// missing input could not be found in the log — it took two flight recordings and a diff to locate
// one. A produced frame now carries its own low-order tick byte, so the receiver knows the next index
// it expects and says so on the spot.
//
// Note WHEN detection happens: the gap is invisible while the frame is merely late (a lost tail frame
// is indistinguishable from silence), and becomes visible when the NEXT frame arrives carrying a
// sequence one past what was expected. That is early enough to matter — the ceiling is gated on
// PeerEvents.size(), so the hole's tick has not been executed yet.
static void TestLostInputFrameIsDetectedAndLocated() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x163, 0, Enqueue, &Qa);
    B.Init(0x163, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());
    CHECK(B.InputGaps() == 0);

    // Five clean produced ticks.
    for (int I = 0; I < 5; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(B.InputGaps() == 0);   // a clean run must never cry wolf

    // Lose A's next produced frame. When A is caught up, the next frame it produces is for tick ==
    // its current head (ProduceAndSend stamps LocalEvents.size() == WallTicks == ExecTick), so capture
    // that as the tick the gap detector must name.
    const uint32_t LostTick = A.ExecTick();
    A.Tick(OneTickNs);
    B.Tick(OneTickNs);
    DeliverDroppingNthInput(Qa, B, 0);   // A produces exactly one frame per Tick at this rate
    Deliver(Qb, A);
    CHECK(B.InputGaps() == 0);           // still just silence — nothing has proven a loss yet

    // The following frame exposes it, and names the tick.
    A.Tick(OneTickNs);
    B.Tick(OneTickNs);
    Deliver(Qa, B);
    Deliver(Qb, A);
    CHECK(B.InputGaps() == 1);
    CHECK(B.LastInputGapTick() == LostTick);
    // One report per gap, not one per later frame: a counter that inflates on every subsequent frame
    // would make "how many did we lose" unanswerable, which is the number that matters.
    for (int I = 0; I < 5; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(B.InputGaps() == 1);
    // A's side saw a complete stream, so it must report nothing — the loss was one-directional, which
    // is the whole character of a half-open link.
    CHECK(A.InputGaps() == 0);
}

// ---- #163: the half-open signature must be REPORTED, not look like a hang ----
// "The owner placed a camp on both phones and nothing happened." One peer had started and was
// advancing; the other sat at started=0 with untouched starting gold because the peer's camp never
// arrived. From the player's side that is indistinguishable from a frozen app, and nothing in the log
// named it. Pre-match, being ready while the peer is not is a bounded condition, so say so.
static void TestPreMatchStallIsReported() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1631, 0, Enqueue, &Qa);
    B.Init(0x1631, 1, Enqueue, &Qb);

    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
    A.Tick(OneTickNs);
    CHECK(A.HasLocalCamp());
    CHECK(!A.PreMatchStalled());   // not a verdict you may reach immediately

    // A keeps re-sending into the failing direction; nothing is delivered either way.
    uint64_t Held = OneTickNs;
    while (Held < LockstepPeer::PreMatchStallWarnNs + OneTickNs) { A.Tick(OneTickNs); Held += OneTickNs; }
    CHECK(!A.MatchStarted());
    CHECK(A.PreMatchStalled());

    // And it must CLEAR when the handshake completes — a diagnostic that stays lit after the fault is
    // one people learn to ignore (the same mistake #112's build-mismatch latch made).
    Qa.Q.clear();
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 8 && !A.MatchStarted(); ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.MatchStarted());
    CHECK(!A.PreMatchStalled());
}

// ---- #160: a PRODUCED tick that LOOKS like the pre-match camp re-send must not be dropped ----
// The #149 re-send used to be recognised by its PAYLOAD — "a one-event batch equal to the peer's
// opening camp" — and dropped without buffering. A player re-placing a camp at the coordinates their
// opening camp already occupies produces exactly those bytes, so a real input tick was silently
// discarded, and that shifts PeerEvents' index==tick alignment for the REST OF THE MATCH.
//
// What this asserts is alignment, not hashes: re-placing onto an occupied square is rejected by the
// sim, so the two states stay identical by luck while the peer's stream sits a tick early — which is
// why the bug survived every existing test and had to be found by diffing two recordings. Any event
// whose effect depends on WHEN it lands would have diverged the sims from that point.
static void TestProducedCampLookAlikeIsNotDropped() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x160, 0, Enqueue, &Qa);
    B.Init(0x160, 1, Enqueue, &Qb);
    A.SetRecording(true);
    B.SetRecording(true);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    // The collision, then a DISTINGUISHABLE event a few ticks later: the second one is what makes
    // the one-tick skew visible rather than just the lost event.
    const int CollideAt = 6, MarkerAt = 11;
    for (int I = 0; I < 60; ++I) {
        if (I == CollideAt) {
            A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
            B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
        }
        if (I == MarkerAt) {
            A.QueueLocalEvent(InputEvent::Queue(0, 0, 3));
            B.QueueLocalEvent(InputEvent::Queue(1, 1, 3));
        }
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    for (int I = 0; I < 6; ++I) {  // settle to a common frontier
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(!A.Desynced() && !B.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());

    // TICK-EXACT equality of the two executed streams. Both peers apply the same combined batch on
    // the same tick, so anything else is a lost or misaligned frame.
    const std::vector<std::vector<InputEvent>>& Ra = A.RecordedEvents();
    const std::vector<std::vector<InputEvent>>& Rb = B.RecordedEvents();
    CHECK(Ra.size() == Rb.size());
    bool Same = Ra.size() == Rb.size();
    for (std::size_t T = 0; Same && T < Ra.size(); ++T) {
        Same = Ra[T].size() == Rb[T].size();
        for (std::size_t K = 0; Same && K < Ra[T].size(); ++K)
            Same = Ra[T][K].Kind == Rb[T][K].Kind && Ra[T][K].Team == Rb[T][K].Team &&
                   Ra[T][K].Type == Rb[T][K].Type && Ra[T][K].X == Rb[T][K].X &&
                   Ra[T][K].Y == Rb[T][K].Y;
    }
    CHECK(Same);
    // And both re-places reached both peers: under the bug each peer executed only its OWN, so this
    // counts 3 (two opening camps + one own re-place) instead of 4.
    int Places = 0;
    for (const std::vector<InputEvent>& Batch : Ra)
        for (const InputEvent& E : Batch)
            if (E.Kind == EventPlaceBuilding && E.X == CampTestX.Raw) ++Places;
    CHECK(Places == 4);
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
}

// ---- #167: "ready" must mean "ready with a camp that will actually exist" ----
// PreMatchTick captured the first PlaceBuilding/UnitMiner off the pending queue as LocalCamp_ and
// set LocalReady_ WITHOUT validating placement. The match then started and tick 0's ApplyPlace threw
// the camp away, so a peer could enter a live match with no camp and its full opening gold — a state
// no rule of the game produces, and one that reads as an economy desync rather than a bad input.
//
// A human cannot reach it: drag-to-place only emits an event once ResolvePlacement has already
// succeeded. The agent harness reaches it trivially, because injecting EXACT coordinates (bypassing
// the snap) is the entire point of it — which is how this was found.
static void TestPreMatchRejectsUnplaceableCamp() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x167, 0, Enqueue, &Qa);
    B.Init(0x167, 1, Enqueue, &Qb);

    // Team 0 "places" its opening camp on team 1's ground — outside its own frontier, so tick 0
    // would reject it.
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(1)));
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 8; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }

    // An unplaceable camp is not a readiness. Neither peer may start: B is legitimately ready, but a
    // match needs BOTH camps, and starting here is what produced the campless peer.
    CHECK(!A.HasLocalCamp());
    CHECK(!A.MatchStarted());
    CHECK(!B.MatchStarted());

    // The rejection must not wedge the handshake — a legal camp afterwards still readies and starts.
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
    for (int I = 0; I < 8 && !(A.MatchStarted() && B.MatchStarted()); ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.HasLocalCamp());
    CHECK(A.MatchStarted() && B.MatchStarted());

    // And the camp is REALLY there once tick 0 has run — the check above only proves we agreed to
    // start, which is exactly the thing that used to be true while the camp did not exist.
    for (int I = 0; I < 4; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    const Sim& Sa = A.GetSim();
    int Camps0 = 0;
    for (int32_t I = 0; I < Sa.Count; ++I)
        if (Sa.IsAlive(I) && Sa.IsBuilding(I) && !Sa.IsHomeBase(I) && Sa.Team[I] == 0 &&
            Sa.Type[I] == UnitMiner)
            ++Camps0;
    CHECK(Camps0 == 1);
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
}

// The gate above is SPATIAL only (CanPlaceBuilding), and this pins that on purpose. Using the
// stricter WouldAcceptPlace would also demand the camp be AFFORDABLE, and gold is precisely the
// input that legitimately changes between the placement and tick 0: MsgCvarSync converges the
// tunables pre-tick-0 (#147) and the two phones genuinely arrive holding different starting_gold.
// A peer that refused its camp on pre-merge gold would sit silently unready on a camp tick 0 would
// have accepted — indistinguishable from #163's half-open link, and a much worse bug than the one
// #167 fixes. So: an unaffordable camp on LEGAL ground still readies and still starts the match.
static void TestPreMatchAcceptsUnaffordableCampOnLegalGround() {
    const int32_t GoldWas = CvStartingGold.Get();
    CvStartingGold.Set(100);  // far below a camp: tick 0 will discard it, the handshake must not stall
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x167b, 0, Enqueue, &Qa);
    B.Init(0x167b, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.HasLocalCamp() && B.HasLocalCamp());
    CHECK(A.MatchStarted() && B.MatchStarted());
    for (int I = 0; I < 6; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    CvStartingGold.Set(GoldWas);
}

// Re-encode ONE of the sender's produced frames with an extra event injected, keeping the sequence
// byte and appending rather than replacing — so nothing looks lost, no gap is reported, and the two
// sims genuinely diverge. That is the shape #159 captured on hardware: a long clean match, then one
// peer's executed stream containing an event the other's does not, with the link reporting nothing.
// Injecting a real divergence matters — a test that only forges a bad ANCHOR proves the detector
// fires but cannot prove two different states are brought back together.
static bool TamperOneInput(Outbox& From, LockstepPeer& To, uint8_t ForgedTeam) {
    bool Did = false;
    for (auto& M : From.Q) {
        if (!Did && M.first == MsgInput) {
            BitReader R(M.second.data(), M.second.size());
            const uint32_t Seq = R.ReadBits(8);
            InputEvent Buf[MaxEventsPerTick];
            const int Cnt = DecodeEventBatch(R, Buf, MaxEventsPerTick);
            if (Cnt >= 0 && Cnt + 1 < MaxEventsPerTick) {
                // Slot 2/3 is each team's OPENING CAMP: Init auto-places the two home bases at slots
                // 0/1 (#146) and tick 0's two camps land next. Queueing at the camp really does move
                // state (gold down, queue up) — queueing at the HOME BASE is rejected, so it would
                // inject nothing and the test would prove nothing.
                Buf[Cnt] = InputEvent::Queue(ForgedTeam, ForgedTeam == 0 ? 2 : 3, 4);
                BitWriter W;
                W.WriteBits(Seq, 8);
                EncodeEventBatch(W, Buf, Cnt + 1);
                const std::vector<uint8_t>& B = W.Finish();
                To.OnMessage(MsgInput, B.data(), B.size());
                Did = true;
                continue;
            }
        }
        To.OnMessage(M.first, M.second.data(), M.second.size());
    }
    From.Q.clear();
    return Did;
}

// ---- #161: A DESYNC MUST RECOVER THE MATCH. Owner direction, replacing e6d6abf's draw ----
// e6d6abf made a desync end the match as a draw. That was a stopgap for something worse (both phones
// froze forever, tick 8180, datagrams still flowing, no way out but killing the app) but it throws
// the match away, and a draw is a lie about what happened.
//
// The rule: the peer whose device GUID is lower keeps its timeline and the other rebuilds from it.
// MyTeam already IS that comparison on every path (Team = MyGuid < PeerGuid ? 0 : 1), so both peers
// compute the same survivor locally with nothing negotiated. Note it converges regardless of WHICH
// side lost data — both end up replaying one identical history — so consistency holds even when the
// discarded timeline was the more complete one. That is the stated priority: consistency, not
// fairness (there is no referee to appeal to, and the players share a room).
static void TestDesyncRecoversToACommonStateAndKeepsPlaying() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1610, 0, Enqueue, &Qa);
    B.Init(0x1610, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());

    for (int I = 0; I < 10; ++I) {  // play cleanly, and bank gold so the forged queue has an effect
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());

    // Diverge for real: B executes an event A never sent. (It lands InputDelayTicks later, so the two
    // states are still equal on the very next line — the divergence appears when the tick executes.)
    A.Tick(OneTickNs);
    B.Tick(OneTickNs);
    CHECK(TamperOneInput(Qa, B, 0));
    Deliver(Qb, A);

    // Play on: the next anchor cross-check trips on BOTH peers and recovery runs. Recovery having run
    // at all is also the proof the injection LANDED — a forged event the sim rejected would change no
    // state, trip no anchor, and leave this at zero.
    for (int I = 0; I < 30; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.RecoveryAttempts() >= 1);      // the divergence was detected, not absorbed
    CHECK(B.RecoveryAttempts() >= 1);

    // THE POINT: one state again, and the match is still being played. Settle heads first — the
    // recovery reseed left the two speculative heads a tick apart (independent wall clocks), though
    // their confirmed timelines already agree.
    SettleUntilEqual(A, B, Qa, Qb);
    CHECK(A.GetSim().Result == ResultOngoing);
    CHECK(B.GetSim().Result == ResultOngoing);
    CHECK(!A.Desynced() && !B.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());

    // And it keeps playing in lockstep from the recovery point onward, with real input — a recovery
    // that converges and then wedges at the ceiling would satisfy every check above.
    const uint32_t Resumed = A.ExecTick();
    for (int I = 0; I < 60; ++I) {
        if (I % 13 == 4) {
            A.QueueLocalEvent(InputEvent::Queue(0, 0, 2));
            B.QueueLocalEvent(InputEvent::Queue(1, 1, 2));
        }
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.ExecTick() > Resumed + 40);
    SettleUntilEqual(A, B, Qa, Qb);
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    CHECK(A.RecoveryAttempts() == 1);      // one divergence, one recovery — it did not re-trip
}

// ---- #162: a peer starved at the ceiling must not wait forever ----
// The load collapse ended with the two phones in DIFFERENT MATCHES and nothing to reconcile them:
// "iPhone: tick=5195 ... frozen at the ceiling, waiting for peer input that will never come.
// Galaxy: tick=0 ... gave up, restarted, sitting pre-match." From the player's side both phones were
// dead. The ceiling stall itself was unbounded — the one hold in the netcode with no timeout, which is
// how "the peer is slow" became indistinguishable from "the peer is in another match".
//
// A bound turns that into an outcome: the match ends, #149's restart runs, and both sides return to
// the camp handshake, which is the one state that always re-converges. Deliberately generous — a peer
// this far behind is already unplayable, and the transport's own 5 s timeout fires long before.
static void TestStarvedCeilingEndsTheMatch() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1620, 0, Enqueue, &Qa);
    B.Init(0x1620, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    for (int I = 0; I < 10; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    const uint32_t Reached = A.ExecTick();
    CHECK(Reached > 0);

    // B goes silent (it collapsed under load, or restarted into another match). A keeps ticking and
    // stalls at the ceiling — the sim cannot advance without B's frames.
    uint64_t Held = 0;
    while (A.GetSim().Result == ResultOngoing && Held < 3 * LockstepPeer::CeilingStallTimeoutNs) {
        A.Tick(OneTickNs);
        Qa.Q.clear();       // nothing is delivered to B, and B sends nothing back
        Held += OneTickNs;
    }
    CHECK(A.Stalled() || A.GetSim().Result != ResultOngoing);
    CHECK(A.GetSim().Result == ResultDraw);          // it REACHED an outcome instead of hanging
    CHECK(Held >= LockstepPeer::CeilingStallTimeoutNs);   // and not before the bound
    CHECK(A.ExecTick() >= Reached);                  // nothing was rewound to get there

    // And the session recovers into a fresh match, which is what makes it survivable: both peers end
    // up back at the camp handshake, the one state that always re-converges.
    const uint32_t Before = A.MatchIndex();
    A.Tick(PostMatchHoldNs + OneTickNs);
    CHECK(A.MatchIndex() == Before + 1);
    CHECK(!A.MatchStarted());

    // A stall that RESOLVES must not be punished: the bound resets when the peer's frames resume, or a
    // busy phone that briefly falls behind would lose matches to a timer.
    Outbox Qc, Qd;
    LockstepPeer C, D;
    C.Init(0x1621, 0, Enqueue, &Qc);
    D.Init(0x1621, 1, Enqueue, &Qd);
    PlaceCampsAndStart(C, D, Qc, Qd);
    for (int Round = 0; Round < 6; ++Round) {
        uint64_t Gap = 0;                                  // stall for most of the bound...
        while (Gap < LockstepPeer::CeilingStallTimeoutNs - 10 * OneTickNs) {
            C.Tick(OneTickNs);
            Gap += OneTickNs;
        }
        D.Tick(OneTickNs);                                  // ...then D speaks again
        Deliver(Qd, C);
        Deliver(Qc, D);
        CHECK(C.GetSim().Result == ResultOngoing);           // never ends: the bound re-armed
    }
}

// ---- #161: recovery must be BOUNDED, and the draw survives as the last resort ----
// "A recovery loop that never converges is the freeze again by another name." Input-history replay
// cannot converge if the cause is genuine nondeterminism rather than lost input — it faithfully
// reproduces the divergence — so the attempt count has to be capped and the terminal outcome defined.
// That is the one place a draw legitimately lives: after recovery has failed, not instead of trying.
static void TestDesyncRecoveryIsBoundedThenDraws() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1611, 0, Enqueue, &Qa);
    B.Init(0x1611, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    for (int I = 0; I < 10; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }

    // Re-diverge on EVERY delivery, so no recovery can ever succeed — the pathological case.
    int Rounds = 0;
    for (; Rounds < 400 && A.GetSim().Result == ResultOngoing; ++Rounds) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        if (!TamperOneInput(Qa, B, 0)) Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(Rounds < 400);                                        // it TERMINATED — no infinite loop
    CHECK(A.RecoveryAttempts() <= LockstepPeer::MaxDesyncRecoveries);
    CHECK(A.GetSim().Result == ResultDraw);                     // A hit the bounded last resort first
    // BOTH peers must reach the SAME verdict (Draw) — one drawing while the other plays on would break
    // the consistency rule. Under rollback the two exhaust their budgets a beat apart on independent
    // clocks (the loop above stopped the instant A drew), so drive B alone to its own terminal draw —
    // ticking A would start its post-match restart. With A drawn and silent, B starves past the
    // rollback horizon and the #162 ceiling bound turns that into B's draw. Same verdict, reached
    // independently, which is exactly the consistency the rule demands.
    uint64_t BHeld = 0;
    while (B.GetSim().Result == ResultOngoing && BHeld < 3 * LockstepPeer::CeilingStallTimeoutNs) {
        B.Tick(OneTickNs);
        BHeld += OneTickNs;
    }
    CHECK(B.GetSim().Result == ResultDraw);
    // And the draw still recovers the SESSION, which is what e6d6abf bought and must not be lost:
    // the post-match hold expires, a fresh match begins, and the latch clears.
    const uint32_t Before = A.MatchIndex();
    A.Tick(PostMatchHoldNs + OneTickNs);
    CHECK(A.MatchIndex() == Before + 1);
    CHECK(!A.Desynced());
    CHECK(A.RecoveryAttempts() == 0);                           // the budget is per match
    CHECK(!A.MatchStarted());
}

// ---- #161 + #163: a LOST frame recovers without the sims ever diverging at all ----
// The best outcome available, and the reason the sequence byte was worth a byte per frame: the gap is
// detected when the NEXT frame arrives, and the ceiling is gated on PeerEvents.size(), so the hole's
// tick has not executed yet. Recovery therefore happens BEFORE any divergence exists — no bad state,
// no anchor alarm, no lost work. This is the exact hardware case from #163 observation 2 (an input
// executed on the iPhone at tick 4528, absent from the Galaxy, link reporting nothing).
static void TestLostInputRecoversBeforeDiverging() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1612, 0, Enqueue, &Qa);
    B.Init(0x1612, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    for (int I = 0; I < 6; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }

    // A's next frame carries real input and is LOST.
    A.QueueLocalEvent(InputEvent::Queue(0, 0, 3));
    A.Tick(OneTickNs);
    B.Tick(OneTickNs);
    DeliverDroppingNthInput(Qa, B, 0);
    Deliver(Qb, A);

    for (int I = 0; I < 30; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(B.InputGaps() == 1);                  // the loss was seen
    CHECK(!A.Desynced() && !B.Desynced());      // and never became a divergence
    CHECK(A.GetSim().Result == ResultOngoing);
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    // Recovering from the peer's history means the input that was dropped is not lost from the match
    // either: B rebuilt on A's stream, which contains it.
    CHECK(A.ExecTick() > InputDelayTicks + 10);
}

// ---- A DESYNC MUST NOT FREEZE THE GAME: it recovers, and a draw is only the last resort ----
// Every other test here asserts a desync does NOT happen, which is the right thing to assert and is
// also exactly why this shipped broken: nothing covered what happens WHEN one does. Observed on two
// phones 2026-07-30 — both peers pinned at tick 8180 with different hashes, datagrams still flowing,
// no message on screen, no way out but killing the app. Desync gated the exec loop and only
// BeginMatch cleared it, and a match that never ends never reaches BeginMatch.
//
// This covers the LONE-PEER trip, which is not a real-world shape (both peers cross-check the same
// anchor, so both see a genuine divergence) but is the one that pins down the failure mode: the peer
// must not be left holding a flag that nothing clears. #161 replaced the instant draw with recovery,
// so what is asserted here is that the trip is no longer terminal and that it always REACHES an
// outcome — converged or, once the attempt budget is spent, drawn — instead of stopping.
//
// The divergence is injected by handing A an anchor for a tick it has ALREADY hashed, carrying a
// hash that cannot be right. That is precisely the input CrossCheck consumes, so this exercises the
// real detection path rather than poking the flag.
static void TestDesyncIsNeverTerminal() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x9001, 0, Enqueue, &Qa);
    B.Init(0x9001, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted());
    // Run past an anchor tick (they land every 10th) so A has a hash of its own to compare against.
    for (int I = 0; I < 12; ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(!A.Desynced() && A.GetSim().Result == ResultOngoing);
    const uint32_t AnchorTick = (A.ExecTick() / 10) * 10;   // the most recent anchor A emitted
    Lur::Serialization::BitWriter W;
    Lur::Serialization::WriteVarUint(W, AnchorTick);
    W.WriteBits(0xDEADBEEFu, 32);                           // a hash A cannot possibly have produced
    const std::vector<uint8_t>& Bad = W.Finish();
    A.OnMessage(MsgAnchor, Bad.data(), Bad.size());

    // Detected, and NOT resolved as an instant draw any more: the match is still alive while recovery
    // is attempted. That is the behaviour change #161 asked for.
    CHECK(A.RecoveryAttempts() == 1);
    CHECK(A.GetSim().Result == ResultOngoing);

    // A holds the lower device id, so ITS timeline is the survivor — nothing to adopt, nothing to wait
    // for. It hands its history over and plays on, which for a forged mismatch (A's state was in fact
    // fine) is the right answer: the match is not thrown away.
    for (int I = 0; I < 20; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(!A.Desynced());
    CHECK(A.GetSim().Result == ResultOngoing);
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());

    // The other side of the same coin, and the one that could still freeze: forge the mismatch into the
    // ADOPTER, whose repair depends on a history the peer has no reason to send. It must reach an
    // outcome anyway. A permanent hold is the original bug wearing a new hat, so the timeout converts
    // it into the draw — the bounded last resort — and the session then restarts.
    Outbox Qc, Qd;
    LockstepPeer C, D;
    C.Init(0x9002, 0, Enqueue, &Qc);
    D.Init(0x9002, 1, Enqueue, &Qd);
    PlaceCampsAndStart(C, D, Qc, Qd);
    for (int I = 0; I < 12; ++I) {
        C.Tick(OneTickNs); D.Tick(OneTickNs); Deliver(Qc, D); Deliver(Qd, C);
    }
    const uint32_t DAnchor = (D.ExecTick() / 10) * 10;
    Lur::Serialization::BitWriter W2;
    Lur::Serialization::WriteVarUint(W2, DAnchor);
    W2.WriteBits(0xDEADBEEFu, 32);
    const std::vector<uint8_t>& Bad2 = W2.Finish();
    D.OnMessage(MsgAnchor, Bad2.data(), Bad2.size());
    CHECK(D.RecoveryAttempts() == 1);
    CHECK(D.GetSim().Result == ResultOngoing);   // still not an instant draw

    uint64_t Held = 0;
    while (D.GetSim().Result == ResultOngoing && Held < 120 * OneTickNs) {
        C.Tick(OneTickNs);
        D.Tick(OneTickNs);
        Deliver(Qc, D);
        Deliver(Qd, C);
        Held += OneTickNs;
    }
    CHECK(D.GetSim().Result == ResultDraw);
    CHECK(Held <= LockstepPeer::DesyncRecoveryTimeoutNs + 4 * OneTickNs);  // bounded, and by THAT bound

    // And the session recovers from that draw, which is what e6d6abf bought and must not regress.
    const uint32_t MatchBefore = D.MatchIndex();
    D.Tick(PostMatchHoldNs + OneTickNs);
    CHECK(!D.Desynced());
    CHECK(D.MatchIndex() == MatchBefore + 1);
    CHECK(!D.MatchStarted());                               // fresh match: awaiting both camps
    CHECK(D.GetSim().Result == ResultOngoing);
}

// ---- #90: the speculate step caps ticks per call so a catch-up burst can't starve input (ANR) ----
// Under rollback the head is bounded by the LOCAL wall clock (we never speculate past our own produced
// ticks), so piling peer input on A while A doesn't tick advances nothing — the ceiling is A's own
// WallTicks. The cap then bites on the one big local advance that follows.
static void TestLockstepExecuteCapBounded() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x2468, 0, Enqueue, &Qa);
    B.Init(0x2468, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);
    for (int I = 0; I < 3; ++I) {  // settle so the head has caught up to A's own wall clock
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    const uint32_t Head0 = A.ExecTick();  // head == A's WallTicks now (both idle, caught up)

    // Pile up a big peer-input backlog on A WITHOUT letting it advance its own clock: A never ticks,
    // so its WallTicks stays put and the head cannot run past it however many peer frames arrive.
    const int N = 40;
    for (int I = 0; I < N; ++I) B.Tick(OneTickNs);  // B produces N frames (empty batches suffice)
    Deliver(Qb, A);
    CHECK(A.ExecTick() == Head0);  // peer input alone does not move the head — the local clock gates it

    // One big local advance jumps WallTicks by N at once (production caps at 64). WITHOUT the per-call
    // cap the speculate step would drain all N here — the ANR. WITH it, at most MaxExecTicksPerService.
    A.Tick(static_cast<uint64_t>(N) * OneTickNs);
    CHECK(A.ExecTick() <= Head0 + MaxExecTicksPerService);  // the per-call cap held
    CHECK(A.ExecTick() > Head0);                            // but it made progress

    // Backlog drains over subsequent calls; nothing is discarded.
    for (int I = 0; I < 100; ++I) A.Tick(OneTickNs);
    CHECK(A.ExecTick() >= Head0 + static_cast<uint32_t>(N));  // drained past the whole backlog
    CHECK(!A.Desynced());
}

// ---- Flight-recorder replay: a recorded match replays to a hash-identical state ----
static void TestLockstepReplayHashIdentical() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x77, 0, Enqueue, &Qa);
    B.Init(0x77, 1, Enqueue, &Qb);
    A.SetRecording(true);  // record the executed stream

    for (int I = 0; I < 200; ++I) {
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    for (int I = 0; I < 4; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }

    CHECK(A.RecordedEvents().size() == A.ExecTick());  // recorded every executed tick
    // Feed the recording into a FRESH sim -> must land on the same state (the replay law).
    const uint64_t Replayed = ReplayHashEvents(A.Seed(), A.RecordedEvents());
    CHECK(Replayed == A.GetSim().StateHash());
    CHECK(Replayed == B.GetSim().StateHash());  // both peers, one recording
}

// ---- Injected divergence trips the anchor hash ----
// (The seed is currently gameplay-inert — the v1 map is fixed + mirrored and no RNG
// runs in the tick, per spec §2 — so we inject a genuine state divergence and prove the
// anchor hash catches it, which is exactly what the mechanism is for.)
static void TestLockstepDetectsDivergence() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x99, 0, Enqueue, &Qa);
    B.Init(0x99, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);  // #139: start the match before warming up
    for (int I = 0; I < 12; ++I) {  // warm up in sync
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(!A.Desynced());
    // Corrupt A's state (simulate a lost input / a determinism bug on one peer).
    const_cast<Sim&>(A.GetSim()).Teams[0].Gold += 999;

    // Run past the anchor. Under rollback, detection is no longer simultaneous — the confirmed frontier
    // crosses the anchor tick a tick apart on the two peers — so wait for BOTH to see it, not just A.
    for (int I = 0; I < 20 && (A.RecoveryAttempts() == 0 || B.RecoveryAttempts() == 0); ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    // Both peers cross-check the same anchor, so both must SEE it — detection on one side only would
    // leave the pair unable to agree on a repair. Asserted via RecoveryAttempts rather than
    // Desynced(): since #161 the flag is transient by design (it gates execution while the repair
    // runs and is cleared on convergence), so it is no longer a record that a divergence happened.
    CHECK(A.RecoveryAttempts() >= 1);  // anchor hash mismatch caught within ~1 s
    CHECK(B.RecoveryAttempts() >= 1);
}

// ---- Rollback rides through a peer blip by SPECULATING (not stalling), then re-converges ----
// The old lockstep version asserted the starved peer STALLED at the ceiling after only the delay
// slack. Rollback inverts exactly that: with the peer's frames withheld, the local head keeps
// advancing on its own wall clock (predicting the peer idle), up to the horizon — that is the
// responsiveness the refactor exists for. On resume the held backlog arrives and both re-converge.
static void TestRollbackRidesThroughPeerBlipAndResumes() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x99, 0, Enqueue, &Qa);
    B.Init(0x99, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);

    for (int I = 0; I < 15; ++I) {  // warm up in sync
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    const uint32_t Before = A.ExecTick();

    // Blip: A stops receiving B's frames (Qb held); both keep ticking. Fewer than the horizon, so A
    // never hits the speculation cap.
    const int Blip = static_cast<int>(RollbackHorizon) - 4;
    for (int I = 0; I < Blip; ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);  // A -> B still flows
    }
    CHECK(A.ExecTick() >= Before + static_cast<uint32_t>(Blip) - 1);  // it SPECULATED ahead, not stalled
    CHECK(A.ConfirmedTick() < static_cast<int64_t>(A.ExecTick()));    // and the head is ahead of confirmed

    // Resume: B's held backlog is delivered in order. Both peers were idle, so the "peer idle"
    // predictions all held — no rollback needed, just confirmation — and the pair converges.
    Deliver(Qb, A);
    for (int I = 0; I < 8; ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(!A.Desynced());
    CHECK(A.ExecTick() == B.ExecTick());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());  // resumed to a bit-identical state
}

// ---- Integration: lockstep over the REAL Session (framing) + deferred Loopback ----
// The faithful path — LockstepPeer sends via Session::Send (the framed game slots #44),
// and Session dispatches inbound datagrams back to LockstepPeer::OnMessage. Because a
// lockstep receiver sends from its callback (an inbound input unblocks a tick that emits
// an anchor), this is exactly the re-entrancy the DEFERRED loopback removes: with
// synchronous delivery it would recurse A->B->A; here the reply queues for the next
// Pump. Proves the whole wiring end-to-end, host-side, before the two-window Vulkan main.
struct SessPeer {
    Lur::Transport::LoopbackTransport T;
    Lur::Net::Session S;
    LockstepPeer Lp;
};
static void SendViaSession(void* Ctx, Lur::Net::EMsgType Type, const uint8_t* D, std::size_t N) {
    static_cast<Lur::Net::Session*>(Ctx)->Send(Type, D, N);
}
// The mains' routing table verbatim (Rps/SessionWiring.h), not a hand-rolled copy — this test file
// used to carry a fifth copy, and a test composition that differs from the shipping one silently
// proves less than it looks like it does (#147).
static void RouteToPeer(Lur::Net::Session& S, LockstepPeer& Lp) { RouteSessionToPeer(S, Lp); }

#if LUR_INTERNAL
// ---- #147: the cvar sync ACROSS A REAL SESSION, which is what the phones run. Every other cvar
// test calls Lp.OnMessage directly, so all of them passed while the sync was completely dead on
// the wire: Session's handler table was bounded at 8 and MsgCvar/MsgCvarSync/MsgFingerprint live
// at 8/9/10, so they were dropped at registration AND at dispatch, in silence. Two phones showed
// hash=90c238b0 gold=400 vs hash=9aaa0e2a gold=190 and nothing in either log said why. ----
static void TestCvarSyncOverSessionLoopback() {
    const int32_t GoldDefault = CvStartingGold.Default();
    const Fixed   FrontDefault = CvInitialFrontier.Default();

    auto A = std::make_unique<SessPeer>();
    auto B = std::make_unique<SessPeer>();
    A->T.SetDeferred(true);
    B->T.SetDeferred(true);
    Lur::Transport::LoopbackTransport::Link(A->T, B->T);
    RouteToPeer(A->S, A->Lp);
    RouteToPeer(B->S, B->Lp);
    const std::string AGuid = "guid-aaaa", BGuid = "guid-bbbb";
    A->S.Start(&A->T, AGuid);
    B->S.Start(&B->T, BGuid);
    int Guard = 0;
    while (!(A->S.IsReady() && B->S.IsReady()) && Guard++ < 200) { A->S.Tick(OneTickNs); B->S.Tick(OneTickNs); }
    CHECK(A->S.IsReady() && B->S.IsReady());

    // A is the phone with the persisted rps-cvars.cfg; B is clean. Same order the mains use:
    // Lp.Init, then fingerprint, then seed from the globals, then SendCvarSync.
    CvStartingGold.Set(400);
    CvInitialFrontier.Set(F(60));
    A->Lp.Init(0x50FA, 0, SendViaSession, &A->S);
    A->Lp.SendFingerprint();
    A->Lp.SeedGameplayCvar(CvIdStartingGold,    400,       800);
    A->Lp.SeedGameplayCvar(CvIdInitialFrontier, F(60).Raw, 800);
    A->Lp.SendCvarSync();
    CvStartingGold.Set(GoldDefault);
    CvInitialFrontier.Set(FrontDefault);
    B->Lp.Init(0x50FA, 1, SendViaSession, &B->S);
    B->Lp.SendFingerprint();
    B->Lp.SendCvarSync();
    for (int I = 0; I < 4; ++I) { A->S.Tick(OneTickNs); B->S.Tick(OneTickNs); }  // deliver

    CHECK(!A->Lp.BuildMismatch() && !B->Lp.BuildMismatch());  // the gate ran at all (also was dead)
    CHECK(B->Lp.GetSim().Cv.StartingGold == 400);             // the sync CROSSED the session...
    CHECK(B->Lp.GetSim().Teams[0].Gold == 400);               // ...and re-derived the Init state
    CHECK(B->Lp.GetSim().FrontierT0 == F(60));
    CHECK(A->Lp.GetSim().StateHash() == B->Lp.GetSim().StateHash());  // the on-device readout

    for (int I = 0; I < 120; ++I) {
        A->S.Tick(OneTickNs); B->S.Tick(OneTickNs);
        DriveInput(A->Lp, 0, I); DriveInput(B->Lp, 1, I);
        A->Lp.Tick(OneTickNs); B->Lp.Tick(OneTickNs);
        CHECK(!A->Lp.Desynced() && !B->Lp.Desynced());
    }
    CHECK(A->Lp.ExecTick() > 10);   // past the anchor that desynced on hardware
    CHECK(A->Lp.GetSim().StateHash() == B->Lp.GetSim().StateHash());

    CvStartingGold.Set(GoldDefault);
    CvInitialFrontier.Set(FrontDefault);
}
#endif
static void TestLockstepOverSessionLoopback() {
    auto A = std::make_unique<SessPeer>();  // Sim inside each -> heap, not stack
    auto B = std::make_unique<SessPeer>();
    A->T.SetDeferred(true);
    B->T.SetDeferred(true);
    Lur::Transport::LoopbackTransport::Link(A->T, B->T);
    RouteToPeer(A->S, A->Lp);
    RouteToPeer(B->S, B->Lp);

    const std::string AGuid = "guid-aaaa", BGuid = "guid-bbbb";
    A->S.Start(&A->T, AGuid);
    B->S.Start(&B->T, BGuid);
    int Guard = 0;
    while (!(A->S.IsReady() && B->S.IsReady()) && Guard++ < 200) {
        A->S.Tick(OneTickNs);
        B->S.Tick(OneTickNs);
    }
    CHECK(A->S.IsReady() && B->S.IsReady());

    // Each peer derives its team from the two GUIDs identically (smaller GUID = team 0).
    const uint8_t ATeam = AGuid < A->S.GetPeerGuid() ? 0 : 1;
    const uint8_t BTeam = BGuid < B->S.GetPeerGuid() ? 0 : 1;
    CHECK(ATeam != BTeam);
    A->Lp.Init(0xABCD, ATeam, SendViaSession, &A->S);
    B->Lp.Init(0xABCD, BTeam, SendViaSession, &B->S);

    for (int I = 0; I < 250; ++I) {
        A->S.Tick(OneTickNs);   // pump transports -> deliver queued datagrams to OnMessage
        B->S.Tick(OneTickNs);
        DriveInput(A->Lp, ATeam, I);
        DriveInput(B->Lp, BTeam, I);
        A->Lp.Tick(OneTickNs);  // produce + send (enqueues to peer inbox), execute
        B->Lp.Tick(OneTickNs);
        CHECK(!A->Lp.Desynced() && !B->Lp.Desynced());
    }
    for (int I = 0; I < 8; ++I) {  // settle
        A->S.Tick(OneTickNs);
        B->S.Tick(OneTickNs);
        A->Lp.Tick(OneTickNs);
        B->Lp.Tick(OneTickNs);
    }
    CHECK(A->Lp.ExecTick() > 200);
    CHECK(A->Lp.ExecTick() == B->Lp.ExecTick());
    CHECK(A->Lp.GetSim().StateHash() == B->Lp.GetSim().StateHash());  // bit-identical over the wire
}

// ---- Cold rejoin: kill a peer, rebuild it from the survivor's chunked history, resume ----
static void TestLockstepColdRejoinResync() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x55, 0, Enqueue, &Qa);
    B.Init(0x55, 1, Enqueue, &Qb);

    for (int I = 0; I < 40; ++I) {  // play a while
        DriveInput(A, 0, I);
        DriveInput(B, 1, I);
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    const uint32_t F = A.ExecTick();
    const uint64_t StateAtF = A.GetSim().StateHash();
    CHECK(F > 20);

    // B is KILLED and relaunches fresh (lost everything) — a brand-new peer at tick 0.
    Outbox Qb2;
    LockstepPeer B2;
    B2.Init(0x55, 1, Enqueue, &Qb2);

    // Reconnect: both peers offer their history. Exchange the resync chunks + markers.
    A.BeginResync();
    B2.BeginResync();
    Deliver(Qa, B2);   // A's F-tick history + marker -> B2 rebuilds
    Deliver(Qb2, A);   // B2's 0-tick history + marker -> A ignores (it's ahead)

    CHECK(!A.AwaitingResync() && !B2.AwaitingResync());
    CHECK(B2.ExecTick() == F);
    CHECK(B2.GetSim().StateHash() == StateAtF);   // rejoiner rebuilt to the frozen frontier
    CHECK(A.GetSim().StateHash() == StateAtF);     // survivor unchanged (re-based, sim still at F)

    // Resume LIVE lockstep A <-> B2 from the frontier — must stay bit-identical, no desync.
    for (int I = 0; I < 60; ++I) {
        DriveInput(A, 0, I);
        DriveInput(B2, 1, I);
        A.Tick(OneTickNs);
        B2.Tick(OneTickNs);
        Deliver(Qa, B2);
        Deliver(Qb2, A);
        CHECK(!A.Desynced() && !B2.Desynced());
    }
    for (int I = 0; I < 4; ++I) { A.Tick(OneTickNs); B2.Tick(OneTickNs); Deliver(Qa, B2); Deliver(Qb2, A); }
    CHECK(A.ExecTick() > F + 40);                  // the match advanced well past the rejoin
    CHECK(A.ExecTick() == B2.ExecTick());
    CHECK(A.GetSim().StateHash() == B2.GetSim().StateHash());  // still bit-identical after rejoin
}

// ---- #149: post-match hold -> a FRESH match awaiting both camps ----
// Every test here needs a match that ENDS, fast and deterministically. The cheapest real ending is
// economic exhaustion: with starting gold set to exactly the camp price, placing the opening camp
// leaves 0 gold, no units and nothing queued — so both teams are doomed on that very tick and the
// win rule declares a draw. Real result, one tick, no army needed.
static void ForcedDrawPair(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb, uint64_t Seed) {
    A.Init(Seed, 0, Enqueue, &Qa);
    B.Init(Seed, 1, Enqueue, &Qb);
#if LUR_INTERNAL
    const int32_t CampPrice = CvMinerBuildingCost.Default();
    A.SeedGameplayCvar(CvIdStartingGold, CampPrice, 1000);
    B.SeedGameplayCvar(CvIdStartingGold, CampPrice, 1000);
    A.SendCvarSync();
    B.SendCvarSync();
    Deliver(Qa, B);
    Deliver(Qb, A);
    CHECK(A.GetSim().Cv.StartingGold == CampPrice && B.GetSim().Cv.StartingGold == CampPrice);
#endif
}

// Both peers ready, the match runs to its (draw) result, and the hold has NOT expired yet.
static void RunToResult(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb) {
    PlaceCampsAndStart(A, B, Qa, Qb);
    for (int I = 0; I < 12 && A.GetSim().Result == ResultOngoing; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.GetSim().Result != ResultOngoing);
    CHECK(A.GetSim().Result == B.GetSim().Result);
}

// ---- #159: the "no recording open" sentinel must never be a real match index ----
// A main opens its linked recording on the match-started EDGE, keyed on MatchIndex(). iOS held that
// key as a zero-initialised Obj-C ivar, so "unset" and "match 0" were the same value: the edge never
// fired for the first match and the iPhone recorded NOTHING until a post-match restart bumped the
// index. The pair captured one side of the first linked match — and comparing two peers is the whole
// point of #159, so the first match is the one you least want half of. Found on hardware 2026-07-31,
// by pulling the Galaxy's .rec and finding the iPhone had no counterpart file at all.
//
// The sentinel is shared (Rps::NoRecMatchIdx) now, and this pins the property that makes it safe: no
// match index a session can actually reach may equal it. Fails on the host, not on two phones.
static void TestNoRecMatchIdxIsNotAReachableMatchIndex() {
    CHECK(NoRecMatchIdx != 0);   // the exact collision that hid: match 0 is the FIRST match
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x159C, 0, Enqueue, &Qa);
    B.Init(0x159C, 1, Enqueue, &Qb);
    CHECK(A.MatchIndex() == 0);                 // a fresh session starts AT the value that collided
    CHECK(A.MatchIndex() != NoRecMatchIdx);
    // ...and stays clear of it across restarts, which is the only way the index ever moves.
    for (int I = 0; I < 4; ++I) {
        ForcedDrawPair(A, B, Qa, Qb, 0x159C + static_cast<uint64_t>(I));
        RunToResult(A, B, Qa, Qb);
        const uint32_t Before = A.MatchIndex();
        uint64_t Held = 0;
        while (Held < PostMatchHoldNs + OneTickNs) {
            A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
            Held += OneTickNs;
        }
        CHECK(A.MatchIndex() == Before + 1);
        CHECK(A.MatchIndex() != NoRecMatchIdx);
    }
}

// ---- #180: the match-start edge must reach a main BEFORE tick 0 is executed ----
// A recording opened even one tick late loses tick 0, and tick 0 is the one tick guaranteed to carry
// input: TryStartMatch installs BOTH camps there. On hardware 2026-08-01 the Galaxy armed 77 ms after
// its match started and the iPhone 3 s before, so a 23-minute desync-free run diffed as
// "EVENTS differ at tick 0 ... look at the transport" — a false lead, and pointed at the one
// subsystem #163 had just made everyone suspicious of.
//
// The old arm was a POLL of MatchStarted() from each main's own loop, and no amount of call-site
// ordering makes that safe: TryStartMatch also runs while DELIVERING the peer's camp, so the peer
// that starts on a delivered message can reach tick 0 before its loop looks again. This pins the
// invariant at the seam instead — for BOTH peers, since the two start by different routes (one
// during its own Tick, the other during delivery), which is exactly why it was asymmetric on
// hardware.
namespace {
struct StartOrderProbe {
    int  Starts = 0;
    int  TicksSeen = 0;
    int  TicksBeforeFirstStart = 0;   // must stay 0: nothing may execute before the edge
    uint32_t FirstTick = 0xFFFFFFFFu;
};
void OnStartProbe(void* C) { ++static_cast<StartOrderProbe*>(C)->Starts; }
void OnTickProbe(void* C, uint32_t Tick, const InputEvent*, int, uint64_t) {
    StartOrderProbe* P = static_cast<StartOrderProbe*>(C);
    if (P->Starts == 0) ++P->TicksBeforeFirstStart;
    if (P->TicksSeen == 0) P->FirstTick = Tick;
    ++P->TicksSeen;
}
}  // namespace

static void TestMatchStartEdgePrecedesTickZero() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    StartOrderProbe Pa, Pb;
    A.Init(0x180A, 0, Enqueue, &Qa);
    B.Init(0x180A, 1, Enqueue, &Qb);
    A.SetMatchStartSink(OnStartProbe, &Pa);  A.SetTickSink(OnTickProbe, &Pa);
    B.SetMatchStartSink(OnStartProbe, &Pb);  B.SetTickSink(OnTickProbe, &Pb);

    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());
    CHECK(Pa.Starts == 1 && Pb.Starts == 1);   // fired, exactly once, on both

    for (int I = 0; I < 6; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    // The whole point: no tick reached the recorder before it was told to open, on either peer...
    CHECK(Pa.TicksBeforeFirstStart == 0);
    CHECK(Pb.TicksBeforeFirstStart == 0);
    // ...and the first tick each of them sees is tick 0 — the one carrying both camps. A file whose
    // first recorded tick is 1 is precisely the artifact that made recdiff blame the wire.
    CHECK(Pa.TicksSeen > 0 && Pb.TicksSeen > 0);
    CHECK(Pa.FirstTick == 0);
    CHECK(Pb.FirstTick == 0);
}

// ...and again across the #149 post-match restart, since each match gets its own file: an edge that
// fired only for the first match would leave every later one unrecorded (the shape of the iOS
// zero-init bug above, arrived at from the other direction).
static void TestMatchStartEdgeFiresOnEveryRestart() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    StartOrderProbe Pa, Pb;
    A.SetMatchStartSink(OnStartProbe, &Pa);
    B.SetMatchStartSink(OnStartProbe, &Pb);
    ForcedDrawPair(A, B, Qa, Qb, 0x180B);     // Init + cvar sync only — no match yet
    A.SetMatchStartSink(OnStartProbe, &Pa);
    B.SetMatchStartSink(OnStartProbe, &Pb);
    RunToResult(A, B, Qa, Qb);                // the first match starts (and ends) here
    const int AfterFirst = Pa.Starts;
    CHECK(AfterFirst >= 1);
    CHECK(Pb.Starts >= 1);
    uint64_t Held = 0;
    while (Held < PostMatchHoldNs + OneTickNs) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
        Held += OneTickNs;
    }
    // The restart re-runs the camp handshake, so the edge must come round again on both peers.
    RunToResult(A, B, Qa, Qb);
    CHECK(Pa.Starts > AfterFirst);
    CHECK(Pb.Starts > AfterFirst);
}

// The core behaviour: the result stands for PostMatchHoldNs (no restart early), then a fresh match
// begins — pre-match again, tick 0, both camps required, seed bumped and AGREED by both peers.
static void TestPostMatchHoldThenFreshMatch() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    ForcedDrawPair(A, B, Qa, Qb, 0x149A);
    const uint64_t OldSeed = A.Seed();
    RunToResult(A, B, Qa, Qb);
    const uint32_t Idx = A.MatchIndex();

    // Just under the hold: still the finished match, so the win/lose screen is still up.
    uint64_t Held = 0;
    while (Held + OneTickNs < PostMatchHoldNs) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
        Held += OneTickNs;
    }
    CHECK(A.MatchIndex() == Idx);                       // no restart yet
    CHECK(A.GetSim().Result != ResultOngoing);
    CHECK(A.Seed() == OldSeed);

    // Past the hold: a fresh match, waiting on camps again.
    for (int I = 0; I < 3; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }
    CHECK(A.MatchIndex() == Idx + 1 && B.MatchIndex() == Idx + 1);
    CHECK(!A.MatchStarted() && !B.MatchStarted());      // the #139 gate is armed again
    CHECK(A.ExecTick() == 0 && B.ExecTick() == 0);
    CHECK(A.GetSim().Result == ResultOngoing);
    CHECK(!A.HasLocalCamp() && !B.HasLocalCamp());      // last match's camp does not carry over
    CHECK(A.Seed() == OldSeed + 1 && B.Seed() == A.Seed());   // hazard 5: agreed, and varied
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
#if LUR_INTERNAL
    // Hazard 6: the merged cvar set survives the restart — a fresh sim must never fall back to
    // Sim::Init off the local globals (that is #147 returning through a new door).
    CHECK(A.GetSim().Cv.StartingGold == CvMinerBuildingCost.Default());
    CHECK(B.GetSim().Cv.StartingGold == CvMinerBuildingCost.Default());
#endif
    // And the second match is playable and stays bit-identical.
    PlaceCampsAndStart(A, B, Qa, Qb);
    CHECK(A.MatchStarted() && B.MatchStarted());
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
}

// Hazard 3 — the restart SKEW. The peers time the hold on their own clocks, so one rebuilds first
// and sends its camp while the other is still on its win screen, where it lands as the OLD match's
// input and is then wiped by that peer's own restart. Without the periodic re-send both sides wait
// forever on a camp neither will send again. Here A restarts a full hold-period before B.
//
// This also covers the SECOND half of the fix, which the re-send alone does not give you: B's camp
// reaches A, so A has both and STARTS — and a started peer has left PreMatchTick and will never
// re-send again, leaving B stranded forever on a camp it can no longer be told. So a started peer
// that sees the peer still repeating its camp answers by re-sending its own. Take that answer-back
// out and this test hangs B at MatchStarted()==false.
static void TestPostMatchRestartSkewStillStarts() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    ForcedDrawPair(A, B, Qa, Qb, 0x149B);
    RunToResult(A, B, Qa, Qb);

    // Only A's clock advances past the hold: A rebuilds, B is still holding its result.
    uint64_t Held = 0;
    while (Held < PostMatchHoldNs + OneTickNs) { A.Tick(OneTickNs); Held += OneTickNs; }
    CHECK(!A.MatchStarted() && A.ExecTick() == 0);   // A is in the new match
    CHECK(B.GetSim().Result != ResultOngoing);       // B is still on the win screen

    // A's player drops a camp immediately. Its first send reaches a B that has not restarted yet —
    // exactly the packet the old code lost.
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0)));
    for (int I = 0; I < 3; ++I) { A.Tick(OneTickNs); Deliver(Qa, B); }
    CHECK(!A.MatchStarted());                        // still waiting on B's camp, as it must

    // Now B's hold expires too, and its player drops a camp. The re-send closes the gap.
    Held = 0;
    while (Held < PostMatchHoldNs + OneTickNs) { B.Tick(OneTickNs); Deliver(Qb, A); Held += OneTickNs; }
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 20 && !(A.MatchStarted() && B.MatchStarted()); ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(A.MatchStarted() && B.MatchStarted());     // the whole point: neither is stranded
    for (int I = 0; I < 20; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
}

// Hazard 4 — a REPEATED camp must be idempotent. The re-sends keep arriving until the peer's own
// camp comes back, and every one of them lands on a receiver that is already PeerReady_. Buffered
// as input they would shift the peer's whole event stream by one tick per duplicate, which surfaces
// as a desync. Also asserts a duplicate does not place a second camp.
static void TestPreMatchDuplicateCampIsIdempotent() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x149C, 0, Enqueue, &Qa);
    B.Init(0x149C, 1, Enqueue, &Qb);

    // A readies; its camp reaches B. B is now PeerReady_ but has not readied itself.
    const InputEvent ACamp = InputEvent::Place(0, UnitMiner, CampTestX, CampTestY(0));
    A.QueueLocalEvent(ACamp);
    A.Tick(OneTickNs);
    Deliver(Qa, B);
    // Five more hold-periods worth of A ticking: every one of these is a re-send arriving at a
    // PeerReady_ B (five duplicates, more than any real restart skew would produce).
    for (int I = 0; I < 5; ++I) {
        for (int J = 0; J < 6; ++J) A.Tick(OneTickNs);   // 600ms > PreMatchCampResendNs
        Deliver(Qa, B);
    }
    CHECK(!B.MatchStarted());   // duplicates are not a readiness signal for B itself

    // B readies -> the match starts. If the duplicates had been buffered as input, B's view of A's
    // timeline is shifted and the anchor cross-check trips within ten ticks.
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, CampTestX, CampTestY(1)));
    for (int I = 0; I < 40; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
        CHECK(!A.Desynced() && !B.Desynced());
    }
    CHECK(A.MatchStarted() && B.MatchStarted());
    CHECK(A.ExecTick() > 10);                                 // past the first anchor
    CHECK(A.GetSim().StateHash() == B.GetSim().StateHash());
    // Exactly one camp per team: a duplicate never became a second placement.
    int Camps[2] = {0, 0};
    const Sim& S = B.GetSim();
    for (int32_t J = 0; J < S.Count; ++J)
        if (S.IsAlive(J) && S.IsBuilding(J) && S.Type[J] == UnitMiner) ++Camps[S.Team[J]];
    CHECK(Camps[0] == 1 && Camps[1] == 1);
}

int main() {
    TestEventBatchRoundTrip();
    TestEventBatchFuzz();
    TestLockstepReadyGate();
    TestLockstepStaysInSync();
    // Rollback Phase 1 scaffolding (Docs/Journal/2026-08-03)
    TestSnapshotRingRoundTrip();
    TestSnapshotRingEviction();
    TestSnapshotRingCapturesLoopbackStates();
    TestPredictPeerBatchIsEmpty();
    TestConfirmedTickTracksBothTimelines();
    // Rollback Phase 2 behaviour
    TestRollbackLocalInputHasNoSchedulingDelay();
    TestRollbackAdvancesUnderPeerLag();
    TestRollbackCorrectsMispredictionAndConverges();
    TestRollbackSpeculationCappedAtHorizon();
    TestSendOnTapProducesBeforeWallTick();
#if LUR_INTERNAL
    TestLockstepCvarSyncStaysIdentical();
    TestCvarSyncMatchStartMerge();
    TestCvarSyncRederivesInitState();
    TestCvarSyncArrivingBeforeInit();
    TestCvarSyncSurvivesResync();
    TestLateJoinerReceivesIncumbentCvars();   // #169
    TestMidMatchCvarSyncIsIgnored();          // #169
    TestBuildFingerprintGate();
    TestBuildFingerprintRefusesMatchStart();
    TestFingerprintSurvivesLaterInit();
    TestCvarEditSoakStaysHashIdentical();
    TestResyncReoffersFingerprintAndTunables();   // #169/#170
#endif
#if LUR_INTERNAL
    TestNoRecMatchIdxIsNotAReachableMatchIndex();  // #159
    TestMatchStartEdgePrecedesTickZero();          // #180
    TestMatchStartEdgeFiresOnEveryRestart();       // #180
    TestLinkedRecordingsMatchAcrossPeers();
    TestRecordingCvarsAreNameKeyedAndSurviveReorder();
#endif
    TestProducedCampLookAlikeIsNotDropped();     // #160
    TestPreMatchRejectsUnplaceableCamp();        // #167
    TestPreMatchAcceptsUnaffordableCampOnLegalGround();  // #167: spatial only, by design
    TestLostInputFrameIsDetectedAndLocated();    // #163
    TestDuplicateInputFrameIsNotAGap();          // #163
    TestTwoPeerDuplicateSoakStaysIdentical();    // #172/#159: long-duration host soak
    TestInputGapDoesNotSpendTheDesyncBudget();   // #167
    TestPreMatchStallIsReported();               // #163
    TestStarvedCeilingEndsTheMatch();                   // #162
    TestDesyncRecoversToACommonStateAndKeepsPlaying();  // #161
    TestDesyncRecoveryIsBoundedThenDraws();             // #161
    TestLostInputRecoversBeforeDiverging();             // #161 + #163
    TestDesyncIsNeverTerminal();
    TestLockstepExecuteCapBounded();
    TestLockstepReplayHashIdentical();
    TestLockstepDetectsDivergence();
    TestRollbackRidesThroughPeerBlipAndResumes();
    TestLockstepColdRejoinResync();
    TestResyncStallCannotWedgeSurvivor();
    TestResyncReoffersToBehindPeer();
    TestLockstepOverSessionLoopback();
    TestPostMatchHoldThenFreshMatch();          // #149
    TestPostMatchRestartSkewStillStarts();      // #149 hazard 3
    TestPreMatchDuplicateCampIsIdempotent();    // #149 hazard 4
#if LUR_INTERNAL
    TestCvarSyncOverSessionLoopback();
#endif

    if (GFailures == 0) std::printf("rps_net_tests: ALL PASS\n");
    else std::printf("rps_net_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
