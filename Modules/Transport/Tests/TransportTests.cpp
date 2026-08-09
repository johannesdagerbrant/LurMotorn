// Dependency-free unit tests for the BLE transport contract (Phase A): the role
// tie-break, the in-process loopback transport, and the EventInbox handoff. No game is
// linked: an engine test binary that needs a game to build is the module wall broken
// where nobody looks (the game-side move-over-transport test now lives in that game's suite).
// move round-tripping through the ITransport seam, which is issue #3's "the move
// codec round-trips real moves over the live link" proven in software, no radio.
// No framework: each CHECK records a failure; the process exits non-zero if any
// failed, which CTest reports.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "Lur/Save/DeviceId.h"   // #146: the device-id shape the role tie-break demands
#include "Lur/Transport/BleProtocol.h"
#include "Lur/Transport/EventInbox.h"
#include "Lur/Transport/Loopback.h"

#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"

using namespace Lur::Transport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// Two devices reaching the tie-break with swapped ids must pick OPPOSITE roles —
// exactly one peripheral — so they never both host the GATT server.
static void TestRoleTieBreakIsOpposite() {
    const std::string_view A = "AA:BB:CC:00:00:01";
    const std::string_view B = "AA:BB:CC:00:00:02";

    const EBleRole RoleOnA = DecideBleRole(/*Local*/ A, /*Peer*/ B);
    const EBleRole RoleOnB = DecideBleRole(/*Local*/ B, /*Peer*/ A);

    CHECK(RoleOnA != RoleOnB);
    CHECK(RoleOnA == EBleRole::Peripheral);  // smaller id hosts the peripheral
    CHECK(RoleOnB == EBleRole::Central);
}

// Pure function: identical inputs give identical output, and order of the two ids
// is the only thing that decides it (symmetry is total over the id space).
static void TestRoleTieBreakIsDeterministic() {
    CHECK(DecideBleRole("x", "y") == DecideBleRole("x", "y"));
    CHECK(DecideBleRole("device-9", "device-10") == EBleRole::Central);   // "9" > "1"
    CHECK(DecideBleRole("device-10", "device-9") == EBleRole::Peripheral);
}

// #146: the tie-break DEADLOCK BREAKER. A healthy pair never needs it — but on hardware both
// an Android and an iPhone once decided Peripheral, so nobody connected and the link never came
// up (a state retrying can't escape: the same comparison gives the same answer forever). After
// BleMaxPeripheralDefers fruitless defers the loser stops believing the tie-break and takes
// Central, which is the one role that was missing.
static void TestRoleBreakerEscapesBothPeripheral() {
    const std::string_view Small = "7ef0ddb8", Large = "87f01c50";  // the on-device pair (#146)
    // Healthy: the breaker changes NOTHING while the pair still disagrees productively.
    for (int Defers = 0; Defers <= BleMaxPeripheralDefers + 1; ++Defers)
        CHECK(DecideBleRoleBreaking(Large, Small, Defers) == EBleRole::Central);
    // The peripheral side holds its role while defers are still plausible...
    for (int Defers = 0; Defers < BleMaxPeripheralDefers; ++Defers)
        CHECK(DecideBleRoleBreaking(Small, Large, Defers) == EBleRole::Peripheral);
    // ...and takes Central once they're not, so the deadlock cannot persist.
    CHECK(DecideBleRoleBreaking(Small, Large, BleMaxPeripheralDefers) == EBleRole::Central);
    CHECK(DecideBleRoleBreaking(Small, Large, BleMaxPeripheralDefers + 5) == EBleRole::Central);
}

#if LUR_AGENT
// Runs only in the agent tree (`build.ps1 -Agent`), because that is the only build where a pin
// can exist at all — which is itself the point of #197's resolution.
// #146: a DELIBERATE pin is never broken — the rig pins Android=Peripheral against its
// central-only Windows peer, so force-taking Central there would break the rig, not fix it.
// (A stale pin is meant to be diagnosed from the log via IsBleRolePinned instead.)
static void TestRoleBreakerRespectsDevPin() {
    CHECK(!IsBleRolePinned());
    SetBleRoleOverride(EBleRole::Peripheral);
    CHECK(IsBleRolePinned());
    CHECK(DecideBleRoleBreaking("a", "b", BleMaxPeripheralDefers + 10) == EBleRole::Peripheral);
    ClearBleRoleOverride();
    CHECK(!IsBleRolePinned());
    // Pin cleared -> the breaker is live again.
    CHECK(DecideBleRoleBreaking("a", "b", BleMaxPeripheralDefers) == EBleRole::Central);
}
#endif

