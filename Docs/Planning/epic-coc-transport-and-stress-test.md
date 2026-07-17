<!--
  Each block below delimited by "===== ISSUE =====" is one GitHub issue.
  The first line of each block is "Title:"; everything after is the issue body (Markdown).
  Example (run from the repo root):
    gh issue create --title "Epic: High-throughput BLE data path (L2CAP CoC) + stream stress-test" \
                    --body-file body.md --label epic,transport
  Create the Epic first, then paste its issue number into the child issues' "Part of #NN" lines.
  Scope: BLE-only, targeting the real test pair (iPhone 11 Pro + Samsung Galaxy A14 / SM-A145).
-->

===== ISSUE =====
Title: Epic: High-throughput BLE data path via L2CAP CoC + stream stress-testing

## Goal

Give the engine a high-throughput Bluetooth data path by moving game data off GATT characteristics/notifications and onto an **L2CAP connection-oriented channel (CoC)** — a socket-like byte stream — while keeping the seamless, proximity-based connect flow exactly as it is today. The epic ends by **stress-testing the real link** so we know the actual throughput, cadence, and latency ceilings before any game commits to a sync model.

Scope is deliberately **BLE-only** for the current test pair (iPhone 11 Pro + Samsung Galaxy A14, SM-A145). Wi-Fi Aware and hotspot are explicitly out of scope: Aware isn't supported on the A14, and hotspot breaks the walk-up-and-play feel.

## Key facts driving the design

- CoC is available on both targets: iOS `CBL2CAPChannel` (iOS 11+), Android `listenUsingL2capChannel` / `createL2capChannel` (API 29+ / Android 10; the A14 is on Android 13+).
- The PSM (the CoC "port") is assigned dynamically and, on iOS, cannot be advertised — so it must be exchanged out-of-band over a small **GATT control characteristic**. That one exchange is the only GATT traffic on the data path.
- **GAP (advertising + scanning) and the existing Pairing/Session layer are untouched.** The "it just connects when you're close" behaviour is a property of discovery, not of the data transport, so swapping GATT→CoC changes nothing about the feel.
- CoC raises **bandwidth and lowers overhead** (no per-notification MTU cap to hand-fragment, no write-with-response ACK per op, credit-based flow control). It does **not** lower the cadence floor — the ~15 ms iOS connection-interval minimum still bounds how often data can go out. The 2M PHY can't be forced on iOS, so treat top-end throughput as best-effort.

## Design at a glance

- **Transport interface** gains a byte-stream `IDataChannel`. Everything above it (session, channels, sync models) is unchanged.
- A thin **GATT control channel** carries the handshake + PSM.
- The **CoC socket** is the data channel; the platform backends implement it natively.
- **Framing** is length-prefixed by the engine (CoC is a stream, not messages).
- Data path is **capability-routed**: CoC when both ends can, else GATT notifications; chess stays on GATT.

## Child issues

- [ ] #NN — Transport: byte-stream data-channel abstraction (`IDataChannel`)
- [ ] #NN — Transport: GATT-bootstrap → L2CAP CoC handoff (`IL2capUpgrade`)
- [ ] #NN — Transport: length-prefix framing over the CoC byte stream
- [ ] #NN — Transport: capability-routed data path (CoC vs GATT), chess stays on GATT
- [ ] #NN — **FINAL** — Stream stress-test harness on the real device pair

## Definition of done

- CoC data channel comes up on the real pair via the GATT PSM handoff, with automatic fallback to GATT.
- A game can send/receive framed messages over the channel without touching Bluetooth specifics.
- Stress-test produces reproducible throughput / cadence / latency numbers over both CoC and GATT, written up so each sync model's affordable budget is known.


===== ISSUE =====
Title: Transport: byte-stream data-channel abstraction (IDataChannel)

Part of #NN (epic).

## Summary

Add a transport-agnostic **byte-stream** channel to `Modules/Transport` that the session/channels/sync layers send and receive over, without knowing whether the bytes ride L2CAP CoC or GATT underneath. This is the seam every later issue plugs into.

## Scope

