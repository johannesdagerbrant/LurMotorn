// Fuzz + determinism + byte-budget suite (roadmap Phase 0.5, Review #2 §4.4/§7).
//
//  * Codec/frame fuzz: throw megabytes of random/mutated bytes at DecodeGame and the
//    Session frame parser — the attacker-facing surface (peer-supplied datagrams) must
//    survive garbage by construction. (Run under ASan/UBSan in CI to amplify; here we
//    assert termination + no crash on a Debug build.)
//  * Determinism fuzz: thousands of random LEGAL games; each move-index stream must
//    decode on a fresh board to the identical history + position — the property both
//    peers rely on (state = replay(inputs)).
//  * Byte budget: the manifesto ("squeeze as much game out of as few bytes") turned
//    into asserts — a live move is <= 1 byte; a 60-ply resync stays slim.
//
// No framework: each CHECK records a failure; the process exits non-zero if any failed.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Chess/Board.h"
#include "Chess/ChessRecord.h"
#include "Chess/MoveCodec.h"
#include "Chess/Types.h"
#include "Lur/Net/Session.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Transport/Transport.h"

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// Deterministic PRNG (xorshift64) so any failure reproduces from its seed.
static uint64_t Next(uint64_t& S) {
    S ^= S << 13;
    S ^= S >> 7;
    S ^= S << 17;
    return S;
}

// Iteration multiplier. Default 1 keeps the fast host loop snappy; a CI soak job sets
// LUR_FUZZ_SCALE=100 (etc.) to run the "millions of games" pass — same code, bigger N.
static int FuzzScale() {
    const char* S = std::getenv("LUR_FUZZ_SCALE");
    const int V = (S != nullptr) ? std::atoi(S) : 1;
    return V > 0 ? V : 1;
}

static bool SamePosition(const Chess::Board& A, const Chess::Board& B) {
    if (A.SideToMove != B.SideToMove) return false;
    if (A.Castling != B.Castling || A.EnPassant != B.EnPassant) return false;
    for (int C = 0; C < 2; ++C)
        for (int T = 0; T < 6; ++T)
            if (A.Pieces[C][T] != B.Pieces[C][T]) return false;
    return true;
}

// Play a random legal game (random move each ply until mate/stalemate or the cap).
static void PlayRandomGame(uint64_t& Seed, std::vector<Chess::Move>& Out, int MaxPlies = 120) {
    Chess::Board B = Chess::Board::StartPosition();
    Out.clear();
    for (int Ply = 0; Ply < MaxPlies; ++Ply) {
        Chess::MoveList L;
        Chess::GenerateLegalMoves(B, L);
        if (L.Count == 0) break;
        const Chess::Move M = L.Moves[Next(Seed) % static_cast<uint64_t>(L.Count)];
        B.MakeMove(M);
        Out.push_back(M);
    }
}

// Determinism: encode a random game's move-index stream, decode on a FRESH board, and
// require the identical history + position (what a resyncing peer must reconstruct).
static void TestDeterminismFuzz() {
    uint64_t Seed = 0x123456789abcdef0ull;
    const int Games = 150 * FuzzScale();  // movegen-heavy; scale up in the CI soak
    for (int Game = 0; Game < Games; ++Game) {
        std::vector<Chess::Move> Hist;
        PlayRandomGame(Seed, Hist);

        Lur::Serialization::BitWriter W;
        Chess::EncodeGame(Hist, W);
        const std::vector<uint8_t> Bytes = W.Finish();

        Chess::Board Decoded;
        std::vector<Chess::Move> DHist;
        Lur::Serialization::BitReader R(Bytes.data(), Bytes.size());
        CHECK(Chess::DecodeGame(R, Decoded, DHist));
        CHECK(DHist.size() == Hist.size());

        Chess::Board Expected = Chess::Board::StartPosition();
        for (const Chess::Move& M : Hist) Expected.MakeMove(M);
        CHECK(SamePosition(Expected, Decoded));
    }
}

