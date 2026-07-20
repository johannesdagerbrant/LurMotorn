# LurMotorn Networking & Engine Lexicon

A reference for the terminology and abbreviations used while designing the peer-to-peer transport layer. Scoped to this project's stack (BLE, local Wi-Fi, P2P game sync, and the engine/graphics bits that came up). Drop it in as `docs/GLOSSARY.md` if useful.

> Recency note: a few Wi-Fi Aware / iOS facts are from 2025 and moving fast — those entries carry a "verify" flag. Confirm against current Apple/Android docs before committing.

---

## 1. Bluetooth stack & layers

**Bluetooth Classic (BR/EDR)** — Basic Rate / Enhanced Data Rate. The older, higher-power Bluetooth used for audio etc. *Not* what you're using.

**BLE (Bluetooth Low Energy)** — The low-power Bluetooth mode LurMotorn runs on. Different protocol stack from Classic.

**GAP (Generic Access Profile)** — The discovery + connection layer: advertising, scanning, and connection roles. **This is the layer that makes LurMotorn feel seamless** ("walk up and it connects"), and it's untouched whether data rides GATT or L2CAP CoC.

**GATT (Generic Attribute Profile)** — The request/response data layer most BLE apps use: a hierarchy of services and characteristics. Convenient, but capped and chatty for bulk data.

**ATT (Attribute Protocol)** — The protocol underneath GATT. The "ATT MTU" limit comes from here.

**L2CAP (Logical Link Control and Adaptation Protocol)** — The multiplexer/segmenter beneath GATT and everything else. It routes multiple protocols over one link and splits big packets into radio-sized chunks (see SAR). GATT itself rides a fixed L2CAP channel.

**SAR (Segmentation and Reassembly)** — L2CAP chopping a large payload into radio packets on send and rebuilding it on receive. With CoC you get this for free; with GATT you hand-roll it.

**Central / Peripheral** — The two GAP connection roles. Central scans and initiates; peripheral advertises and accepts. A phone can be either.

**Multi-role** — A single device acting as central *and* peripheral at once (since Bluetooth 4.1). Enables star/relay topologies for >2 players.

---

## 2. BLE data path & performance

**Characteristic** — A single readable/writable value in GATT (like a variable with a handle). The PSM handshake uses one dedicated characteristic.

**Notification** — A server-pushed characteristic update with **no acknowledgement**. The usual way to stream over GATT. Capped near the MTU per push.

**Indication** — Like a notification but **acknowledged** by the receiver (slower, confirmed).

**Write with/without response** — GATT write types. "With response" costs a round-trip ACK per write; "without" doesn't. Part of why GATT has more overhead than a CoC stream.

**MTU (Maximum Transmission Unit)** — Max bytes per ATT operation. Default ATT MTU is 23 → **20 usable** (3-byte header). Negotiable up to ~185 (iOS) / ~512 (Android); the smaller end wins.

**PDU / SDU** — Protocol Data Unit (a packet at a given layer) / Service Data Unit (a whole logical message handed to L2CAP, which it segments). You write an SDU; L2CAP handles the PDUs.

**CID (Channel Identifier)** — L2CAP's "which channel" tag, like a port. GATT/ATT is the fixed CID `0x0004`.

**CoC (Connection-oriented Channel)** — An L2CAP channel you open dynamically that behaves like a **socket / byte stream**, with credit-based flow control. The high-throughput path that skips GATT's per-op overhead. **The core of the transport epic.**

**PSM (Protocol/Service Multiplexer)** — The "port number" a CoC listens on. Assigned dynamically; on iOS it can't be advertised, so it's exchanged over a GATT characteristic (the bootstrap).

**Credit-based flow control** — CoC's backpressure: the receiver grants "credits" so the sender can't overrun it. Maps to `SendCapacity()` in the sketched interface.

**DLE (Data Length Extension)** — BLE 4.2+ feature raising the link-layer packet from 27 to **251 bytes**, so a big MTU rides in one radio packet instead of being fragmented.

**PHY (Physical layer)** — The radio mode. **LE 1M** (baseline), **LE 2M** (double rate, BLE 5 — the fast path), **LE Coded** (long range, slow). Both your phones have 2M hardware, but **iOS negotiates the PHY for you — you can't force 2M.**

**Connection interval** — How often the two devices wake to exchange packets. Spec range **7.5 ms – 4 s**; multiple packets can go per interval.

**Connection event** — The actual scheduled slot (once per interval) where packets are exchanged. With multiple links, the controller has to fit each connection's events into non-overlapping slots.