- Define `IDataChannel` (byte stream: send, send-capacity, state) and a delivery sink for inbound bytes + state changes.
- Byte-stream semantics only — **message boundaries are not this layer's job** (see the framing issue).
- Do **not** touch GAP advertising/scanning or the existing Pairing/Session GATT link.
- Callback (`IDataChannelSink`) vs poll (`Drain()`) delivery is an implementation choice; a push sink is sketched below, adapt to whatever the game loop prefers.

## Proposed interface

`Modules/Transport/Public/Lur/Transport/DataChannel.h`

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace Lur::Transport
{
    enum class EChannelState : uint8_t
    {
        Idle,      // Not yet opened.
        Opening,   // Handshake / CoC connect in progress.
        Open,      // Ready for Send / receive.
        Closing,
        Closed,    // Clean local or peer close.
        Failed,    // Link lost; caller should fall back to the GATT data path.
    };

    // Push-style delivery of inbound bytes + state transitions.
    class IDataChannelSink
    {
    public:
        virtual ~IDataChannelSink() = default;
        virtual void OnChannelState(EChannelState State) = 0;
        virtual void OnChannelReceive(const uint8_t* Data, size_t Size) = 0;
    };

    // A reliable, ordered byte stream between the two paired peers.
    // Backed by an L2CAP CoC socket when both ends support it, else the GATT data path.
    // NOTE: this is a *byte stream* — message boundaries are the caller's job (see FrameReader/FrameWriter).
    class IDataChannel
    {
    public:
        virtual ~IDataChannel() = default;

        virtual EChannelState State() const = 0;

        // Queue bytes for transmission. Returns bytes accepted right now; fewer than Size
        // means the send window (L2CAP credits + local buffer) is full — retry the rest later.
        virtual size_t Send(const uint8_t* Data, size_t Size) = 0;

        // Bytes acceptable by Send() without buffering pressure.
        virtual size_t SendCapacity() const = 0;

        virtual void SetSink(IDataChannelSink* Sink) = 0;
    };
}
```

## Acceptance criteria

- [ ] `IDataChannel` + sink compile in the pure-C++ side of `Modules/Transport` with no Bluetooth/platform deps.
- [ ] A trivial in-memory/loopback implementation exists for host-side unit tests (send bytes on one end, receive on the other).
- [ ] No change to GAP discovery or the existing pairing flow.


===== ISSUE =====
Title: Transport: GATT-bootstrap to L2CAP CoC handoff (IL2capUpgrade)

Part of #NN (epic). Depends on the `IDataChannel` issue.

## Summary

Bring up an L2CAP CoC socket by exchanging the dynamically-assigned **PSM** over one GATT characteristic, then expose the socket as an `IDataChannel`. Reuses the GATT link the Pairing/Session layer already established — GAP and pairing are untouched.

## Handoff sequence

1. **Listener** (role taken from the pairing handshake — whoever advertised) opens a CoC listener and obtains a PSM: `BluetoothServerSocket.getPsm()` on Android; the `publishL2CAPChannel` delegate callback on iOS.
2. Listener writes an `L2capHandshake` into the **PSM control characteristic**.
3. **Connector** reads the characteristic and opens the CoC socket to that PSM: `createL2capChannel` (Android) / `openL2CAPChannel` (iOS).
4. Both ends hold a socket (`BluetoothSocket` streams on Android; `NSInput/NSOutputStream` on iOS), wrapped as an `IDataChannel` in state `Open`.

## Notes / decisions

- **ProtocolVersion coupling:** refuse the upgrade if `L2capHandshake.ProtocolVersion != Lur::Net::ProtocolVersion`. The handshake byte layout is part of the wire protocol — bump `ProtocolVersion` if it changes.
- **Secure vs insecure:** `createL2capChannel` requires bonding/encryption; `createInsecureL2capChannel` (Android) / the insecure path does not. Pick based on whether the pairing flow bonds the devices; carry the choice in `Flags`.
- **Failure = fall back, not drop:** any failure (unsupported, timeout, socket error) lands in `Failed`; the caller keeps using the GATT data path so the session survives.
- Native implementation lives in the platform backends; the C++ interface is the seam.

## Proposed interface

`Modules/Transport/Public/Lur/Transport/L2capBootstrap.h`

```cpp
#pragma once
#include <cstdint>