// The attacker surface: DecodeGame must never crash/hang on hostile bytes, only
// return true/false. (varint shift guard, illegal-index reject, reader underrun.)
static void TestCodecFuzzNoCrash() {
    uint64_t Seed = 0xDEADBEEFCAFEBABEull;
    const int Iters = 40000 * FuzzScale();
    for (int i = 0; i < Iters; ++i) {
        uint8_t Buf[80];
        const std::size_t N = Next(Seed) % sizeof(Buf);
        for (std::size_t j = 0; j < N; ++j) Buf[j] = static_cast<uint8_t>(Next(Seed));
        Chess::Board Bd;
        std::vector<Chess::Move> H;
        Lur::Serialization::BitReader R(Buf, N);
        (void)Chess::DecodeGame(R, Bd, H);  // return value irrelevant; must just terminate
    }
    CHECK(true);  // reached here => no crash/hang across all hostile inputs this run
}

namespace {
// A connected transport that lets the test push arbitrary datagrams at the Session.
struct FuzzTransport : Lur::Transport::ITransport {
    Receiver Rx;
    void Send(const uint8_t*, std::size_t) override {}
    void SetReceiver(Receiver R) override { Rx = std::move(R); }
    bool IsConnected() const override { return true; }
    void Deliver(const uint8_t* D, std::size_t N) { if (Rx) Rx(D, N); }
};
}  // namespace

// The Session frame parser (OnDatagram -> handlers) must survive random datagrams.
static void TestSessionFrameFuzzNoCrash() {
    FuzzTransport T;
    Lur::Net::Session S;
    S.Start(&T, std::string(32, 'a'));
    S.SetMoveHandler([](const uint8_t*, std::size_t) {});
    S.SetHandler(Lur::Net::EMsgType::Sync, [](const uint8_t* D, std::size_t N) {
        Chess::ChessRecord Rec;
        Rec.Read(D, N);  // exercise the sync-decode path on garbage too
    });

    uint64_t Seed = 0x99AA55CC1234ull;
    const int Iters = 40000 * FuzzScale();
    for (int i = 0; i < Iters; ++i) {
        uint8_t Buf[72];
        const std::size_t N = Next(Seed) % sizeof(Buf);
        for (std::size_t j = 0; j < N; ++j) Buf[j] = static_cast<uint8_t>(Next(Seed));
        T.Deliver(Buf, N);
    }
    CHECK(true);
}

// Byte budget = the product. A live move is always <= 1 byte; a ~60-ply resync is slim.
static void TestByteBudget() {
    // Every legal move, across many random games, encodes to at most one byte.
    uint64_t Seed = 0xB0D6E77ull;
    for (int Game = 0; Game < 300; ++Game) {
        Chess::Board B = Chess::Board::StartPosition();
        for (int Ply = 0; Ply < 80; ++Ply) {
            Chess::MoveList L;
            Chess::GenerateLegalMoves(B, L);
            if (L.Count == 0) break;
            const Chess::Move M = L.Moves[Next(Seed) % static_cast<uint64_t>(L.Count)];
            Lur::Serialization::BitWriter W;
            Chess::EncodeMove(M, L, W);
            CHECK(W.Finish().size() <= 1);  // THE claim: a move costs 0-8 bits
            B.MakeMove(M);
        }
    }

    // A 60-ply in-progress game syncs slim: <= 64 bytes (well under one BLE datagram),
    // i.e. ~1 byte/ply including the 2-byte ply count. Guards against wire bloat.
    uint64_t S2 = 0x5EED60ull;
    std::vector<Chess::Move> Hist;
    Chess::Board B = Chess::Board::StartPosition();
    for (int Ply = 0; Ply < 60; ++Ply) {
        Chess::MoveList L;
        Chess::GenerateLegalMoves(B, L);
        if (L.Count == 0) break;
        const Chess::Move M = L.Moves[Next(S2) % static_cast<uint64_t>(L.Count)];
        B.MakeMove(M);
        Hist.push_back(M);
    }
    CHECK(Hist.size() >= 40);  // got a game of real length to measure
    Lur::Serialization::BitWriter W;
    Chess::EncodeGame(Hist, W);
    CHECK(W.Finish().size() <= 64);
}

int main() {
    TestDeterminismFuzz();
    TestCodecFuzzNoCrash();
    TestSessionFrameFuzzNoCrash();
    TestByteBudget();

    if (GFailures == 0) {
        std::printf("All fuzz/budget tests passed.\n");
        return 0;
    }
    std::printf("%d fuzz/budget test(s) failed.\n", GFailures);
    return 1;
}
