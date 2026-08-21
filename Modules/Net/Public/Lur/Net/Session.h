#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include "Lur/Transport/BleSendQueue.h"   // EBleSendPriority — which messages jump the queue
#include "Lur/Transport/Transport.h"

namespace Lur::Net {

// Top byte of every datagram: which kind of message follows. Keeping this a
// single byte (often foldable into spare bits later) preserves the slim-payload
// goal while letting one transport channel carry handshake, moves, and keepalive.
enum class EMsgType : uint8_t {
    Hello       = 0,  // version + nonce, exchanged on connect (see Session::Start)
    // 1..2 are RETIRED clock-sync slots. Nothing sends or handles them; they are kept
    // occupied because removing an enumerator RENUMBERS every later slot, which is a
    // silent wire break with no version guard. They are reclaimed in the phase that
    // resets ProtocolVersion, where the break is already paid for.
    Retired1    = 1,
    Retired2    = 2,
    // 3..5 are GENERIC game-defined framed-message slots — the engine names no game concept
    // (a move, a resign, a unit order) in its own enum; each game aliases these to its own
    // message kinds and documents the mapping on its side (issue #44).
    Game0       = 3,
    Game1       = 4,
    Game2       = 5,
    Keepalive   = 6,  // detect a silently dropped BLE link
    Sync        = 7,  // full game-state resync after a reconnect (game-defined payload)
    Game3       = 8,  // extra generic game slots. 8..10 are DEV-ONLY in practice: the RTS aliases
    Game4       = 9,  // them to MsgCvar / MsgCvarSync / MsgFingerprint (#112, gameplay-CVar sync
    Game5       = 10, // + the build-fingerprint gate), sent/accepted only under LUR_INTERNAL.
                      // Shipping never emits them and rejects them as unknown, so the
                      // shipping wire is untouched — ProtocolVersion stays 5.
    Game6       = 11, // SHIPPING slot: the RTS aliases it to MsgCamp, the pre-match opening-camp
                      // exchange (#160). Costs a wire id on purpose — see ProtocolVersion v7.
};

// Protocol version negotiated in Hello. Bump on any wire-format change so two
// app versions refuse to mis-decode each other rather than corrupt a game.
//
// NOTE — the one DELIBERATE, documented exception to "Modules/* must not name a game":
// the changelog below names which game's change earned each version. That is not an API
// shaped by a game, it is the reason a number exists, and stripping it would leave a
// version history nobody can audit. It exists because ONE version counter is shared by
// the engine and every game — 4 of the 10 entries are one game's gameplay-wire changes,
// which is itself the argument for splitting engine ProtocolVersion from a per-game
// GameProtocolVersion. When that split lands, these entries move to the game that owns
// them and this exception goes with them.
// v2: Hello gained a trailing "ready" byte for a loss-tolerant handshake.
// v3: Hello carries the persistent device GUID (was a random session nonce), so
//     each peer learns the other's stable identity for colour + the per-opponent
//     stats key (issue #18), independent of the BLE radio role.
// v4: live moves are a BARE 1-byte index (no type tag); every framed message is
//     padded to >=2 bytes, so datagram length disambiguates (issue #19/#15).
// v5: Keepalive carries an 8-byte game state hash so a mid-game desync (a lost move
//     while the link stays up) is detected and auto-healed by re-exchanging Sync,
//     instead of deadlocking with no recovery (issue #72).
// (2026-07-19: enum slots 3..5 renamed Move/Resign/DrawOffer -> generic Game0..2 for the
//  RTS; a pure source rename with no wire-format change, so ProtocolVersion stays 5.)
// v6: RPS buildings rework (#137) — MsgInput's per-tick payload changed from a 4-bit press
//     mask to a framed input-EVENT batch (place/queue), and the resync history now carries
//     event batches. A genuine game-wire-format change, so a mixed-build session must be
//     refused at the Hello handshake rather than desync mid-match.
// v7: RPS pre-match camp exchange moved to its own slot, Game6/MsgCamp (#160). It used to
//     travel as a MsgInput batch and be told apart from produced input by INSPECTING THE
//     PAYLOAD ("a one-event batch equal to the peer's opening camp"), which a real produced
//     tick can match exactly — a player re-placing a camp where theirs already stands. That
//     batch was dropped as a re-send, permanently skewing the peer's input stream by one tick.
//     A separate slot cannot be fooled by a payload coincidence; the ambiguity is gone rather
//     than narrowed. A v6 peer would read MsgCamp as an unknown type and never ready, so the
//     Hello handshake must refuse the pair.
// v8: RPS MsgInput frames carry a leading 1-byte sequence (the low byte of the exec tick the frame
//     is for) so the receiver can tell a lost frame from silence and name the tick it lost (#163).
//     A frame dropped inside a link that reports no error was invisible: frames were logged as
//     `recv msg type=N size=M`, nothing tied one to a tick, and finding a single missing input
//     needed two flight recordings and a diff.
// v9: RPS netcode replaced delay-based lockstep with ROLLBACK (Docs/Journal/2026-08-03). The wire
//     FRAME is byte-identical to v8 — same per-tick MsgInput batch + 1-byte sequence — but the
//     EXECUTION SEMANTICS changed: input now applies at the tick it was issued (no +InputDelayTicks)
//     and each peer speculates the other forward and rolls back on a misprediction. A v8 (lockstep)
//     peer schedules the same input three ticks later, so a v8<->v9 pair would apply identical input
//     on different ticks and desync with no wire error to point at. The bump makes the Hello handshake
//     refuse the mixed pair — belt-and-suspenders alongside the #166 build-fingerprint gate.
// v10: the "a bare 1-byte datagram is always a move" rule is GONE (#200). It was a chess
//     assumption sitting in the engine's dispatch — only chess has a 1-byte move — and it
//     made an empty-payload framed message unreachable for every game, because the length
//     check ran before the type byte was ever read. Chess moves now travel framed on its
//     own Game1 slot like every other message. A v9 peer would read a framed move as a
//     2-byte datagram of an unknown type and drop it, so it must not be allowed to link.
// v11: the RPS opening-camp message (MsgCamp) carries the sender's MATCH ORDINAL (#214). It had no
//     match identity at all, so a camp from the peer's PREVIOUS match read as readiness for the
//     receiver's NEXT one: the peer that restarted first accepted a stale camp, started a match the
//     other was still finishing, and came out of it one restart ahead. Both peers then played
//     bit-identical games under different labels and different seeds forever, which makes their
//     flight recordings unpairable and their score tallies disagree. A v10 peer sends no ordinal, so
//     a v10<->v11 pair would read the varint off the end of the payload; it must not link.
inline constexpr uint8_t ProtocolVersion = 11;

// Coarse link state for UI feedback (is a game live? did the link fail?).
enum class ELinkState : uint8_t {
    Searching,        // no peer connected yet (advertising + scanning)
    Handshaking,      // link up, exchanging Hello
    Linked,           // handshake done — the game is live
    Disconnected,     // the link came up once and then dropped
    VersionMismatch,  // peer speaks a different ProtocolVersion
};

// Short, stable label for a link state. Lives HERE, with the enum, because naming a
// value is the owner's job: the debug overlay used to carry this switch, which forced
// a presentation module to include the net session just to print a word. Callers pass
// the result straight to a text field, so the strings are lowercase and terse.
inline const char* LinkStateName(ELinkState S) {
    switch (S) {
        case ELinkState::Searching:       return "searching";
        case ELinkState::Handshaking:     return "handshaking";
        case ELinkState::Linked:          return "linked";
        case ELinkState::Disconnected:    return "disconnected";
        case ELinkState::VersionMismatch: return "ver-mismatch";
    }
    return "?";
}

// The symmetric peer-to-peer session that sits between ITransport (raw datagrams)
// and the game (typed messages). It owns two things:
//
//   1. The Hello HANDSHAKE. On connect each peer sends its ProtocolVersion + its
//      persistent device GUID, so each learns the other's stable identity. The game
//      derives colour + the per-opponent stats key from the two GUIDs (GUID order +
//      match parity) — independent of the BLE peripheral/central role (a radio
//      mechanic only), so there is no "host".
//
//   2. MESSAGE FRAMING. Send() prepends the EMsgType byte; inbound datagrams are
//      dispatched to a per-type handler. EVERY datagram is framed, with no exceptions
//      and no length-based special cases — the session moves opaque payload bytes and
//      knows nothing about what any game puts in them.
//
// Not thread-safe: drive it from one thread. Per the ITransport contract the
// receiver fires on the engine thread, which is also where Tick()/Send() are called.
class Session {
public:
    using Handler = std::function<void(const uint8_t* Payload, std::size_t Size)>;

