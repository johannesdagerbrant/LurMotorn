// Host tests for the RPS lockstep netcode. Starts with the event codec: round-trip,
// the byte budget (a press/watermark is 1 byte before framing), and fuzz-safety
// (hostile bytes never crash the decoder). Chess's fuzz_tests as the pattern.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "Lur/Net/Session.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Sim/Random.h"
#include "Lur/Transport/Loopback.h"
#include "Rps/EventCodec.h"
#include "Rps/LockstepPeer.h"

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

// ---- Round-trip: every (delta, mask) survives encode -> decode ----
static void TestRoundTrip() {
    const uint32_t Deltas[] = {1, 2, 7, 14, 15, 16, 100, 9000, 1u << 20};
    for (uint32_t D : Deltas)
        for (uint8_t M = 0; M < 16; ++M) {
            BitWriter W;
            EncodeEvent(W, D, M);
            const std::vector<uint8_t>& Bytes = W.Finish();
            BitReader R(Bytes.data(), Bytes.size());
            uint32_t Dd = 0;
            uint8_t Mm = 0;
            CHECK(DecodeEvent(R, Dd, Mm));
            CHECK(Dd == D && Mm == M);
        }
}

// ---- Byte budget: a live-wire event (delta 1..14) is exactly ONE byte ----
static void TestByteBudget() {
    for (uint32_t D = 1; D < EventDeltaInline; ++D) {
        BitWriter W;
        EncodeEvent(W, D, 0xB);
        CHECK(W.Finish().size() == 1);  // 1 byte; framed (+type) = the 2-byte budget
    }
    // A watermark (mask 0, delta 1) is also one byte — the idle case is not special.
    BitWriter Wm;
    EncodeEvent(Wm, 1, 0x0);
    CHECK(Wm.Finish().size() == 1);
    // The escape (delta >= 15) costs the varint but stays small.
    BitWriter We;
    EncodeEvent(We, 9000, 0x5);
    CHECK(We.Finish().size() <= 4);
}

// ---- A stream of events (resync/recorder use): decode reconstructs absolute ticks ----
static void TestStreamAbsoluteTicks() {
    struct EV { uint32_t Delta; uint8_t Mask; };
    const EV In[] = {{1, 0x2}, {3, 0x4}, {1, 0x0}, {20, 0x8}, {1, 0x1}};
    BitWriter W;
    for (const EV& E : In) EncodeEvent(W, E.Delta, E.Mask);
    const std::vector<uint8_t>& Bytes = W.Finish();

    BitReader R(Bytes.data(), Bytes.size());
    uint32_t Tick = 0;  // events are delta-coded; accumulate to absolute ticks
    int I = 0;
    for (;;) {
        uint32_t D = 0;
        uint8_t M = 0;
        if (!DecodeEvent(R, D, M)) break;
        Tick += D;
        CHECK(I < 5 && M == In[I].Mask);
        ++I;
        if (I == 5) break;  // consumed all five (trailing padding bits may look like a 0,0 event)
    }
    CHECK(I == 5);
    CHECK(Tick == 1 + 3 + 1 + 20 + 1);
}

// ---- Fuzz: hostile bytes never crash; the decoder stays total ----
static void TestFuzzDecode() {
    SplitMix64 Rng(0xF0FF);
    for (int Iter = 0; Iter < 20000; ++Iter) {
        uint8_t Buf[8];
        const int N = 1 + static_cast<int>(Rng.NextBounded(8));
        for (int I = 0; I < N; ++I) Buf[I] = static_cast<uint8_t>(Rng.NextBounded(256));
        BitReader R(Buf, static_cast<size_t>(N));
        // Drain events until the reader runs dry — must always terminate, never trap.
        for (int Guard = 0; Guard < 64; ++Guard) {
            uint32_t D = 0;
            uint8_t M = 0;
            if (!DecodeEvent(R, D, M)) break;
        }
    }
    CHECK(true);  // reaching here without a crash/hang is the assertion
}

// Replay a recorded/reassembled (mask0, mask1) stream into a fresh sim -> final hash.
static uint64_t ReplayHash(uint64_t Seed, const std::vector<uint8_t>& M0,
                           const std::vector<uint8_t>& M1,
                           std::size_t MaxTicks = static_cast<std::size_t>(-1)) {
    static Sim S;
    S.Init(Seed);
    std::size_t N = M0.size() < M1.size() ? M0.size() : M1.size();
    if (N > MaxTicks) N = MaxTicks;
    for (std::size_t I = 0; I < N; ++I) S.Step(M0[I], M1[I]);
    return S.StateHash();
}