#include "Lur/Transport/DataChannel.h"

namespace Lur::Transport
{
    // Exchanged over a single GATT characteristic to bootstrap the CoC data channel.
    // Fixed 4-byte little-endian layout. Keep layout + ProtocolVersion in lockstep with Lur::Net.
    struct L2capHandshake
    {
        uint16_t Psm;             // Dynamically-assigned PSM from the listening peer.
        uint8_t  ProtocolVersion; // Must equal Lur::Net::ProtocolVersion or the upgrade is refused.
        uint8_t  Flags;           // Reserved: secure/insecure, PHY hints, future capabilities.
    };

    enum class EUpgradeState : uint8_t
    {
        Idle,        // GATT link up; CoC not started.
        Publishing,  // Listener: opening the CoC listener, awaiting a PSM.
        Advertised,  // Listener: PSM written to the characteristic, awaiting connect.
        Connecting,  // Connector: PSM read, opening the CoC socket.
        Ready,       // Data channel Open — Channel() is valid.
        Failed,      // Upgrade failed; stay on the GATT notification data path.
    };

    // Drives the GATT-characteristic PSM exchange, then yields an open CoC data channel.
    //
    // Reuses the GATT link from the Pairing/Session layer. GAP advertising, scanning and pairing
    // are untouched, so the proximity-based "it just connects" flow is unchanged.
    //
    // Platform backends:
    //   iOS     -> CBL2CAPChannel (publishL2CAPChannel / openL2CAPChannel), NSInput/NSOutputStream
    //   Android -> BluetoothAdapter.listenUsingL2capChannel / BluetoothDevice.createL2capChannel, BluetoothSocket
    class IL2capUpgrade
    {
    public:
        virtual ~IL2capUpgrade() = default;

        // Kick off the handoff. AsListener publishes + advertises the PSM; otherwise reads it and connects.
        virtual void Begin(bool AsListener) = 0;

        // Poll each tick. Ends at Ready (Channel() valid) or Failed (fall back to GATT).
        virtual EUpgradeState State() const = 0;

        // Valid only when State() == Ready. Ownership stays with the upgrade object.
        virtual IDataChannel* Channel() = 0;
    };
}
```

## Acceptance criteria

- [ ] CoC socket opens on the real pair via the GATT PSM handoff, in both role directions.
- [ ] Mismatched `ProtocolVersion` refuses the upgrade cleanly.
- [ ] Any failure transitions to `Failed`; the session continues on GATT.
- [ ] The resulting `IDataChannel` round-trips bytes both ways.


===== ISSUE =====
Title: Transport: length-prefix framing over the CoC byte stream

Part of #NN (epic). Depends on the `IDataChannel` issue.

## Summary

CoC delivers a **byte stream**, not messages — a single read may contain part of a message or several. Add a small length-prefix framer so the channels/sync layers get whole messages back regardless of how the stream chunks them.

## Scope

- Wire format per message: `[uint32 LE length][payload]`.
- `FrameWriter::Frame` prepends the length for `IDataChannel::Send`.
- `FrameReader` buffers inbound bytes from `OnChannelReceive` and pops complete messages.
- Pure C++, host-testable; independent of Bluetooth.
- Guard against absurd lengths (cap max message size, drop/reset on overflow).

## Proposed interface

`Modules/Transport/Public/Lur/Transport/Framing.h`

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Lur::Transport
{
    // Length-prefixed framing over a byte-stream IDataChannel.
    // Per message on the wire: [uint32 LE length][payload].
    class FrameWriter
    {
    public:
        // Prepends the 4-byte length; returns framed bytes ready for IDataChannel::Send.
        static std::vector<uint8_t> Frame(const uint8_t* Payload, size_t Size);
    };

    class FrameReader
    {
    public:
        // Feed raw bytes as they arrive (from IDataChannelSink::OnChannelReceive).
        void Push(const uint8_t* Data, size_t Size);

        // Pop the next fully-arrived message; false if none is ready yet.
        bool Next(std::vector<uint8_t>& OutMessage);

    private:
        std::vector<uint8_t> Buffer;   // unconsumed stream bytes
        // (Track the expected payload length across Push calls.)
    };
}
```

