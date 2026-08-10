// Host tests for Lur::Transport::BleDiscoveryTimers — the three deadlines that govern an UNLINKED
// radio: the discovery watchdog (#79), the outgoing-connect watchdog, and the rescan delay (#17).
//
// These three were the only part of the BLE policy that had NOT drifted: both games carry the same
// 8 s / 6 s / 1.5 s, byte for byte. That makes them pure duplication rather than a reconciliation
// problem — and it is the clearest possible case for one tested copy, because the next edit to
// either number is where the drift would have started.
//
// What each one is for, and why it is a DECISION rather than ceremony:
//   * Discovery watchdog (#79) — cached-role gating keys off a peer identity we have not verified
//     this session. If the peer re-rolled its GUID (reset/reinstall), advertise-only leaves BOTH
//     phones deaf forever. Any unlinked stretch past the deadline drops the gates and resumes the
//     symmetric dance. PERIODIC: it must keep firing for as long as we are unlinked, because
//     there is no state in which an unlinked phone has stopped trying.
//   * Connect watchdog — an outgoing central attempt that neither links nor resolves is torn down
//     and retried, rather than hanging on a connection the stack will never complete.
//   * Rescan delay (#17) — a collided or failed connect must NOT be retried immediately: the peer
//     (doing its own exploratory connect) needs a moment to settle into peripheral-only, and the
//     shared LE link needs to finish tearing down, or the retry collides again or hangs.
#include <cstdint>
#include <cstdio>

#include "Lur/Transport/BleDiscoveryTimers.h"

using namespace Lur::Transport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// Unlinked long enough -> go symmetric. This is #79: without it, two phones whose cached roles
// disagree with reality never find each other again.
static void TestDiscoveryWatchdogFiresWhileUnlinked() {
    BleDiscoveryTimers T;
    T.OnUnlinked();

    CHECK(!T.Tick(BleDiscoveryTimers::DiscoveryWatchdogNs - 1).GoSymmetric);
    CHECK(T.Tick(1).GoSymmetric);
}

// PERIODIC, not one-shot. An unlinked phone must keep dropping its gates and re-trying for as long
// as it is unlinked — a single fire would leave a phone that missed its one chance deaf forever,
// which is the bug #79 exists to prevent.
static void TestDiscoveryWatchdogIsPeriodic() {
    BleDiscoveryTimers T;
    T.OnUnlinked();

    for (int i = 0; i < 4; ++i)
        CHECK(T.Tick(BleDiscoveryTimers::DiscoveryWatchdogNs).GoSymmetric);
}

// Once linked it must stop. Firing into a live link would drop the very gates that link depends
// on — churn, and churn is what degraded the radio in #163.
static void TestDiscoveryWatchdogStopsWhenLinked() {
    BleDiscoveryTimers T;
    T.OnUnlinked();
    T.OnLinked();

    CHECK(!T.Tick(BleDiscoveryTimers::DiscoveryWatchdogNs * 4).GoSymmetric);

    // ...and re-arms on the next drop, from zero rather than from banked time.
    T.OnUnlinked();
    CHECK(!T.Tick(BleDiscoveryTimers::DiscoveryWatchdogNs - 1).GoSymmetric);
    CHECK(T.Tick(1).GoSymmetric);
}

// An outgoing connect that neither links nor resolves is abandoned, instead of hanging on a
// connection the stack will never complete.
static void TestConnectWatchdogAbortsAStalledAttempt() {
    BleDiscoveryTimers T;
    T.OnUnlinked();
    T.OnConnectStarted();

    CHECK(!T.Tick(BleDiscoveryTimers::ConnectWatchdogNs - 1).AbortConnect);
    CHECK(T.Tick(1).AbortConnect);
    // One-shot: the caller tears down and decides what to do next; a second abort would fight it.
    CHECK(!T.Tick(BleDiscoveryTimers::ConnectWatchdogNs * 2).AbortConnect);
}

