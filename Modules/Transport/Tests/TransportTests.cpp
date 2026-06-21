// Dependency-free unit tests for the BLE transport contract (Phase A): the role
// tie-break, the in-process loopback transport, and — the payoff — a real chess
// move round-tripping through the ITransport seam, which is issue #3's "the move
// codec round-trips real moves over the live link" proven in software, no radio.
// No framework: each CHECK records a failure; the process exits non-zero if any
// failed, which CTest reports.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Transport/BleProtocol.h"
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
    CHECK(BleNonceCharacteristicUuid.size() == 36);
    // Service + both characteristics are three distinct UUIDs.
    CHECK(BleServiceUuid != BleDatagramCharacteristicUuid);
    CHECK(BleServiceUuid != BleNonceCharacteristicUuid);
    CHECK(BleDatagramCharacteristicUuid != BleNonceCharacteristicUuid);
    CHECK(!BleAdvertisedName.empty());
}

int main() {
    TestRoleTieBreakIsOpposite();
    TestRoleTieBreakIsDeterministic();
    TestLoopbackRoundtrip();
    TestMoveRoundtripsOverTransport();
    TestProtocolConstants();

    if (GFailures == 0) {
        std::printf("All transport tests passed.\n");
        return 0;
    }
    std::printf("%d transport test(s) failed.\n", GFailures);
    return 1;
}
