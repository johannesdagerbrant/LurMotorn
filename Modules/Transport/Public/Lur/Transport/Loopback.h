#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>
#include "Lur/Transport/Transport.h"

namespace Lur::Transport {

// An in-process ITransport pair, for tests and host bring-up. It satisfies the same
// ITransport contract the BLE backends do, so code tested against Loopback runs
// unchanged over the real link.
//
// Two delivery modes:
//   * SYNCHRONOUS (default): Send() delivers straight to the peer's receiver on the
//     calling stack. Simplest for request/reply tests that check a result right after
//     Send. CAUTION: a receiver that Sends back from inside its own callback recurses
//     (A->B->A...); such users must enable deferred mode.
//   * DEFERRED (SetDeferred(true)): Send() enqueues on the PEER's inbox; the peer's
//     Pump() — which Session::Tick() calls once per tick — drains it to the peer's
//     receiver. This matches the real BLE backends' EventInbox (issue #40) and removes
//     the re-entrancy hazard (issue #76): a lockstep receiver whose inbound input
//     unblocks a tick that emits an anchor no longer recurses — the reply queues for
//     the next Pump. The two-window lockstep desktop uses this so it exercises the same
//     deferred semantics as the phones.
class LoopbackTransport : public ITransport {
public:
    // Switch this endpoint to deferred (queue + Pump) delivery. Set on BOTH endpoints.
    void SetDeferred(bool On) { Deferred = On; }

    void Send(const uint8_t* Data, std::size_t Size) override {
        if (Peer == nullptr) return;
        if (Peer->Deferred) Peer->Inbox.emplace_back(Data, Data + Size);
        else if (Peer->ReceiverFn) Peer->ReceiverFn(Data, Size);
    }

    // Deferred mode: drain our inbound queue to our receiver. Swap the queue out first
    // so a receiver that sends can't disturb the batch we're delivering. No-op when
    // synchronous (nothing is ever queued).
    void Pump() override {
        if (!Deferred) return;
        std::vector<std::vector<uint8_t>> Batch;
        Batch.swap(Inbox);
        if (ReceiverFn)
            for (const std::vector<uint8_t>& Msg : Batch) ReceiverFn(Msg.data(), Msg.size());
    }

    void SetReceiver(Receiver NewReceiver) override { ReceiverFn = std::move(NewReceiver); }

    bool IsConnected() const override { return Peer != nullptr; }

    // Wire two endpoints together in both directions.
    static void Link(LoopbackTransport& A, LoopbackTransport& B) {
        A.Peer = &B;
        B.Peer = &A;
    }

private:
    LoopbackTransport* Peer = nullptr;
    Receiver           ReceiverFn;
    bool               Deferred = false;
    std::vector<std::vector<uint8_t>> Inbox;  // datagrams awaiting our Pump() (deferred mode)
};

} // namespace Lur::Transport
