// Dependency-free unit tests for Lur::Net::Session (issue #5): the Hello handshake
// + deterministic seat assignment, message framing, protocol-version refusal, and
// — the payoff — a real chess move crossing TWO sessions and landing identically
// on the far board, i.e. the move codec wired over the session/transport seam that
// the live BLE link presents. No framework: each CHECK records a failure and the
// process exits non-zero if any failed, which CTest reports.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Net/Session.h"
#include "Lur/Transport/Loopback.h"

#include "Chess/Board.h"
#include "Chess/MoveCodec.h"
#include "Chess/Types.h"
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
    for (int i = 0; i < 4 && !(SA.IsReady() && SB.IsReady()); ++i) { SA.Tick(); SB.Tick(); }

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
    SB.SetHandler(EMsgType::Move, [&](const uint8_t* D, std::size_t N) { Got.assign(D, D + N); });

    const uint8_t Payload[] = {0xAB, 0xCD};
    SA.Send(EMsgType::Move, Payload, sizeof(Payload));
    CHECK(Got.size() == 2);
    CHECK(Got.size() == 2 && Got[0] == 0xAB && Got[1] == 0xCD);
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

    // No inbound traffic: well past LinkTimeoutTicks (~300) the link is declared dead.
    for (int i = 0; i < 400; ++i) S.Tick();
    CHECK(T.ResetCount == 1);  // fired once (ResetLink drops Connected, so it can't re-fire)
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
        S.Tick();
        if (i % 50 == 0) T.Deliver(KA, sizeof(KA));  // peer alive, well within the timeout
    }
    CHECK(T.ResetCount == 0);
}

static bool SamePosition(const Chess::Board& A, const Chess::Board& B) {
    if (A.SideToMove != B.SideToMove) return false;
    if (A.Castling != B.Castling || A.EnPassant != B.EnPassant) return false;
    for (int C = 0; C < 2; ++C)
        for (int T = 0; T < 6; ++T)
            if (A.Pieces[C][T] != B.Pieces[C][T]) return false;
    return true;
}

// THE integration test: White plays e2e4; the move's INDEX crosses the session as a
// framed Move message; Black decodes it against its own regenerated legal list and
// applies it — landing on the identical position. Squares never travel, only the
// index. (Colour is a game-layer concern now; here White is just the side that moves
// first.)
static void TestChessMoveAcrossSessions() {
    LoopbackTransport TWhite, TBlack;
    LoopbackTransport::Link(TWhite, TBlack);
    Session SWhite, SBlack;
    SWhite.Start(&TWhite, Guid('w'));
    SBlack.Start(&TBlack, Guid('b'));
    CHECK(SWhite.IsReady() && SBlack.IsReady());

    Chess::Board BoardWhite = Chess::Board::StartPosition();
    Chess::Board BoardBlack = Chess::Board::StartPosition();

    SBlack.SetMoveHandler([&](const uint8_t* D, std::size_t N) {
        Chess::MoveList Legal;
        Chess::GenerateLegalMoves(BoardBlack, Legal);
        Lur::Serialization::BitReader R(D, N);
        const Chess::Move Mv = Chess::DecodeMove(R, Legal);
        if (R.IsOk() && !(Mv == Chess::Move{})) BoardBlack.MakeMove(Mv);
    });

    // White picks e2e4 and ships only its index.
    constexpr Chess::Square E2 = 12, E4 = 28;
    Chess::MoveList Legal;
    Chess::GenerateLegalMoves(BoardWhite, Legal);
    const Chess::Move* Chosen = nullptr;
    for (int i = 0; i < Legal.Count; ++i)
        if (Legal.Moves[i].From == E2 && Legal.Moves[i].To == E4) Chosen = &Legal.Moves[i];
    CHECK(Chosen != nullptr);
    if (Chosen == nullptr) return;

    Lur::Serialization::BitWriter W;
    Chess::EncodeMove(*Chosen, Legal, W);
    const std::vector<uint8_t>& Bytes = W.Finish();
    SWhite.SendMove(Bytes.data(), Bytes.size());  // bare 1-byte index (issue #19)
    BoardWhite.MakeMove(*Chosen);

    CHECK(BoardBlack.SideToMove == Chess::EColor::Black);
    CHECK(SamePosition(BoardWhite, BoardBlack));  // both boards advanced identically
}

