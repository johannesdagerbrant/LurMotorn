#pragma once
#include <cstdint>

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

}  // namespace Rps
