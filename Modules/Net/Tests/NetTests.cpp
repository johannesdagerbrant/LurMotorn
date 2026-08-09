// Dependency-free unit tests for Lur::Net::Session (issue #5): the Hello handshake,
// message framing, protocol-version refusal, keepalive/timeout, and the half-open link
// detection + radio-restart escalation. Nothing here needs a game — every assertion holds
// with no game in the repo at all, which is the point: an engine test binary that needs a
// game to build is the module wall broken where nobody looks. A game's own integration
// tests over the session live in that game's suite.
// No framework: each CHECK records a failure and the process exits non-zero if any
// failed, which CTest reports.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Lur/Net/Session.h"
#include "Lur/Transport/Loopback.h"

#include "Lur/Core/FlightRecorder.h"
#include "Lur/Core/Hash.h"

#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"

using namespace Lur::Net;
using Lur::Transport::LoopbackTransport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// A 32-char device id filled with one character (matches Lur::Save::DeviceIdHexLen).
static std::string Guid(char C) { return std::string(32, C); }

// One ~60 Hz frame in nanoseconds — Session::Tick is now real-time-denominated.
static constexpr uint64_t FrameNs = 16'666'667ull;

// Two sessions over a linked loopback pair, already connected before Start, must
// both become ready and each learns the OTHER's device id from the Hello.
static void TestHandshakeExchangesGuids() {
    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);

    Session SA, SB;
    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));

    CHECK(SA.IsReady());
    CHECK(SB.IsReady());
    CHECK(SA.GetPeerGuid() == Guid('b'));
    CHECK(SB.GetPeerGuid() == Guid('a'));
}

// The first Hello is dropped when it is sent before the link is up (IsConnected
// is false). Tick() must resend and complete the handshake once connected.
static void TestHandshakeResendsUntilConnected() {
    Session SA, SB;
    LoopbackTransport TA, TB;

    SA.Start(&TA, Guid('a'));  // not linked yet -> Hello skipped (not connected)
    SB.Start(&TB, Guid('b'));
    CHECK(!SA.IsReady());
    CHECK(!SB.IsReady());

    LoopbackTransport::Link(TA, TB);
    for (int i = 0; i < 4 && !(SA.IsReady() && SB.IsReady()); ++i) { SA.Tick(FrameNs); SB.Tick(FrameNs); }

    CHECK(SA.IsReady() && SB.IsReady());
    CHECK(SA.GetPeerGuid() == Guid('b') && SB.GetPeerGuid() == Guid('a'));
}

// Send() prepends the type byte; the far side's per-type handler receives the
// payload with that byte stripped.
static void TestMessageFramingStripsType() {
    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));

    std::vector<uint8_t> Got;
    SB.SetHandler(EMsgType::Game0, [&](const uint8_t* D, std::size_t N) { Got.assign(D, D + N); });

    const uint8_t Payload[] = {0xAB, 0xCD};
    SA.Send(EMsgType::Game0, Payload, sizeof(Payload));
    CHECK(Got.size() == 2);
    CHECK(Got.size() == 2 && Got[0] == 0xAB && Got[1] == 0xCD);
}

// EVERY game slot must actually be deliverable. The handler table is indexed by the raw enum
// value at BOTH registration and dispatch, and an out-of-range slot is dropped at both ends in
// silence — so a bound that doesn't cover the enum makes a whole message type a no-op that still
// LOGS as received. That shipped: the bound stopped at 8 while Game3..Game5 live at 8..10, so the
// RTS's gameplay-CVar sync and its build-fingerprint gate were silently dead on the wire (#147).
// Only Game0 was ever tested here, which is exactly why nothing caught it. This walks the lot.
static void TestEveryGameSlotDispatches() {
    const EMsgType Slots[] = {EMsgType::Game0, EMsgType::Game1, EMsgType::Game2,
                              EMsgType::Game3, EMsgType::Game4, EMsgType::Game5};
    for (const EMsgType Slot : Slots) {
        LoopbackTransport TA, TB;
        LoopbackTransport::Link(TA, TB);
        Session SA, SB;
        SA.Start(&TA, Guid('a'));
        SB.Start(&TB, Guid('b'));

        int Calls = 0;
        std::vector<uint8_t> Got;
        SB.SetHandler(Slot, [&](const uint8_t* D, std::size_t N) { ++Calls; Got.assign(D, D + N); });
        const uint8_t Payload[] = {0x5A, 0xA5, 0x11};
        SA.Send(Slot, Payload, sizeof(Payload));
        CHECK(Calls == 1);  // registered AND dispatched — a dropped slot leaves this at 0
        CHECK(Got.size() == 3);
        CHECK(Got.size() == 3 && Got[0] == 0x5A && Got[2] == 0x11);
    }
}