**Bonding vs pairing** — Pairing establishes encryption for a session; bonding stores the keys for next time. Matters for CoC: Android's `createL2capChannel` needs a bonded/encrypted link; `createInsecureL2capChannel` doesn't.

---

## 3. Platform Bluetooth APIs

### iOS (Core Bluetooth)
**Core Bluetooth** — Apple's BLE framework.
**CBL2CAPChannel** — iOS's L2CAP CoC channel object (available **iOS 11+**).
**publishL2CAPChannel** — Peripheral side: opens a CoC listener; the assigned PSM arrives via a delegate callback.
**openL2CAPChannel** — Central side: connects to a peer's PSM.
**NSInputStream / NSOutputStream** — The read/write streams you get from an open `CBL2CAPChannel`.

### Android
**listenUsingL2capChannel / listenUsingInsecureL2capChannel** — Server side: opens a CoC listener (**API 29+ / Android 10**). Your Galaxy A14 (Android 13+) supports this.
**createL2capChannel / createInsecureL2capChannel** — Client side: connects to a peer's PSM (secure requires bonding; insecure doesn't).
**BluetoothServerSocket / BluetoothSocket** — The listener and the connected socket; `BluetoothSocket` gives normal input/output streams.
**getPsm()** — `BluetoothServerSocket.getPsm()` returns the dynamically-assigned PSM to share with the peer.

---

## 4. Alternative transports (Wi-Fi & friends)

**P2P (peer-to-peer)** — Devices talking directly, with no central server to arbitrate. LurMotorn's whole model.

**Wi-Fi Aware / NAN (Neighbor Awareness Networking)** — Standards-based, proximity-based Wi-Fi discovery + direct data path with **no SSID, no password, no join prompt** — i.e. the seamless feel at Wi-Fi bandwidth. *Verify:* third-party access on iOS arrived in **iOS 26 (2025)**; it's hardware-gated on Android and **not supported on the Galaxy A14**, so it's out for your current pair.

**Wi-Fi Direct** — Android's peer-to-peer Wi-Fi. **Android-only** (no third-party iOS access), so not viable cross-platform.

**MultipeerConnectivity** — Apple's slick P2P framework (Wi-Fi + AWDL + Bluetooth). **Apple devices only** — Android can't speak it.

**AWDL (Apple Wireless Direct Link)** — Apple's proprietary device-to-device Wi-Fi that MultipeerConnectivity/AirDrop use under the hood.

**Local-only hotspot** — Android's `WifiManager.startLocalOnlyHotspot()` (API 26+): spins up a temporary access point with a **randomized** SSID/passphrase, no internet. The workable-but-awkward Wi-Fi path (randomized SSID = a join prompt every session).

**NEHotspotConfiguration** — iOS API (iOS 11+) to join a specific Wi-Fi network programmatically; shows a one-time system "join?" prompt.

**Personal Hotspot** — iOS's user-toggled internet-sharing hotspot. No third-party API to start it — why the Android side hosts in the hotspot plan.

**Network.framework** — Apple's modern socket API for TCP/UDP over an established link.

**NsdManager** — Android Network Service Discovery (its zeroconf/mDNS API).

**Bonjour / mDNS (multicast DNS)** — Zero-config service discovery on a local network (find peers without a server). Bonjour is Apple's implementation.

**SSID / passphrase** — A Wi-Fi network's name / password.

**Link-local network** — A local network with no router/internet (e.g. over a hotspot); devices still get addresses and can open sockets.

**TCP / UDP** — Reliable-ordered vs fast-unordered transport protocols. Note: BLE doesn't give you a UDP-style "unreliable datagram" channel — design around that.

**Wi-Fi 5 (802.11ac) / Wi-Fi 6** — Wi-Fi generations. The A14 is Wi-Fi 5; the iPhone 11 Pro is Wi-Fi 6.

---

## 5. P2P game-networking models & concepts

**Lockstep / deterministic lockstep** — Both peers run the **identical** simulation and exchange only **inputs/commands**, each computing the same result. Tiny, army-size-independent bandwidth. What chess already does; the recommended default for P2P on BLE.

**Command sync / input sync** — Another name for the above: sync the orders, not the state.

**Rollback / rollback netcode** — A lockstep variant that **predicts** the peer's input, then rolls back and re-simulates when a real input contradicts the guess. Hides latency for fast/action games; needs fast state save/restore.

**GGPO ("Good Game Peace Out")** — The library/technique that popularized rollback netcode; often used as shorthand for the approach.

**Snapshot sync / state sync** — Each peer sends the **state** of its objects (positions, health…) rather than inputs. No determinism requirement, but bandwidth scales with object count. The escape hatch when determinism isn't practical.