    // Begin the session over Transport (which must outlive the session). LocalGuid is
    // this device's persistent id (Lur::Save::LoadOrCreateDeviceId); it is exchanged
    // in the Hello so each peer learns the other's stable identity. Installs the
    // transport receiver and sends the first Hello.
    void Start(Lur::Transport::ITransport* NewTransport, std::string_view LocalGuid);

    // Drive Hello retransmission + link liveness. Call once per frame with the wall-
    // clock nanoseconds elapsed since the previous call. Timing is denominated in real
    // time, NOT frame count, so keepalive/timeout behave identically at 60 Hz, 120 Hz,
    // or under a throttled loop. (A paused loop — e.g. a backgrounded iOS app whose
    // CADisplayLink stops — simply sends no keepalives; the peer's own timeout +
    // reconnect flow is the recovery path, by design.) A no-op for handshaking once
    // ready except for keepalive/timeout.
    void Tick(uint64_t ElapsedNs);

    // Deliver any datagrams the radio thread has queued, and NOTHING else — no keepalive,
    // no timeout, no clock (issue #189). Tick() already begins with this; the point of
    // exposing it separately is to run it a SECOND time, immediately before presenting.
    //
    // Why that matters: the loop applies inbound moves at the top and presents at the
    // bottom, so a datagram landing between those two points waits a whole iteration — and
    // the loop is vsync-bound, so that is a display refresh (~16 ms, ~25 ms on the iPhone's
    // 40 Hz beat) of pure sitting-still. Draining again just before the present costs one
    // pass over an empty queue in the common case and removes half a frame of latency on
    // average from every move the peer makes.
    //
    // Safe to call as often as you like: it carries no elapsed time, so it cannot advance
    // or double-count the liveness clock. Receiving IS still observed (an inbound datagram
    // resets the peer-silence timer), which is correct whenever it happens.
    void PumpInbox();

