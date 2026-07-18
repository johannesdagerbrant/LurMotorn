#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "Lur/Transport/Transport.h"

namespace Lur::Net {

// Top byte of every datagram: which kind of message follows. Keeping this a
// single byte (often foldable into spare bits later) preserves the slim-payload
// goal while letting one transport channel carry handshake, moves, and keepalive.
enum class EMsgType : uint8_t {
    Hello       = 0,  // version + nonce, exchanged on connect (see Session::Start)
    ClockPing   = 1,  // clock-sync probe (see ClockSync.h)
    ClockPong   = 2,  // clock-sync reply
    Move        = 3,  // a game move (chess: a legal-move index, ~4-6 bits payload)
    Resign      = 4,
    DrawOffer   = 5,
    Keepalive   = 6,  // detect a silently dropped BLE link
    Sync        = 7,  // full game-state resync after a reconnect (game-defined payload)
};

// Protocol version negotiated in Hello. Bump on any wire-format change so two
// app versions refuse to mis-decode each other rather than corrupt a game.
// v2: Hello gained a trailing "ready" byte for a loss-tolerant handshake.
// v3: Hello carries the persistent device GUID (was a random session nonce), so
//     each peer learns the other's stable identity for colour + the per-opponent
//     stats key (issue #18), independent of the BLE radio role.
// v4: live moves are a BARE 1-byte index (no type tag); every framed message is
//     padded to >=2 bytes, so datagram length disambiguates (issue #19/#15).
// v5: Keepalive carries an 8-byte game state hash so a mid-game desync (a lost move
//     while the link stays up) is detected and auto-healed by re-exchanging Sync,
//     instead of deadlocking with no recovery (issue #72).
inline constexpr uint8_t ProtocolVersion = 5;

// Coarse link state for UI feedback (is a game live? did the link fail?).
enum class ELinkState : uint8_t {
    Searching,        // no peer connected yet (advertising + scanning)
    Handshaking,      // link up, exchanging Hello
    Linked,           // handshake done — the game is live
    Disconnected,     // the link came up once and then dropped
    VersionMismatch,  // peer speaks a different ProtocolVersion
};

// The symmetric peer-to-peer session that sits between ITransport (raw datagrams)
// and the game (typed messages). It owns two things:
//
//   1. The Hello HANDSHAKE. On connect each peer sends its ProtocolVersion + its
//      persistent device GUID, so each learns the other's stable identity. The game
//      derives colour + the per-opponent stats key from the two GUIDs (GUID order +
//      match parity) — independent of the BLE peripheral/central role (a radio
//      mechanic only), so there is no "host".
//
//   2. MESSAGE FRAMING. Send() prepends the EMsgType byte; inbound datagrams are
//      dispatched to a per-type handler. The session is game-agnostic — it moves
//      opaque payload bytes and never names a chess type; the chess layer encodes
//      a move into those bytes with Chess::MoveCodec.
//
// Not thread-safe: drive it from one thread. Per the ITransport contract the
// receiver fires on the engine thread, which is also where Tick()/Send() are called.
class Session {
public:
    using Handler = std::function<void(const uint8_t* Payload, std::size_t Size)>;

    // Begin the session over Transport (which must outlive the session). LocalGuid is
    // this device's persistent id (Lur::Save::LoadOrCreateDeviceId); it is exchanged
    // in the Hello so each peer learns the other's stable identity. Installs the
    // transport receiver and sends the first Hello.
    void Start(Lur::Transport::ITransport* NewTransport, std::string_view LocalGuid);

    // Drive Hello retransmission + link liveness. Call once per frame with the wall-
    // clock nanoseconds elapsed since the previous call. Timing is denominated in real
    // time, NOT frame count, so keepalive/timeout behave identically at 60 Hz, 120 Hz,
    // or under a throttled loop. (A paused loop — e.g. a backgrounded iOS app whose
    // CADisplayLink stops — simply sends no keepalives; the peer's own timeout +
    // reconnect flow is the recovery path, by design.) A no-op for handshaking once
    // ready except for keepalive/timeout.
    void Tick(uint64_t ElapsedNs);

    bool IsReady() const { return Ready; }

