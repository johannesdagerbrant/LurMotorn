#include "Lur/Audio/PcmCodec.h"

#include <cstdint>
#include <cstdlib>   // std::abs

// LSF1 lossless codec. See PcmCodec.h for the format overview. Everything here is integer
// and self-contained (its own tiny bit I/O) so the exact same translation unit compiles
// into both the engine and the offline cook encoder — guaranteeing encode/decode symmetry.

namespace Lur::Audio {
namespace {

constexpr uint32_t BlockFrames = 4096;  // samples per predictor/Rice block
constexpr uint32_t EscapeQ = 40;        // unary quotient at/above which we store raw (bounds worst case)
constexpr int MaxK = 24;                // Rice parameter search ceiling

// --- bit I/O (MSB-first) ------------------------------------------------------------------

struct BitWriter {
    std::vector<uint8_t>& Out;
    uint8_t Cur = 0;
    int NBits = 0;

    explicit BitWriter(std::vector<uint8_t>& O) : Out(O) {}

    void PutBit(uint32_t B) {
        Cur = static_cast<uint8_t>((Cur << 1) | (B & 1u));
        if (++NBits == 8) { Out.push_back(Cur); Cur = 0; NBits = 0; }
    }
    void PutBits(uint32_t V, int N) {
        for (int i = N - 1; i >= 0; --i) PutBit((V >> i) & 1u);
    }
    void PutUnary(uint32_t Q) {           // Q ones then a terminating zero
        for (uint32_t i = 0; i < Q; ++i) PutBit(1);
        PutBit(0);
    }
    void Flush() {                        // pad the final partial byte with zeros
        if (NBits) { Cur = static_cast<uint8_t>(Cur << (8 - NBits)); Out.push_back(Cur); Cur = 0; NBits = 0; }
    }
};

struct BitReader {
    const uint8_t* Data;
    size_t Size;
    size_t Pos = 0;
    uint8_t Cur = 0;
    int NBits = 0;
    bool Overran = false;

    BitReader(const uint8_t* D, size_t S, size_t Start) : Data(D), Size(S), Pos(Start) {}

