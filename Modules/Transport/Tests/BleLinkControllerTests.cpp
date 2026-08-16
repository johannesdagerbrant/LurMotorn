// Host tests for Lur::Transport::BleLinkController — the whole unlinked-radio policy behind ONE
// object, so a driver holds one thing and applies what it is told.
//
// Why it exists, concretely. After the #197 cutover both drivers ended up holding four policy
// objects and hand-composing them, and the composition had already started to differ:
//
//   Android   OnRadioLinked()  { Timers.OnLinked(); RadioState.OnLinked();
//                                AdvRetry.Cancel(); ScanRetry.Cancel(); }
//   iOS       onLinked         { _Timers.OnLinked(); _RadioState.OnLinked(); }
//
// Both are "correct" — iOS has no start-failure edge to retry, so it has no retries to cancel — but
// that is a judgement being re-made in each platform file, in a language no host test can reach.
// That is the exact shape that produced 650 lines of drift and the one-sided #194 fix. Four objects
// composed by hand in two places is three objects too many to keep honest.
//
// So the controller owns BleRadioState, BleDiscoveryTimers and the two BleStartRetry instances, and
// the drivers own none of them. A driver reports facts and applies Actions. iOS simply never reports
// a start failure, which turns "BleStartRetry is Android-only" from a special case into an edge one
// platform happens not to have.
#include <cstdint>
#include <cstdio>

#include "Lur/Transport/BleLinkController.h"

using namespace Lur::Transport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// Bring a controller to the state a running, unlinked, powered-on radio is in.
static BleLinkController Powered() {
    BleLinkController C;
    C.OnAdapterOn();
    return C;
}

