#pragma once
#include "Lur/Transport/Transport.h"

namespace Lur::Transport {

// BLE is the only serverless local link that works BETWEEN iOS and Android:
//   - Bluetooth Classic / RFCOMM is locked to MFi hardware on iOS.
//   - NFC peer handover isn't available to apps on iOS.
//
// One device hosts a GATT *peripheral* exposing a LurMotorn service with a single
// writable + notifiable characteristic that acts as the datagram channel; the
// other connects as *central*. After the pairing module selects roles, the link
// presents itself through the platform-neutral ITransport above.
//
// The concrete classes that implement this contract live in the app builds:
//   - Android: a C++ shim over a Kotlin BluetoothGatt* layer, bridged via JNI.
//   - iOS:     a C++/Obj-C++ shim over CoreBluetooth (CBPeripheralManager/CBCentralManager).
//
// This header is the seam they share. The factory is declared here and defined
// per platform so engine code can obtain a transport without naming a backend.

enum class EBleRole { Peripheral, Central };

// Implemented separately in Games/Chess/Android and Games/Chess/iOS.
ITransport* CreateBleTransport(EBleRole Role);

} // namespace Lur::Transport
