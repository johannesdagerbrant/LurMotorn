#pragma once
#include <array>
#include <cstdint>
#include "Chess/Types.h"

// Bitboard primitives for move generation — private to chess_core.
//
// A uint64_t is a set of squares (bit S = square S). Attacks are computed as
// bitboards: leaper attacks (knight/king/pawn) from precomputed constexpr tables,
// slider attacks (bishop/rook/queen) by ray-scanning against occupancy. No magic
// bitboards yet — correctness first; this is the spot to optimize later.
namespace Chess {

inline constexpr uint64_t Bit(Square S) { return uint64_t{1} << S; }
inline constexpr int FileOf(Square S)   { return S & 7; }
inline constexpr int RankOf(Square S)   { return S >> 3; }

inline int PopCount(uint64_t B) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_popcountll(B);
#else
    int C = 0; while (B) { B &= B - 1; ++C; } return C;
#endif
}

// Index of the least-significant set bit. B must be non-zero.
inline int LsbIndex(uint64_t B) {
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(B);
#else
    int I = 0; while (!((B >> I) & 1)) ++I; return I;
#endif
}

// Pop (clear + return) the least-significant set square.
inline Square PopLsb(uint64_t& B) {
    const int I = LsbIndex(B);
    B &= B - 1;
    return static_cast<Square>(I);
}

// --- Precomputed leaper attack tables (compile-time) ---

constexpr std::array<uint64_t, 64> ComputeKnightTable() {
    std::array<uint64_t, 64> T{};
    const int DF[8] = { 1, 2, 2, 1, -1, -2, -2, -1 };
    const int DR[8] = { 2, 1, -1, -2, -2, -1, 1, 2 };
    for (int S = 0; S < 64; ++S) {
        const int F = S & 7, R = S >> 3;
        uint64_t B = 0;
        for (int K = 0; K < 8; ++K) {
            const int NF = F + DF[K], NR = R + DR[K];
            if (NF >= 0 && NF < 8 && NR >= 0 && NR < 8) B |= uint64_t{1} << (NR * 8 + NF);
        }
        T[S] = B;
    }
    return T;
}

constexpr std::array<uint64_t, 64> ComputeKingTable() {
    std::array<uint64_t, 64> T{};
    for (int S = 0; S < 64; ++S) {
        const int F = S & 7, R = S >> 3;
        uint64_t B = 0;
        for (int DF = -1; DF <= 1; ++DF)
            for (int DR = -1; DR <= 1; ++DR) {
                if (DF == 0 && DR == 0) continue;
                const int NF = F + DF, NR = R + DR;
                if (NF >= 0 && NF < 8 && NR >= 0 && NR < 8) B |= uint64_t{1} << (NR * 8 + NF);
            }
        T[S] = B;
    }
    return T;
}

constexpr std::array<std::array<uint64_t, 64>, 2> ComputePawnTable() {
    std::array<std::array<uint64_t, 64>, 2> T{};
    for (int S = 0; S < 64; ++S) {
        const int F = S & 7, R = S >> 3;
        uint64_t W = 0, B = 0;
        if (R < 7) { if (F > 0) W |= uint64_t{1} << ((R + 1) * 8 + F - 1);
                     if (F < 7) W |= uint64_t{1} << ((R + 1) * 8 + F + 1); }
        if (R > 0) { if (F > 0) B |= uint64_t{1} << ((R - 1) * 8 + F - 1);
                     if (F < 7) B |= uint64_t{1} << ((R - 1) * 8 + F + 1); }
        T[0][S] = W;  // EColor::White == 0
        T[1][S] = B;  // EColor::Black == 1
    }
    return T;
}

inline constexpr std::array<uint64_t, 64>                KnightTable = ComputeKnightTable();
inline constexpr std::array<uint64_t, 64>                KingTable   = ComputeKingTable();
inline constexpr std::array<std::array<uint64_t, 64>, 2> PawnTable   = ComputePawnTable();

inline uint64_t KnightAttacks(Square S)            { return KnightTable[S]; }
inline uint64_t KingAttacks(Square S)              { return KingTable[S]; }
inline uint64_t PawnAttacks(Square S, EColor C)    { return PawnTable[static_cast<int>(C)][S]; }

// --- Slider attacks by ray-scan against occupancy ---

inline uint64_t SlideRay(Square S, uint64_t Occ, int DF, int DR) {
    uint64_t B = 0;
    int F = FileOf(S) + DF, R = RankOf(S) + DR;
    while (F >= 0 && F < 8 && R >= 0 && R < 8) {
        const Square T = static_cast<Square>(R * 8 + F);
        B |= Bit(T);
        if (Occ & Bit(T)) break;  // ray stops at (and includes) the first blocker
        F += DF; R += DR;
    }
    return B;
}

inline uint64_t BishopAttacks(Square S, uint64_t Occ) {
    return SlideRay(S, Occ, 1, 1) | SlideRay(S, Occ, 1, -1)
         | SlideRay(S, Occ, -1, 1) | SlideRay(S, Occ, -1, -1);
}

inline uint64_t RookAttacks(Square S, uint64_t Occ) {
    return SlideRay(S, Occ, 1, 0) | SlideRay(S, Occ, -1, 0)
         | SlideRay(S, Occ, 0, 1) | SlideRay(S, Occ, 0, -1);
}

inline uint64_t QueenAttacks(Square S, uint64_t Occ) {
    return BishopAttacks(S, Occ) | RookAttacks(S, Occ);
}

} // namespace Chess
