// CoreBluetooth backend of Lur::Transport::ITransport — the iOS counterpart of
// Games/Chess/Android/.../AndroidBleTransport.cpp. This is the iPhone side of the
// cross-platform BLE link that lets an iPhone and an Android phone play locally
// with no server (CLAUDE.md).
//
// Protocol identity is SHARED and defined exactly once in BleProtocol.h; this file
// uses those exact constants so it interoperates with the Kotlin/Android side:
//   - BleServiceUuid                    the GATT service both sides agree on
//   - BleDatagramCharacteristicUuid     the write+notify datagram channel
//   - BleDeviceIdCharacteristicUuid     readable; carries this device's persistent id
//   - BleAdvertisedName                 the local name in the advertisement
//   - DecideBleRole(LocalId, PeerId)    the symmetric role tie-break
//
// The role is decided IN-BAND, after connecting — NOT from the advertisement —
// because iOS cannot advertise custom data (CoreBluetooth only advertises the local
// name + service UUIDs). So both devices advertise only the service UUID, scan, and
// host a GATT server exposing the readable device-id characteristic. On discovering
// a peer, a device connects as central and READS the peer's device id; DecideBleRole
// then settles who keeps the link: the larger id stays central, the smaller drops
// the connection and serves as peripheral. Both keep advertising/scanning until the
// canonical link is up, so it self-corrects. This mirrors BleShim.kt exactly.
//
// The device id is PERSISTENT (a GUID minted once by the engine's Modules/Save and
// kept in Application Support), so the role settled above is STABLE across app
// restarts — the reconnect-on-restart fix (issue #17).
//
// STATUS: full flow implemented, NOT yet run on hardware (no Mac/iPhone at authoring
// time). The chess-core smoke test and the build are what CI proves today; the live
// link is proven by the first Android<->iPhone test.
#import <CoreBluetooth/CoreBluetooth.h>
#import <Foundation/Foundation.h>

#include <cstdint>
#include <string>
#include <vector>

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Transport/Ble.h"
#include "Lur/Transport/BleProtocol.h"

using namespace Lur::Transport;

static CBUUID* MakeUuid(std::string_view Uuid) {
    NSString* S = [[NSString alloc] initWithBytes:Uuid.data()
                                           length:(NSUInteger)Uuid.size()
                                         encoding:NSUTF8StringEncoding];
    return [CBUUID UUIDWithString:S];
}

// This device's PERSISTENT id (issue #17) — a GUID minted once by the engine's
// shared Modules/Save and kept in Application Support, so every launch reads back
// the same value and the role it drives is stable across restarts. Exchanged
// in-band over the device-id characteristic (never advertised). Android mints its
// id from the very same Modules/Save code, so the two are directly comparable.
static std::string LoadOrCreateIosDeviceId() {
    NSArray<NSString*>* Dirs =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* Dir = Dirs.firstObject ?: NSTemporaryDirectory();
    Lur::Save::Store DeviceStore(std::string(Dir.UTF8String));  // Save() creates the dir if absent
    return Lur::Save::LoadOrCreateDeviceId(DeviceStore);
}

// ---------------------------------------------------------------------------
// The Obj-C driver: owns BOTH a peripheral manager (advertise + host the service)
// and a central manager (scan + connect), and implements the delegate protocols.
// ---------------------------------------------------------------------------
@interface IosBleDriver : NSObject <CBCentralManagerDelegate,
                                    CBPeripheralManagerDelegate,
                                    CBPeripheralDelegate>
@end

@implementation IosBleDriver {
    CBCentralManager*    _Central;
    CBPeripheralManager* _Peripheral;

    CBUUID* _ServiceUuid;
    CBUUID* _DatagramUuid;
    CBUUID* _DeviceIdUuid;

    std::string _LocalId;

    // Central-side state (we connected OUT to a peer's GATT server).
    CBPeripheral*     _PeerDevice;
    CBCharacteristic* _RemoteDatagram;

    // Peripheral-side state (we host the service; peer connected to us).
    CBMutableCharacteristic* _LocalDatagram;
    CBCentral*               _Subscriber;

    bool _Connected;
    bool _Linked;            // canonical link established; stop discovery
    bool _Connecting;        // an outgoing central attempt is mid-flight
    bool _DecidedPeripheral; // we settled as peripheral; stop connecting out

    ITransport::Receiver _Receiver;
    std::vector<std::vector<uint8_t>> _Outbox;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _ServiceUuid  = MakeUuid(BleServiceUuid);
        _DatagramUuid = MakeUuid(BleDatagramCharacteristicUuid);
        _DeviceIdUuid = MakeUuid(BleDeviceIdCharacteristicUuid);
        _LocalId      = LoadOrCreateIosDeviceId();
        _Connected = _Linked = _Connecting = _DecidedPeripheral = false;
        _Central    = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
        _Peripheral = [[CBPeripheralManager alloc] initWithDelegate:self queue:nil];
        NSLog(@"OnlyChess BLE: driver up, local device id=%s", _LocalId.c_str());
    }
    return self;
}

