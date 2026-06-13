// Correctness tests for the chess rules engine. The heavy lifting is PERFT —
// counting the exact number of leaf nodes in the move tree to a given depth and
// comparing against long-published reference values. A single bug in move
// generation, make-move, castling, en passant, or promotion shifts these counts,
// so matching them across several positions is strong evidence of correctness.
#include <cstdint>
#include <cstdio>

#include "Chess/Board.h"
#include "Chess/MoveCodec.h"
#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"

using namespace Chess;

static int GFailures = 0;
#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

static uint64_t Perft(const Board& B, int Depth) {
    MoveList Moves;
    GenerateLegalMoves(B, Moves);
    if (Depth <= 1) return static_cast<uint64_t>(Moves.Count);
    uint64_t Nodes = 0;
    for (int I = 0; I < Moves.Count; ++I) {
        Board C = B;
        C.MakeMove(Moves.Moves[I]);
        Nodes += Perft(C, Depth - 1);
    }
    return Nodes;
}

struct PerftCase { const char* Fen; int Depth; uint64_t Expected; };

static void TestPerft() {
    // Reference values from the standard perft suite (chessprogramming wiki).
    static const PerftCase Cases[] = {
        // Start position.
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 1, 20},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 2, 400},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 3, 8902},
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281},
        // "Kiwipete" — castling both sides, pins, many captures.
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 1, 48},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 2, 2039},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862},
        // Position 3 — en passant heavy.
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 1, 14},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 2, 191},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 3, 2812},
        {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238},
        // Position 4 — promotions and checks.
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 1, 6},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 2, 264},
        {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9467},
        // Position 5.
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 1, 44},
        {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 2, 1486},
    };
    for (const PerftCase& T : Cases) {
        const uint64_t Got = Perft(Board::FromFen(T.Fen), T.Depth);
        if (Got != T.Expected) {
            std::printf("PERFT FAIL d%d  expected %llu  got %llu   [%s]\n",
                        T.Depth, static_cast<unsigned long long>(T.Expected),
                        static_cast<unsigned long long>(Got), T.Fen);
            ++GFailures;
        }
    }
}

// The codec works with REAL generated moves: every legal move round-trips through
// its index in the (deterministic) legal list.
static void TestCodecRoundTrip() {
    const Board B = Board::StartPosition();
    MoveList Moves;
    GenerateLegalMoves(B, Moves);
    CHECK(Moves.Count == 20);

    for (int I = 0; I < Moves.Count; ++I) {
        Lur::Serialization::BitWriter W;
        EncodeMove(Moves.Moves[I], Moves, W);
        const auto& Bytes = W.Finish();
        Lur::Serialization::BitReader R(Bytes.data(), Bytes.size());
        CHECK(DecodeMove(R, Moves) == Moves.Moves[I]);
    }

    // 20 legal moves -> ceil(log2(20)) = 5 bits per move.
    Lur::Serialization::BitWriter W;
    EncodeMove(Moves.Moves[0], Moves, W);
    CHECK(W.GetBitCount() == 5);
}

static void TestResults() {
    // Scholar's mate final position, black to move and checkmated.
    CHECK(Result(Board::FromFen(
        "r1bqkb1r/pppp1Qpp/2n2n2/4p3/2B1P3/8/PPPP1PPP/RNB1K1NR b KQkq - 0 4")) == EGameResult::Checkmate);
    // Classic K+Q stalemate, black to move, not in check, no legal moves.
    CHECK(Result(Board::FromFen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1")) == EGameResult::Stalemate);
    // Bare kings.
    CHECK(Result(Board::FromFen("8/8/4k3/8/8/4K3/8/8 w - - 0 1")) == EGameResult::DrawInsufficientMaterial);
    // Opening position is ongoing.
    CHECK(Result(Board::StartPosition()) == EGameResult::Ongoing);
}

int main() {
    TestPerft();
    TestCodecRoundTrip();
    TestResults();

    if (GFailures == 0) {
        std::printf("All chess rules tests passed.\n");
        return 0;
    }
    std::printf("%d chess rules test(s) failed.\n", GFailures);
    return 1;
}
