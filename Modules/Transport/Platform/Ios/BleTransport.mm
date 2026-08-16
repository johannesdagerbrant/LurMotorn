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
#include "Lur/Transport/BleLinkController.h"
#include <chrono>

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

// #206: how far an OUTGOING central attempt got before it died.
//
// The connect watchdog reports "neither linked nor resolved", which is true and useless — it cannot
// say whether CoreBluetooth never called back at all, or called back and we dropped the thread
// ourselves. Three handlers on this path swallowed their `error` argument and two had silent
// dead-ends (a service list without ours; characteristics without a device id), so a stall looked
// identical from the outside whatever caused it.
//
// Stages are ordered, so the last one reached names the step that hung.
enum class EConnectStage {
    Idle,            // no attempt in flight
    Connecting,      // connectPeripheral called, no callback yet
    Connected,       // didConnectPeripheral — the LE link is up
    ServicesFound,   // our service was present in the discovered list
    CharsFound,      // datagram + device-id characteristics resolved
    PeerIdRead,      // the peer's device id came back and the tie-break ran
    Subscribing,     // we asked to notify and are waiting for confirmation
    Linked,          // notifications confirmed
};

static const char* ConnectStageName(EConnectStage S) {
    switch (S) {
        case EConnectStage::Idle:          return "Idle";
        case EConnectStage::Connecting:    return "Connecting(no callback yet)";
        case EConnectStage::Connected:     return "Connected(LE up)";
        case EConnectStage::ServicesFound: return "ServicesFound";
        case EConnectStage::CharsFound:    return "CharsFound";
        case EConnectStage::PeerIdRead:    return "PeerIdRead";
        case EConnectStage::Subscribing:   return "Subscribing";
        case EConnectStage::Linked:        return "Linked";
    }
    return "?";
}

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
    //
    // The DEADLINE is Lur::Transport::BleDiscoveryTimers — the same tested policy Android
    // drives (#197). It replaced an NSTimer holding a hardcoded 8.0 that had to agree, by
    // hand, with the Kotlin's hardcoded 8000. Both games carried both copies. Note it is
    // PERIODIC, so the fired handler must NOT re-arm it.
    // ONE object, not several hand-composed ones — the composition (a link cancels the backoffs, a
    // power cycle forgets the started-state) is itself a decision, and hand-composing it per
    // platform is how these two drivers had already begun to differ. See BleLinkController.h.
    Lur::Transport::BleLinkController _Link;
    bool _TimersStarted;
    // #206 diagnostics: how far the current outgoing attempt got, and one write-path line per
    // link episode (not per write).
    EConnectStage _ConnectStage;
    bool _WritePathLogged;
    bool _AmbiguousPathLogged;
    std::chrono::steady_clock::time_point _TimersLast;

    // Central-side state (we connected OUT to a peer's GATT server).
    CBPeripheral*     _PeerDevice;
    CBCharacteristic* _RemoteDatagram;

    // Peripheral-side state (we host the service; peer connected to us).
    CBMutableCharacteristic* _LocalDatagram;
    CBCentral*               _Subscriber;
    // #202: whether we have already reported adopting the link from a write this episode. The adoption
    // itself is idempotent and cheap, but CoreBluetooth makes no promise that CBATTRequest.central
    // hands back the SAME object every callback, so an ungated log could repeat per datagram. Cleared
    // wherever _Subscriber is (onLinkLost / restartRadio), so each episode says it at most once.
    bool                     _NotifyTargetSwapLogged;
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
        _NotifyTargetSwapLogged = false;
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

    // #206 DIAGNOSTIC. Which path a write takes is decided here, and it has never been observable:
    // both branches return YES, and `updateValue:onSubscribedCentrals:` returns YES even for a DEAD
    // central handle (noted below). So an iPhone writing into a corpse looks exactly like an iPhone
    // sending fine, which is precisely the half-open shape #206 shows — Android hears nothing while
    // this side reports success.
    //
    // The ambiguous state is the suspect: `_RemoteDatagram` is set during characteristic discovery
    // on an OUTGOING central attempt, BEFORE the role tie-break has decided anything. So a phone
    // that connected out, discovered characteristics, then ended up serving an incoming central can
    // hold both. The central branch is tested first, so it would win — and write to the wrong peer.
    // Logged once per link episode (reset in onLinked/onLinkLost) so it cannot become a per-write
    // firehose.
    const bool CentralPath    = (_RemoteDatagram != nil && _PeerDevice != nil);
    const bool PeripheralPath = (_LocalDatagram != nil && _Subscriber != nil);
    if (!_WritePathLogged) {
        _WritePathLogged = true;
        BLE_LOG(@"write path: central=%d peripheral=%d -> taking %s", CentralPath ? 1 : 0,
                PeripheralPath ? 1 : 0,
                CentralPath ? "CENTRAL(write)" : (PeripheralPath ? "PERIPHERAL(notify)" : "NONE"));
    }
    if (CentralPath && PeripheralPath && !_AmbiguousPathLogged) {
        _AmbiguousPathLogged = true;
        BLE_LOG(@"write path AMBIGUOUS (#206): holding BOTH an outgoing central connection and an "
                 "incoming subscriber. The central branch wins, so if the live link is the incoming "
                 "one every send goes to the wrong peer and still reports success.");
    }

    if (CentralPath) {                                   // we are central -> write
        if (!_PeerDevice.canSendWriteWithoutResponse) return NO;   // wait for the ready callback
        [_PeerDevice writeValue:Payload
              forCharacteristic:_RemoteDatagram
                           type:CBCharacteristicWriteWithoutResponse];
        return YES;
    }
    if (PeripheralPath) {                                // we are peripheral -> notify
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
    [self tickTimers];   // ...and advance the discovery deadlines on the same cadence
}