- (void)setReceiver:(ITransport::Receiver)Receiver { _Receiver = std::move(Receiver); }
- (bool)isConnected { return _Connected; }

// ---- Outbound ----
- (void)sendData:(const uint8_t*)Data size:(std::size_t)Size {
    std::vector<uint8_t> Bytes(Data, Data + Size);
    if (!_Connected) { _Outbox.push_back(std::move(Bytes)); return; }
    [self transmit:Bytes];
}

- (void)transmit:(const std::vector<uint8_t>&)Bytes {
    NSData* Payload = [NSData dataWithBytes:Bytes.data() length:Bytes.size()];
    if (_RemoteDatagram && _PeerDevice) {            // we are central -> write
        // WithResponse: the peer's datagram characteristic only declares
        // write-with-response (PROPERTY_WRITE on Android), and chess payloads are
        // tiny so latency is irrelevant. Both platforms write-with-response.
        [_PeerDevice writeValue:Payload
              forCharacteristic:_RemoteDatagram
                           type:CBCharacteristicWriteWithResponse];
    } else if (_LocalDatagram && _Subscriber) {      // we are peripheral -> notify
        [_Peripheral updateValue:Payload
               forCharacteristic:_LocalDatagram
            onSubscribedCentrals:@[_Subscriber]];
    }
}

- (void)flushOutbox { for (auto& B : _Outbox) [self transmit:B]; _Outbox.clear(); }

- (void)deliverInbound:(NSData*)Data {
    if (_Receiver && Data.length > 0) {
        _Receiver(static_cast<const uint8_t*>(Data.bytes), (std::size_t)Data.length);
    }
}

- (void)onLinked { if (_Linked) return; _Linked = _Connected = true;
    [_Central stopScan];
    if (_Peripheral.isAdvertising) [_Peripheral stopAdvertising];
    [self flushOutbox];
    if (_RemoteDatagram && _PeerDevice) {   // central initiates the demo round-trip (ping)
        std::vector<uint8_t> Ping{0xC5};
        [self transmit:Ping];
    }
}

- (void)advertiseService {
    if (_Peripheral.state != CBManagerStatePoweredOn || _Peripheral.isAdvertising) return;
    NSString* Name = [NSString stringWithUTF8String:std::string(BleAdvertisedName).c_str()];
    [_Peripheral startAdvertising:@{
        CBAdvertisementDataServiceUUIDsKey: @[_ServiceUuid],
        CBAdvertisementDataLocalNameKey: Name,
    }];
}