    // True from the moment the link is (re)established until the peer's link-time Sync
    // has been received (or a short fallback timeout elapses). While this holds, the
    // game must NOT make or apply live moves: a bare move index is only meaningful
    // against a board both peers agree on, and the reconciling Sync hasn't landed yet.
    // Applying a move before the resync silently decodes it against an unreconciled
    // board -> permanent divergence -> deadlock (issue #71). The game gates on this in
    // CanMoveNow() (local moves) and its move handler (inbound moves).
    bool IsAwaitingResync() const { return AwaitingResync; }

    // The peer's persistent device id once ready, else empty. The game pairs it with
    // our own id to derive colour + the per-opponent stats key (independent of the
    // BLE radio role).
    const std::string& GetPeerGuid() const { return PeerGuid; }

    // Coarse link state for a UI indicator. Cheap; call each frame.
    ELinkState GetLinkState() const {
        if (VersionMismatchSeen) return ELinkState::VersionMismatch;
        if (Transport != nullptr && Transport->IsConnected())
            return Ready ? ELinkState::Linked : ELinkState::Handshaking;
        return EverConnected ? ELinkState::Disconnected : ELinkState::Searching;
    }

    // Lightweight counters + liveness, for the debug overlay (issue #54). Cheap reads.
    uint32_t GetDatagramsSent() const { return DatagramsSent; }
    uint32_t GetDatagramsReceived() const { return DatagramsReceived; }
    uint64_t GetNsSinceRecv() const { return SinceRecvNs; }

    // Register the handler for one application message type (framed, >=2 bytes).
    void SetHandler(EMsgType Type, Handler H);

    // Register the handler for a live move — a bare 1-byte index datagram (no type
    // tag). Since every framed message is >=2 bytes, a 1-byte datagram is always a
    // move, so it needs no type (issue #19). The payload is the move's index bits.
    void SetMoveHandler(Handler H) { MoveHandler = std::move(H); }

    // Send a live move: the raw index byte with NO type prefix, so it arrives as a
    // 1-byte datagram. A forced move (0-bit index) still sends a single 0 byte.
    void SendMove(const uint8_t* Data, std::size_t Size);

    // Fired once, when the handshake completes and the seat is known.
    void SetReadyHandler(std::function<void()> H) { ReadyHandler = std::move(H); }

    // Fired when the link is re-established after a drop (post-handshake). This is
    // the generic reconnect-flow shell: the engine detects the reconnect and pokes
    // the game, which resynchronises its own state (e.g. exchange move history via
    // an EMsgType::Sync message) so both peers converge again. Also fired when a
    // mid-game desync is detected (see RequestResync / the keepalive state hash).
    void SetResyncHandler(std::function<void()> H) { ResyncHandler = std::move(H); }

    // Optional hook returning a hash of the game's authoritative state (chess: the
    // board position). If set, it rides every Keepalive; the peer compares it to its
    // own and, on a mismatch, triggers a resync — so a mid-game divergence (a live
    // move that was lost while the link stayed up) self-heals instead of deadlocking
    // (issue #72). Must be identical on both peers when they agree (game-defined).
    void SetStateHashFn(std::function<uint64_t()> F) { StateHashFn = std::move(F); }

    // Force a resync now: hold moves (IsAwaitingResync) and fire ResyncHandler so the
    // game re-sends its state. Call when the game detects a desync (e.g. an inbound
    // move that won't decode against the local board). Also invoked internally on a
    // keepalive state-hash mismatch. Both peers re-exchanging Sync reconciles them.
    void RequestResync();

    // Optional debug sink for handshake tracing. The app supplies a platform logger
    // (logcat / os_log); the session stays platform-free. No-op if unset.
    using LogFn = std::function<void(const char* Line)>;
    void SetLogger(LogFn L) { Log = std::move(L); }

    // Frame [Type][Payload] and send it to the peer. Returns false (and logs) if the
    // payload exceeds MaxFramedPayload, so an over-budget message fails LOUDLY instead
    // of being silently dropped — most payloads are tiny (a move is ~1 byte), but a
    // reconnect Sync grows with the game (issue: the old 64-byte cap silently killed
    // resync past ply ~61). Never truncates the wire.
    bool Send(EMsgType Type, const uint8_t* Payload, std::size_t Size);

private:
    void SendHello();
    void SendKeepalive();
    void OnDatagram(const uint8_t* Data, std::size_t Size);
    void OnHello(const uint8_t* Payload, std::size_t Size);
    void Logf(const char* Fmt, ...);

