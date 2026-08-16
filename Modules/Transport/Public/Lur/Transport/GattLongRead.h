#pragma once
#include <cstddef>

namespace Lur::Transport {

// How many bytes one ATT read may return — the GATT server side of a LONG READ.
//
// This is five lines of arithmetic, and it has broken the cross-platform link TWICE:
//
//   #17   the Android GATT server ignored the read OFFSET and returned the whole 32-byte device id
//         for every request, corrupting the central's reassembly.
//   #206  the offset was honoured but the LENGTH never was — every remaining byte was returned
//         regardless of the connection's ATT MTU. Measured on the pair 2026-08-16: every outgoing
//         iPhone attempt stalled at the device-id read (`stalled at CharsFound`), with
//         didUpdateValueForCharacteristic never firing at all, while Android logged
//         `serve device id: offset=0 -> 32B` and no continuation at any non-zero offset.
//
// The failure mode is worth stating because it is not intuitive: claiming to have sent MORE than a
// response can carry does not truncate gracefully. The central never learns there is a tail, never
// issues the Read Blob, and the read simply never completes — no value, no error, no timeout of its
// own. It looks exactly like a peer that went quiet.
//
// So this is a DECISION (how much do we send?) living in engine C++ under tests, not ceremony in a
// platform file. Only the Android server needs it today — iOS gives CoreBluetooth a static
// characteristic value and lets it chunk — but a rule that has cost two link outages earns a test
// regardless of how many callers it has.
//
// The ATT rules encoded here:
//   * A Read Response and a Read Blob Response each carry at most MTU-1 bytes (one opcode byte).
//   * The default ATT MTU is 23; no connection is below it, so anything smaller is clamped.
//   * Offset == size is legal and returns zero bytes — that is how a long read terminates.
//   * Offset > size is an error (ATT INVALID_OFFSET), and must be distinguishable from the empty
//     terminating read, or a central spins on it.

// The ATT default, in force until an MTU exchange says otherwise.
inline constexpr std::size_t AttDefaultMtu = 23;

// Returned for an offset past the end of the value: respond with ATT INVALID_OFFSET, not an empty
// success. Negative so it can never be mistaken for a length.
inline constexpr int GattReadInvalidOffset = -1;

// Bytes to return for a read of `ValueSize` at `Offset` on a connection whose ATT MTU is `AttMtu`.
inline int GattReadLength(std::size_t ValueSize, std::size_t Offset, std::size_t AttMtu) {
    if (Offset > ValueSize) return GattReadInvalidOffset;
    const std::size_t Mtu = AttMtu < AttDefaultMtu ? AttDefaultMtu : AttMtu;
    const std::size_t Room = Mtu - 1;                 // one byte of opcode
    const std::size_t Remaining = ValueSize - Offset;
    return static_cast<int>(Remaining < Room ? Remaining : Room);
}

}  // namespace Lur::Transport