// #146: only a well-formed device id may settle a role. A failed GATT read leaves a stale or
// empty value behind, and a long read with a mishandled offset arrives truncated — deciding
// from any of those produces a role the peer cannot mirror, i.e. the deadlock.
static void TestDeviceIdValidation() {
    const std::string Good = "7ef0ddb8c41a4f0e9b2d5a3c8e7f1024";
    CHECK(Good.size() == Lur::Save::DeviceIdHexLen);
    CHECK(Lur::Save::IsValidDeviceId(Good));
    CHECK(!Lur::Save::IsValidDeviceId(""));                        // failed read -> nothing
    CHECK(!Lur::Save::IsValidDeviceId(Good.substr(0, 22)));        // default-MTU truncation
    CHECK(!Lur::Save::IsValidDeviceId(Good.substr(10)));           // long read, offset mishandled
    CHECK(!Lur::Save::IsValidDeviceId(Good + "0"));                // too long
    CHECK(!Lur::Save::IsValidDeviceId("7EF0DDB8C41A4F0E9B2D5A3C8E7F1024"));  // uppercase: not ours
    CHECK(!Lur::Save::IsValidDeviceId("7ef0ddb8c41a4f0e9b2d5a3c8e7f10zz"));  // non-hex
}

// ---- #83: a pairwise link must survive a third device in the room ----
// Matches are strictly 1:1, but the peripheral treated ANY CCCD subscription as "the canonical
// central" (`connectedCentral = device` / `_Subscriber = central`, unconditional) and delivered
// datagrams from ANY connected central into the engine. So a third phone could silently redirect a
// live match's notifications to itself and inject bytes into the lockstep stream — and the engine was
// never told, because onLinked no-ops on the `linked` guard. Present in all four transports.
//
// The rule is small enough to be obvious and was still wrong four times over, which is why it lives
// here as ONE tested policy called from each transport (the same reason DecideBleRole does).
static void TestPeerBindingRejectsAThirdDevice() {
    PeerBinding B;
    CHECK(!B.HasPeer());
    CHECK(B.AcceptData("AA:BB:CC:DD:EE:01"));   // unbound: anything may open a link

    // The first subscriber binds.
    CHECK(B.AcceptSubscriber("AA:BB:CC:DD:EE:01"));
    CHECK(B.HasPeer());
    CHECK(B.IsPeer("AA:BB:CC:DD:EE:01"));

    // A THIRD device subscribing mid-match must not become the notify target — that is the hijack:
    // every outgoing frame silently redirects and the real peer goes deaf inside a "healthy" link.
    CHECK(!B.AcceptSubscriber("FF:EE:DD:CC:BB:02"));
    CHECK(B.IsPeer("AA:BB:CC:DD:EE:01"));       // still bound to the original
    CHECK(!B.IsPeer("FF:EE:DD:CC:BB:02"));

    // ...nor may its bytes reach the engine, where they would land in the lockstep/move stream.
    CHECK(!B.AcceptData("FF:EE:DD:CC:BB:02"));
    CHECK(B.AcceptData("AA:BB:CC:DD:EE:01"));

    // The bound central re-subscribing is NOT an intruder (an MTU renegotiation or a CCCD rewrite
    // does this); rejecting it would break the real link to defend against a device that isn't there.
    CHECK(B.AcceptSubscriber("AA:BB:CC:DD:EE:01"));
    CHECK(B.IsPeer("AA:BB:CC:DD:EE:01"));

    // Only the BOUND peer's disconnect/unsubscribe may end the link. Treating a third device's
    // departure as link loss is the hijack in reverse: an outsider could drop a match by leaving.
    CHECK(!B.IsPeer("FF:EE:DD:CC:BB:02"));

    // On real link loss the binding opens up again — this is what keeps a deliberate
    // opponent-switch (#38) working: that flow operates at session level AFTER link loss, so the gate
    // must apply only WHILE linked. A binding that outlived the link would forbid changing opponents.
    B.Clear();
    CHECK(!B.HasPeer());
    CHECK(B.AcceptSubscriber("FF:EE:DD:CC:BB:02"));
    CHECK(B.IsPeer("FF:EE:DD:CC:BB:02"));

    // Degenerate ids must not accidentally bind or match: an empty address is a failed read, not a
    // peer, and it must never compare equal to the bound one.
    PeerBinding C;
    CHECK(!C.AcceptSubscriber(nullptr));
    CHECK(!C.AcceptSubscriber(""));
    CHECK(!C.HasPeer());
    CHECK(C.AcceptSubscriber("AA:01"));
    CHECK(!C.IsPeer(""));
    CHECK(!C.IsPeer(nullptr));
    CHECK(!C.AcceptData(""));

    // An over-long id is malformed (no platform produces one) and must be REFUSED, not stored
    // truncated: a truncated copy would compare equal to every id sharing that prefix, which is
    // binding the wrong device — the exact failure this class exists to prevent.
    PeerBinding D;
    const std::string Long(PeerBinding::MaxIdLen + 8, 'x');
    CHECK(!D.AcceptSubscriber(Long.c_str()));
    CHECK(!D.HasPeer());
    CHECK(!D.IsPeer(Long.c_str()));
    // The longest id that still FITS is fine, and a longer one does not match it.
    const std::string Fits(PeerBinding::MaxIdLen - 1, 'x');
    CHECK(D.AcceptSubscriber(Fits.c_str()));
    CHECK(D.IsPeer(Fits.c_str()));
    CHECK(!D.IsPeer(std::string(PeerBinding::MaxIdLen - 2, 'x').c_str()));  // a prefix is not the peer
}

