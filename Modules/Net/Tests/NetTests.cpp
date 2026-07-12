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

// Two sessions over a linked loopback pair, already connected before Start, must
// both become ready with OPPOSITE seats — and the larger nonce takes seat 0.
static void TestHandshakeSeatsAreOpposite() {
    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);

    Session SA, SB;
    SA.Start(&TA, /*nonce*/ 100);
    SB.Start(&TB, /*nonce*/ 200);

    CHECK(SA.IsReady());
    CHECK(SB.IsReady());
    CHECK(SA.GetSeat() != SB.GetSeat());
    CHECK(SB.GetSeat() == 0);  // larger nonce (200) -> seat 0
    CHECK(SA.GetSeat() == 1);
}

// The first Hello is dropped when it is sent before the link is up (IsConnected
// is false). Tick() must resend and complete the handshake once connected.
static void TestHandshakeResendsUntilConnected() {
    Session SA, SB;
    LoopbackTransport TA, TB;

    SA.Start(&TA, 7);  // not linked yet -> Hello skipped (not connected)
    SB.Start(&TB, 9);
    CHECK(!SA.IsReady());
    CHECK(!SB.IsReady());

    LoopbackTransport::Link(TA, TB);
    for (int i = 0; i < 4 && !(SA.IsReady() && SB.IsReady()); ++i) { SA.Tick(); SB.Tick(); }

    CHECK(SA.IsReady() && SB.IsReady());
    CHECK(SB.GetSeat() == 0 && SA.GetSeat() == 1);  // 9 > 7 -> SB seat 0
}

// Send() prepends the type byte; the far side's per-type handler receives the
// payload with that byte stripped.
static void TestMessageFramingStripsType() {
    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    SA.Start(&TA, 1);
    SB.Start(&TB, 2);

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
    S.Start(&T, 500);

    uint8_t Hello[1 + 8 + 1 + 1] = {};                     // type + version + nonce + ready
    Hello[0] = static_cast<uint8_t>(EMsgType::Hello);
    Hello[1] = static_cast<uint8_t>(ProtocolVersion + 1);  // wrong version
    Hello[2] = 0x2C;                                        // some nonce (600)
    Hello[10] = 1;                                          // ready flag
    Peer.Send(Hello, sizeof(Hello));
    CHECK(!S.IsReady());
}

static bool SamePosition(const Chess::Board& A, const Chess::Board& B) {
    if (A.SideToMove != B.SideToMove) return false;
    if (A.Castling != B.Castling || A.EnPassant != B.EnPassant) return false;
    for (int C = 0; C < 2; ++C)
        for (int T = 0; T < 6; ++T)
            if (A.Pieces[C][T] != B.Pieces[C][T]) return false;
    return true;
}

// THE integration test: seat 0 (White) plays e2e4; the move's INDEX crosses the
// session as a framed Move message; seat 1 decodes it against its own regenerated
// legal list and applies it — landing on the identical position. Squares never
// travel, only the index.
static void TestChessMoveAcrossSessions() {
    LoopbackTransport TWhite, TBlack;
    LoopbackTransport::Link(TWhite, TBlack);
    Session SWhite, SBlack;
    SWhite.Start(&TWhite, 900);  // larger nonce -> seat 0 -> White
    SBlack.Start(&TBlack, 100);
    CHECK(SWhite.GetSeat() == 0 && SBlack.GetSeat() == 1);

    Chess::Board BoardWhite = Chess::Board::StartPosition();
    Chess::Board BoardBlack = Chess::Board::StartPosition();

    SBlack.SetHandler(EMsgType::Move, [&](const uint8_t* D, std::size_t N) {
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
    SWhite.Send(EMsgType::Move, Bytes.data(), Bytes.size());
    BoardWhite.MakeMove(*Chosen);

    CHECK(BoardBlack.SideToMove == Chess::EColor::Black);
    CHECK(SamePosition(BoardWhite, BoardBlack));  // both boards advanced identically
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
    TestHandshakeSeatsAreOpposite();
    TestHandshakeResendsUntilConnected();
    TestMessageFramingStripsType();
    TestVersionMismatchRefused();
    TestChessMoveAcrossSessions();
    TestGameHistoryResync();

    if (GFailures == 0) {
        std::printf("All net tests passed.\n");
        return 0;
    }
    std::printf("%d net test(s) failed.\n", GFailures);
    return 1;
}