## Acceptance criteria

- [ ] Unit tests: a message split across multiple `Push` calls reassembles correctly; multiple messages in one `Push` all pop; a zero-length payload is handled.
- [ ] Oversized declared length is rejected without allocating unbounded memory.
- [ ] Byte-identical round-trip through `FrameWriter` → stream → `FrameReader`.


===== ISSUE =====
Title: Transport: capability-routed data path (CoC vs GATT), chess stays on GATT

Part of #NN (epic). Depends on the `IL2capUpgrade` and framing issues.

## Summary

Choose the data path at connect time: use CoC when both ends support it, otherwise fall back to a GATT-notification `IDataChannel`. Selection sits behind the Transport interface so the sync layer never branches on it. Chess deliberately stays on GATT.

## Scope

- Feature-detect / negotiate CoC support during the handshake (attempt the upgrade; on `Failed`, use the GATT data channel).
- Provide a GATT-notification-backed `IDataChannel` (with the existing hand-fragmentation) as the fallback implementation of the same interface.
- Expose the negotiated link's rough capability (e.g. `bandwidth class`, max usable payload) so a sync model can pick strategy — lockstep-only on the low path vs richer streaming when CoC is up.
- **Chess routing:** keep chess on GATT — a move is ~1 byte, 20× under a single notification, so CoC adds only handshake overhead. Reserve CoC for games that actually stream.

## Acceptance criteria

- [ ] On the real pair, CoC is selected; if the upgrade is forced to fail, GATT is used with no session drop.
- [ ] A game reads a capability hint from the channel and can choose a sync model from it.
- [ ] Chess continues to run over GATT unchanged; no protocol bump for chess.


===== ISSUE =====
Title: FINAL — Stream stress-test harness on the real device pair

Part of #NN (epic). Depends on all prior issues. **This is the closing part of the epic.**

## Summary

Measure what the Bluetooth link between the **real test pair (iPhone 11 Pro + Galaxy A14 / SM-A145)** can actually do, over both CoC and GATT, so every future game picks a sync model against real numbers instead of guesses. Not a feature — a measurement + report.

## What to measure

- **Sustained throughput** (kbps/Mbps) over CoC vs GATT, for a range of message sizes (e.g. 20 B, 185 B, 512 B, ~4 KB, bulk).
- **Cadence:** max messages/sec actually achievable against the connection interval; confirm the ~15 ms iOS floor in practice and what Android negotiates.
- **Round-trip latency** (ping/echo) — median and tail — including the one-way floor.
- **Max usable payload per interval** and how throughput scales as payload grows (find the batch-vs-tiny-frequent crossover).
- **Burst vs sustained** behaviour, and effect of the send window / L2CAP credits filling.
- Note negotiated MTU and PHY (2M vs 1M) when observable — but don't assume 2M on iOS.

## How

- A dedicated diagnostic mode / "game" that opens the data channel and runs fixed send/echo patterns; both directions.
- Log results to CSV on-device (steer toward `Download/` so the PC can `adb pull` it over wireless debugging), plus a live on-screen readout.
- Run the same patterns over CoC and the GATT fallback for a direct comparison.
- Repeat at a couple of distances / with light interference to get a realistic range, not just a best case.

## Deliverables / acceptance criteria

- [ ] Reproducible harness runnable on the real pair, covering the payload sizes above, over both CoC and GATT.
- [ ] CSV/log export retrievable from the phone (Download/ + adb pull).
- [ ] A short written summary: observed sustained throughput, achievable Hz, latency median/tail and floor, and the payload-size crossover point.
- [ ] A recommendation table mapping the measured ceiling to what each sync model can afford (lockstep / rollback / snapshot), to guide future games.
