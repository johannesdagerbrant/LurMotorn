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
//   Characteristic 4C55524D-4F54-4F52-4E01-446174616772 = "LURM" "OT" "OR" "N\1" "Datagr"
// They share a prefix (same product family) and differ only in the 4th group, so
// the datagram characteristic is visibly a child of the LurMotorn service.

// GATT service the peripheral exposes and the central scans for.
inline constexpr std::string_view BleServiceUuid =
    "4C55524D-4F54-4F52-4E00-5472616E7370";

// The single writable + notifiable characteristic that carries datagrams both
// ways (central writes to it; peripheral notifies on it).
inline constexpr std::string_view BleDatagramCharacteristicUuid =
    "4C55524D-4F54-4F52-4E01-446174616772";

// Short name the peripheral puts in its advertisement, so a human scanning sees
// something recognizable. Kept tiny — BLE advertisement payloads are ~31 bytes.
inline constexpr std::string_view BleAdvertisedName = "LurMotorn";

// Pick GATT roles without negotiation.
//
// Discovery is symmetric: both phones advertise AND scan, so both end up here
// with the two ids swapped (each sees itself as Local and the other as Peer). A
// total order on the ids hands them OPPOSITE answers for free — the smaller id
// hosts the peripheral, the larger connects as central — so they never both try
// to be the server. `LocalId`/`PeerId` are the platform BLE identifiers; they are
// distinct for two distinct devices.
inline EBleRole DecideBleRole(std::string_view LocalId, std::string_view PeerId) {
    return LocalId < PeerId ? EBleRole::Peripheral : EBleRole::Central;
}

} // namespace Lur::Transport