// ---- Resync: chunk a full input history, reassemble, replay hash-identical (§4) ----
static void TestResyncChunking() {
    constexpr int Ticks = 9000;  // ~15 min at 10 Hz — the worst-case bound
    std::vector<uint8_t> M0, M1;
    SplitMix64 Rng(0x1234);
    for (int T = 0; T < Ticks; ++T) {
        M0.push_back(static_cast<uint8_t>(Rng.NextBounded(16)));
        M1.push_back(static_cast<uint8_t>(Rng.NextBounded(16)));
    }

    const auto C0 = EncodeResyncChunks(0, M0);
    const auto C1 = EncodeResyncChunks(0, M1);
    // Byte budget: no chunk exceeds the framed cap; the whole history is a handful of them.
    for (const auto& C : C0) CHECK(C.size() <= MaxResyncChunkBytes);
    CHECK(C0.size() <= 20);  // 9000/500 ~ 18 chunks/stream

    // Reassemble (append in order, as reliable transport delivers) and check exactness.
    std::vector<uint8_t> D0, D1;
    uint32_t Ft = 0;
    for (const auto& C : C0) CHECK(DecodeResyncChunk(C.data(), C.size(), Ft, D0));
    for (const auto& C : C1) CHECK(DecodeResyncChunk(C.data(), C.size(), Ft, D1));
    CHECK(D0 == M0 && D1 == M1);

    // Smoke-check that the reassembled bytes actually DRIVE the sim identically to the
    // original. The reassembly is already byte-exact (D0==M0 above), and the replay law
    // (same inputs -> same hash over a full match) is covered by TestLockstepReplayHash-
    // Identical + rps_sim_tests — so a short prefix suffices here. Replaying the WHOLE
    // 9000-tick history twice added ~18k flock-heavy Steps that dominated the CI gate for
    // zero extra coverage; the 9000-tick CHUNKING/byte-budget checks above are the point of
    // this test and stay intact.
    constexpr std::size_t ReplaySmoke = 400;
    CHECK(ReplayHash(0xAA, D0, D1, ReplaySmoke) == ReplayHash(0xAA, M0, M1, ReplaySmoke));
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

// ---- Two peers, random inputs, stay bit-identical in lockstep with zero desyncs ----
static void TestLockstepStaysInSync() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x1234, 0, Enqueue, &Qa);
    B.Init(0x1234, 1, Enqueue, &Qb);

    SplitMix64 Rng(0x5151);
    for (int I = 0; I < 300; ++I) {
        A.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
        B.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
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

#if LUR_INTERNAL
// ---- #112: a gameplay-CVar override on ONE peer syncs to the other, applies at the same
// stamped tick, and keeps both bit-identical — AND actually changes the match. ----
static void TestLockstepCvarSyncStaysIdentical() {
    auto RunMatch = [](bool Tweak) {
        Outbox Qa, Qb;
        LockstepPeer A, B;
        A.Init(0xC0DE, 0, Enqueue, &Qa);
        B.Init(0xC0DE, 1, Enqueue, &Qb);
        SplitMix64 Rng(0x9);
        for (int I = 0; I < 200; ++I) {
            // At tick ~20, A shoves the goal-seek weight to 3.0. This consumes no RNG, so
            // the INPUT stream is identical to the untweaked run — only the CVar differs.
            if (Tweak && I == 20)
                A.SetGameplayCvar(CvIdWSeek, Fixed::FromInt(3).Raw, /*wallMs*/ 1000);
            A.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)) | 0x2);
            B.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)) | 0x4);
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
#endif

// ---- #90: Execute caps ticks per call so a catch-up burst can't starve input (ANR) ----
static void TestLockstepExecuteCapBounded() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x2468, 0, Enqueue, &Qa);
    B.Init(0x2468, 1, Enqueue, &Qb);

    // Pile up a big peer-input backlog on A WITHOUT letting it execute: A never ticks,
    // so WallTicks=0 keeps the ceiling shut while PeerMasks accumulates.
    const int N = 40;
    for (int I = 0; I < N; ++I) { B.SetLocalMask(1); B.Tick(OneTickNs); }  // B produces N inputs
    Deliver(Qb, A);
    CHECK(A.ExecTick() == 0);

    // One big local advance opens the ceiling to N at once (production caps at 64, so
    // WallTicks jumps to N in a single Tick). WITHOUT the cap Execute would drain all N
    // here — the ANR. WITH it, at most MaxExecTicksPerService this call.
    A.Tick(static_cast<uint64_t>(N) * OneTickNs);
    CHECK(A.ExecTick() <= MaxExecTicksPerService);  // the per-call cap held
    CHECK(A.ExecTick() > 0);                         // but it made progress

    // Backlog drains over subsequent calls; nothing is discarded.
    for (int I = 0; I < 100; ++I) A.Tick(OneTickNs);
    CHECK(A.ExecTick() >= static_cast<uint32_t>(N));  // drained past the whole backlog
    CHECK(!A.Desynced());
}

