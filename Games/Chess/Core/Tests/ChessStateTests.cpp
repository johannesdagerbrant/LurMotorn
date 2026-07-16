// Host tests for the Phase B game state (issue #18): the per-opponent ChessRecord
// serialisation, identity-based colour (GUID order + match parity), and the
// link-time record merge (adopt the strictly-newer record). Pure logic — no
// transport, no device. Same tiny CHECK harness as the other suites.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Chess/Board.h"
#include "Chess/ChessMatchState.h"
#include "Chess/ChessRecord.h"
#include "Chess/MatchMeta.h"
#include "Chess/OpponentRegistry.h"
#include "Chess/Types.h"
#include "Lur/Save/Store.h"

using namespace Chess;

static int GFailures = 0;
#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

static const Move* Find(const MoveList& L, Square From, Square To) {
    for (int i = 0; i < L.Count; ++i)
        if (L.Moves[i].From == From && L.Moves[i].To == To) return &L.Moves[i];
    return nullptr;
}

// Play a few plies into a match state by From/To.
static void Play(ChessMatchState& S, Square From, Square To) {
    MoveList L;
    GenerateLegalMoves(S.CurrentBoard(), L);
    const Move* M = Find(L, From, To);
    CHECK(M != nullptr);
    if (M) S.ApplyMove(*M);
}

// Squares (rank-major).
static constexpr Square E2 = 12, E4 = 28, E7 = 52, E5 = 36, G1 = 6, F3 = 21;

// A record with tallies + a few moves round-trips through Write/Read byte-for-byte.
static void TestRecordRoundTrip() {
    ChessMatchState S;
    S.SetIdentity("aaaa", "bbbb");
    Play(S, E2, E4);
    Play(S, E7, E5);
    Play(S, G1, F3);

    std::vector<uint8_t> Bytes;
    S.Write(Bytes);

    ChessMatchState T;
    T.SetIdentity("aaaa", "bbbb");
    T.Read(Bytes.data(), Bytes.size());
    CHECK(T.Record().Moves.size() == 3);

    std::vector<uint8_t> Bytes2;
    T.Write(Bytes2);
    CHECK(Bytes == Bytes2);                    // stable serialisation
    // Board rebuilt to the same position (side to move after 3 plies = Black).
    CHECK(T.CurrentBoard().SideToMove == EColor::Black);
}

// An empty buffer restores fresh defaults (a brand-new opponent).
static void TestReadAbsentIsFresh() {
    ChessMatchState S;
    S.SetIdentity("aaaa", "bbbb");
    S.Read(nullptr, 0);
    CHECK(S.Record().TotalMatches() == 0);
    CHECK(S.Record().Moves.empty());
    CHECK(S.CurrentBoard().SideToMove == EColor::White);
}

// Colour: deterministic, identical + opposite on both phones, independent of who
// holds which GUID, and flips with match-count parity.
static void TestIdentityColour() {
    const std::string Lo = "1111", Hi = "9999";

    ChessMatchState A; A.SetIdentity(Lo, Hi);   // A is the lower GUID
    ChessMatchState B; B.SetIdentity(Hi, Lo);   // B is the higher GUID
    CHECK(A.IsLocalLower());
    CHECK(!B.IsLocalLower());

    // Even total matches (0) -> lower is White.
    CHECK(A.MyColor() == EColor::White);
    CHECK(B.MyColor() == EColor::Black);
    CHECK(A.MyColor() != B.MyColor());          // always opposite

    // Odd total matches -> polarity flips (lower is Black).
    std::vector<uint8_t> Rec = {0, 1, 0};       // WinsLower=0, WinsHigher=1, Draws=0 -> total 1
    // append an empty move list (u16 ply count = 0)
    Rec.push_back(0); Rec.push_back(0);
    A.Read(Rec.data(), Rec.size());
    B.Read(Rec.data(), Rec.size());
    CHECK(A.Record().TotalMatches() == 1);
    CHECK(A.MyColor() == EColor::Black);
    CHECK(B.MyColor() == EColor::White);
    CHECK(A.MyColor() != B.MyColor());
}

