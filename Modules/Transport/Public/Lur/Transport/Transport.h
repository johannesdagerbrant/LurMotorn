#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace Lur::Transport {

// The one interface the engine and games speak to move bytes between two phones.
//
// Nothing above this line knows whether the link is BLE, a future WiFi/UDP
// backend, or an in-process loopback used by tests. That ignorance is the whole
// point: it's what lets the same chess code run over BLE today and lets a reflex
// game swap in a lower-latency transport tomorrow without touching game logic.
//
// Payloads handed to Send() are already minimal (see Lur::Serialization); the
// backend transmits them as-is and must not re-frame or bloat them.
class ITransport {
public:
    virtual ~ITransport() = default;

    // Deliver one datagram to the peer. Best-effort; ordering/reliability beyond
    // what the backend guarantees is the net module's concern, per message type.
    virtual void Send(const uint8_t* Data, std::size_t Size) = 0;

    // Invoked on the engine thread when a datagram arrives from the peer.
    using Receiver = std::function<void(const uint8_t* Data, std::size_t Size)>;
    virtual void SetReceiver(Receiver NewReceiver) = 0;

    // True once the link is established and usable.
    virtual bool IsConnected() const = 0;

    // Drain any inbound events queued by the radio thread onto the CALLING thread,
    // which the net layer calls once per Tick() so the receiver + connection state
    // always land on the engine thread (see EventInbox / issue #40). A synchronous
    // backend (loopback, tests) delivers inline and leaves this a no-op.
    virtual void Pump() {}

    // Force the current link down and resume discovery. Called by the net layer when
    // its keepalive times out — i.e. the link is silently dead but the backend never
    // got a disconnect callback. This is the ONLY reliable path on an iOS peripheral,
    // whose CBPeripheralManager gets no notification when a central is abruptly
    // killed. Backends that always receive a real disconnect callback (any central,
    // and Android's GATT server) can leave this a no-op.
    virtual void ResetLink() {}
};

} // namespace Lur::Transport