    bool IsReady() const { return Ready; }

    // True from the moment the link is (re)established until the peer's link-time Sync
    // has been received (or a short fallback timeout elapses). While this holds, the
    // game must NOT issue or apply live input: input that is only meaningful against a
    // state both peers agree on cannot be interpreted before the reconciling Sync lands.
    // Applying it early decodes it against an unreconciled state -> permanent divergence
    // -> deadlock (issue #71). The game gates on this both when producing local input
    // and when consuming the peer's.
    bool IsAwaitingResync() const { return AwaitingResync; }

    // The peer's persistent device id once ready, else empty. The game pairs it with
    // our own id to derive colour + the per-opponent stats key (independent of the
    // BLE radio role).
    const std::string& GetPeerGuid() const { return PeerGuid; }

    // Coarse link state for a UI indicator. Cheap; call each frame.
    ELinkState GetLinkState() const {
        if (VersionMismatchSeen) return ELinkState::VersionMismatch;
        if (Transport != nullptr && Transport->IsConnected())
            return Ready ? ELinkState::Linked : ELinkState::Handshaking;
        return EverConnected ? ELinkState::Disconnected : ELinkState::Searching;
    }

    // Lightweight counters + liveness, for the debug overlay (issue #54). Cheap reads.
    uint32_t GetDatagramsSent() const { return DatagramsSent; }
    uint32_t GetDatagramsReceived() const { return DatagramsReceived; }
    uint64_t GetNsSinceRecv() const { return SinceRecvNs; }

