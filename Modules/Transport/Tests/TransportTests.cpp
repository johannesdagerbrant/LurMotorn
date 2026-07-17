// Dependency-free unit tests for the BLE transport contract (Phase A): the role
// tie-break, the in-process loopback transport, and — the payoff — a real chess
// move round-tripping through the ITransport seam, which is issue #3's "the move
// codec round-trips real moves over the live link" proven in software, no radio.
// No framework: each CHECK records a failure; the process exits non-zero if any
// failed, which CTest reports.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "Lur/Transport/BleProtocol.h"
#include "Lur/Transport/EventInbox.h"
#include "Lur/Transport/Loopback.h"

#include "Chess/Board.h"
#include "Chess/MoveCodec.h"
#include "Chess/Types.h"
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

// THE integration test: encode a real chess move, ship the bytes over the
// transport, and have the far side decode it back to the identical move — using
// its own legal-move list regenerated from the same position. Squares/flags never
// travel; only the move's index does.
static void TestMoveRoundtripsOverTransport() {
    // Sender side: pick e2e4 from the start position.
    const Chess::Board SenderBoard = Chess::Board::StartPosition();
    Chess::MoveList SenderLegal;
    Chess::GenerateLegalMoves(SenderBoard, SenderLegal);

    constexpr Chess::Square E2 = 12;  // rank-major: rank 2 (row 1) * 8 + file e (4)
    constexpr Chess::Square E4 = 28;  // rank 4 (row 3) * 8 + file e (4)
    const Chess::Move* Chosen = nullptr;
    for (int i = 0; i < SenderLegal.Count; ++i) {
        if (SenderLegal.Moves[i].From == E2 && SenderLegal.Moves[i].To == E4) {
            Chosen = &SenderLegal.Moves[i];
        }
    }
    CHECK(Chosen != nullptr);
    if (Chosen == nullptr) return;

    Lur::Serialization::BitWriter W;
    Chess::EncodeMove(*Chosen, SenderLegal, W);
    const std::vector<uint8_t> Wire = W.Finish();
    CHECK(Wire.size() == 1);  // 20 legal moves -> 5 bits -> 1 byte

    // Transport the bytes.
    LoopbackTransport Sender, Receiver;
    LoopbackTransport::Link(Sender, Receiver);

    Chess::Move Decoded;
    bool GotMove = false;
    Receiver.SetReceiver([&](const uint8_t* D, std::size_t N) {
        // Receiver reconstructs the SAME legal list from its own board copy.
        const Chess::Board ReceiverBoard = Chess::Board::StartPosition();
        Chess::MoveList ReceiverLegal;
        Chess::GenerateLegalMoves(ReceiverBoard, ReceiverLegal);

        Lur::Serialization::BitReader R(D, N);
        Decoded = Chess::DecodeMove(R, ReceiverLegal);
        GotMove = R.IsOk();
    });

    Sender.Send(Wire.data(), Wire.size());

    CHECK(GotMove);
    CHECK(Decoded == *Chosen);   // same From/To/Promo/Flags, reconstructed locally
    CHECK(Decoded.From == E2 && Decoded.To == E4);
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
    TestLoopbackRoundtrip();
    TestMoveRoundtripsOverTransport();
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
