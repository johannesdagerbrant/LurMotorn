#pragma once
#include <cstdint>

namespace Lur::Net {

// Top byte of every datagram: which kind of message follows. Keeping this a
// single byte (often foldable into spare bits later) preserves the slim-payload
// goal while letting one transport channel carry handshake, moves, and keepalive.
enum class EMsgType : uint8_t {
    Hello       = 0,  // version + role + nonce, exchanged on connect
    ClockPing   = 1,  // clock-sync probe (see ClockSync.h)
    ClockPong   = 2,  // clock-sync reply
    Move        = 3,  // a game move (chess: a legal-move index, ~4-6 bits payload)
    Resign      = 4,
    DrawOffer   = 5,
    Keepalive   = 6,  // detect a silently dropped BLE link
};

// Protocol version negotiated in Hello. Bump on any wire-format change so two
// app versions refuse to mis-decode each other rather than corrupt a game.
inline constexpr uint8_t ProtocolVersion = 1;

// The Session owns the handshake and the rules for which side moves first; it
// sits between ITransport (raw datagrams) and the game (typed events). Fleshed
// out alongside the end-to-end wiring task.
class Session {
    // TODO(net): Hello handshake, role/color assignment, framing of EMsgType +
    // payload, keepalive timer, and dispatch to clock-sync / game handlers.
};

} // namespace Lur::Net