    // #163: a HALF-OPEN link — we are connected and our writes leave, but the peer's notify path is
    // wedged so NO inbound ever arrives. Distinct from a transient blip (which brings traffic and
    // resets the count) and from a clean drop (IsConnected goes false). True once
    // HalfOpenResetThreshold consecutive peer-silent resets have fired with no inbound between them;
    // cleared the instant real traffic resumes. Exposed so the app can TELL the player the actual
    // fix — a soft reset cannot clear a wedged BLE stack; the silent peer must toggle Bluetooth or
    // reboot (proven on hardware 2026-08-02) — instead of the silent reconnect-cycle that reads as a
    // freeze, which is the whole complaint in #163.
    bool IsLinkHalfOpen() const { return LinkHalfOpen_; }
    int  ConsecutiveSilentResets() const { return SilentResets_; }

    // #182: how many HARD per-OS radio restarts (ITransport::RestartRadio) we have fired during the
    // current half-open episode — the escalation past a soft ResetLink that provably can't clear a
    // wedged BLE stack. Capped at MaxRadioRestarts so the recovery can't itself become churn; reset
    // to 0 the instant inbound traffic proves the link recovered. Exposed for the on-device diag line
    // (`restarts=`) so a real-radio verification can confirm the escalation actually fired — this half
    // is not host-testable end to end, so seeing the count climb on the phone is the proof.
    int  RadioRestartsAttempted() const { return RadioRestarts_; }

    // Register the handler for one application message type. Every datagram is framed,
    // so a message with an empty payload is legal and arrives here with Size == 0.
    void SetHandler(EMsgType Type, Handler H);

    // Fired once, when the handshake completes and the seat is known.
    void SetReadyHandler(std::function<void()> H) { ReadyHandler = std::move(H); }

    // Fired when the link is re-established after a drop (post-handshake). This is
    // the generic reconnect-flow shell: the engine detects the reconnect and pokes
    // the game, which resynchronises its own state (e.g. exchange move history via
    // an EMsgType::Sync message) so both peers converge again. Also fired when a
    // mid-game desync is detected (see RequestResync / the keepalive state hash).
    void SetResyncHandler(std::function<void()> H) { ResyncHandler = std::move(H); }

    // Optional hook returning a hash of the game's authoritative state. If set, it
    // rides every Keepalive; the peer compares it to its
    // own and, on a mismatch, triggers a resync — so a mid-game divergence (a live
    // move that was lost while the link stayed up) self-heals instead of deadlocking
    // (issue #72). Must be identical on both peers when they agree (game-defined).
    void SetStateHashFn(std::function<uint64_t()> F) { StateHashFn = std::move(F); }

    // Force a resync now: hold moves (IsAwaitingResync) and fire ResyncHandler so the
    // game re-sends its state. Call when the game detects a desync (e.g. an inbound
    // move that won't decode against the local board). Also invoked internally on a
    // keepalive state-hash mismatch. Both peers re-exchanging Sync reconciles them.
    void RequestResync();

    // Optional debug sink for handshake tracing. The app supplies a platform logger
    // (logcat / os_log); the session stays platform-free. No-op if unset.
    using LogFn = std::function<void(const char* Line)>;
    void SetLogger(LogFn L) { Log = std::move(L); }

