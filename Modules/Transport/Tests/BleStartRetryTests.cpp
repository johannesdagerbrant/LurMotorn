// Host tests for Lur::Transport::BleStartRetry — the advertise/scan start-failure policy.
//
// Why this exists as engine C++ rather than platform code: it is the one-sided fix from #194 that
// chess has and RPS does not, so **RPS silently never advertises if its first attempt fails** and
// stays invisible for the life of the process. One Log.e, no retry, no give-up state — a phone
// that looks fine and cannot be found. Being in Kotlin is why it could exist in one game and not
// the other; being here, with these tests, is why it can only exist once.
//
// The policy, restated from the chess Kotlin it generalizes:
//   * A failed start is TRANSIENT until proven otherwise — retry on a capped exponential backoff.
//   * Fast retries are BOUNDED. Past the cap we stop the fast cadence and hand back to the slow
//     discovery watchdog, which keeps trying for as long as we are unlinked. So there is no
//     give-up state, just a slower one.
//   * Retry only while it could still matter: not once linked, not once stopped.
//   * ALREADY_STARTED means something holds the registration — possibly ours, possibly a previous
//     process's the system has not reaped. The caller must release its own side first or the
//     retry earns the same error forever (#194's actual bug).
#include <cstdint>
#include <cstdio>

#include "Lur/Transport/BleStartRetry.h"

using namespace Lur::Transport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// The failure that used to be permanent: one attempt, one log, invisible forever. A retry must be
// scheduled, and it must actually fire.
static void TestFailedStartRetries() {
    BleStartRetry R;
    R.OnStartFailed();
    CHECK(R.IsPending());
    CHECK(!R.Tick(R.DelayNs() - 1));        // not before its delay
    CHECK(R.Tick(1));                        // ...and then yes
    CHECK(!R.IsPending());                   // consumed: one retry per fire, never a storm
}

// Exponential, and CAPPED. Unbounded doubling would push the cadence to minutes and read as a
// hang; the cap is what keeps the slow path honest.
static void TestBackoffGrowsThenCaps() {
    BleStartRetry R;
    uint64_t Prev = 0;
    for (int i = 0; i < BleStartRetry::MaxFastRetries; ++i) {
        R.OnStartFailed();
        const uint64_t D = R.DelayNs();
        CHECK(D >= Prev);                    // never shrinks
        if (i > 0 && i <= 3) CHECK(D == Prev * 2);   // doubling while under the shift cap
        Prev = D;
        CHECK(R.Tick(D));
    }
    CHECK(Prev == BleStartRetry::MaxDelayNs);
}

// Past the fast cap we stop scheduling, and say so — the caller hands back to the slow discovery
// watchdog. Crucially this is NOT a give-up: HandedOff is a cadence change, not a terminal state.
static void TestFastRetriesAreBoundedThenHandOff() {
    BleStartRetry R;
    for (int i = 0; i < BleStartRetry::MaxFastRetries; ++i) {
        R.OnStartFailed();
        CHECK(R.IsPending());
        CHECK(R.Tick(R.DelayNs()));
    }
    R.OnStartFailed();
    CHECK(!R.IsPending());                   // no more fast retries scheduled
    CHECK(R.HandedOff());                    // ...and the reason is legible to the caller
}

// A success clears everything: the next failure starts from the shortest delay again, because a
// transient blip an hour later has nothing to do with this one.
static void TestSuccessResetsTheBackoff() {
    BleStartRetry R;
    R.OnStartFailed();
    R.OnStartFailed();
    const uint64_t Grown = R.DelayNs();
    CHECK(Grown > BleStartRetry::BaseDelayNs);

    R.OnStarted();
    CHECK(!R.IsPending());
    CHECK(!R.HandedOff());

    R.OnStartFailed();
    CHECK(R.DelayNs() == BleStartRetry::BaseDelayNs);
}

// Retrying after the link is up, or after we stopped, is pure churn — and churn is what degraded
// the radio in #163/#194. A pending retry is abandoned, not merely ignored on arrival.
static void TestCancelDropsAPendingRetry() {
    BleStartRetry R;
    R.OnStartFailed();
    CHECK(R.IsPending());

    R.Cancel();
    CHECK(!R.IsPending());
    CHECK(!R.Tick(R.DelayNs() * 10));        // and it never fires later
}

// Only the FIRST failure in a run should be logged loudly; the retries must not bury the log. The
// caller needs to know which failure is worth a line — the chess Kotlin carried an
// `advFailLogged` flag for exactly this.
static void TestOnlyTheFirstFailureWantsALoudLog() {
    BleStartRetry R;
    CHECK(R.OnStartFailed());                // first: say it
    CHECK(!R.OnStartFailed());               // repeats: quiet
    CHECK(!R.OnStartFailed());

    R.OnStarted();
    CHECK(R.OnStartFailed());                // a NEW run of failures is loud again
}

// Time only accumulates against a pending retry. A long idle stretch with nothing scheduled must
// not "bank" time that makes the next retry fire instantly.
static void TestIdleTimeIsNotBanked() {
    BleStartRetry R;
    CHECK(!R.Tick(BleStartRetry::MaxDelayNs * 4));   // nothing pending: no-op

    R.OnStartFailed();
    CHECK(!R.Tick(R.DelayNs() - 1));                 // full delay still required
    CHECK(R.Tick(1));
}

int main() {
    TestFailedStartRetries();
    TestBackoffGrowsThenCaps();
    TestFastRetriesAreBoundedThenHandOff();
    TestSuccessResetsTheBackoff();
    TestCancelDropsAPendingRetry();
    TestOnlyTheFirstFailureWantsALoudLog();
    TestIdleTimeIsNotBanked();

    if (GFailures == 0) {
        std::printf("ble_start_retry_tests: all checks passed\n");
        return 0;
    }
    std::printf("ble_start_retry_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
