#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include "Lur/Transport/Transport.h"

namespace Lur::Transport {

// An in-process ITransport pair, for tests and host bring-up.
//
// Whatever one endpoint Sends is delivered synchronously to the other endpoint's
// Receiver — no radio, no threads, no framing. It exists so the net and game-codec
// layers can exercise a full datagram round-trip on the host (and in CI) long
// before any BLE backend is wired up. Because it satisfies the same ITransport
// contract the BLE backends do, code tested against Loopback runs unchanged over
// the real link.
class LoopbackTransport : public ITransport {
public:
    void Send(const uint8_t* Data, std::size_t Size) override {
        if (Peer != nullptr && Peer->ReceiverFn) {
            Peer->ReceiverFn(Data, Size);
        }
    }

    void SetReceiver(Receiver NewReceiver) override { ReceiverFn = std::move(NewReceiver); }

    bool IsConnected() const override { return Peer != nullptr; }

    // Wire two endpoints together in both directions. After this, A.Send reaches
    // B's receiver and vice versa.
    static void Link(LoopbackTransport& A, LoopbackTransport& B) {
        A.Peer = &B;
        B.Peer = &A;
    }

private:
    LoopbackTransport* Peer = nullptr;
    Receiver           ReceiverFn;
};

} // namespace Lur::Transport
