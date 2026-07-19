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
    const_cast<Sim&>(A.GetSim()).Teams[0].Wood += 999;

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

int main() {
    TestRoundTrip();
    TestByteBudget();
    TestStreamAbsoluteTicks();
    TestFuzzDecode();
    TestLockstepStaysInSync();
    TestLockstepDetectsDivergence();
    TestLockstepCeilingStallAndResume();
    TestLockstepOverSessionLoopback();

    if (GFailures == 0) std::printf("rps_net_tests: ALL PASS\n");
    else std::printf("rps_net_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
