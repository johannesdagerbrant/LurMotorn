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

// GATT service the peripheral exposes and the central scans for. This is PER-GAME:
// chess and the RTS ride the SAME engine transport, so if they advertised the same
// service UUID they would cross-link (a chess phone connecting an RTS phone, and vice
// versa). An app gives its game a distinct identity by defining LUR_BLE_SERVICE_UUID at
// build time; the default is chess's original value, so chess's wire identity is
// unchanged. Only the SERVICE UUID needs to differ — it is the advertise/scan
// discriminator; the datagram/device-id characteristics live inside the matched service.
#ifndef LUR_BLE_SERVICE_UUID
#define LUR_BLE_SERVICE_UUID "4C55524D-4F54-4F52-4E00-5472616E7370"
#endif
inline constexpr std::string_view BleServiceUuid = LUR_BLE_SERVICE_UUID;

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

// Is a dev pin currently forcing this device's role? (Always false in Shipping — there is no
// pin.) Callers use this to leave a deliberately pinned configuration alone, and to LOG that
// the pin is why a role looks wrong: the pin outlives the app (an Android system prop survives
// until reboot; the iOS marker until deleted), so a stale one from an earlier rig session is a
// prime suspect whenever the roles come out wrong — and silence made that undiagnosable (#146).
inline bool IsBleRolePinned() {
#if LUR_INTERNAL
    return GBleRoleOverride >= 0;
#else
    return false;
#endif
}

// #146 deadlock breaker.
//
// DecideBleRole is a total order, so two HEALTHY peers always get OPPOSITE answers and exactly
// one of them is Central. Observed on hardware (Android<->iPhone), they didn't: BOTH settled on
// Peripheral, so nobody ever connected and the link never formed — an endless "link not up".
// That state is unrecoverable by retrying, because retrying re-runs the same comparison and
// reaches the same answer; the #79 rediscovery watchdog loops forever on it.
//
// Whatever made the two sides disagree (a bad device-id read on one side, a stale dev role pin,
// an id that isn't what the peer thinks it is), the *shape* of the deadlock is always the same
// and is locally detectable: we connected out, were told "you are the peripheral", deferred —
// and no peer ever came to claim Central. Do that a couple of times fruitlessly and the
// tie-break has forfeited its credibility: stop believing it and take Central ourselves. The
// peer, by its own decision, is a peripheral and is advertising, so a central is exactly what's
// missing. Worst case (we were right all along and the peer is merely slow) we connect to a
// willing peripheral — which is a working link either way.
//
// Deliberate exception: an explicit dev pin is never broken. The dev rig pins Android=Peripheral
// against its central-only Windows peer, and force-taking Central there would break the rig
// instead of fixing it. A stale pin is diagnosed from the log (IsBleRolePinned), not overridden.
inline constexpr int BleMaxPeripheralDefers = 2;

inline EBleRole DecideBleRoleBreaking(std::string_view LocalId, std::string_view PeerId,
                                      int FruitlessDefers) {
    const EBleRole Role = DecideBleRole(LocalId, PeerId);
    if (Role == EBleRole::Peripheral && FruitlessDefers >= BleMaxPeripheralDefers &&
        !IsBleRolePinned())
        return EBleRole::Central;  // breaker: the tie-break has failed us; somebody must connect
    return Role;
}

// #83 — PAIRWISE PEER BINDING. A match is strictly 1:1, and a third device in the room must be able
// to link with SOMEONE ELSE, or wait, but never disturb a live pair.
//
// The holes this closes (found 2026-07-19, present in all four transports — chess + RPS × Android +
// iOS) were all the same shape: the peripheral believed whoever spoke last.
//
//   * MID-MATCH SUBSCRIBER HIJACK. Any CCCD subscription became "the canonical central"
//     (`connectedCentral = device` on Android, `_Subscriber = central` on iOS — unconditional
//     overwrites). Every outgoing notification then silently redirected to the newcomer, and the
//     engine was never told, because onLinked no-ops on the `linked` guard. The real peer goes deaf
//     inside a link that reports itself healthy.
//   * UNFILTERED INBOUND WRITES. Both write handlers delivered datagrams from ANY connected central
//     into the engine, so a third device's bytes inject straight into the lockstep/move stream.
//   * A THIRD DEVICE'S DEPARTURE READ AS LINK LOSS — the hijack in reverse: an outsider could end a
//     match by unsubscribing or disconnecting.
//
// Stopping advertising is not protection: a device that scanned the advertisement before the link
// formed keeps the peer handle and can connect and subscribe later with no fresh advertising. So the
// gate has to be at the point of USE, which is what this is.
//
// Centrals need no equivalent — a central connects to one peripheral it chose, so it is safe by
// construction. Peripherals serve whoever arrives, which is why only they need binding.
//
// It lives HERE, as one tested policy, for the same reason DecideBleRole does: a rule this small was
// still wrong in four hand-maintained copies, and each transport's callbacks are platform code that no
// host test can reach. Kotlin reaches it over JNI (as it already does for the role tie-break); the
// Obj-C++ transports call it directly.
//
// Ids are opaque, platform-supplied strings — a BluetoothDevice address on Android, a CBCentral
// identifier UUID string on iOS. Compared, never parsed. Fixed capacity, no allocation: these calls
// land on a Binder / CoreBluetooth callback thread.
class PeerBinding {
public:
    // Long enough for a MAC address (17) or a UUID string (36) with room to spare.
    static constexpr std::size_t MaxIdLen = 64;

