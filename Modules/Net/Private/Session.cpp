#include "Lur/Net/Session.h"

#include <cstdarg>
#include <cstdio>

namespace Lur::Net {

void Session::Logf(const char* Fmt, ...) {
    if (!Log) return;
    char Buf[160];
    va_list Args;
    va_start(Args, Fmt);
    std::vsnprintf(Buf, sizeof(Buf), Fmt, Args);
    va_end(Args);
    Log(Buf);
}

void Session::Start(Lur::Transport::ITransport* NewTransport, uint64_t Nonce) {
    Transport = NewTransport;
    LocalNonce = Nonce;
    if (Transport == nullptr) return;
    Transport->SetReceiver([this](const uint8_t* Data, std::size_t Size) {
        OnDatagram(Data, Size);
    });
    Logf("start: localNonce lo=%08X", static_cast<uint32_t>(LocalNonce));
    SendHello();  // best-effort; if the link isn't up yet, Tick() resends
}

void Session::Tick() {
    const bool Connected = Transport != nullptr && Transport->IsConnected();

    // Reconnect edge (post-handshake): the link came back after a drop. Poke the
    // game to resynchronise its state. Generic flow — the payload is game-defined.
    if (Ready && Connected && !PrevConnected) {
        Logf("reconnected — requesting resync");
        if (ResyncHandler) ResyncHandler();
    }
    PrevConnected = Connected;

    if (Connected) EverConnected = true;  // latch, so a later drop reads as Disconnected
    if (Ready) return;
    // Resend on tick 0 (snappy once connected) and every HelloResendTicks after,
    // until the handshake completes.
    if (TickCounter++ % HelloResendTicks == 0) SendHello();
}

void Session::SetHandler(EMsgType Type, Handler H) {
    const int Idx = static_cast<int>(Type);
    if (Idx >= 0 && Idx < MaxMsgTypes) Handlers[Idx] = std::move(H);
}

void Session::Send(EMsgType Type, const uint8_t* Payload, std::size_t Size) {
    if (Transport == nullptr) return;
    uint8_t Frame[1 + 64];                        // [type][payload]; messages are tiny
    if (Size > sizeof(Frame) - 1) return;         // guard, don't truncate the wire
    Frame[0] = static_cast<uint8_t>(Type);
    for (std::size_t i = 0; i < Size; ++i) Frame[1 + i] = Payload[i];
    Transport->Send(Frame, 1 + Size);
}

void Session::SendHello() {
    // Sending before the link is up would just be dropped; skip and let Tick retry.
    if (Transport == nullptr) return;
    if (!Transport->IsConnected()) { Logf("hello: link not up yet"); return; }
    uint8_t Payload[1 + 8 + 1];
    Payload[0] = ProtocolVersion;
    for (int i = 0; i < 8; ++i)                   // little-endian nonce
        Payload[1 + i] = static_cast<uint8_t>((LocalNonce >> (8 * i)) & 0xFF);
    Payload[9] = Ready ? 1 : 0;                   // let the peer know if we're done
    Send(EMsgType::Hello, Payload, sizeof(Payload));
    Logf("hello SENT (nonce lo=%08X ready=%d)", static_cast<uint32_t>(LocalNonce), Ready ? 1 : 0);
}

void Session::OnDatagram(const uint8_t* Data, std::size_t Size) {
    if (Size == 0) return;
    const EMsgType Type = static_cast<EMsgType>(Data[0]);
    const uint8_t* Payload = Data + 1;
    const std::size_t PayloadSize = Size - 1;

    if (Type == EMsgType::Hello) { OnHello(Payload, PayloadSize); return; }

    Logf("recv msg type=%u size=%zu", static_cast<unsigned>(Data[0]), PayloadSize);
    const int Idx = static_cast<int>(Type);
    if (Idx >= 0 && Idx < MaxMsgTypes && Handlers[Idx]) Handlers[Idx](Payload, PayloadSize);
}

void Session::OnHello(const uint8_t* Payload, std::size_t Size) {
    if (Size < 1 + 8 + 1) { Logf("hello RECV malformed (size=%zu)", Size); return; }
    const uint8_t PeerVersion = Payload[0];
    if (PeerVersion != ProtocolVersion) {         // refuse to play across wire versions
        VersionMismatchSeen = true;
        Logf("hello RECV version mismatch (peer=%u ours=%u)", PeerVersion, ProtocolVersion);
        return;
    }

    uint64_t Nonce = 0;
    for (int i = 0; i < 8; ++i) Nonce |= static_cast<uint64_t>(Payload[1 + i]) << (8 * i);
    const bool PeerReady = Payload[9] != 0;
    PeerNonce = Nonce;
    Logf("hello RECV (peerNonce lo=%08X peerReady=%d oursReady=%d)",
         static_cast<uint32_t>(PeerNonce), PeerReady ? 1 : 0, Ready ? 1 : 0);

    if (Ready) {
        // We're done. If the peer isn't yet, re-send our Hello so it receives our
        // nonce even if our earlier reply was dropped (e.g. sent before its link was
        // up). Once the peer is also ready it stops asking, so this can't storm.
        if (!PeerReady) SendHello();
        return;
    }
    if (LocalNonce == PeerNonce) return;          // (astronomically unlikely) tie: wait

    // A total order on distinct nonces hands the two peers opposite seats for free.
    Seat = (LocalNonce > PeerNonce) ? 0 : 1;
    Ready = true;
    Logf("READY: seat=%d (%s)", Seat, Seat == 0 ? "White" : "Black");
    SendHello();  // ready-flagged reply, so the peer learns our nonce + that we're set
    if (ReadyHandler) ReadyHandler();
}

} // namespace Lur::Net
