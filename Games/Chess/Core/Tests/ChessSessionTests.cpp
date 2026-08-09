// Chess over the ENGINE session: the move codec, the match record and the resync flow
// driven through a real Lur::Net::Session pair on a loopback transport.
//
// These tests used to live in Modules/Net/Tests, which made the engine's own test binary
// link chess::core — the module wall (Games may depend on Modules, never the reverse)
// broken in the one place nobody looks. They are chess tests: they assert what chess does
// with the session, not what the session does. The engine keeps the tests that hold with
// no game present at all.
//
// Same hand-rolled harness as the rest of the repo: a CHECK macro, a failure count, and a
// non-zero exit that CTest reports.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Lur/Core/FlightRecorder.h"
#include "Lur/Core/Hash.h"
#include "Lur/Net/Session.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Transport/Loopback.h"

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/ChessRecord.h"
#include "Chess/MoveCodec.h"
#include "Chess/Types.h"

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

// A 32-char device GUID made of one repeated character, so ordering between two peers
// is obvious at a glance ('a' < 'b').
static std::string Guid(char C) { return std::string(32, C); }

// One display frame in nanoseconds, for the Session::Tick timing arguments.
static constexpr uint64_t FrameNs = 16'666'667ull;

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

    SBlack.SetHandler(EMsgType::Game1, [&](const uint8_t* D, std::size_t N) {
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
    SWhite.Send(EMsgType::Game1, Bytes.data(), Bytes.size());   // framed move index
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

// Regression (P0): a framed Sync payload larger than the old 64-byte frame cap must
// still reach the peer. A mid-game reconnect resyncs by sending the full ChessRecord
// as an EMsgType::Sync; past ply ~61 the serialized record exceeds 64 bytes, and the
// old Session::Send() dropped it SILENTLY (returned void), so a long in-progress match
// never resynced. Build a genuinely >64-byte record, send it, and assert the peer
// decodes it back byte-for-byte.
static void TestLongGameSyncNotDropped() {
    // 160 plies of knight shuffles (g1-f3-g1 / g8-f6-g8) -> a serialized record well
    // over the old 64-byte cap. Repetition is legal here (EncodeGame just replays
    // legal moves; it does not adjudicate draws).
    const Chess::Square G1 = 6, F3 = 21, G8 = 62, F6 = 45;
    Chess::Board B = Chess::Board::StartPosition();
    std::vector<Chess::Move> History;
    auto Play = [&](Chess::Square From, Chess::Square To) {
        Chess::MoveList L; Chess::GenerateLegalMoves(B, L);
        const Chess::Move* M = Find(L, From, To);
        CHECK(M != nullptr);
        if (M) { B.MakeMove(*M); History.push_back(*M); }
    };
    for (int i = 0; i < 40; ++i) { Play(G1, F3); Play(G8, F6); Play(F3, G1); Play(F6, G8); }

    Chess::ChessRecord Rec;
    Rec.WinsLower = 3; Rec.WinsHigher = 2; Rec.Draws = 1;
    Rec.Moves = History;
    std::vector<uint8_t> Bytes;
    Rec.Write(Bytes);
    CHECK(Bytes.size() > 64);  // guard: we are genuinely testing the past-cap regime

    LoopbackTransport TA, TB;
    LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));
    CHECK(SA.IsReady() && SB.IsReady());

    Chess::ChessRecord Got;
    bool GotSync = false;
    SB.SetHandler(EMsgType::Sync, [&](const uint8_t* D, std::size_t N) { GotSync = Got.Read(D, N); });

    const bool Sent = SA.Send(EMsgType::Sync, Bytes.data(), Bytes.size());
    CHECK(Sent);        // no longer silently dropped
    CHECK(GotSync);     // and the peer actually decoded a valid record
    CHECK(Got.Moves.size() == History.size());
    CHECK(Got.WinsLower == 3 && Got.WinsHigher == 2 && Got.Draws == 1);
}