    // A central subscribed to the datagram characteristic. Returns whether it may be served: true if
    // we were unbound (it becomes the peer) or if it IS the bound peer re-subscribing — an MTU
    // renegotiation or a CCCD rewrite does that, and rejecting it would break the real link to defend
    // against a device that isn't there. False for anyone else: do not redirect notifications to them.
    bool AcceptSubscriber(const char* Id) {
        if (Closed_ || !Valid(Id)) return false;
        if (!Bound_) { Store(Id); Bound_ = true; return true; }
        return IsPeer(Id);
    }

    // May a datagram from this central enter the engine? Unbound (pre-link) traffic is allowed —
    // that is the handshake itself, and refusing it would mean no link could ever form. Once bound,
    // only the peer's bytes pass.
    bool AcceptData(const char* Id) const {
        if (Closed_) return false;
        return !Bound_ ? Valid(Id) : IsPeer(Id);
    }

    // Is this the bound peer? The test for "may this disconnect/unsubscribe end the link".
    bool IsPeer(const char* Id) const {
        if (Closed_ || !Bound_ || !Valid(Id)) return false;
        std::size_t K = 0;
        for (; Peer_[K] != '\0' && Id[K] != '\0'; ++K)
            if (Peer_[K] != Id[K]) return false;
        return Peer_[K] == '\0' && Id[K] == '\0';   // equal length too, so no prefix passes
    }

    bool HasPeer() const { return Bound_ && !Closed_; }

    // WE LINKED AS CENTRAL, so our own GATT server has no legitimate peer at all — shut it to
    // everyone.
    //
    // This is the half of #83 the first fix missed, and it is not a variation of the hijack: it is the
    // hijack against the OTHER role. Binding only ever happened on a CCCD subscribe, i.e. on the
    // PERIPHERAL path. A device that linked as central therefore sat in a live match with its binding
    // still OPEN and its GATT server still published — and "stopped advertising" is not protection,
    // because a device that scanned before the link formed keeps the handle. So a third device could
    // connect to a central-role phone, subscribe, and:
    //   * bind itself as that phone's "peer" (AcceptSubscriber succeeded — nobody had bound),
    //   * inject datagrams straight into the lockstep stream (AcceptData then said yes), and
    //   * END THE MATCH by disconnecting, because IsPeer now named the intruder, so onLinkLost fired
    //     on a link that was perfectly healthy.
    // Only the notify path was accidentally safe, because the send picks the client-write branch
    // whenever we hold a GATT client.
    //
    // Closing needs no id matching, which is what makes it right on both platforms: a central's remote
    // handle lives in a different namespace from the CBCentral/BluetoothDevice its server reports, so
    // "bind the same peer from the central side" would have to assume those identifiers agree. They
    // need not. "Serve nobody" is exact, and true: a central receives via notifications on its client,
    // never through its own server's datagram characteristic.
    void Close() { Bound_ = false; Peer_[0] = '\0'; Closed_ = true; }

    // The link is genuinely gone: open up again. This is what keeps chess's deliberate opponent-switch
    // (#38) working — that flow operates at SESSION level after link loss, so the gate must apply only
    // WHILE linked. A binding that outlived the link would forbid ever changing opponents. Reopens a
    // Close()d server too, since the next link may well be one we serve.
    void Clear() { Bound_ = false; Closed_ = false; Peer_[0] = '\0'; }

private:
    // An id must be non-empty AND representable. Empty means a failed read, not a peer. Over-long
    // means malformed — no platform produces one — and REFUSING it is the safe answer: storing a
    // truncated copy would make every id sharing that prefix compare equal, i.e. bind the wrong
    // device, which is the very thing this class exists to prevent.
    static bool Valid(const char* Id) {
        if (Id == nullptr || Id[0] == '\0') return false;
        for (std::size_t K = 0; K < MaxIdLen; ++K)
            if (Id[K] == '\0') return true;
        return false;
    }
    void Store(const char* Id) {
        std::size_t K = 0;
        for (; Id[K] != '\0'; ++K) Peer_[K] = Id[K];   // Valid() proved it fits
        Peer_[K] = '\0';
    }
    char Peer_[MaxIdLen] = {};
    bool Bound_ = false;
    bool Closed_ = false;   // linked as CENTRAL: our server serves nobody until the link drops
};

} // namespace Lur::Transport