// ---- #83, the other role: a device that linked as CENTRAL still ran an open GATT server ----
// The first fix bound the peer on a CCCD subscribe, which only ever happens on the PERIPHERAL path.
// So half the pair — whichever phone won the role tie-break — played the whole match with its binding
// UNBOUND and its server still published, and a third device that had scanned before the link formed
// could walk straight in: subscribe (nobody was bound, so it bound ITSELF), inject datagrams into the
// lockstep stream, and then end a healthy match simply by disconnecting, because IsPeer now named it.
//
// Close() is the answer rather than "bind the peer from the central side too", because a central's
// remote handle and the CBCentral/BluetoothDevice its own server reports are different namespaces —
// assuming they agree would be binding on an unguaranteed identity. A central has no legitimate
// server-side peer at all, so the exact statement is "serve nobody".
static void TestPeerBindingClosedWhenLinkedAsCentral() {
    PeerBinding B;

    // Pre-link, the server is open — that is the handshake, and closing it would stop links forming.
    CHECK(B.AcceptData("AA:BB:CC:DD:EE:01"));

    B.Close();                                    // the role tie-break made us CENTRAL
    CHECK(!B.HasPeer());

    // Nobody may subscribe: not a stranger, and not even a plausible-looking one. Before this, the
    // FIRST arrival became the binding, because "unbound" and "serves nobody" were the same state.
    CHECK(!B.AcceptSubscriber("FF:EE:DD:CC:BB:02"));
    CHECK(!B.HasPeer());
    CHECK(!B.AcceptData("FF:EE:DD:CC:BB:02"));

    // ...and crucially nobody is the peer, so no outsider's disconnect can end the live match. This
    // is the failure that would have read as a random mid-match link drop with nothing in the log.
    CHECK(!B.IsPeer("FF:EE:DD:CC:BB:02"));
    CHECK(!B.IsPeer("AA:BB:CC:DD:EE:01"));

    // The link drops: reopen, because the NEXT link may be one we serve as peripheral (the role can
    // flip — a fresh identity or a different opponent re-runs the tie-break). A Close() that outlived
    // the link would leave the device permanently unable to be a peripheral, which is worse than the
    // bug it fixes.
    B.Clear();
    CHECK(B.AcceptSubscriber("FF:EE:DD:CC:BB:02"));
    CHECK(B.HasPeer());
    CHECK(B.IsPeer("FF:EE:DD:CC:BB:02"));

    // Close() from a BOUND state (we were peripheral, then re-linked as central without an
    // intervening Clear) must drop the stale peer, not keep serving it.
    B.Close();
    CHECK(!B.IsPeer("FF:EE:DD:CC:BB:02"));
    CHECK(!B.AcceptData("FF:EE:DD:CC:BB:02"));
}