// The live link dropped — reset role state and resume discovery so it re-forms.
- (void)onLinkLost {
    if (!_Linked) return;
    _Linked = _Connected = _Connecting = _DecidedPeripheral = false;
    _Subscriber = nil;
    _RemoteDatagram = nil;
    if (_PeerDevice) { [_Central cancelPeripheralConnection:_PeerDevice]; _PeerDevice = nil; }
    if (_Central.state == CBManagerStatePoweredOn)
        [_Central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    [self advertiseService];
}

// ===========================================================================
// CBCentralManagerDelegate — scan, connect out, read the peer's device id.
// ===========================================================================
- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
    if (central.state == CBManagerStatePoweredOn) {
        NSLog(@"OnlyChess BLE: central powered on, scanning");
        [central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    }
}

- (void)centralManager:(CBCentralManager*)central
 didDiscoverPeripheral:(CBPeripheral*)peripheral
     advertisementData:(NSDictionary*)advertisementData
                  RSSI:(NSNumber*)RSSI {
    if (_Linked || _Connecting || _DecidedPeripheral) return;
    _Connecting = true;
    _PeerDevice = peripheral;            // retain across the connect
    peripheral.delegate = self;
    [central stopScan];
    [central connectPeripheral:peripheral options:nil];
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral {
    [peripheral discoverServices:@[_ServiceUuid]];
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error {
    NSLog(@"OnlyChess BLE: connect failed: %@", error);
    [self resetClientAndRescan];
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error {
    if (_Linked && peripheral == _PeerDevice) { [self onLinkLost]; }
    else { [self resetClientAndRescan]; }
}

- (void)resetClientAndRescan {
    _Connecting = false;
    _RemoteDatagram = nil;
    _PeerDevice = nil;
    if (!_Linked && !_DecidedPeripheral && _Central.state == CBManagerStatePoweredOn) {
        [_Central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    }
}

// ---- CBPeripheralDelegate (the peer's server we connected to as central) ----
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
    for (CBService* Service in peripheral.services) {
        if ([Service.UUID isEqual:_ServiceUuid]) {
            [peripheral discoverCharacteristics:@[_DatagramUuid, _DeviceIdUuid] forService:Service];
        }
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service error:(NSError*)error {
    CBCharacteristic* DeviceIdChar = nil;
    for (CBCharacteristic* Char in service.characteristics) {
        if ([Char.UUID isEqual:_DatagramUuid]) _RemoteDatagram = Char;
        else if ([Char.UUID isEqual:_DeviceIdUuid]) DeviceIdChar = Char;
    }
    if (DeviceIdChar) [peripheral readValueForCharacteristic:DeviceIdChar];
    else [_Central cancelPeripheralConnection:peripheral];  // no device id -> not our peer
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateValueForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error {
    if ([characteristic.UUID isEqual:_DeviceIdUuid]) {
        // Got the peer's device id -> run the shared tie-break.
        NSData* V = characteristic.value;
        std::string PeerId(V ? static_cast<const char*>(V.bytes) : "", V ? V.length : 0);
        const EBleRole Role = DecideBleRole(_LocalId, PeerId);
        NSLog(@"OnlyChess BLE: role decided = %s",
              Role == EBleRole::Peripheral ? "Peripheral" : "Central");
        if (Role == EBleRole::Central && _RemoteDatagram) {
            [peripheral setNotifyValue:YES forCharacteristic:_RemoteDatagram];  // keep this link
        } else {
            // We should be the peripheral: drop this connection, let the peer connect to us.
            _DecidedPeripheral = true;
            [_Central stopScan];
            [_Central cancelPeripheralConnection:peripheral];
        }
    } else if ([characteristic.UUID isEqual:_DatagramUuid] && characteristic.value) {
        [self deliverInbound:characteristic.value];   // peer -> us datagram
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error {
    if ([characteristic.UUID isEqual:_DatagramUuid] && characteristic.isNotifying) {
        NSLog(@"OnlyChess BLE: central linked + notifications on");
        [self onLinked];                              // central side: link is live
    }
}

// ===========================================================================
// CBPeripheralManagerDelegate — advertise + host the service.
// ===========================================================================
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral {
    if (peripheral.state != CBManagerStatePoweredOn) return;
    NSLog(@"OnlyChess BLE: peripheral powered on, publishing service");

    _LocalDatagram = [[CBMutableCharacteristic alloc]
        initWithType:_DatagramUuid
          properties:(CBCharacteristicPropertyWrite | CBCharacteristicPropertyNotify)
               value:nil
         permissions:CBAttributePermissionsWriteable];

    // Static, cached read-only device id: CoreBluetooth answers reads automatically.
    NSData* DeviceIdData = [NSData dataWithBytes:_LocalId.data() length:_LocalId.size()];
    CBMutableCharacteristic* DeviceIdChar = [[CBMutableCharacteristic alloc]
        initWithType:_DeviceIdUuid
          properties:CBCharacteristicPropertyRead
               value:DeviceIdData
         permissions:CBAttributePermissionsReadable];

    CBMutableService* Service = [[CBMutableService alloc] initWithType:_ServiceUuid primary:YES];
    Service.characteristics = @[_LocalDatagram, DeviceIdChar];
    [peripheral addService:Service];
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
            didAddService:(CBService*)service error:(NSError*)error {
    if (error) { NSLog(@"OnlyChess BLE: addService error: %@", error); return; }
    // Advertise the service UUID + a human name ONLY (iOS allows no more, and the
    // device id now travels in-band via the device-id characteristic).
    [self advertiseService];
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
                  central:(CBCentral*)central
didSubscribeToCharacteristic:(CBCharacteristic*)characteristic {
    if ([characteristic.UUID isEqual:_DatagramUuid]) {
        _Subscriber = central;
        NSLog(@"OnlyChess BLE: central subscribed (peripheral) — link ready");
        [self onLinked];                              // peripheral side: link is live
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
                  central:(CBCentral*)central
didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic {
    if ([characteristic.UUID isEqual:_DatagramUuid] && _Linked) {
        [self onLinkLost];
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
  didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
    for (CBATTRequest* Req in requests) {
        if ([Req.characteristic.UUID isEqual:_DatagramUuid] && Req.value) {
            [self deliverInbound:Req.value];
        }
    }
    [peripheral respondToRequest:requests.firstObject withResult:CBATTErrorSuccess];
}

@end

// ---------------------------------------------------------------------------
// The C++ ITransport — thin forwarder to the Obj-C driver, mirroring Android.
// ---------------------------------------------------------------------------
namespace Lur::Transport {
namespace {

class IosBleTransport : public ITransport {
public:
    void EnsureDriver() { if (!Driver) Driver = [[IosBleDriver alloc] init]; }

    void Send(const uint8_t* Data, std::size_t Size) override { [Driver sendData:Data size:Size]; }
    void SetReceiver(Receiver NewReceiver) override { [Driver setReceiver:std::move(NewReceiver)]; }
    bool IsConnected() const override { return Driver && [Driver isConnected]; }

private:
    IosBleDriver* Driver = nil;  // ARC-retained
};

IosBleTransport g_Transport;  // one link to one peer — strictly 1:1

} // namespace

ITransport* CreateBleTransport(EBleRole /*Role*/) {
    g_Transport.EnsureDriver();
    return &g_Transport;
}

} // namespace Lur::Transport
