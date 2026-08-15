#pragma once
#include "Lur/Transport/Transport.h"

namespace Lur::Transport {

// BLE is the only serverless local link that works BETWEEN iOS and Android:
//   - Bluetooth Classic / RFCOMM is locked to MFi hardware on iOS.
//   - NFC peer handover isn't available to apps on iOS.
//
// One device hosts a GATT *peripheral* exposing a LurMotorn service with a single
// writable + notifiable characteristic that acts as the datagram channel; the
// other connects as *central*. The link then presents itself through the
// platform-neutral ITransport above.
//
// The concrete drivers live in the ENGINE, one per platform:
//   - Modules/Transport/Platform/Android — a C++ shim over a Kotlin BluetoothGatt*
//     layer, bridged via JNI.
//   - Modules/Transport/Platform/Ios     — C++/Obj-C++ over CoreBluetooth
//     (CBPeripheralManager / CBCentralManager).
//
// This header is the seam they share. The factory is declared here and defined
// per platform so engine code can obtain a transport without naming a backend.

// WHO IS PERIPHERAL IS DECIDED AT RUNTIME, NOT AT CONSTRUCTION. Both peers run the
// same tie-break over the two device GUIDs (DecideBleRole in BleProtocol.h), which is
// what keeps the design symmetric: the peripheral/central split is a radio mechanic
// only and confers no authority. Do not reintroduce a role parameter here — the old
// one was ignored by both backends while the mains passed contradictory values
// (Android "Central", iOS "Peripheral"), which read as if the platform chose (#47).
enum class EBleRole { Peripheral, Central };

// Defined once per PLATFORM in the engine — a link-time seam, not an interface, because
// there is one implementation per platform and never two at runtime.
ITransport* CreateBleTransport();

} // namespace Lur::Transport
