#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Lur/Serialization/BitReader.h"
#include "Lur/Serialization/BitWriter.h"
#include "Lur/Serialization/Varint.h"
#include "Rps/Sim.h"  // InputEvent (#137)

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

// ---- #137: input-EVENT batch codec (buildings rework). Replaces the 4-bit mask as the
// per-tick input payload: most ticks carry ZERO events (a 1-byte varint 0 — slimmer than the
// old mask), and a place/queue tap is a handful of bytes, occasionally. One MsgInput frame per
// produced tick carries the local team's batch; the same codec serves the resync history and
// the flight recorder (one batch per tick). Layout per batch:
//   [varint count] then per event: [kind:1][team:1]
//     place: [type:2][varint PosX.raw][varint PosY.raw]   (positions are >=0, in-bounds)
//     queue: [varint slot][varint count]
constexpr int MaxEventsPerTick = 16;  // a human can't issue more than a few taps per 100 ms tick
constexpr std::size_t MaxResyncChunkBytes = 512;  // transport's framed payload cap (also used below)

inline void EncodeEventBatch(Lur::Serialization::BitWriter& W, const InputEvent* Evs, int Count) {
    Lur::Serialization::WriteVarUint(W, static_cast<uint32_t>(Count));
    for (int I = 0; I < Count; ++I) {
        const InputEvent& E = Evs[I];
        W.WriteBits(E.Kind & 1u, 1);
        W.WriteBits(E.Team & 1u, 1);
        if (E.Kind == EventPlaceBuilding) W.WriteBits(E.Type & 3u, 2);
        Lur::Serialization::WriteVarUint(W, static_cast<uint32_t>(E.X));
        Lur::Serialization::WriteVarUint(W, static_cast<uint32_t>(E.Y));
    }
}

// Decode a batch into Out (capacity MaxOut). Returns the event count, or -1 on malformed /
// hostile input (never traps — BitReader::IsOk + the count bound keep it total).
inline int DecodeEventBatch(Lur::Serialization::BitReader& R, InputEvent* Out, int MaxOut) {
    const uint32_t Count = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
    if (!R.IsOk() || Count > static_cast<uint32_t>(MaxOut)) return -1;
    for (uint32_t I = 0; I < Count; ++I) {
        InputEvent E{};
        E.Kind = static_cast<uint8_t>(R.ReadBits(1));
        E.Team = static_cast<uint8_t>(R.ReadBits(1));
        if (E.Kind == EventPlaceBuilding) E.Type = static_cast<uint8_t>(R.ReadBits(2));
        E.X = static_cast<int32_t>(Lur::Serialization::ReadVarUint(R));
        E.Y = static_cast<int32_t>(Lur::Serialization::ReadVarUint(R));
        if (!R.IsOk()) return -1;
        Out[I] = E;
    }
    return R.IsOk() ? static_cast<int>(Count) : -1;
}

// ---- #137: EVENT-history resync (buildings rework). The cold-rejoin/blip path serialises the
// executed COMBINED per-tick batches (team0's events then team1's, the order Execute applies)
// so a rejoiner free-runs them through a fresh sim (the replay law) and snaps to the frontier.
// Byte-bounded chunking: a tick batch is variable-length now (not the old 1-byte mask), so pack
// as many whole ticks as fit under MaxResyncChunkBytes. Header: [varint firstTick][varint
// tickCount] then tickCount batches. A single batch is <=~160 B (<< the cap), so every chunk
// holds >=1 tick.
inline std::vector<std::vector<uint8_t>> EncodeEventResyncChunks(
        uint32_t FirstTick, const std::vector<std::vector<InputEvent>>& Hist) {
    std::vector<std::vector<uint8_t>> Chunks;
    std::size_t I = 0;
    while (I < Hist.size()) {
        const std::size_t Start = I;
        std::size_t Bits = 0;
        while (I < Hist.size()) {
            Lur::Serialization::BitWriter Tmp;
            EncodeEventBatch(Tmp, Hist[I].data(), static_cast<int>(Hist[I].size()));
            const std::size_t Add = Tmp.GetBitCount();
            // Reserve ~8 header bytes; require at least one tick per chunk.
            if (I > Start && (Bits + Add) / 8 + 8 > MaxResyncChunkBytes) break;
            Bits += Add;
            ++I;
        }
        Lur::Serialization::BitWriter W;
        Lur::Serialization::WriteVarUint(W, FirstTick + static_cast<uint32_t>(Start));
        Lur::Serialization::WriteVarUint(W, static_cast<uint32_t>(I - Start));
        for (std::size_t T = Start; T < I; ++T)
            EncodeEventBatch(W, Hist[T].data(), static_cast<int>(Hist[T].size()));
        const std::vector<uint8_t>& B = W.Finish();
        Chunks.emplace_back(B.begin(), B.end());
    }
    return Chunks;
}

// Decode one event-history chunk, appending its per-tick batches to Out (reliable ordered
// transport -> append). Total on hostile input (never traps).
inline bool DecodeEventResyncChunk(const uint8_t* Data, std::size_t N, uint32_t& OutFirstTick,
                                   std::vector<std::vector<InputEvent>>& Out) {
    Lur::Serialization::BitReader R(Data, N);
    OutFirstTick = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
    const uint32_t Count = static_cast<uint32_t>(Lur::Serialization::ReadVarUint(R));
    if (!R.IsOk() || Count > 100000u) return false;
    for (uint32_t K = 0; K < Count; ++K) {
        InputEvent Buf[MaxEventsPerTick];
        const int M = DecodeEventBatch(R, Buf, MaxEventsPerTick);
        if (M < 0) return false;
        Out.emplace_back(Buf, Buf + M);
    }
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
