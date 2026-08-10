#pragma once
#include <cstdint>

namespace Lur::Transport {

// The three deadlines that govern an UNLINKED radio, in one place.
//
// Unlike the rest of the BLE policy these had not drifted: both games carry the same 8 s / 6 s /
// 1.5 s, byte for byte. That makes them pure duplication rather than a reconciliation problem —
// and the clearest possible case for one tested copy, because the next edit to any of these
// numbers is exactly where the drift would have begun.
//
// Timing is nanoseconds fed to Tick(), not platform timers, so every deadline is host-testable and
// behaves identically on Android and iOS. The Kotlin used three Handlers with postDelayed and
// hand-rolled cancellation; a tick-driven deadline cannot leak a callback and cannot fire into a
// state that has since changed.
//
// This class decides WHEN. It never touches a radio — starting, stopping and tearing down are
// platform API verbs and stay with the driver.
class BleDiscoveryTimers {
public:
    // #79: cached-role gating keys off a peer identity we have NOT verified this session. If the
    // peer re-rolled its GUID (reset/reinstall), advertise-only leaves BOTH phones deaf forever.
    // Any unlinked stretch past this drops the gates and resumes the symmetric advertise+scan
    // dance; the fresh in-band tie-break then re-caches the true role.
    static constexpr uint64_t DiscoveryWatchdogNs = 8'000'000'000ull;   // 8 s
    // An outgoing central attempt that neither links nor resolves within this is torn down and
    // retried, rather than hanging on a connection the stack will never complete. Deliberately
    // SHORTER than the discovery watchdog, so a stalled attempt is cleaned up before the symmetric
    // reset lands on top of it (asserted in the tests, so tuning cannot silently invert them).
    static constexpr uint64_t ConnectWatchdogNs = 6'000'000'000ull;     // 6 s
    // #17: a collided or failed connect must NOT be retried immediately. The peer — doing its own
    // exploratory connect — needs a moment to settle into peripheral-only, and the shared LE link
    // needs to finish tearing down, or the retry collides again or hangs.
    static constexpr uint64_t RescanDelayNs = 1'500'000'000ull;         // 1.5 s

    // What the caller should do this tick. Several can be true at once: a long stall (an app
    // suspended and resumed) can leave every deadline overdue, and the caller needs to hear about
    // each rather than only the first.
    struct Actions {
        bool GoSymmetric = false;    // discovery watchdog: drop cached-role gates, advertise+scan
        bool AbortConnect = false;   // connect watchdog: tear the attempt down
        bool Rescan = false;         // rescan delay elapsed: start scanning again
    };

    // The link is up. Stops every deadline: firing into a live link would drop the gates that link
    // depends on, or scan on top of it — churn, and churn is what degraded the radio in #163.
    void OnLinked();

    // We are not linked (startup, or the link dropped). Arms the discovery watchdog from zero, so
    // time spent linked is never banked against the next unlinked stretch.
    void OnUnlinked();

    // An outgoing central attempt has begun. Arms the connect watchdog.
    void OnConnectStarted();

    // The attempt resolved — linked, refused, or self-corrected to peripheral. Cancels the connect
    // watchdog, so a link that just formed is not torn down moments later.
    void OnConnectResolved();

    // Ask to resume scanning after RescanDelayNs. Re-requesting while one is pending supersedes it
    // rather than queueing a second, so a burst of failures cannot produce a burst of scan starts
    // (the ALREADY_STARTED shape from #194).
    void ScheduleRescan();

    // Advance all armed deadlines. Call once per transport tick with the nanoseconds elapsed since
    // the previous call.
    Actions Tick(uint64_t ElapsedNs);

private:
    bool     Linked_ = false;
    // Discovery watchdog: armed whenever unlinked, and PERIODIC — an unlinked phone must keep
    // trying, since there is no state in which it has given up.
    bool     WatchdogArmed_ = false;
    uint64_t WatchdogNs_ = 0;
    // Connect watchdog: one-shot per attempt.
    bool     ConnectArmed_ = false;
    uint64_t ConnectNs_ = 0;
    // Rescan: one-shot per request.
    bool     RescanArmed_ = false;
    uint64_t RescanNs_ = 0;
};

}  // namespace Lur::Transport