// Flight recorder (Review #2 §4.2): record a game's input stream to a FILE, read it
// back, replay it into a fresh board, and assert a byte-identical final state (hash).
// This is the "a bug becomes a file that replays" property, proven end to end.
static void TestFlightRecordReplayHashIdentical() {
    Chess::Board B = Chess::Board::StartPosition();
    Chess::ChessRecord Live;
    Lur::Core::FlightRecorder Rec;
    uint64_t T = 0;
    auto Play = [&](Chess::Square From, Chess::Square To) {
        Chess::MoveList L; Chess::GenerateLegalMoves(B, L);
        const Chess::Move* M = Find(L, From, To);
        CHECK(M != nullptr);
        if (M == nullptr) return;
        Lur::Serialization::BitWriter W;
        Chess::EncodeMove(*M, L, W);
        const std::vector<uint8_t> Bytes = W.Finish();
        Rec.Record(Lur::Core::EFlightEvent::Input, T += 1000, Bytes.data(), Bytes.size());
        B.MakeMove(*M);
        Live.Moves.push_back(*M);
    };
    // Ruy Lopez: 1.e4 e5 2.Nf3 Nc6 3.Bb5 a6 (rank-major squares).
    Play(12, 28); Play(52, 36); Play(6, 21); Play(57, 42); Play(5, 33); Play(48, 40);

    // The recording becomes a file, then is read back (the crash-ships-as-a-file path).
    const char* Path = "flightrec_test.bin";
    CHECK(Rec.WriteFile(Path));
    std::vector<Lur::Core::FlightRecorder::Event> Events;
    CHECK(Lur::Core::FlightRecorder::ReadFile(Path, Events));
    std::remove(Path);
    CHECK(Events.size() == 6);

    // Replay the recorded input stream into a FRESH board.
    Chess::Board Replay = Chess::Board::StartPosition();
    Chess::ChessRecord Rebuilt;
    for (const Lur::Core::FlightRecorder::Event& E : Events) {
        Chess::MoveList L; Chess::GenerateLegalMoves(Replay, L);
        Lur::Serialization::BitReader R(E.Data.data(), E.Data.size());
        const Chess::Move M = Chess::DecodeMove(R, L);
        CHECK(R.IsOk() && !(M == Chess::Move{}));
        Replay.MakeMove(M);
        Rebuilt.Moves.push_back(M);
    }

    // Hash-identical final state (records serialize the same bytes; positions match).
    std::vector<uint8_t> LiveBytes, RebuiltBytes;
    Live.Write(LiveBytes);
    Rebuilt.Write(RebuiltBytes);
    CHECK(Lur::Core::Fnv1a64(LiveBytes.data(), LiveBytes.size()) ==
          Lur::Core::Fnv1a64(RebuiltBytes.data(), RebuiltBytes.size()));
    CHECK(SamePosition(B, Replay));
}

// The hazard the gate prevents: the same wire bytes applied to two different boards
// produce two different positions.
static Chess::Board PlayedBoard(const Chess::Square* From, const Chess::Square* To, int N) {
    Chess::Board B = Chess::Board::StartPosition();
    for (int i = 0; i < N; ++i) {
        Chess::MoveList L; Chess::GenerateLegalMoves(B, L);
        const Chess::Move* M = Find(L, From[i], To[i]);
        if (M) B.MakeMove(*M);
    }
    return B;
}

static void TestPrematureMoveDesyncsUnreconciledBoard() {
    const Chess::Square LF[3] = {12, 52, 6}, LT[3] = {28, 36, 21};  // 1.e4 e5 2.Nf3
    const Chess::Square SF[1] = {12},        ST[1] = {28};          // 1.e4 (unreconciled peer)
    Chess::Board Long  = PlayedBoard(LF, LT, 3);   // Black to move
    Chess::Board Short = PlayedBoard(SF, ST, 1);   // Black to move, but a DIFFERENT position

    Chess::MoveList LL; Chess::GenerateLegalMoves(Long, LL);
    CHECK(LL.Count > 0);
    Lur::Serialization::BitWriter W; Chess::EncodeMove(LL.Moves[0], LL, W);
    const std::vector<uint8_t>& Bytes = W.Finish();

    Chess::Board LongAfter = Long; LongAfter.MakeMove(LL.Moves[0]);  // sender's true result

    Chess::MoveList SL; Chess::GenerateLegalMoves(Short, SL);
    Lur::Serialization::BitReader R(Bytes.data(), Bytes.size());
    const Chess::Move Decoded = Chess::DecodeMove(R, SL);
    Chess::Board ShortAfter = Short;
    if (R.IsOk() && !(Decoded == Chess::Move{})) ShortAfter.MakeMove(Decoded);

    CHECK(!SamePosition(LongAfter, ShortAfter));  // same bytes, divergent result -> why we gate
}

