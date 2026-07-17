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

void Session::Start(Lur::Transport::ITransport* NewTransport, std::string_view Guid) {
    Transport = NewTransport;
    LocalGuid = std::string(Guid);
    if (Transport == nullptr) return;
    Transport->SetReceiver([this](const uint8_t* Data, std::size_t Size) {
        OnDatagram(Data, Size);
    });
    Logf("start: local id set (%zu bytes)", LocalGuid.size());
    SendHello();  // best-effort; if the link isn't up yet, Tick() resends
}

void Session::Tick(uint64_t ElapsedNs) {
    // Drain any radio-thread events onto THIS (engine) thread first, so inbound
    // datagrams + connect/disconnect are processed here, before we read link state
    // below — honouring the "receiver fires on the engine thread" contract (issue #40).
    if (Transport != nullptr) Transport->Pump();

    const bool Connected = Transport != nullptr && Transport->IsConnected();

    // Reconnect edge (post-handshake): the link came back after a drop. Poke the
    // game to resynchronise its state. Generic flow — the payload is game-defined.
    if (Ready && Connected && !PrevConnected) {
        Logf("reconnected — requesting resync");
        if (ResyncHandler) ResyncHandler();
    }
    PrevConnected = Connected;

    if (Connected) EverConnected = true;  // latch, so a later drop reads as Disconnected

    if (Ready && Connected) {
        // Liveness: keep the link warm and notice if the peer went silent. Any inbound
        // datagram resets SinceRecvNs (see OnDatagram); if it runs out, the link is
        // dead even though the backend never told us (the iOS-peripheral case).
        KeepaliveAccumNs += ElapsedNs;
        if (KeepaliveAccumNs >= KeepaliveNs) { KeepaliveAccumNs = 0; SendKeepalive(); }
        SinceRecvNs += ElapsedNs;
        if (SinceRecvNs >= LinkTimeoutNs) {
            Logf("link timeout — peer silent, resetting transport");
            SinceRecvNs = 0;
            if (Transport) Transport->ResetLink();  // drop + resume discovery -> reconnect
        }
        return;
    }
    if (Ready) return;
    // Resend Hello: immediately the first time (snappy once connected), then every
    // HelloResendNs, until the handshake completes.
    HelloResendAccumNs += ElapsedNs;
    if (!HelloEverSent || HelloResendAccumNs >= HelloResendNs) {
        HelloEverSent = true;
        HelloResendAccumNs = 0;
        SendHello();
    }
}

void Session::SetHandler(EMsgType Type, Handler H) {
    const int Idx = static_cast<int>(Type);
    if (Idx >= 0 && Idx < MaxMsgTypes) Handlers[Idx] = std::move(H);
}

bool Session::Send(EMsgType Type, const uint8_t* Payload, std::size_t Size) {
    if (Transport == nullptr) return false;
    if (Size > MaxFramedPayload) {                // never truncate the wire — fail loudly
        Logf("send DROPPED: framed payload %zu > max %zu (type=%u)",
             Size, MaxFramedPayload, static_cast<unsigned>(Type));
        return false;
    }
    uint8_t Frame[1 + MaxFramedPayload];          // [type][payload]
    Frame[0] = static_cast<uint8_t>(Type);
    for (std::size_t i = 0; i < Size; ++i) Frame[1 + i] = Payload[i];
    Transport->Send(Frame, 1 + Size);
    ++DatagramsSent;
    return true;
}

void Session::SendHello() {
    // Sending before the link is up would just be dropped; skip and let Tick retry.
    if (Transport == nullptr) return;
    if (!Transport->IsConnected()) { Logf("hello: link not up yet"); return; }
    uint8_t Payload[1 + GuidLen + 1];
    Payload[0] = ProtocolVersion;
    for (std::size_t i = 0; i < GuidLen; ++i)     // our device id (zero-padded if short)
        Payload[1 + i] = i < LocalGuid.size() ? static_cast<uint8_t>(LocalGuid[i]) : 0;
    Payload[1 + GuidLen] = Ready ? 1 : 0;         // let the peer know if we're done
    Send(EMsgType::Hello, Payload, sizeof(Payload));
    Logf("hello SENT (ready=%d)", Ready ? 1 : 0);
}

void Session::SendMove(const uint8_t* Data, std::size_t Size) {
    if (Transport == nullptr) return;
    const uint8_t Byte = Size >= 1 ? Data[0] : 0;  // the index byte (0 for a forced move)
    Transport->Send(&Byte, 1);                      // bare, no type -> a 1-byte datagram
    ++DatagramsSent;
}

void Session::SendKeepalive() {
    if (Transport == nullptr || !Transport->IsConnected()) return;
    const uint8_t Pad = 0;
    Send(EMsgType::Keepalive, &Pad, 1);  // [type][pad] = 2 bytes, never a 1-byte move
}

void Session::OnDatagram(const uint8_t* Data, std::size_t Size) {
    if (Size == 0) return;
    SinceRecvNs = 0;  // any traffic from the peer proves the link is alive
    ++DatagramsReceived;

    // A bare 1-byte datagram is a live move (framed messages are always >=2 bytes).
    if (Size == 1) {
        if (MoveHandler) MoveHandler(Data, Size);
        return;
    }

    const EMsgType Type = static_cast<EMsgType>(Data[0]);
    const uint8_t* Payload = Data + 1;
    const std::size_t PayloadSize = Size - 1;

    if (Type == EMsgType::Hello) { OnHello(Payload, PayloadSize); return; }
    if (Type == EMsgType::Keepalive) return;  // liveness only; already counted above

    Logf("recv msg type=%u size=%zu", static_cast<unsigned>(Data[0]), PayloadSize);
    const int Idx = static_cast<int>(Type);
    if (Idx >= 0 && Idx < MaxMsgTypes && Handlers[Idx]) Handlers[Idx](Payload, PayloadSize);
}

void Session::OnHello(const uint8_t* Payload, std::size_t Size) {
    if (Size < 1 + GuidLen + 1) { Logf("hello RECV malformed (size=%zu)", Size); return; }
    const uint8_t PeerVersion = Payload[0];
    if (PeerVersion != ProtocolVersion) {         // refuse to play across wire versions
        VersionMismatchSeen = true;
        Logf("hello RECV version mismatch (peer=%u ours=%u)", PeerVersion, ProtocolVersion);
        return;
    }

    PeerGuid.assign(reinterpret_cast<const char*>(Payload + 1), GuidLen);
    const bool PeerReady = Payload[1 + GuidLen] != 0;
    Logf("hello RECV (peerReady=%d oursReady=%d)", PeerReady ? 1 : 0, Ready ? 1 : 0);

    if (Ready) {
        // We're done. If the peer isn't yet, re-send our Hello so it receives our id
        // even if our earlier reply was dropped (e.g. sent before its link was up).
        // Once the peer is also ready it stops asking, so this can't storm.
        if (!PeerReady) SendHello();
        return;
    }
    if (LocalGuid == PeerGuid) {                  // identical id (cloned save dir?): can't seat
        Logf("hello RECV self-collision: identical GUID — stalling (cloned save dir?)");
        return;
    }

    Ready = true;
    Logf("READY (peer id known)");
    SendHello();  // ready-flagged reply, so the peer learns our id + that we're set
    if (ReadyHandler) ReadyHandler();
}

} // namespace Lur::Net