**Authority / distributed authority / ownership** — Who is the source of truth for a given object. In P2P with no server you must define ownership and how cross-owner events (e.g. my unit hits yours) are arbitrated.

**Client-server** — The alternative to P2P: an authoritative server arbitrates. Contrast only — not your model.

**Determinism / deterministic simulation** — Same inputs always produce bit-for-bit the same result on every device. The precondition for lockstep and rollback.

**Fixed-point math** — Integer-based fractional math used **instead of floating-point** to guarantee determinism across iOS/Android (floats round differently per compiler/CPU and cause silent desyncs).

**Desync** — When two peers' simulations diverge. Detected by comparing periodic state checksums/hashes.

**State checksum / hash** — A fingerprint of the full game state each tick; mismatched hashes flag a desync.

**Tick / tick rate / step** — The fixed simulation heartbeat. The engine's game contract is essentially `Step(state, inputs) → state`, advanced one tick at a time.

**Input delay** — Deliberately delaying local input a few frames so the peer's input arrives in time — lockstep's simple latency-hiding lever (vs rollback's prediction).

**Dead reckoning** — Extrapolating an object's motion from its last known state + velocity/target between updates, so it moves smoothly without a packet every tick.

**Interpolation / extrapolation** — Smoothing received state by blending between known points (interp) or projecting past the latest one (extrap).

**Keyframe** — A periodic full/authoritative state sample sent to correct accumulated drift between sparse updates.

**Topology: mesh / star / host-relay** — How >2 peers connect. Full **mesh** (everyone-to-everyone) is expensive on BLE airtime; a **star** with one **host relaying** is the practical choice for more than two players.

**Serialization / deserialization** — Converting game state or messages to/from bytes for sending, saving, or hashing.

**Replay** — Re-running a stored input stream to reproduce a game. Falls out of lockstep for free — chess's move list *is* a replay.

**Snapshot** — A serialized full game state; used for save games and for resyncing a reconnecting peer.

---

## 6. Engine & graphics (LurMotorn)

**Vulkan** — The low-level cross-platform graphics API LurMotorn's single renderer targets.

**MoltenVK** — A translation layer that runs Vulkan on top of Apple's **Metal**, so one Vulkan renderer works on iOS.

**Metal** — Apple's native graphics API (what MoltenVK translates Vulkan into).

**Render backend** — The platform-specific implementation behind the engine's render interface (here, one Vulkan backend for both platforms).

**Sprite2D / QuadMesh** — The engine's 2D sprite path and the quad geometry pieces are drawn with (e.g. the captured-pieces rendering reuses these).

**Protocol version (`Lur::Net::ProtocolVersion`)** — The wire-format version gate; bump it when the on-wire format changes (e.g. the L2CAP handshake layout), leave it alone for purely presentational or already-synced-state features.

**Game contract** — The small interface every game implements so the engine can drive lockstep/rollback/replay/save: `Step(state, inputs) → state`, plus `Serialize`/`Deserialize` and `Hash`, plus input encode/decode.

---

## 7. Tooling & units

**adb (Android Debug Bridge)** — Command-line tool to talk to an Android device (e.g. `adb pull` to grab a file). Works over **wireless debugging**.

**Wireless debugging** — Android's adb-over-Wi-Fi mode, so the PC reaches the phone without a cable.

**gh (GitHub CLI)** — GitHub's command-line tool; `gh issue create` files issues from the terminal.

**API level (Android)** — Android's versioned SDK number. Relevant thresholds here: **26** (local-only hotspot), **29 / Android 10** (L2CAP CoC).

**RTT (round-trip time)** — Time for a message to go to the peer and back; the basis for latency measurement.

**Latency floor** — The lowest achievable one-way delay regardless of payload — ~**15–30 ms** over BLE here, bounded by the connection interval.

**kbps / Mbps** — Kilobits / megabits per second (throughput). **ms** — milliseconds. **Hz** — updates per second (set by the connection interval).

---

## Quick-reference: the numbers for your pair

| Thing | BLE reality (iPhone 11 Pro ↔ Galaxy A14) |
|---|---|
| Usable payload / packet | 20 B default; up to ~185 B (iOS) / ~512 B (Android) after MTU negotiation |
| Cadence | ~15 ms interval floor ≈ up to ~66 Hz |
| Throughput (GATT) | ~100–300 kbps, hand-rolled fragmentation |
| Throughput (L2CAP CoC) | up to ~1 Mbps best-effort on 2M PHY (can't force 2M on iOS) |
| Latency floor | ~15–30 ms one-way |
| Wi-Fi Aware | Not usable — unsupported on the A14 |
| High-throughput WiFi path | Android-hosted local-only hotspot (works, but awkward join prompt) |
