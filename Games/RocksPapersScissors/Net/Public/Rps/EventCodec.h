#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Serialization/Varint.h"

namespace Rps {

// The RPS input event codec — ONE codec, three uses (design doc §2): the live wire,
// resync history, and the flight-recorder format are the same bytes.
//
// One event is a byte: [delta:4 | mask:4].
//   * mask  (low nibble): the 4-bit button mask this tick. 0000 = watermark ("I have
//     no input through this tick") — the idle case IS an event, so the protocol has no
//     idle special case.
//   * delta (high nibble): ticks since the previous event. On the LIVE wire it is
//     always 1 (per-tick input-or-empty, tick implicit by message count), so an event
//     is exactly 1 byte; the nibble is kept anyway so the SAME codec serves the sparse
//     cases (resync/recorder) where only non-empty ticks are stored and deltas vary.
//   * delta == 15 is the ESCAPE: a varint follows with the true delta (protected by
//     #48's varint shift guard — this format is exactly why that guard exists).
//
// Byte budget: any delta in 1..14 with any mask encodes to a single byte; framed
// (type byte + this byte) that is the 2-byte press/watermark the CI budget asserts.

// Largest delta that fits the nibble without the varint escape.
constexpr uint32_t EventDeltaInline = 15;

inline void EncodeEvent(Lur::Serialization::BitWriter& W, uint32_t Delta, uint8_t Mask) {
    const uint32_t Nib = Delta < EventDeltaInline ? Delta : EventDeltaInline;
    W.WriteBits(Nib, 4);                 // high nibble
    W.WriteBits(Mask & 0xFu, 4);         // low nibble -> one full byte
    if (Delta >= EventDeltaInline) Lur::Serialization::WriteVarUint(W, Delta);
}

// Decode one event. Returns false if the read ran past the end (caller stops the
// stream loop). Never traps on hostile input — the varint guard + BitReader::IsOk
// keep it total.
inline bool DecodeEvent(Lur::Serialization::BitReader& R, uint32_t& Delta, uint8_t& Mask) {
    const uint32_t Nib = R.ReadBits(4);
    Mask = static_cast<uint8_t>(R.ReadBits(4));
    Delta = Nib < EventDeltaInline ? Nib : static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
    return R.IsOk();
}

// ---- Resync: the SAME event codec, chunked for the cold-rejoin / blip path (design §4).
// A dense input-history stream (one mask per tick from FirstTick) is framed into chunks,
// each no larger than MaxResyncChunkBytes so nothing exceeds the transport's framed
// payload cap (chess's #74 marathon-wedge is designed out): [varint firstTick][varint
// count][count dense events]. Blip = one small chunk; cold rejoin = the whole history in
// order (reliable GATT makes reassembly an append). Worst case (mask-per-tick collapse):
// a 15-min game is ≤ 9 KB ≈ 18 chunks per stream. The rejoiner free-runs the decoded
// stream through a fresh sim (the replay law), then snaps to the frontier.
constexpr std::size_t MaxResyncChunkBytes = 512;
constexpr std::size_t ResyncTicksPerChunk = 500;  // 500 x 1-byte events + a ~6-byte header < 512

inline std::vector<std::vector<uint8_t>> EncodeResyncChunks(uint32_t FirstTick,
                                                            const std::vector<uint8_t>& Masks) {
    std::vector<std::vector<uint8_t>> Chunks;
    std::size_t I = 0;
    while (I < Masks.size()) {
        const std::size_t Count =
            (Masks.size() - I) < ResyncTicksPerChunk ? (Masks.size() - I) : ResyncTicksPerChunk;
        Lur::Serialization::BitWriter W;
        Lur::Serialization::WriteVarUint(W, FirstTick + static_cast<uint32_t>(I));
        Lur::Serialization::WriteVarUint(W, static_cast<uint32_t>(Count));
        for (std::size_t K = 0; K < Count; ++K) EncodeEvent(W, 1, Masks[I + K]);  // dense: delta 1
        const std::vector<uint8_t>& B = W.Finish();
        Chunks.emplace_back(B.begin(), B.end());
        I += Count;
    }
    return Chunks;
}

// Decode one chunk, appending its masks to OutMasks (reliable ordered transport delivers
// chunks in order, so reassembly is an append). Returns false on a malformed/hostile
// chunk — total, never traps.
inline bool DecodeResyncChunk(const uint8_t* Data, std::size_t N, uint32_t& OutFirstTick,
                              std::vector<uint8_t>& OutMasks) {
    Lur::Serialization::BitReader R(Data, N);
    OutFirstTick = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
    const uint32_t Count = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
    if (!R.IsOk() || Count > 100000u) return false;  // sanity bound vs hostile input
    for (uint32_t K = 0; K < Count; ++K) {
        uint32_t D = 0;
        uint8_t M = 0;
        if (!DecodeEvent(R, D, M)) return false;
        OutMasks.push_back(M);
    }
    return R.IsOk();
}

}  // namespace Rps