// One datagram Sent on A is delivered byte-for-byte to B's receiver, both ways.
static void TestLoopbackRoundtrip() {
    LoopbackTransport A, B;
    CHECK(!A.IsConnected());
    LoopbackTransport::Link(A, B);
    CHECK(A.IsConnected() && B.IsConnected());

    std::vector<uint8_t> GotOnB;
    B.SetReceiver([&](const uint8_t* D, std::size_t N) { GotOnB.assign(D, D + N); });

    const uint8_t Payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    A.Send(Payload, sizeof(Payload));
    CHECK(GotOnB.size() == 4);
    CHECK(GotOnB[0] == 0xDE && GotOnB[3] == 0xEF);

    std::vector<uint8_t> GotOnA;
    A.SetReceiver([&](const uint8_t* D, std::size_t N) { GotOnA.assign(D, D + N); });
    const uint8_t Reply[] = {0x01, 0x02};
    B.Send(Reply, sizeof(Reply));
    CHECK(GotOnA.size() == 2 && GotOnA[0] == 0x01 && GotOnA[1] == 0x02);
}

// The shared UUIDs are sane and distinct (the service vs the datagram channel).
static void TestProtocolConstants() {
    CHECK(BleServiceUuid.size() == 36);                 // 8-4-4-4-12 + 4 hyphens
    CHECK(BleDatagramCharacteristicUuid.size() == 36);
    CHECK(BleDeviceIdCharacteristicUuid.size() == 36);
    // Service + both characteristics are three distinct UUIDs.
    CHECK(BleServiceUuid != BleDatagramCharacteristicUuid);
    CHECK(BleServiceUuid != BleDeviceIdCharacteristicUuid);
    CHECK(BleDatagramCharacteristicUuid != BleDeviceIdCharacteristicUuid);
    CHECK(!BleAdvertisedName.empty());
}

// A recording sink that captures the order and payloads Drain() dispatches.
struct RecordingSink : EventInbox::Sink {
    std::vector<EventInbox::EKind> Kinds;
    std::vector<std::vector<uint8_t>> Datagrams;
    void OnConnected() override    { Kinds.push_back(EventInbox::EKind::Connected); }
    void OnDisconnected() override { Kinds.push_back(EventInbox::EKind::Disconnected); }
    void OnDatagram(const uint8_t* D, std::size_t N) override {
        Kinds.push_back(EventInbox::EKind::Datagram);
        Datagrams.emplace_back(D, D + N);
    }
};

// Events drain in FIFO order across kinds, with datagram payloads intact — connect/
// disconnect can't reorder around the datagrams between them (issue #40 ordering).
static void TestInboxFifoOrder() {
    EventInbox Inbox;
    const uint8_t A[] = {0x11, 0x22};
    const uint8_t B[] = {0x33};
    Inbox.PushConnected();
    Inbox.PushDatagram(A, sizeof(A));
    Inbox.PushDatagram(B, sizeof(B));
    Inbox.PushDisconnected();

    RecordingSink Sink;
    Inbox.Drain(Sink);
    CHECK(Sink.Kinds.size() == 4);
    CHECK(Sink.Kinds[0] == EventInbox::EKind::Connected);
    CHECK(Sink.Kinds[1] == EventInbox::EKind::Datagram);
    CHECK(Sink.Kinds[2] == EventInbox::EKind::Datagram);
    CHECK(Sink.Kinds[3] == EventInbox::EKind::Disconnected);
    CHECK(Sink.Datagrams.size() == 2);
    CHECK(Sink.Datagrams[0].size() == 2 && Sink.Datagrams[0][0] == 0x11 && Sink.Datagrams[0][1] == 0x22);
    CHECK(Sink.Datagrams[1].size() == 1 && Sink.Datagrams[1][0] == 0x33);
    CHECK(!Inbox.Overflowed());
}

