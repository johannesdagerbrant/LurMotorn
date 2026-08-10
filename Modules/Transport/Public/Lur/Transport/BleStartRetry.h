#pragma once
#include <cstdint>

namespace Lur::Transport {

// Advertise/scan start-failure policy: when a radio refuses to start, decide whether and when to
// try again.
//
// This was one game's fix (#194) and not the other's, which is the whole problem it represents. A
// failed start used to be PERMANENT — one error line and the process stayed invisible and deaf for
// the rest of its life, needing a human to toggle Bluetooth. Chess gained a capped-backoff retry;
// RPS did not, so **RPS silently never advertises if its first attempt fails**. Nothing looks
// wrong on that phone; it simply cannot be found. It could exist in one game and not the other
// because it lived in Kotlin, where no test could see it. It lives here now, once.
//
// The policy:
//   * A failed start is TRANSIENT until proven otherwise, so retry on a capped exponential
//     backoff — a blip heals in about a second instead of needing a human.
//   * Fast retries are BOUNDED. Past the cap the fast cadence stops and the caller hands back to
//     the slow discovery watchdog, which keeps trying for as long as we are unlinked. There is no
//     give-up state, only a slower one: HandedOff means "change cadence", never "stop".
//   * Retry only while it could still matter — Cancel() when the link comes up or we stop.
//     Retrying into a live link is pure churn, and churn is what degraded the radio in #163/#194.
//
// Timing is nanoseconds fed to Tick(), not a platform timer, so the whole thing is host-testable
// and identical on every platform. The Kotlin used Handler.postDelayed with a token; a tick-driven
// deadline cannot leak a callback.
//
// What this class does NOT do, deliberately: it never touches the radio. The caller owns the
// ALREADY_STARTED remedy — releasing its own registration before retrying — because that is a
// platform API verb, and it is genuinely required: without it the retry earns the same error
// forever, which was #194's actual bug.
class BleStartRetry {
public:
    // First delay, and the doubling base. ~0.4 s: long enough for a transient stack error to
    // clear, short enough that a human never notices the heal.
    static constexpr uint64_t BaseDelayNs = 400'000'000ull;
    // Ceiling on the doubling (BaseDelayNs << 3).
    static constexpr uint64_t MaxDelayNs = BaseDelayNs << 3;    // ~3.2 s
    // How many fast retries before handing back to the slow watchdog. Five covers a transient
    // failure with room to spare; past that the failure is not transient and a faster cadence
    // would only churn the radio.
    static constexpr int MaxFastRetries = 5;

    // A start attempt failed. Schedules a retry if any fast attempts remain.
    //
    // Returns whether this failure deserves a LOUD log line: true for the first of a run, false
    // for the repeats, so the retry loop cannot bury the message that matters.
    bool OnStartFailed();

    // A start attempt succeeded. Clears the backoff and the pending retry: a failure an hour from
    // now has nothing to do with this run and should start from the shortest delay again.
    void OnStarted();

    // Stop retrying — the link came up, or we shut down. Abandons any pending retry rather than
    // letting it fire into a state where it is only churn.
    void Cancel();

    // Advance the pending retry's deadline. Returns true EXACTLY ONCE per scheduled retry, when
    // the caller should attempt the start again. Time does not accumulate while nothing is
    // pending, so an idle stretch cannot bank time and make the next retry fire instantly.
    bool Tick(uint64_t ElapsedNs);

    // Is a fast retry scheduled?
    bool IsPending() const { return Pending_; }

    // Have the fast retries been exhausted, so the caller should fall back to its slow discovery
    // watchdog? NOT a give-up state — a cadence change.
    bool HandedOff() const { return HandedOff_; }

    // The delay currently in force (for the pending retry, or the next one to be scheduled).
    uint64_t DelayNs() const;

private:
    int      Attempt_ = 0;        // fast retries scheduled so far in this run of failures
    bool     Pending_ = false;
    bool     HandedOff_ = false;
    bool     Logged_ = false;     // has this run of failures had its loud line?
    uint64_t WaitedNs_ = 0;
};

}  // namespace Lur::Transport
