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
//   Device-id char 4C55524D-4F54-4F52-4E02-4E6F6E636500 = "LURM" "OT" "OR" "N\2" "Nonce\0"
// They share a prefix (same product family) and differ only in the 4th group, so
// the characteristics are visibly children of the LurMotorn service. (The device-id
// characteristic's UUID bytes still spell "Nonce" — its endpoint is unchanged; only
// what it carries changed from a random session nonce to the persistent device id,
// see below — so the UUID is kept to avoid churning the wire identity.)

// GATT service the peripheral exposes and the central scans for.
inline constexpr std::string_view BleServiceUuid =
    "4C55524D-4F54-4F52-4E00-5472616E7370";

// The single writable + notifiable characteristic that carries datagrams both
// ways (central writes to it; peripheral notifies on it).
inline constexpr std::string_view BleDatagramCharacteristicUuid =
    "4C55524D-4F54-4F52-4E01-446174616772";

// Readable characteristic exposing this device's PERSISTENT device id (a 128-bit
// GUID, hex-encoded; see Lur::Save::LoadOrCreateDeviceId), read by the central
// right after connecting to drive the role handshake below. It is persistent, not
// a fresh per-session value, so the role it settles is STABLE across app restarts
// — which is what lets a restarted app rejoin an existing peer instead of flipping
// roles and stranding it (issue #17).
inline constexpr std::string_view BleDeviceIdCharacteristicUuid =
    "4C55524D-4F54-4F52-4E02-4E6F6E636500";

// Short name the peripheral puts in its advertisement, so a human scanning sees
// something recognizable. Kept tiny — BLE advertisement payloads are ~31 bytes.
inline constexpr std::string_view BleAdvertisedName = "LurMotorn";

// Pick GATT roles from the two devices' persistent device ids.
//
// The role is decided IN-BAND, after connecting — NOT from the advertisement.
// iOS apps cannot put custom data (an id) in a BLE advertisement (CoreBluetooth
// only advertises the local name + service UUIDs), so a pre-connection tie-break
// is impossible cross-platform. Instead:
//
//   1. Both phones advertise only the service UUID, both scan, and both run a GATT
//      server exposing the device-id characteristic (their own persistent GUID).
//   2. On discovering a peer, a phone connects as central and READS the peer's
//      device-id characteristic.
//   3. DecideBleRole(MyId, PeerId) picks the canonical role: the smaller id is the
//      peripheral, the larger the central. The phone that finds it should be
//      Central keeps the connection (the live link); the phone that finds it
//      should be Peripheral drops that connection and keeps serving, letting its
//      peer connect to it as central. Both keep advertising + scanning until the
//      canonical link is established, so it self-corrects.
//
// A total order on the ids hands the two phones OPPOSITE answers for free. Because
// the ids are PERSISTENT (not per-session), the same two phones settle the SAME
// roles on every reconnect — no role flip on restart (issue #17). Ids are distinct
// for two devices (a 128-bit-space collision is negligible). Comparison is a plain
// lexicographic compare of the hex, which for fixed-width hex equals a numeric
// compare of the underlying 128-bit value.
#if LUR_INTERNAL
// Dev-only role override (compiled out of Shipping): pin THIS device's role regardless
// of the GUID tie-break, so the rig can exercise BOTH role configurations on the same
// device pair — the tie-break is deterministic, so e.g. Android-as-peripheral would
// otherwise never run against a given peer. Set complementary values on the two phones
// (one Central, one Peripheral) or they will never link (two centrals: nobody
// advertises). Sourced per-platform: Android `debug.lur.role` prop, iOS a
// `Documents/role` marker — both read at transport startup.
inline int GBleRoleOverride = -1;  // -1 none, else static_cast<int>(EBleRole)
inline void SetBleRoleOverride(EBleRole Role) { GBleRoleOverride = static_cast<int>(Role); }
inline void ClearBleRoleOverride() { GBleRoleOverride = -1; }
#endif

inline EBleRole DecideBleRole(std::string_view LocalId, std::string_view PeerId) {
#if LUR_INTERNAL
    if (GBleRoleOverride >= 0) return static_cast<EBleRole>(GBleRoleOverride);
#endif
    return LocalId < PeerId ? EBleRole::Peripheral : EBleRole::Central;
}

} // namespace Lur::Transport