// End to end: two peers link, exchange Sync, then autoplay stays in lockstep — and a
// pre-Sync move is correctly HELD (the exact regression #71 reproduced on device).
static void TestResyncGateEnablesCleanCrossPeerPlay() {
    LoopbackTransport TA, TB; LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    Chess::ChessMatchState MA, MB;
    MA.SetIdentity(Guid('a'), Guid('b'));   // A lower GUID -> White (even parity)
    MB.SetIdentity(Guid('b'), Guid('a'));   // B higher GUID -> Black

    SA.SetHandler(EMsgType::Sync, [&](const uint8_t* D, std::size_t N) { MA.MergeIfNewer(D, N); });
    SB.SetHandler(EMsgType::Sync, [&](const uint8_t* D, std::size_t N) { MB.MergeIfNewer(D, N); });
    auto ApplyRemote = [&](Chess::ChessMatchState& M, Session& /*S*/, const uint8_t* D, std::size_t N) {
        // No inbound gate: apply if it decodes; a stale-board decode fails -> resync (#72).
        Chess::MoveList L; Chess::GenerateLegalMoves(M.CurrentBoard(), L);
        Lur::Serialization::BitReader R(D, N);
        const Chess::Move Mv = Chess::DecodeMove(R, L);
        if (!R.IsOk() || Mv == Chess::Move{}) return;
        if (M.SideToMove() == M.MyColor()) return;                      // not the peer's turn
        M.ApplyMove(Mv);
    };
    SA.SetHandler(EMsgType::Game1, [&](const uint8_t* D, std::size_t N) { ApplyRemote(MA, SA, D, N); });
    SB.SetHandler(EMsgType::Game1, [&](const uint8_t* D, std::size_t N) { ApplyRemote(MB, SB, D, N); });

    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));
    CHECK(SA.IsReady() && SB.IsReady());
    CHECK(SA.IsAwaitingResync() && SB.IsAwaitingResync());   // both gated at link

    // A gated autoplay step (mirrors BoardView::CanMoveNow) must NOT move pre-resync.
    auto Step = [&](Chess::ChessMatchState& M, Session& S) -> bool {
        if (S.IsAwaitingResync()) return false;                         // the #71 local gate
        if (M.SideToMove() != M.MyColor()) return false;                // not our turn
        Chess::MoveList L; Chess::GenerateLegalMoves(M.CurrentBoard(), L);
        if (L.Count <= 0) return false;
        Lur::Serialization::BitWriter W; Chess::EncodeMove(L.Moves[0], L, W);
        const std::vector<uint8_t>& B = W.Finish();
        S.Send(EMsgType::Game1, B.data(), B.size());  // synchronous loopback -> peer's handler
        M.ApplyMove(L.Moves[0]);
        return true;
    };
    CHECK(!Step(MA, SA));   // White held pre-resync (this is what breaks the deadlock)
    CHECK(SamePosition(MA.CurrentBoard(), MB.CurrentBoard()));

    // The link-time Sync exchange (what the app's ready/resync handler does) lifts both.
    { std::vector<uint8_t> Snap; MA.Write(Snap); SA.Send(EMsgType::Sync, Snap.data(), Snap.size()); }
    { std::vector<uint8_t> Snap; MB.Write(Snap); SB.Send(EMsgType::Sync, Snap.data(), Snap.size()); }
    CHECK(!SA.IsAwaitingResync() && !SB.IsAwaitingResync());

    // Now play 20 plies alternating; boards must stay byte-identical throughout.
    int Plies = 0;
    for (int i = 0; i < 60 && Plies < 20; ++i) {
        if (Step(MA, SA)) ++Plies;
        if (Step(MB, SB)) ++Plies;
        CHECK(SamePosition(MA.CurrentBoard(), MB.CurrentBoard()));
    }
    CHECK(Plies >= 20);    // progressed to 20 plies with no stall (the deadlock is gone)
}