// MergeIfNewer: adopt a peer record with more moves; reject an older/equal one.
static void TestMergeByMoveCount() {
    ChessMatchState Mine; Mine.SetIdentity("aaaa", "bbbb");
    Play(Mine, E2, E4);                         // 1 ply

    // Peer is two plies ahead (1.e4 e5).
    ChessMatchState Peer; Peer.SetIdentity("bbbb", "aaaa");
    Play(Peer, E2, E4);
    Play(Peer, E7, E5);
    std::vector<uint8_t> PeerBytes; Peer.Write(PeerBytes);

    CHECK(Mine.MergeIfNewer(PeerBytes.data(), PeerBytes.size()));  // adopted
    CHECK(Mine.Record().Moves.size() == 2);
    CHECK(Mine.CurrentBoard().SideToMove == EColor::White);       // after 2 plies

    // Merging the same (now equal) record again does nothing.
    CHECK(!Mine.MergeIfNewer(PeerBytes.data(), PeerBytes.size()));
    CHECK(Mine.Record().Moves.size() == 2);
}

// MergeIfNewer: matches dominate move count (a finished match reset moves to 0).
static void TestMergeMatchesDominate() {
    ChessMatchState Mine; Mine.SetIdentity("aaaa", "bbbb");
    Play(Mine, E2, E4); Play(Mine, E7, E5); Play(Mine, G1, F3);   // 3 plies, 0 matches

    // Peer has completed one match (total 1) and started fresh (0 moves).
    std::vector<uint8_t> Peer = {1, 0, 0, 0, 0};   // WinsLower=1; empty move list
    CHECK(Mine.MergeIfNewer(Peer.data(), Peer.size()));            // adopted despite fewer moves
    CHECK(Mine.Record().TotalMatches() == 1);
    CHECK(Mine.Record().Moves.empty());
}

// A terminal move auto-concludes the match: winner tallied (agnostic, anchored to
// the lower-GUID device), board reset, colour flips with the new match parity.
static void TestCheckmateConcludesMatch() {
    ChessMatchState S;
    S.SetIdentity("1111", "9999");     // S is the lower GUID -> White in match 0
    CHECK(S.MyColor() == EColor::White);

    // Fool's mate: 1.f3 e5 2.g4 Qh4#
    const Square F2 = 13, F3s = 21, G2 = 14, G4 = 30, E7s = 52, E5s = 36, D8 = 59, H4 = 31;
    Play(S, F2, F3s);
    Play(S, E7s, E5s);
    Play(S, G2, G4);
    Play(S, D8, H4);                   // checkmate -> auto-conclude

    CHECK(S.LastResult() == EGameResult::Checkmate);
    CHECK(S.Record().WinsHigher == 1); // Black (the higher-GUID device) delivered mate
    CHECK(S.Record().WinsLower == 0);
    CHECK(S.Record().Draws == 0);
    CHECK(S.Record().TotalMatches() == 1);
    CHECK(S.Record().Moves.empty());                        // next match started
    CHECK(S.CurrentBoard().SideToMove == EColor::White);    // fresh board
    CHECK(S.MyColor() == EColor::Black);                    // parity flipped: lower now Black
}

// 150 quiet plies (knight shuffle) trigger the 75-move auto-draw.
static void TestSeventyFiveMoveDraw() {
    ChessMatchState S;
    S.SetIdentity("1111", "9999");
    const Square G1s = 6, F3s = 21, G8 = 62, F6 = 45;
    const Square From[4] = {G1s, G8, F3s, F6};   // Ng1f3, Ng8f6, Nf3g1, Nf6g8, repeat
    const Square To[4]   = {F3s, F6, G1s, G8};

    int Plies = 0;
    for (int i = 0; i < 200 && S.Record().TotalMatches() == 0; ++i) {
        Play(S, From[i % 4], To[i % 4]);
        ++Plies;
    }
    CHECK(S.LastResult() == EGameResult::DrawFiftyMove);
    CHECK(S.Record().Draws == 1);
    CHECK(S.Record().TotalMatches() == 1);
    CHECK(Plies == 150);               // 75 moves by each side
    CHECK(S.Record().Moves.empty());   // next match started
}