// A Hello carrying a different ProtocolVersion must NOT complete the handshake —
// two app versions refuse each other rather than risk mis-decoding a game.
static void TestVersionMismatchRefused() {
    LoopbackTransport T, Peer;
    LoopbackTransport::Link(T, Peer);
    Session S;
    S.Start(&T, Guid('a'));

    uint8_t Hello[1 + 1 + 32 + 1] = {};                    // type + version + guid + ready
    Hello[0] = static_cast<uint8_t>(EMsgType::Hello);
    Hello[1] = static_cast<uint8_t>(ProtocolVersion + 1);  // wrong version
    for (int i = 0; i < 32; ++i) Hello[2 + i] = 'c';       // some guid
    Hello[34] = 1;                                          // ready flag
    Peer.Send(Hello, sizeof(Hello));
    CHECK(!S.IsReady());
}

// A transport that reports "connected" but delivers nothing on its own, so tests can
// drive Session liveness by hand: Deliver() feeds inbound datagrams, and ResetLink()
// (what a real backend does on the net keepalive timeout) is observable.
struct SilentTransport : Lur::Transport::ITransport {
    bool     Connected  = true;
    int      ResetCount = 0;
    Receiver Rx;
    void Send(const uint8_t*, std::size_t) override {}
    void SetReceiver(Receiver R) override { Rx = std::move(R); }
    bool IsConnected() const override { return Connected; }
    void ResetLink() override { ++ResetCount; Connected = false; }  // mimic backend teardown
    void Deliver(const uint8_t* D, std::size_t N) { if (Rx) Rx(D, N); }
};

// Build a valid peer Hello datagram (type + version + guid + ready), used to push a
// Session to Ready without a live peer.
static void MakeHello(uint8_t (&H)[35], char GuidChar, bool Ready) {
    for (auto& B : H) B = 0;
    H[0]  = static_cast<uint8_t>(EMsgType::Hello);
    H[1]  = ProtocolVersion;
    for (int i = 0; i < 32; ++i) H[2 + i] = static_cast<uint8_t>(GuidChar);
    H[34] = Ready ? 1 : 0;
}

// After going Ready, a Session whose peer falls silent must time out and ask the
// transport to reset the link (the iOS-peripheral silent-drop case).
static void TestKeepaliveTimeoutResetsLink() {
    SilentTransport T;
    Session S;
    S.Start(&T, Guid('a'));
    uint8_t H[35];
    MakeHello(H, 'b', /*ready*/ true);  // a valid peer Hello -> we become ready
    T.Deliver(H, sizeof(H));
    CHECK(S.IsReady());

    // No inbound traffic: ~6.7s of frames (well past the ~5s LinkTimeoutNs) -> dead.
    for (int i = 0; i < 400; ++i) S.Tick(FrameNs);
    CHECK(T.ResetCount == 1);  // fired once (ResetLink drops Connected, so it can't re-fire)
}

// A transport that RECONNECTS-but-stays-silent after a reset — the #163 half-open shape. The real
// backend re-establishes the GATT link every cycle (so IsConnected stays true) but the peer's notify
// path is wedged, so no inbound ever arrives. Unlike SilentTransport (which goes down on ResetLink
// and so can only fire once), this keeps the link "up", so the Session keeps timing out and the
// consecutive-silent-reset count climbs — exactly the cycle seen on hardware (80 resets in a row).
struct ReconnectingSilentTransport : Lur::Transport::ITransport {
    int      ResetCount   = 0;
    int      RestartCount = 0;  // #182: hard RestartRadio()s the Session escalated to
    Receiver Rx;
    void Send(const uint8_t*, std::size_t) override {}
    void SetReceiver(Receiver R) override { Rx = std::move(R); }
    bool IsConnected() const override { return true; }
    void ResetLink() override { ++ResetCount; }  // reconnects instantly, still silent
    void RestartRadio() override { ++RestartCount; }  // #182: the harder reset; still silent (a wedge
    // a radio restart can't clear is the worst case the banner exists for)
    void Deliver(const uint8_t* D, std::size_t N) { if (Rx) Rx(D, N); }
};

