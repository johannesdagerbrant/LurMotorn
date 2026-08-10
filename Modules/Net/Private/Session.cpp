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

void Session::PumpInbox() {
    // Deliberately ONLY the transport pump — see the header. Anything time-denominated
    // added here would be double-counted, because Tick calls this too.
    if (Transport != nullptr) Transport->Pump();
}

void Session::Tick(uint64_t ElapsedNs) {
    // Drain any radio-thread events onto THIS (engine) thread first, so inbound
    // datagrams + connect/disconnect are processed here, before we read link state
    // below — honouring the "receiver fires on the engine thread" contract (issue #40).
    PumpInbox();

    const bool Connected = Transport != nullptr && Transport->IsConnected();

    // Reconnect edge (post-handshake): the link came back after a drop. Poke the
    // game to resynchronise its state. Generic flow — the payload is game-defined.
    // Also arm the resync gate: hold moves until the peer's Sync reconciles boards (#71).
    if (Ready && Connected && !PrevConnected) {
        Logf("reconnected — requesting resync");
        AwaitingResync = true; ResyncWaitNs = 0;
        if (ResyncHandler) ResyncHandler();
    }
    PrevConnected = Connected;

    if (Connected) EverConnected = true;  // latch, so a later drop reads as Disconnected
    else AwaitingResync = false;          // offline: not awaiting a resync (offline moves ok, #19)

    if (Ready && Connected) {
        // Resync-gate fallback: if the peer's Sync never arrives, stop blocking moves
        // after ResyncTimeoutNs so a missing Sync can't wedge the game (#71).
        if (AwaitingResync) {
            ResyncWaitNs += ElapsedNs;
            if (ResyncWaitNs >= ResyncTimeoutNs) {
                AwaitingResync = false;
                Logf("resync wait timed out — enabling moves");
            }
        }
        // Liveness: keep the link warm and notice if the peer went silent. Any inbound
        // datagram resets SinceRecvNs (see OnDatagram); if it runs out, the link is
        // dead even though the backend never told us (the iOS-peripheral case).
        KeepaliveAccumNs += ElapsedNs;
        if (KeepaliveAccumNs >= KeepaliveNs) { KeepaliveAccumNs = 0; SendKeepalive(); }
        SinceRecvNs += ElapsedNs;
        // #163: once the link is confirmed HALF-OPEN, back off — a soft reset cannot clear a wedged
        // BLE stack, and hammering ResetLink every LinkTimeoutNs churns the radio further (that churn
        // is what wedges it). Until then, reset aggressively so a genuine transient blip recovers fast.
        const uint64_t ResetAfterNs = LinkHalfOpen_ ? HalfOpenResetNs : LinkTimeoutNs;
        if (SinceRecvNs >= ResetAfterNs) {
            ++SilentResets_;
            SinceRecvNs = 0;
            // The half-open verdict, logged ONCE on the transition — not every cycle, which would
            // bury the log at exactly the moment someone is trying to read why the match won't start.
            if (SilentResets_ >= HalfOpenResetThreshold && !LinkHalfOpen_) {
                LinkHalfOpen_ = true;
                Logf("link HALF-OPEN — %d peer-silent resets, no inbound between them: we are "
                     "connected and our writes leave, but the peer's notify path is wedged. A soft "
                     "reset cannot clear a stuck BLE stack — the SILENT peer must toggle Bluetooth or "
                     "reboot. Backing off resets and escalating to a hard radio restart.", SilentResets_);
            }
            if (!LinkHalfOpen_) {
                // Transient blip: a soft ResetLink recovers it fast, so reset aggressively.
                Logf("link timeout — peer silent, resetting transport");
                if (Transport) Transport->ResetLink();  // drop + resume discovery -> reconnect
            } else if (Transport != nullptr && !Transport->CanRestartRadio()) {
                // The backend has no hard restart. Say THAT, once, instead of narrating an
                // escalation that cannot happen: the log used to claim three radio restarts against
                // a transport that had never implemented one, which sent a real diagnosis down the
                // wrong path (2026-08-09). A missing capability is a finding, not a silence.
                if (RadioRestarts_ == 0) {
                    ++RadioRestarts_;   // latch, so this says itself once per episode
                    Logf("half-open, and this transport cannot restart the radio — no escalation is "
                         "possible here. The wedged peer must toggle Bluetooth or reboot.");
                }
            } else if (RadioRestarts_ < MaxRadioRestarts) {
                // #182 escalation: the soft reset above provably can't clear a WEDGED stack (80 of them
                // cleared nothing on hardware), so once half-open, escalate to the harder per-OS radio
                // restart — bounded by MaxRadioRestarts so the recovery can't itself become the churn
                // that degrades the radio further (#163's lesson). RestartRadio() also rediscovers, so a
                // peer that reboots mid-episode still reconnects.
                ++RadioRestarts_;
                Logf("half-open: escalating to a full radio restart (attempt %d/%d) — a soft reset "
                     "can't clear a wedged BLE stack", RadioRestarts_, MaxRadioRestarts);
                if (Transport) Transport->RestartRadio();
            }
            // else: cap reached — neither soft nor hard reset cleared it. Stop touching the radio (the
            // last RestartRadio left discovery running, so a human fix on the far phone still reconnects)
            // and let the surfaced LINK STALLED banner drive the guaranteed fix. Silent by design: the
            // "attempt N/N" line above already said the last try fired; re-logging every 20s would only
            // bury it.
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

bool Session::Send(EMsgType Type, const uint8_t* Payload, std::size_t Size,
                   Lur::Transport::EBleSendPriority Priority) {
    if (Transport == nullptr) return false;
    if (Size > MaxFramedPayload) {                // never truncate the wire — fail loudly
        Logf("send DROPPED: framed payload %zu > max %zu (type=%u)",
             Size, MaxFramedPayload, static_cast<unsigned>(Type));
        return false;
    }
    uint8_t Frame[1 + MaxFramedPayload];          // [type][payload]
    Frame[0] = static_cast<uint8_t>(Type);
    for (std::size_t i = 0; i < Size; ++i) Frame[1 + i] = Payload[i];
    if (Priority == Lur::Transport::EBleSendPriority::Expedited)
        Transport->SendExpedited(Frame, 1 + Size);
    else
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

void Session::SendKeepalive() {
    if (Transport == nullptr || !Transport->IsConnected()) return;
    if (StateHashFn) {
        // v5: carry the game state hash so the peer can spot a mid-game desync (#72).
        const uint64_t H = StateHashFn();
        uint8_t P[8];
        for (int i = 0; i < 8; ++i) P[i] = static_cast<uint8_t>(H >> (8 * i));
        Send(EMsgType::Keepalive, P, sizeof(P));   // [type][8-byte hash]
    } else {
        const uint8_t Pad = 0;
        Send(EMsgType::Keepalive, &Pad, 1);        // [type][pad] = 2 bytes, never a 1-byte move
    }
}

// Force a resync: gate moves and ask the game to re-send its state. Both peers doing
// this (each also detects the mismatch on the other's keepalive) re-exchange Sync and
// MergeIfNewer reconciles them — self-healing a mid-game desync (#72).
void Session::RequestResync() {
    if (Transport == nullptr || !Ready) return;
    if (AwaitingResync) return;                    // already resyncing; don't storm
    AwaitingResync = true; ResyncWaitNs = 0;
    Logf("requesting resync (re-sending state)");
    if (ResyncHandler) ResyncHandler();
}

void Session::OnDatagram(const uint8_t* Data, std::size_t Size) {
    if (Size == 0) return;
    SinceRecvNs = 0;  // any traffic from the peer proves the link is alive
    // #163: real inbound proves the notify path is working, so a half-open verdict — or the count
    // building toward one — is retired HERE and nowhere else: it clears on evidence, not a timer,
    // the discipline a latch needs in general: it clears on evidence, so it cannot outlive its fault.
    if (LinkHalfOpen_) Logf("link recovered — inbound traffic resumed after a half-open stall");
    SilentResets_ = 0;
    LinkHalfOpen_ = false;
    RadioRestarts_ = 0;  // #182: next half-open episode gets a fresh budget of hard restarts
    ++DatagramsReceived;

    // Every datagram is framed: byte 0 is the type, the rest is the payload. A 1-byte
    // datagram is therefore a type with an EMPTY payload, dispatched like any other.
    const EMsgType Type = static_cast<EMsgType>(Data[0]);
    const uint8_t* Payload = Data + 1;
    const std::size_t PayloadSize = Size - 1;

    if (Type == EMsgType::Hello) { OnHello(Payload, PayloadSize); return; }
    if (Type == EMsgType::Keepalive) {
        // v5 keepalives carry the peer's state hash. A mismatch alone is NOT proof of
        // desync: during normal play a move is often in flight (we moved, the peer's
        // keepalive still reflects the pre-move board), which differs only transiently.
        // Only resync when the peer is STUCK at the SAME divergent hash across two
        // keepalives — an in-flight move advances the hash, so it won't repeat, but a
        // genuine desync (both peers waiting) holds a constant, mismatched hash (#72).
        if (PayloadSize >= 8 && StateHashFn && Ready && !AwaitingResync) {
            uint64_t Peer = 0;
            for (int i = 0; i < 8; ++i) Peer |= static_cast<uint64_t>(Payload[i]) << (8 * i);
            if (Peer != StateHashFn() && HavePeerHash && Peer == LastPeerHash) {
                Logf("keepalive state mismatch persisted — desync, requesting resync");
                RequestResync();
            }
            LastPeerHash = Peer; HavePeerHash = true;
        }
        return;  // liveness only; already counted above
    }

    Logf("recv msg type=%u size=%zu", static_cast<unsigned>(Data[0]), PayloadSize);
    // The peer's link-time Sync reconciles both boards: lift the resync gate so live
    // moves may flow again (#71). The handler below applies the reconciling payload.
    if (Type == EMsgType::Sync && AwaitingResync) {
        AwaitingResync = false;
        Logf("resync received — moves enabled");
    }
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
    AwaitingResync = true; ResyncWaitNs = 0;  // hold moves until the peer's Sync lands (#71)
    Logf("READY (peer id known)");
    SendHello();  // ready-flagged reply, so the peer learns our id + that we're set
    if (ReadyHandler) ReadyHandler();
}

} // namespace Lur::Net