    // Frame [Type][Payload] and send it to the peer. Returns false (and logs) if the
    // payload exceeds MaxFramedPayload, so an over-budget message fails LOUDLY instead
    // of being silently dropped — most payloads are tiny (a move is ~1 byte), but a
    // reconnect Sync grows with the game (issue: the old 64-byte cap silently killed
    // resync past ply ~61). Never truncates the wire.
    //
    // Expedited marks this message as latency-critical, so the transport sends it ahead of
    // anything already queued (#190) — for the datagram a player is actively waiting on, e.g. a
    // move, as opposed to a keepalive or a bulk resync. The GAME decides: only it knows which of
    // its messages a human is waiting for. Default Normal, so nothing is urgent by accident (if
    // everything is expedited, nothing is).
    bool Send(EMsgType Type, const uint8_t* Payload, std::size_t Size,
              Lur::Transport::EBleSendPriority Priority = Lur::Transport::EBleSendPriority::Normal);

private:
    void SendHello();
    void SendKeepalive();
    void OnDatagram(const uint8_t* Data, std::size_t Size);
    void OnHello(const uint8_t* Payload, std::size_t Size);

    // Arm the #71 resync gate — UNLESS the peer's Sync has already landed on this link, in which
    // case the reconciliation the gate waits for has happened and holding moves only stalls the
    // game (#205). One function because both arming sites — going Ready, and the reconnect edge —
    // are reachable with a Sync already in hand, and the two must not drift apart. `Why` names the
    // occasion for the log.
    void ArmResyncGate(const char* Why);
    void Logf(const char* Fmt, ...);

    // One past the HIGHEST enumerator. This must cover every EMsgType: the value indexes the
    // handler table in BOTH SetHandler and the dispatch, and an out-of-range slot is silently
    // dropped at both ends — no error, no log. It sat at 8 ("covers Sync = 7") while Game3..Game5
    // were later added at 8..10, so the RTS's gameplay-CVar sync and build-fingerprint gate were
    // registered into nothing and every arriving one was discarded. That is why two phones never
    // converged their cvars (#147) and why a mismatched build was never actually refused (#112):
    // the receiver logged the datagram, then threw it away one line later. DERIVED from the enum
    // so adding a slot can't reintroduce it.
    static constexpr int         MaxMsgTypes     = static_cast<int>(EMsgType::Game6) + 1;
    static constexpr std::size_t GuidLen          = 32;  // 128-bit id as hex (Lur::Save::DeviceIdHexLen)

    // Real-time link timing (nanoseconds). Frame-rate-independent: derived from the
    // ElapsedNs fed to Tick(), not a tick count, so these mean the same on a 60 Hz or
    // 120 Hz display and don't drift under a throttled loop.
    static constexpr uint64_t HelloResendNs = 500'000'000ull;   // resend Hello every ~0.5s
    static constexpr uint64_t KeepaliveNs   = 1'000'000'000ull; // keepalive every ~1s
    static constexpr uint64_t LinkTimeoutNs = 5'000'000'000ull; // ~5s of silence -> dead
    // #163: past this many CONSECUTIVE peer-silent resets (no inbound between them) the link is not
    // blipping, it is HALF-OPEN — a wedged notify path. ~3 * LinkTimeoutNs (~15s connected-but-
    // silent) before we say so, long enough that a genuine reconnect blip — which brings traffic and
    // zeroes the count — never trips it.
    static constexpr int      HalfOpenResetThreshold = 3;
    // Once half-open, STOP churning the radio every LinkTimeoutNs. A soft ResetLink cannot clear a
    // stuck BLE stack, and the churn is what degrades it further — on hardware 2026-08-02 the central
    // did 80 soft resets in a row and cleared nothing; only a reboot did. Back off to this slower
    // cadence and let the surfaced state drive the real (human) fix.
    static constexpr uint64_t HalfOpenResetNs = 20'000'000'000ull; // ~20s between resets once wedged
    // #182: once half-open, escalate each backed-off cycle from a soft ResetLink to the HARDER
    // ITransport::RestartRadio — but only this many times. A soft reset can't clear a wedged stack and
    // a hard one may only help a subset of wedges (the hardware case needed the SILENT peer to reboot),
    // so past the cap we stop touching the radio entirely and let the "LINK STALLED" banner drive the
    // guaranteed human fix. Bounded on purpose: the #163 lesson is that churn degrades the radio, so the
    // recovery must not become the churn. 3 * ~20s ≈ 1 min of hard retries before we defer to the human.
    static constexpr int      MaxRadioRestarts = 3;
    // If the peer's link-time Sync never arrives (e.g. it adopted a different game),
    // stop blocking moves after this so a missing Sync can't wedge the game forever.
    static constexpr uint64_t ResyncTimeoutNs = 3'000'000'000ull; // ~3s fallback