// #163: a PERSISTENT half-open (connected + silent, cycle after cycle) must be recognised as such —
// distinct from a transient blip — and must stop churning the radio once it is. On hardware this
// cycled ~80 times every ~6s, a soft reset each time, clearing nothing (only a reboot did) and
// reading to the player as a frozen app. The Session now names the state and backs the resets off.
static void TestHalfOpenLinkIsDetectedAndBacksOff() {
    ReconnectingSilentTransport T;
    Session S;
    S.Start(&T, Guid('a'));
    uint8_t H[35]; MakeHello(H, 'b', /*ready*/ true); T.Deliver(H, sizeof(H));
    CHECK(S.IsReady());
    CHECK(!S.IsLinkHalfOpen());

    // Silence at the aggressive cadence: resets at ~5/10/15s trip the half-open verdict.
    for (int i = 0; i < 1200 && !S.IsLinkHalfOpen(); ++i) S.Tick(FrameNs);
    CHECK(S.IsLinkHalfOpen());
    CHECK(S.ConsecutiveSilentResets() >= 3);   // threshold reached (HalfOpenResetThreshold)
    const int ResetsAtVerdict = T.ResetCount;

    // Back-off: a further ~15s (< HalfOpenResetNs ~20s) produces NO new reset — the churn has stopped.
    for (int i = 0; i < 900; ++i) S.Tick(FrameNs);
    CHECK(T.ResetCount == ResetsAtVerdict);

    // ...but it recovers on EVIDENCE, not a timer: one inbound keepalive (the notify path working
    // again) clears the verdict and the count at once.
    const uint8_t KA[2] = { static_cast<uint8_t>(EMsgType::Keepalive), 0 };
    T.Deliver(KA, sizeof(KA));
    CHECK(!S.IsLinkHalfOpen());
    CHECK(S.ConsecutiveSilentResets() == 0);
}

// #182: once the link is confirmed HALF-OPEN, a soft ResetLink provably can't clear the wedged BLE
// STACK (80 of them cleared nothing on hardware), so the Session escalates to the harder per-OS
// ITransport::RestartRadio — but a BOUNDED number of times, so the recovery can't itself become the
// churn that degrades the radio (#163's lesson). Past the cap it stops touching the radio entirely and
// leaves the LINK STALLED banner to drive the guaranteed human fix. This is the host-testable half of
// #182 (the real radio teardown is not host-reproducible; the Session-side bounding is exactly this).
static void TestHalfOpenEscalatesToRadioRestartThenStops() {
    ReconnectingSilentTransport T;
    Session S;
    S.Start(&T, Guid('a'));
    uint8_t H[35]; MakeHello(H, 'b', /*ready*/ true); T.Deliver(H, sizeof(H));
    CHECK(S.IsReady());

    // Drive to the half-open verdict. The very cycle that trips it ALSO fires the first hard restart:
    // escalation is immediate once we conclude the stack is wedged, not one back-off cycle later.
    for (int i = 0; i < 1200 && !S.IsLinkHalfOpen(); ++i) S.Tick(FrameNs);
    CHECK(S.IsLinkHalfOpen());
    CHECK(T.RestartCount == 1);
    CHECK(S.RadioRestartsAttempted() == 1);

    // Hold the link connected-but-silent well past several ~20s back-off windows. The restarts climb
    // to the cap (MaxRadioRestarts == 3) and then STOP — a fourth would be exactly the runaway churn
    // the bound exists to prevent.
    for (int i = 0; i < 4000; ++i) S.Tick(FrameNs);
    CHECK(T.RestartCount == 3);
    CHECK(S.RadioRestartsAttempted() == 3);

    // Another ~50s (>2 further windows) proves the cap truly holds, not merely lags.
    for (int i = 0; i < 3000; ++i) S.Tick(FrameNs);
    CHECK(T.RestartCount == 3);

    // Recovery on EVIDENCE resets the whole episode: a fresh wedge later gets a fresh restart budget.
    const uint8_t KA[2] = { static_cast<uint8_t>(EMsgType::Keepalive), 0 };
    T.Deliver(KA, sizeof(KA));
    CHECK(!S.IsLinkHalfOpen());
    CHECK(S.RadioRestartsAttempted() == 0);
}

// Steady peer traffic (keepalives) keeps the link alive — no false timeout.
static void TestKeepaliveKeepsLinkAlive() {
    SilentTransport T;
    Session S;
    S.Start(&T, Guid('a'));
    uint8_t H[35];
    MakeHello(H, 'b', true);
    T.Deliver(H, sizeof(H));
    CHECK(S.IsReady());

    const uint8_t KA[1] = { static_cast<uint8_t>(EMsgType::Keepalive) };
    for (int i = 0; i < 400; ++i) {
        S.Tick(FrameNs);
        if (i % 50 == 0) T.Deliver(KA, sizeof(KA));  // peer alive (~every 0.83s), within timeout
    }
    CHECK(T.ResetCount == 0);
}

