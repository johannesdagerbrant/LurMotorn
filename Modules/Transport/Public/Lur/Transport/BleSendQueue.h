#pragma once
#include <cstddef>
#include <cstdint>

namespace Lur::Transport {

// The DUMB driver seam: what a BLE radio can be asked to do, and nothing about when or why.
//
// This exists so the deciding half of the transport can be host-tested. The counting rule says
// interface-not-link-seam here because there are genuinely several implementations alive at
// once: one per platform, plus the fake the tests drive — the same argument that qualifies
// ITransport.
//
// A driver implementing this may contain API verbs and event forwarding. It must not contain
// decisions. If a line chooses (retry or not? which role? what order? how long to wait?) it
// belongs above this seam, in engine C++, where a test can reach it.
class IBleRadio {
public:
    virtual ~IBleRadio() = default;

    // Hand ONE datagram to the radio, right now.
    //
    // Returns true if the radio accepted it for transmission, in which case the driver MUST
    // later report exactly one OnSendComplete() — that is what releases the next datagram.
    // Returns false if the radio could not take it (momentarily busy, or no link); the caller
    // keeps the datagram and retries. Refusing is normal and must never be treated as an error.
    virtual bool Write(const uint8_t* Data, std::size_t Size) = 0;
};

// Send flow control for a BLE link.
//
// A BLE stack allows exactly ONE outstanding operation, and a second write issued before the
// first completes is SILENTLY dropped — no error, no callback, the datagram is simply gone.
// Under real load (a move per turn plus keepalives plus resync) that dropped nearly every
// datagram, and state only propagated via the much slower resync path (#72). So sends are
// serialized: enqueue, issue one, and issue the next only when the driver reports completion.
//
// This costs no network time. The writes are still WRITE_NO_RESPONSE; they are merely paced to
// the connection interval instead of overrunning it.
//
// Timing is denominated in nanoseconds fed to Tick(), not in platform timers, so the watchdog
// is exercisable on the host and behaves identically on every platform. The Kotlin version used
// a Handler with postDelayed plus a token to invalidate stale timers; a tick-driven deadline
// needs no timer and cannot leak a callback.
//
// It still needs to account for a LATE COMPLETION, and that is a bug the move surfaced. Once the
// watchdog has abandoned a datagram and issued the next, "something is outstanding" is true
// again — so the platform's unlabelled completion callback for the ABANDONED datagram is
// indistinguishable from the new one's, and acting on it releases a further datagram while the
// new one is still in flight: two outstanding operations, the exact state this class prevents.
// The Kotlin has the same hole (its token guards the timer, not the callback). We count the
// completions we are owed but no longer want, and swallow exactly that many.
//
// Not thread-safe: drive it from the thread that owns the transport.
class BleSendQueue {
public:
    // One datagram at an ATT MTU of 517 (what both backends negotiate): MTU - 3.
    static constexpr std::size_t MaxDatagram = 514;
    // Depth before Enqueue refuses. Fixed capacity, no allocation in the send path.
    static constexpr int MaxQueued = 32;
    // How long a datagram may stay outstanding before we assume its completion is never coming.
    // A link that dies between issue and ack would otherwise stall the queue forever, which on a
    // phone reads as the game freezing. Bounded, not eager: a healthy write completes in ~one
    // connection interval, far inside this.
    static constexpr uint64_t SendTimeoutNs = 300'000'000ull;   // 300 ms

    // The radio must outlive the queue. Null is legal and means "no link yet": datagrams are
    // held, never invented and never silently discarded.
    void SetRadio(IBleRadio* Radio) { Radio_ = Radio; }

    // Queue one datagram, and issue it immediately if nothing is outstanding.
    //
    // Returns false if it does not fit (> MaxDatagram) or the queue is full. It REFUSES rather
    // than dropping the oldest: this is an ordered stream, so evicting an earlier datagram would
    // silently break the ordering guarantee the receiver relies on, while refusing tells the
    // caller — the same choice Session::Send makes for an over-long payload.
    bool Enqueue(const uint8_t* Data, std::size_t Size);

    // The driver reports that a write finished. Callbacks are unlabelled, so this consumes the
    // oldest outstanding one: if the watchdog previously abandoned a datagram, the first
    // completion after that belongs to the abandoned datagram and is swallowed rather than
    // releasing the next. See the class comment — honouring it would put two datagrams in
    // flight, which is the state this class exists to prevent.
    void OnSendComplete();

    // Advance the watchdog and re-pump. Call once per transport tick with the nanoseconds
    // elapsed since the previous call.
    void Tick(uint64_t ElapsedNs);

    // The link is gone. The backlog is dropped, because a datagram stream is only meaningful to
    // a peer that received the earlier ones — delivering it on reconnect would hand the peer a
    // burst of stale state ahead of the resync that is supposed to reconcile them.
    void OnLinkLost();

    int  Queued() const { return Count_; }
    bool InFlight() const { return InFlight_; }
    // Datagrams refused for want of room, cumulative. A full queue means the radio has stopped
    // draining; the count is what makes that visible instead of merely felt.
    uint32_t Dropped() const { return Dropped_; }

private:
    void Pump();

    struct Slot {
        uint8_t     Bytes[MaxDatagram];
        std::size_t Size = 0;
    };

    IBleRadio* Radio_ = nullptr;
    Slot       Ring_[MaxQueued];
    int        Head_ = 0;      // next to send
    int        Count_ = 0;     // queued, excluding the one in flight
    bool       InFlight_ = false;
    uint64_t   InFlightNs_ = 0;
    // Completions we are still owed for datagrams the watchdog gave up on. Each one must be
    // swallowed, not acted on.
    int        Abandoned_ = 0;
    uint32_t   Dropped_ = 0;
};

}  // namespace Lur::Transport