    // Max payload for a FRAMED ([type][payload]) message. Both backends negotiate an
    // ATT MTU of 517 (seen on every link), so one datagram carries MTU-3 = 514 bytes;
    // minus the 1-byte type tag that leaves 513 for the payload. This was 254 — which
    // still silently dropped a full-history Sync once the accumulated move record passed
    // ~254 bytes (issue #41 / #72: a mid-game resync then never delivered and the game
    // wedged). Sized to the real MTU now. A game whose record exceeds even this can't
    // fit one datagram; a compact position-snapshot resync (or fragmentation) is the
    // follow-up. Send() refuses+logs past this bound rather than truncating the wire.
    static constexpr std::size_t MaxFramedPayload = 512;

    // Link liveness: once ready we send a Keepalive every KeepaliveNs; if NO datagram
    // arrives for LinkTimeoutNs we declare the link dead and ask the transport to
    // reset. This is what lets an iOS peripheral notice an abruptly-killed central (its
    // CBPeripheralManager gets no disconnect callback); every other role also detects a
    // real drop via the backend, so this is a belt-and-suspenders safety net there.

    Lur::Transport::ITransport* Transport = nullptr;
    std::string LocalGuid;
    std::string PeerGuid;
    bool     Ready         = false;
    bool     HelloEverSent = false;  // send the first Hello immediately, then resend on interval
    uint64_t HelloResendAccumNs = 0; // ns since our last Hello (handshake)
    uint64_t KeepaliveAccumNs   = 0; // ns since our last keepalive send
    uint64_t SinceRecvNs        = 0; // ns since ANY datagram arrived (liveness)
    int      SilentResets_      = 0;  // #163: consecutive peer-silent resets, no inbound between them
    bool     LinkHalfOpen_      = false; // #163: notify path wedged -> report + back off, don't churn
    int      RadioRestarts_     = 0;  // #182: hard RestartRadio()s fired this half-open episode (capped)
    uint32_t DatagramsSent      = 0; // total datagrams sent (overlay/debug)
    uint32_t DatagramsReceived  = 0; // total datagrams received (overlay/debug)
    bool     EverConnected      = false;  // for Disconnected vs never-connected
    bool     VersionMismatchSeen = false;
    bool     PrevConnected      = false;  // edge-detect reconnects for the resync hook
    bool     AwaitingResync     = false;  // hold moves until the link-time Sync lands (#71)
    uint64_t ResyncWaitNs       = 0;      // ns spent awaiting the peer's Sync (fallback timeout)
    // #205: has the peer's Sync already landed on THIS link? The gate is armed when we go Ready,
    // but the peer sends on ITS adopt and our Ready waits for its next ~500ms Hello — so the Sync
    // routinely arrives FIRST, and arming then waits for something that already happened. Cleared
    // whenever the transport is seen disconnected, so a genuine reconnect still gates.
    bool     SyncSinceLink      = false;
    uint64_t LastPeerHash       = 0;      // peer's previous keepalive hash (#72 desync detect)
    bool     HavePeerHash       = false;  // have we seen a peer keepalive hash yet?

    Handler               Handlers[MaxMsgTypes];
    std::function<void()>   ReadyHandler;
    std::function<void()>   ResyncHandler;
    std::function<uint64_t()> StateHashFn;      // rides Keepalive for desync detection (#72)
    LogFn                   Log;
};

} // namespace Lur::Net