// EVERY datagram is framed: byte 0 is the type, and a 1-byte datagram is that type with
// an EMPTY payload — not a special case. The session used to carry a GAME-shaped rule
// ("a bare 1-byte datagram is always a move") which swallowed any 1-byte datagram into a
// move handler before the type was ever read, so a framed message with an empty payload
// was silently unreachable for every game. Nothing about a move belongs in the engine's
// dispatch — a 1-byte move is one game's encoding, not a property of datagrams.
static void TestOneByteDatagramDispatchesByType() {
    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));

    int Game0Calls = 0, SyncCalls = 0;
    std::size_t LastSize = 99;
    SB.SetHandler(EMsgType::Game0, [&](const uint8_t*, std::size_t N) { ++Game0Calls; LastSize = N; });
    SB.SetHandler(EMsgType::Sync,  [&](const uint8_t*, std::size_t) { ++SyncCalls; });

    CHECK(SA.Send(EMsgType::Game0, nullptr, 0));   // 1 byte on the wire: just the type
    CHECK(Game0Calls == 1);
    CHECK(LastSize == 0);
    CHECK(SyncCalls == 0);

    // And a 1-byte payload still lands framed, with the payload intact.
    const uint8_t One[1] = {0x2A};
    CHECK(SA.Send(EMsgType::Sync, One, 1));
    CHECK(SyncCalls == 1);
    CHECK(Game0Calls == 1);
}

// The other half of the fix: a payload beyond the datagram bound must fail LOUDLY —
// Send() returns false and delivers nothing — never a silent drop or a truncated wire.
static void TestOversizedFramedSendRefused() {
    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));

    int SyncCalls = 0;
    SB.SetHandler(EMsgType::Sync, [&](const uint8_t*, std::size_t) { ++SyncCalls; });

    std::vector<uint8_t> Huge(600, 0xEE);  // > MaxFramedPayload (512)
    CHECK(!SA.Send(EMsgType::Sync, Huge.data(), Huge.size()));
    CHECK(SyncCalls == 0);

    std::vector<uint8_t> Ok(500, 0x11);    // < MaxFramedPayload -> accepted + delivered
    CHECK(SA.Send(EMsgType::Sync, Ok.data(), Ok.size()));
    CHECK(SyncCalls == 1);
}

// --- issue #71: the resync gate ------------------------------------------------------
// A live move is a bare 1-byte INDEX into the side-to-move's legal list; it only means
// anything against the exact board the sender encoded it on. If a peer applies a move
// before the link-time Sync has reconciled both boards, the index maps onto a stale
// board -> a DIFFERENT move -> permanent divergence -> the cross-peer deadlock (#71).
// Session now holds a resync gate (IsAwaitingResync) from (re)link until the peer's
// Sync arrives, and the game refuses to make/apply moves while it is set.

// The gate arms the moment we go ready and lifts when the peer's Sync arrives.
static void TestResyncGateHoldsThenLiftsOnSync() {
    SilentTransport T; Session S;
    S.Start(&T, Guid('a'));
    uint8_t H[35]; MakeHello(H, 'b', /*ready*/ true);
    T.Deliver(H, sizeof(H));
    CHECK(S.IsReady());
    CHECK(S.IsAwaitingResync());                 // armed at link
    const uint8_t Sync[2] = { static_cast<uint8_t>(EMsgType::Sync), 0x00 };
    T.Deliver(Sync, sizeof(Sync));
    CHECK(!S.IsAwaitingResync());                // lifted by the peer's Sync
}

// Fallback: if the peer's Sync never arrives, the gate lifts after ~3s so a missing
// Sync can't wedge the game forever.
static void TestResyncGateTimeoutFallback() {
    SilentTransport T; Session S;
    S.Start(&T, Guid('a'));
    uint8_t H[35]; MakeHello(H, 'b', true);
    T.Deliver(H, sizeof(H));
    CHECK(S.IsAwaitingResync());
    const uint8_t KA[2] = { static_cast<uint8_t>(EMsgType::Keepalive), 0 };  // keep link alive
    for (int i = 0; i < 120; ++i) { S.Tick(FrameNs); if (i % 20 == 0) T.Deliver(KA, sizeof(KA)); }  // ~2s
    CHECK(S.IsAwaitingResync());                 // still gated under the fallback window
    for (int i = 0; i < 120; ++i) { S.Tick(FrameNs); if (i % 20 == 0) T.Deliver(KA, sizeof(KA)); }  // ~4s total
    CHECK(!S.IsAwaitingResync());                // fallback lifted the gate
}

// --- issue #72: mid-game desync detection + resync recovery --------------------------

