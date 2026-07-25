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
static void DriveInput(LockstepPeer& P, uint8_t Team, int TickIdx) {
    if (TickIdx == 3)
        P.QueueLocalEvent(InputEvent::Place(Team, UnitMiner, F(17), Team == 0 ? F(14) : F(226)));  // clear of the home base (#146)
    else if (TickIdx == 15)
        P.QueueLocalEvent(InputEvent::Queue(Team, Team == 0 ? 0 : 1, 5));
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

// #139: drive both peers through the pre-match placement handshake — each places its mining camp
// (its "ready"), the camps are exchanged, and the match starts from tick 0 with both camps in.
// Tests that don't otherwise place a camp call this so the clock actually starts.
static void PlaceCampsAndStart(LockstepPeer& A, LockstepPeer& B, Outbox& Qa, Outbox& Qb) {
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, F(17), F(14)));  // clear of the home base (#146)
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, F(17), F(226)));  // clear of the home base (#146)
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
    A.QueueLocalEvent(InputEvent::Place(0, UnitMiner, F(17), F(14)));  // clear of the home base (#146)
    for (int I = 0; I < 10; ++I) {
        A.Tick(OneTickNs); B.Tick(OneTickNs); Deliver(Qa, B); Deliver(Qb, A);
    }
    CHECK(!A.MatchStarted() && !B.MatchStarted());
    CHECK(A.ExecTick() == 0 && B.ExecTick() == 0);

    // B readies too -> both camps in -> the match starts and runs bit-identical.
    B.QueueLocalEvent(InputEvent::Place(1, UnitMiner, F(17), F(226)));  // clear of the home base (#146)
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
#endif

// ---- #90: Execute caps ticks per call so a catch-up burst can't starve input (ANR) ----
static void TestLockstepExecuteCapBounded() {
    Outbox Qa, Qb;
    LockstepPeer A, B;
    A.Init(0x2468, 0, Enqueue, &Qa);
    B.Init(0x2468, 1, Enqueue, &Qb);
    PlaceCampsAndStart(A, B, Qa, Qb);  // #139: match started; both still at WallTicks 0

    // Pile up a big peer-input backlog on A WITHOUT letting it execute: A never ticks,
    // so WallTicks=0 keeps the ceiling shut while PeerEvents accumulates.
    const int N = 40;
    for (int I = 0; I < N; ++I) B.Tick(OneTickNs);  // B produces N input frames (empty batches suffice)
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
    PlaceCampsAndStart(A, B, Qa, Qb);  // #139: start the match before the blip scenario

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
    S.SetHandler(MsgResyncChunk, [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(MsgResyncChunk, D, N); });
#if LUR_INTERNAL
    // The dev-only slots too — every main registers these, so the test composition must match or
    // it silently proves less than it looks like it does.
    S.SetHandler(MsgCvar,        [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(MsgCvar, D, N); });
    S.SetHandler(MsgCvarSync,    [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(MsgCvarSync, D, N); });
    S.SetHandler(MsgFingerprint, [&Lp](const uint8_t* D, std::size_t N) { Lp.OnMessage(MsgFingerprint, D, N); });
#endif
}

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

int main() {
    TestEventBatchRoundTrip();
    TestEventBatchFuzz();
    TestLockstepReadyGate();
    TestLockstepStaysInSync();
#if LUR_INTERNAL
    TestLockstepCvarSyncStaysIdentical();
    TestCvarSyncMatchStartMerge();
    TestCvarSyncRederivesInitState();
    TestCvarSyncArrivingBeforeInit();
    TestCvarSyncSurvivesResync();
    TestBuildFingerprintGate();
#endif
    TestLockstepExecuteCapBounded();
    TestLockstepReplayHashIdentical();
    TestLockstepDetectsDivergence();
    TestLockstepCeilingStallAndResume();
    TestLockstepColdRejoinResync();
    TestResyncStallCannotWedgeSurvivor();
    TestResyncReoffersToBehindPeer();
    TestLockstepOverSessionLoopback();
#if LUR_INTERNAL
    TestCvarSyncOverSessionLoopback();
#endif

    if (GFailures == 0) std::printf("rps_net_tests: ALL PASS\n");
    else std::printf("rps_net_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