// ---- Flight-recorder replay: a recorded match replays to a hash-identical state ----
static void TestLockstepReplayHashIdentical() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x77, 0, Enqueue, &Qa);
    B.Init(0x77, 1, Enqueue, &Qb);
    A.SetRecording(true);  // record the executed stream

    SplitMix64 Rng(0xF00D);
    for (int I = 0; I < 200; ++I) {
        A.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
        B.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    for (int I = 0; I < 4; ++I) { A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A); }

    CHECK(A.RecordedTeam0().size() == A.ExecTick());  // recorded every executed tick
    // Feed the recording into a FRESH sim -> must land on the same state (the replay law).
    const uint64_t Replayed = ReplayHash(A.Seed(), A.RecordedTeam0(), A.RecordedTeam1());
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
    for (int I = 0; I < 12; ++I) {  // warm up in sync
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(!A.Desynced());
    // Corrupt A's state (simulate a lost input / a determinism bug on one peer).
    const_cast<Sim&>(A.GetSim()).Teams[0].Gold += 999;

    for (int I = 0; I < 15 && !A.Desynced(); ++I) {  // run to the next anchor
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    CHECK(A.Desynced());  // anchor hash mismatch caught within ~1 s
    CHECK(B.Desynced());
}

// ---- Starve one side: the other stalls at the ceiling, then resumes cleanly ----
static void TestLockstepCeilingStallAndResume() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x99, 0, Enqueue, &Qa);
    B.Init(0x99, 1, Enqueue, &Qb);

    for (int I = 0; I < 15; ++I) {  // warm up in sync
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);
        Deliver(Qb, A);
    }
    const uint32_t Before = A.ExecTick();

    // Blip: A stops receiving B's messages (Qb held), both keep ticking.
    for (int I = 0; I < 15; ++I) {
        A.Tick(OneTickNs);
        B.Tick(OneTickNs);
        Deliver(Qa, B);  // A -> B still flows
    }
    CHECK(A.Stalled());                              // A is waiting on the peer at the ceiling
    CHECK(A.ExecTick() <= Before + InputDelayTicks + 1);  // advanced only the delay slack
    CHECK(B.ExecTick() > A.ExecTick());              // B (which has A's input) pulled ahead

    // Resume: B's held backlog is delivered in order (reliable transport) — A sprints.
    Deliver(Qb, A);
    for (int I = 0; I < 6; ++I) {
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
static void RouteToPeer(Lur::Net::Session& S, LockstepPeer& Lp) {
    S.SetHandler(MsgInput,  [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(MsgInput, D, N); });
    S.SetHandler(MsgAnchor, [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(MsgAnchor, D, N); });
}
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

    SplitMix64 Rng(0x2468);
    for (int I = 0; I < 250; ++I) {
        A->S.Tick(OneTickNs);   // pump transports -> deliver queued datagrams to OnMessage
        B->S.Tick(OneTickNs);
        A->Lp.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
        B->Lp.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
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

    SplitMix64 Rng(0xBADCAB);
    for (int I = 0; I < 40; ++I) {  // play a while
        A.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
        B.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
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
        A.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
        B2.SetLocalMask(static_cast<uint8_t>(Rng.NextBounded(16)));
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

int main() {
    TestRoundTrip();
    TestByteBudget();
    TestStreamAbsoluteTicks();
    TestFuzzDecode();
    TestResyncChunking();
    TestLockstepStaysInSync();
#if LUR_INTERNAL
    TestLockstepCvarSyncStaysIdentical();
#endif
    TestLockstepExecuteCapBounded();
    TestLockstepReplayHashIdentical();
    TestLockstepDetectsDivergence();
    TestLockstepCeilingStallAndResume();
    TestLockstepColdRejoinResync();
    TestLockstepOverSessionLoopback();

    if (GFailures == 0) std::printf("rps_net_tests: ALL PASS\n");
    else std::printf("rps_net_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
