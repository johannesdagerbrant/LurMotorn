#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace Lur::Transport {

// The one interface the engine and games speak to move bytes between two phones.
//
// Nothing above this line knows whether the link is BLE, a future WiFi/UDP
// backend, or an in-process loopback used by tests. That ignorance is the whole
// point: it's what lets the same game code run over BLE today and lets a reflex
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

    // Deliver one datagram AHEAD of anything the backend already has queued (#190).
    //
    // For a latency-critical datagram — the one a player is waiting to see land. A backend with
    // a send queue is otherwise FIFO with no notion of urgency, so this datagram could wait
    // behind a keepalive or a multi-datagram resync payload, each wait costing a whole
    // connection interval. Ordering among expedited datagrams is preserved.
    //
    // NOT pure: the default is plain Send, which is correct for any transport without a queue
    // (loopback, tests). A backend that has one overrides this. Callers may always call it; the
    // worst case is that it behaves exactly like Send.
    //
    // Urgency is stated by the caller and never inferred from the bytes. Two backends used to
    // guess it from the datagram's LENGTH ("1 byte means a live move"), which put one game's
    // wire format inside the transport and then broke silently when that format changed.
    virtual void SendExpedited(const uint8_t* Data, std::size_t Size) { Send(Data, Size); }

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

    // A HARDER reset than ResetLink, for a link the net layer has judged HALF-OPEN: still
    // connected, our writes leave, but the peer's notify path is wedged so nothing inbound
    // ever arrives (issue #163). A soft ResetLink drops+rediscovers the *link*; it provably
    // cannot clear a stuck BLE **stack** — on hardware 2026-08-02 the central did 80 soft
    // resets and a full Bluetooth toggle with no effect. So this tears down the radio object
    // one level deeper and rebuilds it: Android closes+reopens the whole BluetoothGatt (and
    // refresh()es the stale service cache) rather than merely disconnecting; iOS re-adds the
    // CBPeripheralManager service so a stale subscription is re-established, not just
    // re-advertised. The net layer invokes it a BOUNDED number of times (see Session's
    // MaxRadioRestarts) so the recovery can't itself become the churn that degrades the radio,
    // then falls back to telling the player to reboot. Best-effort and per-OS; a backend with
    // no deeper reset than ResetLink (loopback, tests) leaves this a no-op — the escalation
    // then simply achieves nothing and the human-action banner is the floor, which is #182's
    // whole premise (a radio restart may only help a SUBSET of wedges). Not host-testable end
    // to end (the wedge is a real-radio phenomenon); the Session-side bounding IS unit-tested.
    virtual void RestartRadio() {}
};

} // namespace Lur::Transport