// A connect that resolves — linked, or refused, or self-corrected to peripheral — cancels the
// watchdog. Otherwise a successful link would be torn down moments after forming.
static void TestResolvedConnectCancelsItsWatchdog() {
    BleDiscoveryTimers T;
    T.OnUnlinked();
    T.OnConnectStarted();
    T.OnConnectResolved();

    CHECK(!T.Tick(BleDiscoveryTimers::ConnectWatchdogNs * 3).AbortConnect);
}

// The connect watchdog fires sooner than the discovery watchdog, so a stalled attempt is torn down
// before the symmetric reset lands on top of it. Asserted rather than assumed: if someone later
// tunes these numbers past each other, the reset would race the teardown.
static void TestConnectWatchdogFiresBeforeDiscoveryWatchdog() {
    CHECK(BleDiscoveryTimers::ConnectWatchdogNs < BleDiscoveryTimers::DiscoveryWatchdogNs);
}

// A failed or collided connect waits before scanning again (#17): the peer needs a moment to
// settle into peripheral-only and the shared LE link needs to finish tearing down.
static void TestRescanIsDelayedNotImmediate() {
    BleDiscoveryTimers T;
    T.OnUnlinked();
    T.ScheduleRescan();

    CHECK(!T.Tick(BleDiscoveryTimers::RescanDelayNs - 1).Rescan);
    CHECK(T.Tick(1).Rescan);
    CHECK(!T.Tick(BleDiscoveryTimers::RescanDelayNs * 2).Rescan);   // one-shot
}

// Scheduling a rescan twice must not queue two of them — the second request supersedes the first
// rather than producing a double scan start (the ALREADY_STARTED shape from #194).
static void TestRescanDoesNotStack() {
    BleDiscoveryTimers T;
    T.OnUnlinked();
    T.ScheduleRescan();
    T.Tick(BleDiscoveryTimers::RescanDelayNs / 2);
    T.ScheduleRescan();                        // re-requested mid-wait

    int Fires = 0;
    for (int i = 0; i < 4; ++i)
        if (T.Tick(BleDiscoveryTimers::RescanDelayNs).Rescan) ++Fires;
    CHECK(Fires == 1);
}

// Linking cancels a pending rescan: scanning after the link is up is churn, and on some stacks it
// actively degrades the live connection.
static void TestLinkCancelsAPendingRescan() {
    BleDiscoveryTimers T;
    T.OnUnlinked();
    T.ScheduleRescan();
    T.OnLinked();

    CHECK(!T.Tick(BleDiscoveryTimers::RescanDelayNs * 4).Rescan);
}

// All three can be due in the same tick — a long stall while the app was suspended, for instance.
// The caller must be told about each, not just the first.
static void TestSeveralDeadlinesCanFireInOneTick() {
    BleDiscoveryTimers T;
    T.OnUnlinked();
    T.OnConnectStarted();
    T.ScheduleRescan();

    const BleDiscoveryTimers::Actions A = T.Tick(BleDiscoveryTimers::DiscoveryWatchdogNs * 2);
    CHECK(A.GoSymmetric);
    CHECK(A.AbortConnect);
    CHECK(A.Rescan);
}

int main() {
    TestDiscoveryWatchdogFiresWhileUnlinked();
    TestDiscoveryWatchdogIsPeriodic();
    TestDiscoveryWatchdogStopsWhenLinked();
    TestConnectWatchdogAbortsAStalledAttempt();
    TestResolvedConnectCancelsItsWatchdog();
    TestConnectWatchdogFiresBeforeDiscoveryWatchdog();
    TestRescanIsDelayedNotImmediate();
    TestRescanDoesNotStack();
    TestLinkCancelsAPendingRescan();
    TestSeveralDeadlinesCanFireInOneTick();

    if (GFailures == 0) {
        std::printf("ble_discovery_timers_tests: all checks passed\n");
        return 0;
    }
    std::printf("ble_discovery_timers_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