// Overrunning the ring drops the OLDEST events (never corrupts), flags the overflow,
// and the survivors are the most-recent Capacity, still in order.
static void TestInboxOverflowDropsOldest() {
    EventInbox Inbox;
    const int N = 40;                 // Capacity is 32 -> 8 oldest dropped
    for (int i = 0; i < N; ++i) {
        const uint8_t Byte = static_cast<uint8_t>(i);
        Inbox.PushDatagram(&Byte, 1);
    }
    RecordingSink Sink;
    Inbox.Drain(Sink);
    CHECK(Inbox.Overflowed());
    CHECK(Sink.Datagrams.size() == 32);          // exactly Capacity survive
    CHECK(Sink.Datagrams.front()[0] == 8);       // oldest 8 (0..7) dropped
    CHECK(Sink.Datagrams.back()[0] == 39);       // newest kept
    // Survivors are contiguous + in order.
    for (std::size_t i = 0; i < Sink.Datagrams.size(); ++i)
        CHECK(Sink.Datagrams[i][0] == static_cast<uint8_t>(8 + i));
}

// Thread-safety: a producer thread Pushes while the engine thread Drains. Events are
// never torn/corrupt and per-producer FIFO holds — the drained tags are a strictly
// increasing subsequence of what was sent (some may drop under overflow), and if no
// overflow occurred, all arrive.
static void TestInboxThreadedHandoff() {
    EventInbox Inbox;
    constexpr int N = 5000;
    std::atomic<bool> Done{false};

    // Each datagram carries its send index as a little-endian 16-bit payload.
    std::thread Producer([&] {
        for (int i = 0; i < N; ++i) {
            const uint8_t Payload[2] = { static_cast<uint8_t>(i & 0xFF),
                                         static_cast<uint8_t>((i >> 8) & 0xFF) };
            Inbox.PushDatagram(Payload, sizeof(Payload));
        }
        Done.store(true, std::memory_order_release);
    });

    struct IdxSink : EventInbox::Sink {
        std::vector<int> Indices;
        void OnConnected() override {}
        void OnDisconnected() override {}
        void OnDatagram(const uint8_t* D, std::size_t N2) override {
            if (N2 == 2) Indices.push_back(D[0] | (D[1] << 8));
        }
    } Sink;

    while (!Done.load(std::memory_order_acquire)) Inbox.Drain(Sink);
    Producer.join();
    Inbox.Drain(Sink);  // final sweep after the producer finished

    // Strictly increasing subsequence of 0..N-1 (per-producer FIFO, no torn payloads).
    for (std::size_t i = 1; i < Sink.Indices.size(); ++i) CHECK(Sink.Indices[i] > Sink.Indices[i - 1]);
    for (int V : Sink.Indices) CHECK(V >= 0 && V < N);
    if (!Inbox.Overflowed()) CHECK(static_cast<int>(Sink.Indices.size()) == N);
}

int main() {
    TestRoleTieBreakIsOpposite();
    TestRoleTieBreakIsDeterministic();
    TestRoleBreakerEscapesBothPeripheral();
#if LUR_AGENT
    TestRoleBreakerRespectsDevPin();
#endif
    TestDeviceIdValidation();
    TestPeerBindingRejectsAThirdDevice();   // #83
    TestPeerBindingClosedWhenLinkedAsCentral();  // #83, the central-role half
    TestLoopbackRoundtrip();
    TestProtocolConstants();
    TestInboxFifoOrder();
    TestInboxOverflowDropsOldest();
    TestInboxThreadedHandoff();

    if (GFailures == 0) {
        std::printf("All transport tests passed.\n");
        return 0;
    }
    std::printf("%d transport test(s) failed.\n", GFailures);
    return 1;
}