    uint32_t GetBit() {
        if (NBits == 0) {
            if (Pos < Size) { Cur = Data[Pos++]; }
            else { Cur = 0; Overran = true; }
            NBits = 8;
        }
        --NBits;
        return (Cur >> NBits) & 1u;
    }
    uint32_t GetBits(int N) {
        uint32_t V = 0;
        for (int i = 0; i < N; ++i) V = (V << 1) | GetBit();
        return V;
    }
    uint32_t GetUnary() {
        uint32_t Q = 0;
        while (GetBit()) { if (++Q > (1u << 20)) { Overran = true; break; } }
        return Q;
    }
};

// --- zigzag map (signed residual <-> unsigned) --------------------------------------------

inline uint32_t ZigZag(int32_t R) { return (static_cast<uint32_t>(R) << 1) ^ static_cast<uint32_t>(R >> 31); }
inline int32_t UnZigZag(uint32_t U) { return static_cast<int32_t>(U >> 1) ^ -static_cast<int32_t>(U & 1u); }

// Fixed FLAC-style predictor of order 0-3 at index i, reading already-known samples
// (< 0 treated as 0). Returns the predicted sample; residual = actual - prediction.
inline int32_t Predict(const int16_t* X, int64_t I, int Order) {
    const int32_t x1 = I >= 1 ? X[I - 1] : 0;
    const int32_t x2 = I >= 2 ? X[I - 2] : 0;
    const int32_t x3 = I >= 3 ? X[I - 3] : 0;
    switch (Order) {
        case 0:  return 0;
        case 1:  return x1;
        case 2:  return 2 * x1 - x2;
        default: return 3 * x1 - 3 * x2 + x3;   // order 3
    }
}

// Rice code length in bits for unsigned U with parameter K (matching the writer below).
inline uint32_t RiceLen(uint32_t U, int K) {
    const uint32_t Q = U >> K;
    return Q >= EscapeQ ? (EscapeQ + 1 + 32) : (Q + 1 + static_cast<uint32_t>(K));
}
inline void RiceWrite(BitWriter& W, uint32_t U, int K) {
    const uint32_t Q = U >> K;
    if (Q >= EscapeQ) { W.PutUnary(EscapeQ); W.PutBits(U, 32); }
    else { W.PutUnary(Q); W.PutBits(U & ((1u << K) - 1u), K); }
}
inline uint32_t RiceRead(BitReader& R, int K) {
    const uint32_t Q = R.GetUnary();
    if (Q >= EscapeQ) return R.GetBits(32);
    return (Q << K) | (K ? R.GetBits(K) : 0u);
}

} // namespace

std::vector<uint8_t> EncodeLossless(const int16_t* Pcm, uint32_t Frames, uint32_t Rate) {
    std::vector<uint8_t> Out;
    Out.reserve(Frames + 12);
    const uint8_t Header[12] = {
        'L', 'S', 'F', '1',
        static_cast<uint8_t>(Rate), static_cast<uint8_t>(Rate >> 8),
        static_cast<uint8_t>(Rate >> 16), static_cast<uint8_t>(Rate >> 24),
        static_cast<uint8_t>(Frames), static_cast<uint8_t>(Frames >> 8),
        static_cast<uint8_t>(Frames >> 16), static_cast<uint8_t>(Frames >> 24),
    };
    Out.insert(Out.end(), Header, Header + 12);

    BitWriter W(Out);
    std::vector<int32_t> Res(BlockFrames);

    for (uint32_t Start = 0; Start < Frames; Start += BlockFrames) {
        const uint32_t N = (Frames - Start < BlockFrames) ? (Frames - Start) : BlockFrames;

        // Pick the predictor order with the smallest residual magnitude for this block.
        int BestOrder = 0;
        uint64_t BestAbs = UINT64_MAX;
        for (int Order = 0; Order <= 3; ++Order) {
            uint64_t Sum = 0;
            for (uint32_t j = 0; j < N; ++j)
                Sum += static_cast<uint32_t>(std::abs(Pcm[Start + j] - Predict(Pcm, Start + j, Order)));
            if (Sum < BestAbs) { BestAbs = Sum; BestOrder = Order; }
        }
        for (uint32_t j = 0; j < N; ++j)
            Res[j] = Pcm[Start + j] - Predict(Pcm, Start + j, BestOrder);

        // Brute-force the Rice parameter (offline cost is irrelevant; pick the exact best).
        int BestK = 0;
        uint64_t BestBits = UINT64_MAX;
        for (int K = 0; K <= MaxK; ++K) {
            uint64_t Bits = 0;
            for (uint32_t j = 0; j < N; ++j) Bits += RiceLen(ZigZag(Res[j]), K);
            if (Bits < BestBits) { BestBits = Bits; BestK = K; }
        }

        W.PutBits(static_cast<uint32_t>(BestOrder), 2);
        W.PutBits(static_cast<uint32_t>(BestK), 5);
        for (uint32_t j = 0; j < N; ++j) RiceWrite(W, ZigZag(Res[j]), BestK);
    }

    W.Flush();
    return Out;
}

bool DecodeLossless(const uint8_t* Data, size_t Size, Sound& Out) {
    if (Size < 12 || Data[0] != 'L' || Data[1] != 'S' || Data[2] != 'F' || Data[3] != '1')
        return false;
    const uint32_t Rate = static_cast<uint32_t>(Data[4]) | (static_cast<uint32_t>(Data[5]) << 8)
                        | (static_cast<uint32_t>(Data[6]) << 16) | (static_cast<uint32_t>(Data[7]) << 24);
    const uint32_t Frames = static_cast<uint32_t>(Data[8]) | (static_cast<uint32_t>(Data[9]) << 8)
                        | (static_cast<uint32_t>(Data[10]) << 16) | (static_cast<uint32_t>(Data[11]) << 24);

    Out.Rate = Rate;
    Out.Pcm.assign(Frames, 0);
    int16_t* X = Out.Pcm.data();

    BitReader R(Data, Size, 12);
    for (uint32_t Start = 0; Start < Frames; Start += BlockFrames) {
        const uint32_t N = (Frames - Start < BlockFrames) ? (Frames - Start) : BlockFrames;
        const int Order = static_cast<int>(R.GetBits(2));
        const int K = static_cast<int>(R.GetBits(5));
        for (uint32_t j = 0; j < N; ++j) {
            const int32_t Residual = UnZigZag(RiceRead(R, K));
            const int32_t S = Predict(X, Start + j, Order) + Residual;
            X[Start + j] = static_cast<int16_t>(S);   // lossless: S is always in int16 range
        }
        if (R.Overran) return false;
    }
    return !R.Overran;
}

} // namespace Lur::Audio