// A bare 1-byte datagram routes to the move handler; a framed (>=2 byte) message
// routes to its typed handler and NEVER the move handler (the #19 length rule).
static void TestMoveFramingDisambiguation() {
    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));

    int MoveCalls = 0, SyncCalls = 0;
    std::size_t LastMoveSize = 99;
    SB.SetMoveHandler([&](const uint8_t*, std::size_t N) { ++MoveCalls; LastMoveSize = N; });
    SB.SetHandler(EMsgType::Sync, [&](const uint8_t*, std::size_t) { ++SyncCalls; });

    const uint8_t Index[1] = {0x05};
    SA.SendMove(Index, 1);
    CHECK(MoveCalls == 1 && LastMoveSize == 1);
    CHECK(SyncCalls == 0);

    const uint8_t Payload[2] = {0xAA, 0xBB};       // framed Sync (>= 2 bytes on the wire)
    SA.Send(EMsgType::Sync, Payload, sizeof(Payload));
    CHECK(SyncCalls == 1);
    CHECK(MoveCalls == 1);                          // framed message did NOT hit the move handler
}

static const Chess::Move* Find(const Chess::MoveList& L, Chess::Square From, Chess::Square To) {
    for (int i = 0; i < L.Count; ++i)
        if (L.Moves[i].From == From && L.Moves[i].To == To) return &L.Moves[i];
    return nullptr;
}

// The reconnect-resync payload: a game's full move history round-trips through
// EncodeGame/DecodeGame back to the identical position, and a shorter history is
// a prefix of a longer one (so the "adopt the longer game" reconcile is sound).
static void TestGameHistoryResync() {
    // Play 1.e4 e5 2.Nf3 by From/To (rank-major squares).
    const Chess::Square E2 = 12, E4 = 28, E7 = 52, E5 = 36, G1 = 6, F3 = 21;
    Chess::Board B = Chess::Board::StartPosition();
    std::vector<Chess::Move> History;
    auto Play = [&](Chess::Square From, Chess::Square To) {
        Chess::MoveList L; Chess::GenerateLegalMoves(B, L);
        const Chess::Move* M = Find(L, From, To);
        CHECK(M != nullptr);
        if (M) { B.MakeMove(*M); History.push_back(*M); }
    };
    Play(E2, E4); Play(E7, E5); Play(G1, F3);

    Lur::Serialization::BitWriter W;
    Chess::EncodeGame(History, W);
    const std::vector<uint8_t>& Bytes = W.Finish();

    Chess::Board Decoded;
    std::vector<Chess::Move> DecodedHistory;
    Lur::Serialization::BitReader R(Bytes.data(), Bytes.size());
    CHECK(Chess::DecodeGame(R, Decoded, DecodedHistory));
    CHECK(DecodedHistory.size() == History.size());
    CHECK(SamePosition(Decoded, B));  // replayed to the identical position

    // A 2-move prefix decodes to the position after 1.e4 e5 (reconcile soundness:
    // the shorter game is a prefix of the longer, so replaying the longer is safe).
    std::vector<Chess::Move> Prefix(History.begin(), History.begin() + 2);
    Lur::Serialization::BitWriter PW;
    Chess::EncodeGame(Prefix, PW);
    const std::vector<uint8_t>& PBytes = PW.Finish();
    Chess::Board PDecoded;
    std::vector<Chess::Move> PHist;
    Lur::Serialization::BitReader PR(PBytes.data(), PBytes.size());
    CHECK(Chess::DecodeGame(PR, PDecoded, PHist));
    CHECK(PHist.size() == 2);
    CHECK(PDecoded.SideToMove == Chess::EColor::White);  // after two plies, White to move
}

int main() {
    TestHandshakeExchangesGuids();
    TestHandshakeResendsUntilConnected();
    TestMessageFramingStripsType();
    TestVersionMismatchRefused();
    TestChessMoveAcrossSessions();
    TestMoveFramingDisambiguation();
    TestGameHistoryResync();
    TestKeepaliveTimeoutResetsLink();
    TestKeepaliveKeepsLinkAlive();

    if (GFailures == 0) {
        std::printf("All net tests passed.\n");
        return 0;
    }
    std::printf("%d net test(s) failed.\n", GFailures);
    return 1;
}