// EnumerateOpponents lists every stored opponent record, derives whose turn it is
// from GUID order + parity (no live link needed), and filters out control keys and
// this device's own id (#35).
static void TestEnumerateOpponents() {
    namespace fs = std::filesystem;
    const fs::path Dir = fs::temp_directory_path() / "lur_opp_enum_tests";
    std::error_code Ec; fs::remove_all(Dir, Ec);
    Lur::Save::Store S(Dir.string());

    const std::string Local  = "55555555555555555555555555555555";  // 32-hex
    const std::string PeerHi = "99999999999999999999999999999999";  // Local < PeerHi
    const std::string PeerLo = "11111111111111111111111111111111";  // Local > PeerLo

    // Store a fresh (empty) record per key. Records are identity-agnostic, so we can
    // write a default ChessRecord directly.
    auto SaveFresh = [&](const std::string& Key) {
        ChessRecord R; std::vector<uint8_t> B; R.Write(B);
        CHECK(S.Save(Key, B.data(), B.size()));
    };
    SaveFresh(PeerHi);
    SaveFresh(PeerLo);
    SaveFresh(Local);          // our own id — must be excluded
    SaveFresh("device-id");    // a control key (not 32-hex) — must be excluded

    std::vector<OpponentInfo> Ops = EnumerateOpponents(S, Local);
    CHECK(Ops.size() == 2);    // only PeerHi + PeerLo

    auto Get = [&](const std::string& G) -> const OpponentInfo* {
        auto It = std::find_if(Ops.begin(), Ops.end(),
                               [&](const OpponentInfo& O) { return O.Guid == G; });
        return It == Ops.end() ? nullptr : &*It;
    };
    const OpponentInfo* Hi = Get(PeerHi);
    const OpponentInfo* Lo = Get(PeerLo);
    CHECK(Hi != nullptr);
    CHECK(Lo != nullptr);
    // Fresh game, even parity -> lower-GUID device is White and moves first. Against
    // PeerHi we are the lower GUID (our turn); against PeerLo we are higher (not).
    if (Hi) { CHECK(Hi->MyTurn); CHECK(Hi->MoveCount == 0); }
    if (Lo) CHECK(!Lo->MyTurn);
    CHECK(Get(Local) == nullptr);
    CHECK(Get("device-id") == nullptr);
}

// The local-only last-move sidecar round-trips, defaults to 0 when absent, and its
// "meta-"+guid key is never mistaken for an opponent record (#36).
static void TestMatchMeta() {
    namespace fs = std::filesystem;
    const fs::path Dir = fs::temp_directory_path() / "lur_meta_tests";
    std::error_code Ec; fs::remove_all(Dir, Ec);
    Lur::Save::Store S(Dir.string());
    const std::string Guid = "abcdef0123456789abcdef0123456789";

    CHECK(LoadMatchMeta(S, Guid).LastMoveMs == 0);                 // absent -> 0
    MatchMeta M; M.LastMoveMs = 1752600000123ull;                  // > 32 bits, exercises full u64
    CHECK(SaveMatchMeta(S, Guid, M));
    CHECK(LoadMatchMeta(S, Guid).LastMoveMs == M.LastMoveMs);      // round-trip

    // Storing the real opponent record too, the sidecar key must not be enumerated.
    ChessRecord R; std::vector<uint8_t> B; R.Write(B);
    CHECK(S.Save(Guid, B.data(), B.size()));
    std::vector<OpponentInfo> Ops =
        EnumerateOpponents(S, "00000000000000000000000000000000");
    CHECK(Ops.size() == 1);                                        // only the record
    CHECK(Ops.size() == 1 && Ops[0].Guid == Guid);

    CHECK(NowMillisUtc() > 0);                                     // clock is sane
}

int main() {
    TestRecordRoundTrip();
    TestReadAbsentIsFresh();
    TestIdentityColour();
    TestMergeByMoveCount();
    TestMergeMatchesDominate();
    TestCheckmateConcludesMatch();
    TestSeventyFiveMoveDraw();
    TestEnumerateOpponents();
    TestMatchMeta();

    if (GFailures == 0) {
        std::printf("All chess state tests passed.\n");
        return 0;
    }
    std::printf("%d chess state test(s) failed.\n", GFailures);
    return 1;
}