// The composition Android hand-rolled: a link stops every deadline AND abandons both backoffs.
// Retrying or scanning into a live link is pure churn, and churn is what degraded the radio (#163).
static void TestLinkQuiescesEverything() {
    BleLinkController C = Powered();
    C.OnAdvertiseStartFailed();                 // a backoff is now pending
    CHECK(C.HasPendingStartRetry());
    C.OnLinked();
    CHECK(!C.HasPendingStartRetry());           // cancelled, not left to fire into the link
    const BleLinkController::Actions A = C.Tick(60'000'000'000ull);   // a full minute linked
    CHECK(!A.GoSymmetric);                      // no deadline may fire while linked
    CHECK(!A.StartAdvertising && !A.StartScanning && !A.AbortConnect);
}

// ...and dropping the link re-arms discovery FROM ZERO. Time spent linked must not be banked
// against the next unlinked stretch, or the first watchdog after a long game fires instantly.
static void TestUnlinkReArmsFromZero() {
    BleLinkController C = Powered();
    C.OnLinked();
    C.Tick(60'000'000'000ull);
    C.OnUnlinked();
    CHECK(!C.Tick(BleDiscoveryTimers::DiscoveryWatchdogNs - 1).GoSymmetric);
    CHECK(C.Tick(1).GoSymmetric);
}

// The hardware bug from 2026-08-15, now at controller level: a power cycle forgets every
// started-state, so the idempotence guard cannot suppress the recovery.
static void TestAdapterCycleForgetsAndThenAllowsRestart() {
    BleLinkController C = Powered();
    C.OnAdvertising();
    C.OnScanning();
    C.OnServing();
    CHECK(!C.ShouldStartAdvertising());
    C.OnAdapterOff();
    CHECK(!C.ShouldStartAdvertising());         // not while the radio is gone
    C.OnAdapterOn();
    CHECK(C.ShouldStartAdvertising());
    CHECK(C.ShouldStartScanning());
    CHECK(C.ShouldStartServing());
}

// A power cycle must also abandon any pending backoff: it was scheduled against a radio that no
// longer exists, and firing it would earn an error and leave a registration behind.
static void TestAdapterOffCancelsAPendingRetry() {
    BleLinkController C = Powered();
    C.OnAdvertiseStartFailed();
    CHECK(C.HasPendingStartRetry());
    C.OnAdapterOff();
    CHECK(!C.HasPendingStartRetry());
}

// The #194 path end to end: a failed start schedules a retry, and the retry surfaces as an ACTION
// the driver applies — not as something the policy does behind its back.
static void TestFailedStartSurfacesAsARetryAction() {
    BleLinkController C = Powered();
    C.OnAdvertising();
    C.OnAdvertiseStartFailed();                 // clears the state and schedules
    CHECK(C.ShouldStartAdvertising());          // ...so the retry is not suppressed
    CHECK(!C.Tick(BleStartRetry::BaseDelayNs - 1).StartAdvertising);
    CHECK(C.Tick(1).StartAdvertising);
}

// Advertise and scan back off INDEPENDENTLY. One radio verb failing must not delay the other's
// recovery — they are separate registrations and separate failure modes.
static void TestAdvertiseAndScanBackOffIndependently() {
    BleLinkController C = Powered();
    C.OnScanStartFailed();
    const BleLinkController::Actions A = C.Tick(BleStartRetry::BaseDelayNs);
    CHECK(A.StartScanning);
    CHECK(!A.StartAdvertising);                 // never asked, never failed, never retried
}

// Only the first failure of a run wants a loud line; the repeats must stay quiet or the retry loop
// buries the message that matters.
static void TestOnlyTheFirstFailureIsLoud() {
    BleLinkController C = Powered();
    CHECK(C.OnAdvertiseStartFailed());
    CHECK(!C.OnAdvertiseStartFailed());
    C.OnAdvertiseStarted();                     // a success makes a later failure news again
    CHECK(C.OnAdvertiseStartFailed());
}

// The connect watchdog still has to fire BEFORE the discovery watchdog, so a stalled attempt is
// cleaned up before the symmetric reset lands on top of it.
static void TestConnectWatchdogPrecedesTheSymmetricReset() {
    BleLinkController C = Powered();
    C.OnConnectStarted();
    const BleLinkController::Actions A = C.Tick(BleDiscoveryTimers::ConnectWatchdogNs);
    CHECK(A.AbortConnect);
    CHECK(!A.GoSymmetric);
    CHECK(BleDiscoveryTimers::ConnectWatchdogNs < BleDiscoveryTimers::DiscoveryWatchdogNs);
}

// Stopping is not linking, but it quiesces the same way — and it must not leave a backoff armed to
// fire into a torn-down radio.
static void TestStopQuiescesWithoutClaimingALink() {
    BleLinkController C = Powered();
    C.OnScanStartFailed();
    C.OnStopped();
    CHECK(!C.HasPendingStartRetry());
    CHECK(!C.IsLinked());
    CHECK(!C.Tick(60'000'000'000ull).GoSymmetric);
}

// A long stall (app suspended and resumed) can leave several deadlines overdue at once. The driver
// needs to hear about each, not only the first.
static void TestSeveralActionsCanArriveInOneTick() {
    BleLinkController C = Powered();
    C.OnConnectStarted();
    C.ScheduleRescan();
    C.OnAdvertiseStartFailed();
    const BleLinkController::Actions A = C.Tick(60'000'000'000ull);
    CHECK(A.AbortConnect);
    CHECK(A.GoSymmetric);
    CHECK(A.Rescan);
    CHECK(A.StartAdvertising);
}


// ---------------------------------------------------------------------------
// #206: who connects out when we go symmetric.
//
// The discovery watchdog (#79) exists because a cached role keyed to a peer that re-rolled its
// GUID leaves BOTH phones deaf forever. Its cure was to drop the cached-role gates entirely — but
// that put both peers into connect-out mode on the same 8 s cadence, which is the #17 mutual-connect
// collision the cached roles were introduced to prevent.
//
// Two BLE devices share ONE LE link, so when the side the tie-break elects peripheral defers and
// disconnects, it tears down the peer's in-flight incoming attempt too. Measured on the pair
// (2026-08-16): three wasted rounds per recovery, resolving only when #146's fruitless-defer
// breaker fired. Central-role recovery, which never enters the dance, took 2-4.5 s.
//
// So: advertise+scan on BOTH (discovery must stay symmetric, or a re-rolled peer is never found),
// but connect out from ONE, chosen by the tie-break both phones can compute. #79's escape hatch is
// preserved as an escalation — after enough fruitless rounds the cached id is distrusted and
// anyone may connect.

// With no cached peer there is no tie-break to apply — first pairing, both must try.
static void TestNoCachedPeerAlwaysConnectsOut() {
    BleLinkController C = Powered();
    CHECK(C.ShouldConnectOut(/*HaveCachedPeer=*/false, /*TieBreakSaysCentral=*/false));
}

// The whole point: with a cached peer, only the elected central initiates.
static void TestElectedPeripheralDoesNotConnectOut() {
    BleLinkController C = Powered();
    C.OnWentSymmetric();
    CHECK(!C.ShouldConnectOut(true, /*TieBreakSaysCentral=*/false));
    CHECK(C.ShouldConnectOut(true, /*TieBreakSaysCentral=*/true));
}

// #79 preserved: after enough fruitless symmetric rounds the cached id is no longer trusted, so a
// peer that re-rolled its GUID cannot leave us deaf forever.
static void TestCachedRoleIsDistrustedAfterEnoughFruitlessRounds() {
    BleLinkController C = Powered();
    for (int i = 0; i < BleLinkController::SymmetricRoundsBeforeDistrust; ++i) {
        CHECK(!C.ShouldConnectOut(true, false));   // still deferring to the elected central
        C.OnWentSymmetric();
    }
    CHECK(C.ShouldConnectOut(true, false));        // ...and now we stop trusting it
}

// A link resets the escalation: the next unlinked stretch starts from trusting the tie-break again,
// or every later reconnect would begin in the colliding state we are trying to avoid.
static void TestLinkResetsTheEscalation() {
    BleLinkController C = Powered();
    for (int i = 0; i <= BleLinkController::SymmetricRoundsBeforeDistrust; ++i) C.OnWentSymmetric();
    CHECK(C.ShouldConnectOut(true, false));
    C.OnLinked();
    C.OnUnlinked();
    CHECK(!C.ShouldConnectOut(true, false));
}

int main() {
    TestLinkQuiescesEverything();
    TestUnlinkReArmsFromZero();
    TestAdapterCycleForgetsAndThenAllowsRestart();
    TestAdapterOffCancelsAPendingRetry();
    TestFailedStartSurfacesAsARetryAction();
    TestAdvertiseAndScanBackOffIndependently();
    TestOnlyTheFirstFailureIsLoud();
    TestConnectWatchdogPrecedesTheSymmetricReset();
    TestStopQuiescesWithoutClaimingALink();
    TestSeveralActionsCanArriveInOneTick();
    TestNoCachedPeerAlwaysConnectsOut();
    TestElectedPeripheralDoesNotConnectOut();
    TestCachedRoleIsDistrustedAfterEnoughFruitlessRounds();
    TestLinkResetsTheEscalation();

    if (GFailures == 0) std::printf("BleLinkController tests: all passed\n");
    return GFailures == 0 ? 0 : 1;
}