// A keepalive carries the sender's state hash; a mismatch means the boards diverged on a
// live link (a lost move) -> the receiver requests a resync (gate + re-send state).
static void TestKeepaliveHashMismatchTriggersResync() {
    SilentTransport T; Session S;
    uint64_t MyHash = 0xA1A1A1A1ull;
    int ResyncFires = 0;
    S.SetStateHashFn([&] { return MyHash; });
    S.SetResyncHandler([&] { ++ResyncFires; });
    S.Start(&T, Guid('a'));
    uint8_t H[35]; MakeHello(H, 'b', true); T.Deliver(H, sizeof(H));
    CHECK(S.IsReady());
    const uint8_t Sync[2] = { static_cast<uint8_t>(EMsgType::Sync), 0 };
    T.Deliver(Sync, sizeof(Sync));               // clear the link-time gate first
    CHECK(!S.IsAwaitingResync());
    ResyncFires = 0;

    // Peer keepalive with a DIFFERENT hash -> mismatch -> resync requested + gated.
    auto DeliverKA = [&](uint64_t Hash) {
        uint8_t KA[9]; KA[0] = static_cast<uint8_t>(EMsgType::Keepalive);
        for (int i = 0; i < 8; ++i) KA[1 + i] = static_cast<uint8_t>(Hash >> (8 * i));
        T.Deliver(KA, sizeof(KA));
    };
    DeliverKA(0xB2B2B2B2ull);
    CHECK(ResyncFires == 0);                      // one mismatch could be an in-flight move
    CHECK(!S.IsAwaitingResync());
    DeliverKA(0xB2B2B2B2ull);                     // SAME divergent hash again -> peer is stuck
    CHECK(ResyncFires == 1);
    CHECK(S.IsAwaitingResync());

    // Reconcile (a Sync clears the gate); a MATCHING keepalive must NOT re-trigger.
    T.Deliver(Sync, sizeof(Sync));
    CHECK(!S.IsAwaitingResync());
    DeliverKA(MyHash); DeliverKA(MyHash);
    CHECK(ResyncFires == 1);                      // no spurious resync when hashes agree

    // A transient (in-flight) mismatch that changes each keepalive must NOT trigger.
    DeliverKA(0xC3C3C3C3ull); DeliverKA(0xD4D4D4D4ull);
    CHECK(ResyncFires == 1);                      // different hashes each time = normal play
}

// Every ELinkState has a NAME, and the names are distinct. Naming the enum is Net's job,
// not the HUD's: the debug overlay used to include Session.h purely to run this switch,
// which is what coupled presentation to the net session type (lur_hud -> lur_net). Walk
// EVERY slot — a slot added without a name would otherwise ship as "?" and read as a bug
// in the link, not in the label.
static void TestLinkStateNames() {
    const ELinkState All[] = {ELinkState::Searching, ELinkState::Handshaking,
                              ELinkState::Linked,    ELinkState::Disconnected,
                              ELinkState::VersionMismatch};
    for (ELinkState S : All) {
        const char* Name = LinkStateName(S);
        CHECK(Name != nullptr);
        CHECK(Name[0] != 0);
        CHECK(std::strcmp(Name, "?") != 0);      // no slot falls through to the default
    }
    for (std::size_t i = 0; i < sizeof(All) / sizeof(All[0]); ++i)
        for (std::size_t j = i + 1; j < sizeof(All) / sizeof(All[0]); ++j)
            CHECK(std::strcmp(LinkStateName(All[i]), LinkStateName(All[j])) != 0);

    CHECK(std::strcmp(LinkStateName(ELinkState::Linked), "linked") == 0);
}

int main() {
    TestLinkStateNames();
    TestHandshakeExchangesGuids();
    TestHandshakeResendsUntilConnected();
    TestMessageFramingStripsType();
    TestEveryGameSlotDispatches();
    TestVersionMismatchRefused();
    TestOneByteDatagramDispatchesByType();
    TestKeepaliveTimeoutResetsLink();
    TestHalfOpenLinkIsDetectedAndBacksOff();   // #163
    TestHalfOpenEscalatesToRadioRestartThenStops();  // #182
    TestKeepaliveKeepsLinkAlive();
    TestOversizedFramedSendRefused();
    TestResyncGateHoldsThenLiftsOnSync();
    TestResyncGateTimeoutFallback();
    TestKeepaliveHashMismatchTriggersResync();

    if (GFailures == 0) {
        std::printf("All net tests passed.\n");
        return 0;
    }
    std::printf("%d net test(s) failed.\n", GFailures);
    return 1;
}
