#pragma once
#include <string>
#include "Lur/Save/Store.h"

namespace Lur::Save {

// The storage key under which the persistent device GUID lives.
inline constexpr const char* DeviceIdKey = "device-id";

// The storage key for the LAST linked peer's device id. Caching it lets a reconnect
// skip the discovery role tie-break: a device that already knows the peer's id can
// compute its role up front (DecideBleRole) and, if it is the peripheral, simply
// advertise and wait instead of also connecting out — which removes the mutual-
// connect collision that stalls reconnect (issue #17 Step 3). Single last-peer for
// now; becomes a per-opponent record in Phase B (#18).
inline constexpr const char* PeerIdKey = "peer-id";

// The GUID is 128 random bits rendered as 32 lowercase hex characters.
inline constexpr std::size_t DeviceIdHexLen = 32;

// Return this install's persistent device identity, generating it once on first
// call and returning the SAME value on every call thereafter (issue #17).
//
// This GUID is the linchpin of the persistence design: it replaces the old random
// per-session nonce so the BLE role is STABLE across app restarts (role = compare
// the two GUIDs, lower = peripheral), which is the reconnect-on-restart fix. It is
// also the per-opponent stats key and the colour seed later on.
//
// Comparison is a plain lexicographic string compare of the hex, which for
// fixed-width hex equals a numeric compare of the underlying 128-bit value — so
// "lower GUID" is well-defined and identical on both phones.
std::string LoadOrCreateDeviceId(Store& S);

} // namespace Lur::Save
