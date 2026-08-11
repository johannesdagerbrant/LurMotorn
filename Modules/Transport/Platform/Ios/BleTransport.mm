// CoreBluetooth backend of Lur::Transport::ITransport — the iOS counterpart of
// Modules/Transport/Platform/Android/BleTransport.cpp. This is the iPhone side of the
// cross-platform BLE link that lets an iPhone and an Android phone play locally with no
// server (CLAUDE.md).
//
// ONE copy, shared by every game. It was duplicated per app, and the two copies had drifted:
// this one carried the #182 radio restart, the #146 bad-device-id guard and the role-pin
// logging that the other lacked entirely — so the game without them had no recovery from a
// wedged BLE stack and could settle a role from a failed GATT read. Nothing here names an app
// now: the log tag arrives as LUR_LOG_TAG and the BLE UUIDs come from BleProtocol.h.
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
#include <deque>
#include <string>
#include <vector>

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"
#include "Lur/Transport/Ble.h"
#import <os/log.h>

#include "Lur/Core/LogTag.h"
#include "Lur/Transport/BleProtocol.h"
#include "Lur/Transport/BleSendQueue.h"

// Every line this driver logs is prefixed with the APP's tag — the string an iOS syslog capture
// greps for — which is the only per-app value left in this file now that the UUIDs come from
// BleProtocol.h. It used to be hardcoded at all 16 call sites, in two copies of this file.
//
// Formatted by NSString, then emitted as ONE public C string. That indirection is the point, and
// it took two device checks to get right:
//
//   * plain NSLog redacts a DYNAMIC C string to "<private>" (it forwards to os_log, and the device
//     is never attached to Xcode in our workflow). Compile-time literals still render, which is
//     what makes the failure easy to miss — the tag and the role name looked fine while the device
//     ids did not.
//   * %{public}s inside an NSLog format does NOT fix it: that annotation is os_log syntax which
//     NSLog cannot encode, and the line came out "<decode: missing data>" — worse than redacted.
//
// So: let NSString do the formatting (it handles %@ for NSError, %zu, %s alike, so no call site
// changes) and hand os_log the finished string as a single %{public}s. Same pattern the app mains
// already use for their engine log sink. NSString handles the multi-line concatenated formats
// unchanged, and ##__VA_ARGS__ swallows the comma for a call with no arguments of its own.
#define BLE_LOG(Fmt, ...)                                                    \
    os_log(OS_LOG_DEFAULT, "%{public}s BLE: %{public}s", Lur::Core::LogTag,  \
           [[NSString stringWithFormat:Fmt, ##__VA_ARGS__] UTF8String])

#include "Lur/Transport/EventInbox.h"

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
static std::string IosSaveDir() {
    NSArray<NSString*>* Dirs =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
    NSString* Dir = Dirs.firstObject ?: NSTemporaryDirectory();
    return std::string(Dir.UTF8String);  // Store's Save() creates the dir if absent
}

static std::string LoadOrCreateIosDeviceId() {
    Lur::Save::Store DeviceStore(IosSaveDir());
    return Lur::Save::LoadOrCreateDeviceId(DeviceStore);
}

// The last-linked peer's id (issue #17 Step 3), for the cached-role reconnect
// shortcut: knowing the peer up front lets the peripheral-elected side just advertise
// and never connect out, so the two devices never both connect at once on reconnect.
static std::string LoadIosPeerId() {
    Lur::Save::Store S(IosSaveDir());
    const std::vector<uint8_t> V = S.Load(Lur::Save::PeerIdKey);
    return std::string(V.begin(), V.end());
}

static void SaveIosPeerId(const std::string& Id) {
    Lur::Save::Store S(IosSaveDir());
    S.Save(Lur::Save::PeerIdKey, reinterpret_cast<const uint8_t*>(Id.data()), Id.size());
}

// ---------------------------------------------------------------------------
// The Obj-C driver: owns BOTH a peripheral manager (advertise + host the service)
// and a central manager (scan + connect), and implements the delegate protocols.
// ---------------------------------------------------------------------------
// The dumb radio the engine queue drives: one verb, forwarding to the driver's -radioWrite:size:.
// Declared before the driver so the ivar can hold one; the driver pointer is filled in at init.
@class IosBleDriver;

namespace {
class IosRadio final : public Lur::Transport::IBleRadio {
public:
    __weak IosBleDriver* Driver = nil;
    bool Write(const uint8_t* Data, std::size_t Size) override;
};
}  // namespace

@interface IosBleDriver : NSObject <CBCentralManagerDelegate,
                                    CBPeripheralManagerDelegate,
                                    CBPeripheralDelegate>
// Hand ONE datagram to CoreBluetooth; YES if it took it. The engine send queue calls this through
// IosRadio and keeps anything refused. Declared here (the rest of the driver's methods are private
// to the implementation) because the C++ adapter below has to message it.
- (BOOL)radioWrite:(const uint8_t*)Data size:(std::size_t)Size;
- (void)drainSend;
@end

@implementation IosBleDriver {
    CBCentralManager*    _Central;
    CBPeripheralManager* _Peripheral;

    CBUUID* _ServiceUuid;
    CBUUID* _DatagramUuid;
    CBUUID* _DeviceIdUuid;

    std::string _LocalId;

    // Cached-role reconnect (issue #17 Step 3). Once we know the peer's id we know our
    // role up front: a known peripheral only advertises (never connects out), a known
    // central only scans — so reconnect can't hit the mutual-connect collision.
    std::string _PeerId;
    bool _HaveCachedRole;   // _PeerId known from a prior link
    bool _CachedPeripheral; // ...and our stable role for it is peripheral

    // Discovery watchdog (#79): the cached role (#17 fast-path) is keyed to a peer
    // identity we have NOT verified this session. If the peer re-rolled its GUID
    // (reset/reinstall), one-sided gates leave BOTH devices deaf forever — we
    // advertise-only while the peer defers to us. Any unlinked stretch longer than
    // this drops the gates and resumes the full symmetric dance; the fresh in-band
    // tie-break then re-caches the true role.
    NSTimer* _DiscoveryWatchdog;

    // Central-side state (we connected OUT to a peer's GATT server).
    CBPeripheral*     _PeerDevice;
    CBCharacteristic* _RemoteDatagram;

    // Peripheral-side state (we host the service; peer connected to us).
    CBMutableCharacteristic* _LocalDatagram;
    CBCentral*               _Subscriber;
    // #83: which central this peripheral is BOUND to while linked. Any CCCD subscription used to
    // become the canonical central unconditionally, so a third device in the room could silently
    // redirect a live match's notifications to itself — and the engine was never told, because
    // onLinked no-ops on the `linked` guard. The rule is SHARED with the Android shim
    // (Lur::Transport::PeerBinding) rather than written per platform; it was wrong in four copies.
    Lur::Transport::PeerBinding _Binding;

    bool _Connected;
    bool _Linked;            // canonical link established; stop discovery
    bool _Connecting;        // an outgoing central attempt is mid-flight
    bool _DecidedPeripheral; // we settled as peripheral; stop connecting out

    // #146: how many times we have connected out, been told "you are the peripheral", and
    // deferred — with NO peer ever arriving to claim Central. Cleared the moment a link forms.
    // Past BleMaxPeripheralDefers the shared breaker (DecideBleRoleBreaking) hands us Central
    // instead, so two peers that both computed Peripheral cannot stall each other forever.
    int _FruitlessDefers;

    // Send flow control (issue #72). CoreBluetooth drops a writeWithoutResponse /
    // updateValue when its transmit queue is full; unpaced autoplay bursts then lost
    // moves (and resync payloads), wedging the game. We queue datagrams and drain them
    // only while the radio reports ready — no added network time. Everything here runs
    // on the CB delegate queue (nil == main), same thread as sendData, so no locking.
    // The queue itself is Lur::Transport::BleSendQueue (engine C++, host-tested); this object is
    // only the radio it drives. The driver keeps no ordering of its own.
    Lur::Transport::BleSendQueue _SendQueue;
    IosRadio                     _Radio;

    // Inbound deferral (issue #40 contract): CB callbacks land between frames; the
    // receiver must fire from Pump() inside the engine tick. Same EventInbox as
    // Android (here single-threaded, so it's purely a deferral queue).
    Lur::Transport::EventInbox _Inbox;
    struct ReceiverSink : Lur::Transport::EventInbox::Sink {
        Lur::Transport::ITransport::Receiver* R = nullptr;
        void OnConnected() override {}
        void OnDisconnected() override {}
        void OnDatagram(const uint8_t* D, std::size_t N) override { if (R && *R) (*R)(D, N); }
    } _Sink;

    ITransport::Receiver _Receiver;
}

- (instancetype)init {
    self = [super init];
    if (self) {
        _ServiceUuid  = MakeUuid(BleServiceUuid);
        _DatagramUuid = MakeUuid(BleDatagramCharacteristicUuid);
        _DeviceIdUuid = MakeUuid(BleDeviceIdCharacteristicUuid);
        _LocalId      = LoadOrCreateIosDeviceId();
        _Connected = _Linked = _Connecting = _DecidedPeripheral = false;
        _FruitlessDefers = 0;
        _Radio.Driver = self;          // the engine queue writes through us
        _SendQueue.SetRadio(&_Radio);

#if LUR_AGENT
        // Role override (rig-pushed Documents/role = "central"|"peripheral"), LUR_AGENT and not
        // LUR_INTERNAL — settled in #197. It is forced state from a hidden channel that outlives
        // the app, and Development is the build a player plays.
        // Rig-pushed Documents/role = "central"|"peripheral":
        // pins DecideBleRole so the rig can test BOTH role configs on one device
        // pair. Read once at driver startup — push the marker BEFORE launching.
        {
            NSString* Dir = [NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES) firstObject];
            NSString* Marker = [NSString stringWithContentsOfFile:[Dir stringByAppendingPathComponent:@"role"]
                                                         encoding:NSUTF8StringEncoding error:nil];
            NSString* Role = [Marker stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
            if ([Role isEqualToString:@"central"])         SetBleRoleOverride(EBleRole::Central);
            else if ([Role isEqualToString:@"peripheral"]) SetBleRoleOverride(EBleRole::Peripheral);
            else                                           ClearBleRoleOverride();
            // #146: say it LOUDLY — the marker outlives the app, so a stale one from an earlier
            // rig run is the first suspect whenever the roles come out wrong, and it also
            // suppresses the deadlock breaker (a pin is a deliberate choice, never overridden).
            if (IsBleRolePinned())
                BLE_LOG(@"role PINNED by Documents/role = %@ (dev override; delete the "
                      @"marker to restore the auto tie-break)", Role);
        }
#endif

        _PeerId = LoadIosPeerId();
        _HaveCachedRole = !_PeerId.empty();
        _CachedPeripheral = _HaveCachedRole && (DecideBleRole(_LocalId, _PeerId) == EBleRole::Peripheral);
        if (_HaveCachedRole && _CachedPeripheral) _DecidedPeripheral = true;  // never connect out

        _Central    = [[CBCentralManager alloc] initWithDelegate:self queue:nil];
        _Peripheral = [[CBPeripheralManager alloc] initWithDelegate:self queue:nil];
        BLE_LOG(@"driver up, local id=%s, cached role=%s", _LocalId.c_str(),
              _HaveCachedRole ? (_CachedPeripheral ? "PERIPHERAL" : "CENTRAL") : "none");
    }
    return self;
}

- (void)setReceiver:(ITransport::Receiver)Receiver { _Receiver = std::move(Receiver); }
- (bool)isConnected { return _Connected; }

// Cached-role gates (issue #17 Step 3): a device that already knows the peer acts
// one-sided, so reconnect never has both sides connecting at once.
- (BOOL)shouldScan { return !(_HaveCachedRole && _CachedPeripheral); }        // scan unless known peripheral
- (BOOL)shouldAdvertise { return !(_HaveCachedRole && !_CachedPeripheral); }  // advertise unless known central

// ---- Outbound ----
- (void)sendData:(const uint8_t*)Data size:(std::size_t)Size expedited:(BOOL)Expedited {
    // Live-only: drop if not linked. An offline move is healed by the next
    // link-establishment record sync, so we must NOT buffer + replay a stale move
    // (which would decode against a since-advanced position and desync).
    if (!_Connected) return;
    // #190, which RPS never had: an expedited datagram goes to the FRONT. Urgency is an
    // argument, never guessed from the bytes — see the chess sibling's comment for the
    // length-inference bug that cost.
    _SendQueue.Enqueue(Data, Size, Expedited ? Lur::Transport::EBleSendPriority::Expedited
                                             : Lur::Transport::EBleSendPriority::Normal);
    [self drainSend];
}

// Hand ONE datagram to CoreBluetooth. Returns whether it took it; the engine queue keeps anything
// refused and retries. WithoutResponse (#49) is preserved — this only PACES sends to the connection
// interval so none are dropped (#72).
//
// CoreBluetooth has no per-write COMPLETION callback, unlike Android: it exposes a pollable
// readiness flag plus a "ready again" delegate call. So the queue is told the write completed the
// moment it is accepted (see writeOne: below), which reproduces iOS's drain-while-ready behaviour
// exactly. A consequence worth stating: BleSendQueue's lost-completion watchdog is INERT here, and
// correctly so — there is no completion that could go missing.
// Defined after the driver interface so the message send resolves. A write ACCEPTED by
// CoreBluetooth is immediately complete from the queue's point of view — see radioWrite:.
namespace {
bool IosRadio::Write(const uint8_t* Data, std::size_t Size) {
    if (Driver == nil) return false;
    return [Driver radioWrite:Data size:Size] == YES;
}
}  // namespace

- (BOOL)radioWrite:(const uint8_t*)Data size:(std::size_t)Size {
    NSData* Payload = [NSData dataWithBytes:Data length:Size];
    if (_RemoteDatagram && _PeerDevice) {                // we are central -> write
        if (!_PeerDevice.canSendWriteWithoutResponse) return NO;   // wait for the ready callback
        [_PeerDevice writeValue:Payload
              forCharacteristic:_RemoteDatagram
                           type:CBCharacteristicWriteWithoutResponse];
        return YES;
    }
    if (_LocalDatagram && _Subscriber) {                 // we are peripheral -> notify
        return [_Peripheral updateValue:Payload
                      forCharacteristic:_LocalDatagram
                   onSubscribedCentrals:@[_Subscriber]] ? YES : NO;   // NO = queue full
    }
    return NO;                                           // no live characteristic
}

// Drain while CoreBluetooth keeps accepting.
//
// This is the one real difference from Android, and it is why the cutover is not a copy. Android
// reports a per-write COMPLETION, so the queue holds one datagram in flight until that callback
// lands. CoreBluetooth reports no such thing: it exposes a readiness flag and a "ready again"
// delegate call, and a write it ACCEPTS is finished as far as we can ever know. So we tell the
// queue so immediately, which reproduces iOS's drain-while-ready behaviour exactly.
//
// The loop ends when a write is refused (InFlight stays false because nothing was taken) or the
// queue empties — both of which leave InFlight false. Bounded by construction: every iteration
// either removes a datagram or stops.
- (void)drainSend {
    while (_SendQueue.InFlight()) _SendQueue.OnSendComplete();
}

// CoreBluetooth became ready, so anything it refused can go now.
- (void)pumpSend { _SendQueue.Tick(0); [self drainSend]; }

// CoreBluetooth is ready for more datagrams — resume draining the send queue (#72).
- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral*)peripheral { [self pumpSend]; }
- (void)peripheralManagerIsReadyToUpdateSubscribers:(CBPeripheralManager*)peripheral { [self pumpSend]; }

- (void)deliverInbound:(NSData*)Data {
    // Queue, don't deliver: CoreBluetooth callbacks land on the main runloop BETWEEN
    // frames, but the ITransport contract (issue #40) is that the receiver fires from
    // Pump() inside Session::Tick — the engine's frame window. Direct delivery made
    // datagrams apply outside the frame's WasMyTurn/NowMyTurn window, which blinded the
    // same-frame/RTT instrumentation on iOS (and was a contract drift from Android).
    if (Data.length > 0) _Inbox.PushDatagram(static_cast<const uint8_t*>(Data.bytes),
                                             (std::size_t)Data.length);
}

// Engine thread (via ITransport::Pump from Session::Tick): drain queued datagrams.
- (void)pumpInbox {
    _Sink.R = &_Receiver;
    _Inbox.Drain(_Sink);
}

- (void)onLinked { if (_Linked) return; _Linked = _Connected = true;
    _FruitlessDefers = 0;  // #146: a defer that produced a link was not fruitless
    [_DiscoveryWatchdog invalidate];
    _DiscoveryWatchdog = nil;
    [_Central stopScan];
    if (_Peripheral.isAdvertising) [_Peripheral stopAdvertising];
    // The net Session sends the first Hello once it sees the link up — no demo ping
    // (a bare 1-byte ping would now look like a move).
}

// #79: while unlinked, one-sided cached-role behaviour may only last one watchdog
// period. Runs on the main runloop (same thread as every CB callback here).
- (void)armDiscoveryWatchdog {
    [_DiscoveryWatchdog invalidate];
    if (_Linked) return;
    _DiscoveryWatchdog = [NSTimer scheduledTimerWithTimeInterval:8.0
                                                          target:self
                                                        selector:@selector(discoveryWatchdogFired)
                                                        userInfo:nil
                                                         repeats:NO];
}

- (void)discoveryWatchdogFired {
    if (_Linked) return;
    BLE_LOG(@"discovery watchdog: no link in 8s - dropping cached-role "
          @"gates, going symmetric (#79)");
    _HaveCachedRole = _CachedPeripheral = _DecidedPeripheral = false;
    _Connecting = false;
    if (_PeerDevice) { [_Central cancelPeripheralConnection:_PeerDevice]; _PeerDevice = nil; }
    if (_Central.state == CBManagerStatePoweredOn)
        [_Central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    [self advertiseService];
    [self armDiscoveryWatchdog];  // keep watching until a link forms
}

- (void)advertiseService {
    if (![self shouldAdvertise]) return;  // cached CENTRAL role: find the peer by scanning, don't advertise
    if (_Peripheral.state != CBManagerStatePoweredOn || _Peripheral.isAdvertising) return;
    NSString* Name = [NSString stringWithUTF8String:std::string(BleAdvertisedName).c_str()];
    [_Peripheral startAdvertising:@{
        CBAdvertisementDataServiceUUIDsKey: @[_ServiceUuid],
        CBAdvertisementDataLocalNameKey: Name,
    }];
}

// The net layer's keepalive timed out: the peer is silent but CoreBluetooth never
// told us (an abruptly-killed central gives a CBPeripheralManager no callback). Treat
// it as a link loss so we resume discovery and the UI goes to Disconnected.
- (void)resetLink {
    if (!_Linked) return;
    BLE_LOG(@"net keepalive timeout -> forcing link reset");
    [self onLinkLost];
}

// The live link dropped — reset role state and resume discovery so it re-forms.
- (void)onLinkLost {
    if (!_Linked) return;
    _Linked = _Connected = _Connecting = false;
    _DecidedPeripheral = (_HaveCachedRole && _CachedPeripheral);  // known peripheral stays one-sided
    _SendQueue.OnLinkLost();                                      // drop stale send backlog (#72)
    _Subscriber = nil;
    // #83: the link is genuinely gone, so open the binding up again. This is what keeps chess's
    // deliberate opponent-switch (#38) working — it operates at session level AFTER link loss, so a
    // binding that outlived the link would forbid ever changing opponents.
    _Binding.Clear();
    _RemoteDatagram = nil;
    if (_PeerDevice) { [_Central cancelPeripheralConnection:_PeerDevice]; _PeerDevice = nil; }
    // Role-aware rediscovery: known central only scans, known peripheral only advertises,
    // so the two devices never both connect out (the reconnect collision, issue #17).
    if (_Central.state == CBManagerStatePoweredOn && [self shouldScan])
        [_Central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    [self advertiseService];  // gated internally by shouldAdvertise
    [self armDiscoveryWatchdog];  // #79: one-sidedness may not outlive the watchdog
}

// ===========================================================================
// CBCentralManagerDelegate — scan, connect out, read the peer's device id.
// ===========================================================================
- (void)centralManagerDidUpdateState:(CBCentralManager*)central {
    if (central.state != CBManagerStatePoweredOn) return;
    if ([self shouldScan]) {
        BLE_LOG(@"central powered on, scanning");
        [central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    } else {
        BLE_LOG(@"central powered on, cached PERIPHERAL role -> not scanning");
    }
    [self armDiscoveryWatchdog];  // #79: one-sidedness may not outlive the watchdog
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
    BLE_LOG(@"connect failed: %@", error);
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
        // #146: a role settled from a BAD read is a role the peer cannot mirror — and two peers
        // that both land on Peripheral deadlock with nobody central. An errored read leaves a
        // stale/absent value, so never decide from it: treat the READ as the failure and retry.
        if (error != nil || !Lur::Save::IsValidDeviceId(PeerId)) {
            BLE_LOG(@"bad device-id read (err=%@, %zuB) - not deciding a role; "
                  @"retrying discovery", error, PeerId.size());
            [_Central cancelPeripheralConnection:peripheral];
            [self armDiscoveryWatchdog];
            return;
        }
        if (PeerId != _PeerId) {   // cache for the fast cached-role reconnect
            _PeerId = PeerId;
            _HaveCachedRole = true;
            _CachedPeripheral = (DecideBleRole(_LocalId, _PeerId) == EBleRole::Peripheral);
            SaveIosPeerId(_PeerId);
        }
        const EBleRole Role = DecideBleRoleBreaking(_LocalId, PeerId, _FruitlessDefers);
        // Log both id STRINGS (they're ASCII hex): a both-Peripheral deadlock means the two
        // sides compared DIFFERENT bytes, which is only diagnosable if each side prints what
        // it actually compared (#146).
        // The ids must actually PRINT: a both-peripheral deadlock means the two sides compared
        // different BYTES, and the values are the only thing that shows it — sizes agree in exactly
        // that case (#146). BLE_LOG is built to keep them unredacted; see its definition.
        BLE_LOG(@"role decided = %s (mine=%s peer=%s defers=%d)",
              Role == EBleRole::Peripheral ? "Peripheral" : "Central",
              _LocalId.c_str(), PeerId.c_str(), _FruitlessDefers);
        if (Role == EBleRole::Central && _RemoteDatagram) {
            if (_FruitlessDefers >= BleMaxPeripheralDefers)
                BLE_LOG(@"role tie-break BROKEN after %d fruitless defers -> forcing "
                      @"Central (nobody was connecting; #146)", _FruitlessDefers);
            [peripheral setNotifyValue:YES forCharacteristic:_RemoteDatagram];  // keep this link
        } else {
            // We should be the peripheral: drop this connection, let the peer connect to us.
            _DecidedPeripheral = true;
            // #146: cleared by onLinked; counts UNANSWERED defers only — a Central decision that
            // lands here (no datagram characteristic) is a broken peer, not a tie-break failure.
            if (Role == EBleRole::Peripheral) ++_FruitlessDefers;
            [_Central stopScan];
            [self advertiseService];  // ensure findable even if we began in cached-central mode
            [_Central cancelPeripheralConnection:peripheral];
            [self armDiscoveryWatchdog];  // #79: if the peer never comes, go symmetric again
        }
    } else if ([characteristic.UUID isEqual:_DatagramUuid] && characteristic.value) {
        [self deliverInbound:characteristic.value];   // peer -> us datagram
    }
}

- (void)peripheral:(CBPeripheral*)peripheral
didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)characteristic error:(NSError*)error {
    if ([characteristic.UUID isEqual:_DatagramUuid] && characteristic.isNotifying) {
        BLE_LOG(@"central linked + notifications on");
        // #83: we are CENTRAL, so our own peripheral manager has no legitimate peer — shut it to
        // everyone. Binding only ever happened in didSubscribeToCharacteristic (the peripheral path),
        // which left a central-role phone playing a whole match with an OPEN binding on a service that
        // is still published; stopping advertising is not protection, since a device that scanned
        // earlier keeps the handle. Such a device could bind itself, inject datagrams into the
        // lockstep stream, and end a healthy match just by disconnecting.
        _Binding.Close();
        [self onLinked];                              // central side: link is live
    }
}

// ===========================================================================
// CBPeripheralManagerDelegate — advertise + host the service.
// ===========================================================================
- (void)peripheralManagerDidUpdateState:(CBPeripheralManager*)peripheral {
    if (peripheral.state != CBManagerStatePoweredOn) return;
    BLE_LOG(@"peripheral powered on, publishing service");
    [self publishService];
}

// Build + add the GATT service (datagram + device-id characteristics). Factored out of
// peripheralManagerDidUpdateState so #182's restartRadio can RE-publish it: removing and re-adding
// the service is how a wedged peripheral sheds a stale subscription (candidate 1 in #163).
- (void)publishService {
    if (_Peripheral.state != CBManagerStatePoweredOn) return;
    _LocalDatagram = [[CBMutableCharacteristic alloc]
        initWithType:_DatagramUuid
          properties:(CBCharacteristicPropertyWrite | CBCharacteristicPropertyWriteWithoutResponse |
                      CBCharacteristicPropertyNotify)   // WriteWithoutResponse: drop the ATT ack (#49)
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
    [_Peripheral addService:Service];
}

// #182: the HARDER reset the net layer escalates to when it judges the link HALF-OPEN — connected, our
// writes leave, but the peer's notify path is wedged so nothing inbound ever arrives. resetLink (which
// just forces onLinkLost -> re-advertise) provably can't clear a wedged BLE stack. The iOS-specific
// deeper move: REMOVE AND RE-ADD the peripheral service, so a stale central subscription is rebuilt
// rather than merely re-advertised — candidate 1 in #163. Also drops any central-side connection and
// rescans. Bounded by the Session (MaxRadioRestarts) so it can't become churn; may still not clear a
// device-level wedge (the hardware case needed the SILENT peer to reboot), so the LINK STALLED banner
// stays the floor. Runs on the CB delegate queue (nil == main), same thread as every callback here.
- (void)restartRadio {
    BLE_LOG(@"restartRadio (#182): republishing service + dropping the link — a soft reset "
          @"can't clear a wedged BLE stack");
    // Central side: drop any outgoing connection so it re-forms cleanly.
    _Connecting = false;
    _RemoteDatagram = nil;
    if (_PeerDevice) { [_Central cancelPeripheralConnection:_PeerDevice]; _PeerDevice = nil; }
    // Peripheral side: shed the wedged notify path by removing + re-adding the service.
    _Subscriber = nil;
    _Binding.Clear();                 // #83: link is going down, reopen the binding
    _SendQueue.clear();               // drop stale backlog (#72)
    if (_Peripheral.state == CBManagerStatePoweredOn) {
        if (_Peripheral.isAdvertising) [_Peripheral stopAdvertising];
        [_Peripheral removeAllServices];   // drop the stale service + its subscription...
        _LocalDatagram = nil;
        [self publishService];             // ...and re-add it (didAddService re-advertises)
    }
    // Link/role state resets exactly as a real loss would; the engine sees IsConnected() go false on
    // its next Pump (iOS reports connection via the _Connected flag, no explicit disconnect event).
    _Linked = _Connected = false;
    _DecidedPeripheral = (_HaveCachedRole && _CachedPeripheral);  // known peripheral stays one-sided
    // Central rediscovery (role-aware, mirroring onLinkLost).
    if (_Central.state == CBManagerStatePoweredOn && [self shouldScan])
        [_Central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    [self armDiscoveryWatchdog];      // #79: one-sidedness may not outlive the watchdog
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
            didAddService:(CBService*)service error:(NSError*)error {
    if (error) { BLE_LOG(@"addService error: %@", error); return; }
    // Advertise the service UUID + a human name ONLY (iOS allows no more, and the
    // device id now travels in-band via the device-id characteristic).
    [self advertiseService];
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
                  central:(CBCentral*)central
didSubscribeToCharacteristic:(CBCharacteristic*)characteristic {
    if ([characteristic.UUID isEqual:_DatagramUuid]) {
        // #83: bind to the FIRST subscriber and serve only that one. Re-subscription by the bound
        // central is fine (an MTU renegotiation does that); anyone else is a third device trying to
        // take over a live pair's notify channel, and is ignored rather than obeyed.
        if (!_Binding.AcceptSubscriber(central.identifier.UUIDString.UTF8String)) {
            BLE_LOG(@"IGNORING subscribe from a non-bound central (#83) — a live pair "
                   "serves exactly one");
            return;
        }
        _Subscriber = central;
        BLE_LOG(@"central subscribed (peripheral) — link ready");
        [self onLinked];                              // peripheral side: link is live
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
                  central:(CBCentral*)central
didUnsubscribeFromCharacteristic:(CBCharacteristic*)characteristic {
    // #83: only the BOUND peer's departure ends the match. Treating any unsubscribe as link loss is
    // the hijack in reverse — a third device could kill a live pair simply by leaving.
    if ([characteristic.UUID isEqual:_DatagramUuid] && _Linked &&
        _Binding.IsPeer(central.identifier.UUIDString.UTF8String)) {
        [self onLinkLost];
    }
}

- (void)peripheralManager:(CBPeripheralManager*)peripheral
  didReceiveWriteRequests:(NSArray<CBATTRequest*>*)requests {
    for (CBATTRequest* Req in requests) {
        // #83: drop bytes from any central that is not the bound peer. Unfiltered, a third device's
        // writes injected straight into the lockstep/move stream. Pre-link traffic still passes —
        // that IS the handshake — but once bound, only the peer does.
        if (!_Binding.AcceptData(Req.central.identifier.UUIDString.UTF8String)) continue;
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

    void Send(const uint8_t* Data, std::size_t Size) override {
        [Driver sendData:Data size:Size expedited:NO];
    }
    void SendExpedited(const uint8_t* Data, std::size_t Size) override {
        [Driver sendData:Data size:Size expedited:YES];
    }
    void SetReceiver(Receiver NewReceiver) override { [Driver setReceiver:std::move(NewReceiver)]; }
    bool IsConnected() const override { return Driver && [Driver isConnected]; }
    void Pump() override { if (Driver) [Driver pumpInbox]; }  // engine-frame delivery (#40)
    void ResetLink() override { if (Driver) [Driver resetLink]; }
    void RestartRadio() override { if (Driver) [Driver restartRadio]; }  // #182: harder, per-OS reset
    // Declare it, or the session refuses to escalate: CanRestartRadio defaults to false so a
    // transport WITHOUT a hard restart is never narrated as having one — which means one that HAS
    // it must say so. Chess's iOS driver, now deleted in favour of this file, lacked the restart
    // entirely and the session logged three attempts that never happened.
    bool CanRestartRadio() const override { return true; }

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