- (void)onLinked { if (_Linked) return; _Linked = _Connected = true;
    _FruitlessDefers = 0;  // #146: a defer that produced a link was not fruitless
    _Link.OnLinked();      // every deadline off, both backoffs cancelled: churn on a live link is what degraded the radio in #163
    _WritePathLogged = _AmbiguousPathLogged = false;   // #206: a new episode gets a fresh line
    [_Central stopScan];
    if (_Peripheral.isAdvertising) [_Peripheral stopAdvertising];
    // The net Session sends the first Hello once it sees the link up — no demo ping
    // (a bare 1-byte ping would now look like a move).
}

// #79: while unlinked, one-sided cached-role behaviour may only last one watchdog period. Arms the
// shared policy from zero, so time spent linked is never banked against the next unlinked stretch.
- (void)armDiscoveryWatchdog {
    _Link.OnUnlinked();
}

// The deadline elapsed — decided by BleDiscoveryTimers on the engine thread, dispatched here so the
// CoreBluetooth calls stay on the main runloop like every other CB touch in this file.
//
// Does NOT re-arm: the policy's discovery watchdog is periodic. The NSTimer this replaced was
// one-shot and re-armed itself here; doing both would run it at twice the rate.
- (void)discoveryWatchdogFired {
    if (_Linked) return;
    BLE_LOG(@"discovery watchdog: no link in 8s - dropping cached-role "
          @"gates, going symmetric (#79)");
    // #206: DISCOVERY goes symmetric, INITIATION does not. Dropping both gates put this phone and
    // its peer into connect-out mode on the same 8 s cadence — and two BLE devices share ONE LE
    // link, so when the elected peripheral defers and disconnects it tears down the peer's in-flight
    // incoming attempt too. Both sides still advertise + scan (that is what finds a peer at all),
    // but only the side DecideBleRole elects central actually connects out. After
    // SymmetricRoundsBeforeDistrust fruitless rounds the cached id stops being trusted and anyone
    // may initiate — #79's guarantee, kept as an escalation rather than as the default.
    const bool HaveCached = !_PeerId.empty();
    const bool TieBreakCentral =
        HaveCached && (DecideBleRole(_LocalId, _PeerId) == EBleRole::Central);
    const bool ConnectOut = _Link.ShouldConnectOut(HaveCached, TieBreakCentral);
    _HaveCachedRole = _CachedPeripheral = false;   // discovery symmetric again
    _DecidedPeripheral = !ConnectOut;              // ...initiation stays one-sided
    _Connecting = false;
    if (_PeerDevice) { [_Central cancelPeripheralConnection:_PeerDevice]; _PeerDevice = nil; }
    if (_Central.state == CBManagerStatePoweredOn)
        [_Central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    [self advertiseService];
}

// Advance the discovery deadlines. Called from pumpInbox, i.e. once per Session::Tick on the engine
// thread — the same cadence and the same measure-here approach Android uses, so the two platforms
// share the policy AND its clocking rather than only the constants.
- (void)tickTimers {
    const auto Now = std::chrono::steady_clock::now();
    if (!_TimersStarted) { _TimersStarted = true; _TimersLast = Now; return; }
    const auto Delta = std::chrono::duration_cast<std::chrono::nanoseconds>(Now - _TimersLast).count();
    _TimersLast = Now;

    const Lur::Transport::BleLinkController::Actions Act =
        _Link.Tick(static_cast<uint64_t>(Delta));
    // Several can be due at once after a long stall (an app suspended and resumed), so each is
    // handled rather than only the first. Order matches Android's: tear a stalled attempt down
    // before the symmetric reset lands on top of it.
    if (Act.AbortConnect) dispatch_async(dispatch_get_main_queue(), ^{ [self abortConnect]; });
    if (Act.GoSymmetric) {
        _Link.OnWentSymmetric();   // counted: trusting the cached peer id forever is what #79 forbids
        dispatch_async(dispatch_get_main_queue(), ^{ [self discoveryWatchdogFired]; });
    }
    if (Act.Rescan)       dispatch_async(dispatch_get_main_queue(), ^{ [self rescanNow]; });
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
    _Link.OnUnlinked();
    _DecidedPeripheral = (_HaveCachedRole && _CachedPeripheral);  // known peripheral stays one-sided
    _SendQueue.OnLinkLost();                                      // drop stale send backlog (#72)
    _Subscriber = nil;
    _NotifyTargetSwapLogged = false;                              // #202: next episode may say it again
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
    if (central.state != CBManagerStatePoweredOn) { [self onRadioPoweredOff]; return; }
    _Link.OnAdapterOn();
    if ([self shouldScan]) {
        BLE_LOG(@"central powered on, scanning");
        [central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
    } else {
        BLE_LOG(@"central powered on, cached PERIPHERAL role -> not scanning");
    }
    [self armDiscoveryWatchdog];  // #79: one-sidedness may not outlive the watchdog
}

// #206: one line per step of an outgoing attempt, so a stall names the step it hung on.
- (void)setConnectStage:(EConnectStage)Stage {
    if (_ConnectStage == Stage) return;
    _ConnectStage = Stage;
    BLE_LOG(@"central attempt -> %s", ConnectStageName(Stage));
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
    [self setConnectStage:EConnectStage::Connecting];
    // #197: iOS had NO connect watchdog where Android has had one for a long time — a one-sided
    // fix, which is the drift this phase exists to end. CoreBluetooth's connectPeripheral has no
    // timeout of its own and will happily wait forever, so a connect the stack never completes
    // pinned _Connecting true and blocked every later attempt at the line above.
    _Link.OnConnectStarted();
}

- (void)centralManager:(CBCentralManager*)central
  didConnectPeripheral:(CBPeripheral*)peripheral {
    [self setConnectStage:EConnectStage::Connected];
    [peripheral discoverServices:@[_ServiceUuid]];
}

- (void)centralManager:(CBCentralManager*)central
didFailToConnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error {
    BLE_LOG(@"connect failed at %s: %@", ConnectStageName(_ConnectStage), error);
    _ConnectStage = EConnectStage::Idle;
    [self resetClientAndRescan];
}

- (void)centralManager:(CBCentralManager*)central
didDisconnectPeripheral:(CBPeripheral*)peripheral error:(NSError*)error {
    // #206: a disconnect DURING an attempt is the peer tearing the shared LE link down under us —
    // name the stage, because "it just stopped" is what made this invisible.
    if (!_Linked && _ConnectStage != EConnectStage::Idle) {
        BLE_LOG(@"central attempt died at %s (peer disconnected): %@",
                ConnectStageName(_ConnectStage), error);
    }
    _ConnectStage = EConnectStage::Idle;
    if (_Linked && peripheral == _PeerDevice) { [self onLinkLost]; }
    else { [self resetClientAndRescan]; }
}

- (void)resetClientAndRescan {
    _ConnectStage = EConnectStage::Idle;   // #206: the attempt is over, whatever stage it reached
    _Connecting = false;
    _RemoteDatagram = nil;
    _PeerDevice = nil;
    _Link.OnConnectResolved();   // the attempt is over, however it ended
    // #17 / #197: DELAYED, not immediate. Android has waited 1.5 s here for a long time and iOS
    // never did — another one-sided fix. The delay is the point: the peer is running its own
    // exploratory connect and needs a moment to settle into peripheral-only, and the shared LE link
    // needs to finish tearing down, or the retry collides again or hangs. Rescanning instantly is
    // how both ends keep re-colliding, which is visible as the role tie-break needing several
    // fruitless defers before #146's breaker resolves it (measured at 33 s on 2026-08-15).
    if (!_Linked && !_DecidedPeripheral) [self scheduleRescan];
}

// Re-requesting supersedes a pending rescan rather than queueing a second, so a burst of failed
// connects cannot produce a burst of scan starts.
- (void)scheduleRescan {
    _Link.ScheduleRescan();
}

// The rescan delay elapsed (BleDiscoveryTimers). Main queue: CoreBluetooth lives there.
- (void)rescanNow {
    if (_Linked || _DecidedPeripheral) return;
    if (_Central.state == CBManagerStatePoweredOn)
        [_Central scanForPeripheralsWithServices:@[_ServiceUuid] options:nil];
}

// The connect watchdog elapsed: this attempt neither linked nor resolved. Tear it down rather than
// leaving _Connecting pinned true forever.
- (void)abortConnect {
    if (_Linked || !_Connecting) return;
    // #206: WHICH STEP hung is the whole question. "Neither linked nor resolved" is true and
    // useless; the stage says whether CoreBluetooth never called back at all (Connecting), the peer
    // answered without our service (Connected), or we got all the way to a subscribe that was never
    // confirmed (Subscribing).
    BLE_LOG(@"central: connect watchdog - stalled at %s, tearing it down",
            ConnectStageName(_ConnectStage));
    _ConnectStage = EConnectStage::Idle;
    if (_PeerDevice) [_Central cancelPeripheralConnection:_PeerDevice];
    [self resetClientAndRescan];
}

// ---- CBPeripheralDelegate (the peer's server we connected to as central) ----
- (void)peripheral:(CBPeripheral*)peripheral didDiscoverServices:(NSError*)error {
    if (error) { BLE_LOG(@"service discovery FAILED: %@", error); return; }
    bool Found = false;
    for (CBService* Service in peripheral.services) {
        if ([Service.UUID isEqual:_ServiceUuid]) {
            Found = true;
            [peripheral discoverCharacteristics:@[_DatagramUuid, _DeviceIdUuid] forService:Service];
        }
    }
    // #206: this used to return in silence. A peer whose GATT service has not finished publishing
    // answers with a list that does not contain ours, and the attempt then hangs to the watchdog
    // with no line explaining why — which is exactly the stall being hunted.
    if (Found) [self setConnectStage:EConnectStage::ServicesFound];
    else       BLE_LOG(@"services discovered but OURS IS ABSENT (%lu present) — peer's GATT server "
                        "may not have published yet; this attempt will stall",
                       (unsigned long)peripheral.services.count);
}

- (void)peripheral:(CBPeripheral*)peripheral
didDiscoverCharacteristicsForService:(CBService*)service error:(NSError*)error {
    if (error) { BLE_LOG(@"characteristic discovery FAILED: %@", error); return; }
    CBCharacteristic* DeviceIdChar = nil;
    for (CBCharacteristic* Char in service.characteristics) {
        if ([Char.UUID isEqual:_DatagramUuid]) _RemoteDatagram = Char;
        else if ([Char.UUID isEqual:_DeviceIdUuid]) DeviceIdChar = Char;
    }
    if (DeviceIdChar) {
        [self setConnectStage:EConnectStage::CharsFound];
        [peripheral readValueForCharacteristic:DeviceIdChar];
    } else {
        BLE_LOG(@"characteristics found but NO DEVICE-ID characteristic — not our peer, dropping");
        [_Central cancelPeripheralConnection:peripheral];
    }
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
        [self setConnectStage:EConnectStage::PeerIdRead];
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
            [self setConnectStage:EConnectStage::Subscribing];
            [peripheral setNotifyValue:YES forCharacteristic:_RemoteDatagram];  // keep this link
        } else {
            // #206: the tie-break said Central but we have no datagram characteristic, so we defer
            // anyway. That is a broken peer, not a role decision, and it used to be indistinguishable
            // in the log from an ordinary defer.
            if (Role == EBleRole::Central)
                BLE_LOG(@"tie-break said Central but NO datagram characteristic on the peer — "
                         "deferring instead; this peer's GATT service is incomplete");
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
    if (error) BLE_LOG(@"notify-enable FAILED: %@", error);
    if ([characteristic.UUID isEqual:_DatagramUuid] && characteristic.isNotifying) {
        [self setConnectStage:EConnectStage::Linked];
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
    if (peripheral.state != CBManagerStatePoweredOn) { [self onRadioPoweredOff]; return; }
    _Link.OnAdapterOn();
    BLE_LOG(@"peripheral powered on, publishing service");
    [self publishService];
}

// The radio went away — Bluetooth switched off, or the app lost the hardware. Both didUpdateState
// handlers used to just `return` here, invalidating nothing, which on Android was a phone that
// stayed invisible for the life of the process (#197, found on the Galaxy 2026-08-15).
//
// CoreBluetooth resets its own isAdvertising, but every piece of state WE keep survives, and a
// cached role or a half-finished connect surviving a power cycle is what leaves both phones deaf.
- (void)onRadioPoweredOff {
    if (!_Link.IsAdapterOn()) return;   // already handled; both managers report separately
    BLE_LOG(@"BLE radio powered OFF - dropping cached role, connect state and any link");
    _Link.OnAdapterOff();
    _Connecting = false;
    _HaveCachedRole = _CachedPeripheral = _DecidedPeripheral = false;
    if (_PeerDevice) { [_Central cancelPeripheralConnection:_PeerDevice]; _PeerDevice = nil; }
    _RemoteDatagram = nil;
    if (_Linked) [self onLinkLost];
    _Link.OnUnlinked();   // re-arm discovery so the return to power-on resumes the dance
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
    _NotifyTargetSwapLogged = false;                          // #202: next episode may say it again
    _Binding.Clear();                 // #83: link is going down, reopen the binding
    _SendQueue.OnLinkLost();          // drop stale backlog (#72)
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


// We are definitively the PERIPHERAL for this link: a central has subscribed to us. Any outgoing
// central attempt we were still holding is now not just useless but ACTIVELY HARMFUL — radioWrite
// picks its branch by which handles are non-nil and tests the central branch FIRST, so a surviving
// _PeerDevice/_RemoteDatagram silently captures every send and posts it into a connection the peer
// is not listening on. Both write paths return YES, so it reports success either way.
//
// That is #206's one-way link, caught on the pair 2026-08-16 by the write-path diagnostic:
//
//   central subscribed (peripheral) - link ready
//   write path: central=1 peripheral=1 -> taking CENTRAL(write)
//   write path AMBIGUOUS (#206): holding BOTH ...
//
// and on the Android end, five seconds later, `link timeout - peer silent`. Intermittent, because
// it needs an outgoing attempt still in flight when the incoming subscribe lands — which is why
// reading the code could not settle it and one capture appeared to clear it.
- (void)dropOutgoingCentralAttempt {
    if (_PeerDevice == nil && _RemoteDatagram == nil) return;
    BLE_LOG(@"peripheral role settled - dropping our outgoing central attempt so sends cannot "
             "take the wrong branch (#206)");
    if (_PeerDevice) { [_Central cancelPeripheralConnection:_PeerDevice]; _PeerDevice = nil; }
    _RemoteDatagram = nil;
    _Connecting = false;
    _ConnectStage = EConnectStage::Idle;
    _Link.OnConnectResolved();
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
        [self dropOutgoingCentralAttempt];            // #206: we are the peripheral now
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
        // #202: THE LIVE WRITE IS THE EVIDENCE. Peripheral link state (_Subscriber, the binding,
        // _Linked) is established in exactly one place — didSubscribeToCharacteristic — and that
        // callback does NOT reliably fire when a central drops and reconnects to an already-published
        // service. On hardware (2026-08-11) a central reconnected ~15 times over four minutes and
        // neither didSubscribe NOR didUnsubscribe fired once, not even the #83 reject branch.
        //
        // The shape that produces: our own keepalive-driven resetLink had correctly torn this state
        // down (onLinkLost nils _Subscriber and clears the binding, since the peer really was silent),
        // and the reconnect then never rebuilt it. So radioWrite's peripheral branch found no
        // _Subscriber and sent NOTHING, while inbound datagrams kept arriving right here and being
        // delivered into the engine — because this path has no link-state gate. A silent ONE-WAY link:
        // the phone ingests the peer's whole match and cannot answer a byte of it. Nothing was logged
        // because from the driver's point of view there was simply no link, so it had nothing to say.
        // Meanwhile the peer, seeing pure silence, spends its bounded #182 hard radio restarts on its
        // own perfectly healthy radio and gives up.
        //
        // Trusting Req.central here is NOT a new trust decision: AcceptData (#83) has already cleared
        // this central to inject bytes into the lockstep stream, so answering the peer whose data we
        // already accept is strictly weaker than what we just did. This also covers the milder variant
        // where _Subscriber merely points at a previous connection's CBCentral —
        // updateValue:onSubscribedCentrals: returns YES for a dead handle, so radioWrite would report
        // success and BleSendQueue would drain normally while every datagram evaporated.
        // Setting _Subscriber alone is NOT enough, and that is the subtle half: onLinked runs only from
        // didSubscribeToCharacteristic too, so a link recovered this way would leave _Linked/_Connected
        // false — ITransport::IsConnected() then reports no link, Session never sends, and the notify
        // target we just fixed is never used. Recovery has to take the whole peripheral-link path, which
        // is why this mirrors didSubscribe rather than patching one field.
        if (!_Linked || _Subscriber != Req.central) {
            // Same admission decision as a real subscribe, so #83 is preserved exactly: a central that
            // may not be served cannot become our notify target by writing instead of subscribing.
            // (AcceptData above has already passed, so this only ever rejects a genuinely bad case.)
            if (_Binding.AcceptSubscriber(Req.central.identifier.UUIDString.UTF8String)) {
                if (!_NotifyTargetSwapLogged) {
                    _NotifyTargetSwapLogged = true;
                    BLE_LOG(@"peripheral: adopting the link from a live write (#202) — the subscribe "
                             "callback did not fire for this connection");
                }
                _Subscriber = Req.central;
                [self dropOutgoingCentralAttempt];   // #206: same hazard on the adopt path
                [self onLinked];   // idempotent (guards on _Linked); publishes the link to the engine
            }
        }
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

ITransport* CreateBleTransport() {
    g_Transport.EnsureDriver();
    return &g_Transport;
}

} // namespace Lur::Transport
