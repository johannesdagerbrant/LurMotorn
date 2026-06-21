#pragma once
#include <string_view>
#include "Lur/Transport/Ble.h"

namespace Lur::Transport {

// The cross-platform BLE wire identity, defined ONCE so the Android (Kotlin) and
// iOS (CoreBluetooth) backends advertise and scan for the exact same thing. If
// these constants lived separately in Kotlin and Swift they could drift, and two
// phones would silently never see each other. They are part of the protocol:
// changing a UUID is a breaking wire change (bump Lur::Net::ProtocolVersion), the
// same rule that governs the chess move ordering.
//
// The 128-bit UUIDs are ASCII-encoded for memorability — every byte is a printable
// character, which is perfectly legal (a UUID is just a 128-bit number):
//   Service        4C55524D-4F54-4F52-4E00-5472616E7370 = "LURM" "OT" "OR" "N\0" "Transp"
//   Datagram char  4C55524D-4F54-4F52-4E01-446174616772 = "LURM" "OT" "OR" "N\1" "Datagr"
//   Nonce char     4C55524D-4F54-4F52-4E02-4E6F6E636500 = "LURM" "OT" "OR" "N\2" "Nonce\0"
// They share a prefix (same product family) and differ only in the 4th group, so
// the characteristics are visibly children of the LurMotorn service.

// GATT service the peripheral exposes and the central scans for.
inline constexpr std::string_view BleServiceUuid =
    "4C55524D-4F54-4F52-4E00-5472616E7370";

// The single writable + notifiable characteristic that carries datagrams both
// ways (central writes to it; peripheral notifies on it).
inline constexpr std::string_view BleDatagramCharacteristicUuid =
    "4C55524D-4F54-4F52-4E01-446174616772";

// Readable characteristic exposing this device's random session nonce, read by
// the central right after connecting to drive the role handshake below.
inline constexpr std::string_view BleNonceCharacteristicUuid =
    "4C55524D-4F54-4F52-4E02-4E6F6E636500";

// Short name the peripheral puts in its advertisement, so a human scanning sees
// something recognizable. Kept tiny — BLE advertisement payloads are ~31 bytes.
inline constexpr std::string_view BleAdvertisedName = "LurMotorn";

// Pick GATT roles from two session nonces.
//
// The role is decided IN-BAND, after connecting — NOT from the advertisement.
// iOS apps cannot put custom data (a nonce) in a BLE advertisement (CoreBluetooth
// only advertises the local name + service UUIDs), so a pre-connection tie-break
// is impossible cross-platform. Instead:
//
//   1. Both phones advertise only the service UUID, both scan, and both run a GATT
//      server exposing the nonce characteristic (their own random session nonce).
//   2. On discovering a peer, a phone connects as central and READS the peer's
//      nonce characteristic.
//   3. DecideBleRole(MyNonce, PeerNonce) picks the canonical role: the smaller
//      nonce is the peripheral, the larger the central. The phone that finds it
//      should be Central keeps the connection (the live link); the phone that
//      finds it should be Peripheral drops that connection and keeps serving,
//      letting its peer connect to it as central. Both keep advertising + scanning
//      until the canonical link is established, so it self-corrects.
//
// A total order on the nonces hands the two phones OPPOSITE answers for free.
// Nonces are distinct for two devices (a collision is re-rolled by the backend).
inline EBleRole DecideBleRole(std::string_view LocalNonce, std::string_view PeerNonce) {
    return LocalNonce < PeerNonce ? EBleRole::Peripheral : EBleRole::Central;
}

} // namespace Lur::Transport
