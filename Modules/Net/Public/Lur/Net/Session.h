#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
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
inline constexpr uint8_t ProtocolVersion = 2;

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
//   1. The Hello HANDSHAKE. On connect each peer sends its ProtocolVersion + a
//      random 64-bit nonce. Comparing the two nonces yields a "seat" (0 or 1) that
//      the two peers agree on but resolve to OPPOSITE values — a deterministic
//      tie-break, so authority is NOT taken from the BLE peripheral/central role
//      (which is a radio mechanic only) and there is no "host". The chess layer
//      maps seat 0 -> White, seat 1 -> Black.
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

    // Begin the session over Transport (which must outlive the session). LocalNonce
    // seeds the seat tie-break; the caller supplies platform randomness and it must
    // differ from the peer's (a 64-bit random value collides with negligible
    // probability). Installs the transport receiver and sends the first Hello.
    void Start(Lur::Transport::ITransport* NewTransport, uint64_t Nonce);

    // Drive Hello retransmission. Call periodically (e.g. once per rendered frame):
    // the first Hello can be dropped when it is sent before the BLE link is up, so
    // we resend until the handshake completes. A no-op once ready.
    void Tick();

    bool IsReady() const { return Ready; }

    // The local seat once ready (0 or 1, agreed opposite on the two peers), else -1.
    int GetSeat() const { return Ready ? Seat : -1; }

    // Coarse link state for a UI indicator. Cheap; call each frame.
    ELinkState GetLinkState() const {
        if (VersionMismatchSeen) return ELinkState::VersionMismatch;
        if (Transport != nullptr && Transport->IsConnected())
            return Ready ? ELinkState::Linked : ELinkState::Handshaking;
        return EverConnected ? ELinkState::Disconnected : ELinkState::Searching;
    }

    // Register the handler for one application message type (e.g. EMsgType::Move).
    void SetHandler(EMsgType Type, Handler H);

    // Fired once, when the handshake completes and the seat is known.
    void SetReadyHandler(std::function<void()> H) { ReadyHandler = std::move(H); }

    // Fired when the link is re-established after a drop (post-handshake). This is
    // the generic reconnect-flow shell: the engine detects the reconnect and pokes
    // the game, which resynchronises its own state (e.g. exchange move history via
    // an EMsgType::Sync message) so both peers converge again.
    void SetResyncHandler(std::function<void()> H) { ResyncHandler = std::move(H); }

    // Optional debug sink for handshake tracing. The app supplies a platform logger
    // (logcat / os_log); the session stays platform-free. No-op if unset.
    using LogFn = std::function<void(const char* Line)>;
    void SetLogger(LogFn L) { Log = std::move(L); }

    // Frame [Type][Payload] and send it to the peer. Payloads are tiny (a move is
    // ~1 byte); oversized payloads are dropped rather than truncated.
    void Send(EMsgType Type, const uint8_t* Payload, std::size_t Size);

private:
    void SendHello();
    void SendKeepalive();
    void OnDatagram(const uint8_t* Data, std::size_t Size);
    void OnHello(const uint8_t* Payload, std::size_t Size);
    void Logf(const char* Fmt, ...);

    static constexpr int      MaxMsgTypes     = 8;   // covers EMsgType 0..6
    static constexpr unsigned HelloResendTicks = 30; // ~0.5s at 60 fps

    // Link liveness (assuming ~60 fps ticks). Once ready we send a Keepalive every
    // second; if NO datagram arrives for LinkTimeoutTicks we declare the link dead
    // and ask the transport to reset. This is what lets an iOS peripheral notice an
    // abruptly-killed central (its CBPeripheralManager gets no disconnect callback);
    // every other role also detects a real drop via the backend, so this is a
    // belt-and-suspenders safety net there.
    static constexpr unsigned KeepaliveTicks  = 60;  // ~1s between keepalives
    static constexpr unsigned LinkTimeoutTicks = 300; // ~5s of silence -> dead

    Lur::Transport::ITransport* Transport = nullptr;
    uint64_t LocalNonce   = 0;
    uint64_t PeerNonce    = 0;
    bool     Ready        = false;
    int      Seat         = -1;
    unsigned TickCounter  = 0;
    unsigned KeepaliveCounter = 0;   // ticks since our last keepalive send
    unsigned SinceRecvTicks   = 0;   // ticks since ANY datagram arrived (liveness)
    bool     EverConnected      = false;  // for Disconnected vs never-connected
    bool     VersionMismatchSeen = false;
    bool     PrevConnected      = false;  // edge-detect reconnects for the resync hook

    Handler               Handlers[MaxMsgTypes];
    std::function<void()> ReadyHandler;
    std::function<void()> ResyncHandler;
    LogFn                 Log;
};

} // namespace Lur::Net
