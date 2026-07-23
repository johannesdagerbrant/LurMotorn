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
// resync history, and the flight-recorder format are the same bytes. Since the buildings
// rework (#137/#145) the input unit is a per-tick BATCH of tick-stamped place/queue events
// (below); the old 4-bit-mask single-event codec it replaced is gone.

// ---- #137: input-EVENT batch codec (buildings rework). Replaces the 4-bit mask as the
// per-tick input payload: most ticks carry ZERO events (a 1-byte varint 0 — slimmer than the
// old mask), and a place/queue tap is a handful of bytes, occasionally. One MsgInput frame per
// produced tick carries the local team's batch; the same codec serves the resync history and
// the flight recorder (one batch per tick). Layout per batch:
//   [varint count] then per event: [kind:1][team:1]
//     place: [type:2][varint PosX.raw][varint PosY.raw]   (positions are >=0, in-bounds)
//     queue: [varint slot][varint count]
// MaxEventsPerTick lives in Rps/Sim.h (Core) — shared by the sim/runner/AI and this codec.
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

}  // namespace Rps