    static constexpr int         MaxMsgTypes     = 8;   // indices 0..7 (covers Sync = 7)
    static constexpr std::size_t GuidLen          = 32;  // 128-bit id as hex (Lur::Save::DeviceIdHexLen)

    // Real-time link timing (nanoseconds). Frame-rate-independent: derived from the
    // ElapsedNs fed to Tick(), not a tick count, so these mean the same on a 60 Hz or
    // 120 Hz display and don't drift under a throttled loop.
    static constexpr uint64_t HelloResendNs = 500'000'000ull;   // resend Hello every ~0.5s
    static constexpr uint64_t KeepaliveNs   = 1'000'000'000ull; // keepalive every ~1s
    static constexpr uint64_t LinkTimeoutNs = 5'000'000'000ull; // ~5s of silence -> dead
    // If the peer's link-time Sync never arrives (e.g. it adopted a different game),
    // stop blocking moves after this so a missing Sync can't wedge the game forever.
    static constexpr uint64_t ResyncTimeoutNs = 3'000'000'000ull; // ~3s fallback

    // Max payload for a FRAMED ([type][payload]) message. Both backends negotiate an
    // ATT MTU of 517 (seen on every link), so one datagram carries MTU-3 = 514 bytes;
    // minus the 1-byte type tag that leaves 513 for the payload. This was 254 — which
    // still silently dropped a full-history Sync once the accumulated move record passed
    // ~254 bytes (issue #41 / #72: a mid-game resync then never delivered and the game
    // wedged). Sized to the real MTU now. A game whose record exceeds even this can't
    // fit one datagram; a compact position-snapshot resync (or fragmentation) is the
    // follow-up. Send() refuses+logs past this bound rather than truncating the wire.
    static constexpr std::size_t MaxFramedPayload = 512;

    // Link liveness: once ready we send a Keepalive every KeepaliveNs; if NO datagram
    // arrives for LinkTimeoutNs we declare the link dead and ask the transport to
    // reset. This is what lets an iOS peripheral notice an abruptly-killed central (its
    // CBPeripheralManager gets no disconnect callback); every other role also detects a
    // real drop via the backend, so this is a belt-and-suspenders safety net there.

    Lur::Transport::ITransport* Transport = nullptr;
    std::string LocalGuid;
    std::string PeerGuid;
    bool     Ready         = false;
    bool     HelloEverSent = false;  // send the first Hello immediately, then resend on interval
    uint64_t HelloResendAccumNs = 0; // ns since our last Hello (handshake)
    uint64_t KeepaliveAccumNs   = 0; // ns since our last keepalive send
    uint64_t SinceRecvNs        = 0; // ns since ANY datagram arrived (liveness)
    uint32_t DatagramsSent      = 0; // total datagrams sent (overlay/debug)
    uint32_t DatagramsReceived  = 0; // total datagrams received (overlay/debug)
    bool     EverConnected      = false;  // for Disconnected vs never-connected
    bool     VersionMismatchSeen = false;
    bool     PrevConnected      = false;  // edge-detect reconnects for the resync hook
    bool     AwaitingResync     = false;  // hold moves until the link-time Sync lands (#71)
    uint64_t ResyncWaitNs       = 0;      // ns spent awaiting the peer's Sync (fallback timeout)
    uint64_t LastPeerHash       = 0;      // peer's previous keepalive hash (#72 desync detect)
    bool     HavePeerHash       = false;  // have we seen a peer keepalive hash yet?

    Handler               Handlers[MaxMsgTypes];
    Handler                 MoveHandler;        // bare 1-byte live move (issue #19)
    std::function<void()>   ReadyHandler;
    std::function<void()>   ResyncHandler;
    std::function<uint64_t()> StateHashFn;      // rides Keepalive for desync detection (#72)
    LogFn                   Log;
};

} // namespace Lur::Net