// End to end: two synced peers, one loses a live move (never sent), so their boards
// diverge. The keepalive hash exchange detects it and both re-send Sync until they
// reconcile — no deadlock, no manual reconnect (issue #72).
static void TestKeepaliveHashHealsLostMoveDesync() {
    LoopbackTransport TA, TB; LoopbackTransport::Link(TA, TB);
    Session SA, SB;
    Chess::ChessMatchState MA, MB;
    MA.SetIdentity(Guid('a'), Guid('b'));
    MB.SetIdentity(Guid('b'), Guid('a'));
    auto SendSync = [](Session& S, Chess::ChessMatchState& M) {
        std::vector<uint8_t> B; M.Write(B); S.Send(EMsgType::Sync, B.data(), B.size());
    };
    SA.SetStateHashFn([&] { return MA.PositionHash(); });
    SB.SetStateHashFn([&] { return MB.PositionHash(); });
    SA.SetResyncHandler([&] { SendSync(SA, MA); });
    SB.SetResyncHandler([&] { SendSync(SB, MB); });
    SA.SetHandler(EMsgType::Sync, [&](const uint8_t* D, std::size_t N) { MA.MergeIfNewer(D, N); });
    SB.SetHandler(EMsgType::Sync, [&](const uint8_t* D, std::size_t N) { MB.MergeIfNewer(D, N); });
    SA.Start(&TA, Guid('a'));
    SB.Start(&TB, Guid('b'));
    CHECK(SA.IsReady() && SB.IsReady());
    SendSync(SA, MA); SendSync(SB, MB);          // link-time resync -> converged (both empty)
    CHECK(SamePosition(MA.CurrentBoard(), MB.CurrentBoard()));

    // Play 3 plies in lockstep so both advance identically (index encoded on the mover's
    // board, decoded + applied on the other — the real wire path, delivered).
    auto PlayAndSend = [&](Chess::ChessMatchState& From, Chess::ChessMatchState& To) {
        Chess::MoveList L; Chess::GenerateLegalMoves(From.CurrentBoard(), L);
        Lur::Serialization::BitWriter W; Chess::EncodeMove(L.Moves[0], L, W);
        const std::vector<uint8_t>& B = W.Finish();
        Chess::MoveList TL; Chess::GenerateLegalMoves(To.CurrentBoard(), TL);
        Lur::Serialization::BitReader R(B.data(), B.size());
        const Chess::Move Mv = Chess::DecodeMove(R, TL);
        From.ApplyMove(L.Moves[0]); To.ApplyMove(Mv);   // both apply (delivered)
    };
    PlayAndSend(MA, MB); PlayAndSend(MB, MA); PlayAndSend(MA, MB);
    CHECK(SamePosition(MA.CurrentBoard(), MB.CurrentBoard()));

    // Now A makes a move that is LOST (applied locally, never delivered to B).
    { Chess::MoveList L; Chess::GenerateLegalMoves(MA.CurrentBoard(), L); MA.ApplyMove(L.Moves[0]); }
    CHECK(!SamePosition(MA.CurrentBoard(), MB.CurrentBoard()));   // diverged

    // Drive time: keepalives (with hashes) cross, the mismatch is detected, both resync.
    for (int i = 0; i < 400; ++i) { SA.Tick(FrameNs); SB.Tick(FrameNs); }
    CHECK(SamePosition(MA.CurrentBoard(), MB.CurrentBoard()));    // healed, no deadlock
    CHECK(!SA.IsAwaitingResync() && !SB.IsAwaitingResync());
}
// The move codec straight over a transport, with no session in between: encode a real
// chess move, ship the bytes, and have the far side decode it back to the identical
// move from its own regenerated legal list. Squares/flags never travel; only the
// move's index does. (Lived in the engine's transport tests until Phase 0 — it is a
// statement about the chess codec, not about ITransport.)
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

int main() {
    TestMoveRoundtripsOverTransport();
    TestChessMoveAcrossSessions();
    TestGameHistoryResync();
    TestLongGameSyncNotDropped();
    TestFlightRecordReplayHashIdentical();
    TestPrematureMoveDesyncsUnreconciledBoard();
    TestResyncGateEnablesCleanCrossPeerPlay();
    TestKeepaliveHashHealsLostMoveDesync();

    if (GFailures == 0) {
        std::printf("chess_session_tests: all checks passed\n");
        return 0;
    }
    std::printf("chess_session_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
