// Host tests for Lur::Transport::BleRadioState — what the radio is ACTUALLY doing.
//
// Found on hardware (Galaxy A14, 2026-08-15) while verifying the #197 policy cutover:
//
//   adb shell svc bluetooth disable   ... wait ...   adb shell svc bluetooth enable
//
// and the phone never advertised or scanned again. It stayed invisible and deaf for the life of
// the process — a fresh launch on the same phone, seconds later, came up fine, so the radio was
// healthy and the app was not.
//
// The mechanism is #194's idempotence guard meeting a fact nothing tracked. `startAdvertising()`
// begins `if (advertising) return` — correct, because asking twice earns ALREADY_STARTED and that
// churn is what wedged the radio in the first place. But `advertising` was a Kotlin bool set when
// we asked and cleared only by our own stop or our own failure callback. An adapter power cycle
// is neither: it takes the registration away WITHOUT telling us, so the bool stays true, the
// guard suppresses every future attempt, and the discovery watchdog fires forever against a
// function that returns immediately. Observed exactly that: the 8 s watchdog line repeating with
// no radio-state line after it, because nothing had changed — the flags still claimed health.
//
// So this is the same disease as #194 (permanently invisible after a transient fault) reached by a
// different route, and the same disease as #203 (a flag reporting what we HOPED rather than what
// IS). It lives here, in C++ with tests, because "am I advertising" is a fact both platforms need
// and both platforms got wrong: iOS has the identical hole via CBManagerState.
//
// The rule this encodes: a radio power cycle invalidates every started-state. Nothing we asked for
// before the radio went away survives it.
#include <cstdio>

#include "Lur/Transport/BleRadioState.h"

using namespace Lur::Transport;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// The bug, as a test. Everything up, radio cycles, and we must NOT still believe we are advertising
// — because that belief is what suppresses the recovery.
static void TestAdapterOffInvalidatesEveryStartedState() {
    BleRadioState S;
    S.OnAdapterOn();
    S.OnServing();
    S.OnAdvertising();
    S.OnScanning();
    CHECK(S.IsServing() && S.IsAdvertising() && S.IsScanning());

    S.OnAdapterOff();
    CHECK(!S.IsAdvertising());   // <-- the one that was stuck true on the device
    CHECK(!S.IsScanning());
    CHECK(!S.IsServing());       // the GATT server dies with the adapter too
    CHECK(!S.IsAdapterOn());
}

// ...and having forgotten, we must be WILLING to start again. This is the half that actually heals
// the phone: the guard has to let the next attempt through.
static void TestStartIsAllowedAgainAfterAPowerCycle() {
    BleRadioState S;
    S.OnAdapterOn();
    S.OnAdvertising();
    CHECK(!S.ShouldStartAdvertising());   // already up: asking again earns ALREADY_STARTED (#194)

    S.OnAdapterOff();
    CHECK(!S.ShouldStartAdvertising());   // ...but not while the radio is gone

    S.OnAdapterOn();
    CHECK(S.ShouldStartAdvertising());    // <-- the recovery the device never performed
    CHECK(S.ShouldStartScanning());
}

// The idempotence guard #194 added still has to work, or we are back to the churn that wedged the
// radio. This is the case the fix must not break.
static void TestNoDoubleStartWhileAlreadyUp() {
    BleRadioState S;
    S.OnAdapterOn();
    CHECK(S.ShouldStartAdvertising());
    S.OnAdvertising();
    CHECK(!S.ShouldStartAdvertising());
    CHECK(!S.ShouldStartAdvertising());   // still no, however often asked
}

// A start that FAILED leaves us not-advertising, so the retry must be allowed. This is the edge
// BleStartRetry schedules against; if the state said "advertising" the retry would be suppressed
// and the backoff would tick away against nothing.
static void TestFailedStartLeavesUsWillingToRetry() {
    BleRadioState S;
    S.OnAdapterOn();
    S.OnAdvertising();
    S.OnAdvertiseStopped();
    CHECK(!S.IsAdvertising());
    CHECK(S.ShouldStartAdvertising());
}

// Nothing starts while the adapter is off. Attempting it earns an error and, on some stacks, leaves
// a registration behind for the NEXT process to collide with (#194).
static void TestNothingStartsWhileTheAdapterIsOff() {
    BleRadioState S;                      // fresh: adapter state unknown, assume off
    CHECK(!S.IsAdapterOn());
    CHECK(!S.ShouldStartAdvertising());
    CHECK(!S.ShouldStartScanning());
    CHECK(!S.ShouldStartServing());
}

// A link does not stop us tracking the radio, but discovery is over: we stop advertising and
// scanning, and must not be told to start them again while linked. Churn on a live link is what
// degraded the radio in #163.
static void TestLinkedSuppressesDiscoveryStarts() {
    BleRadioState S;
    S.OnAdapterOn();
    S.OnLinked();
    CHECK(!S.ShouldStartAdvertising());
    CHECK(!S.ShouldStartScanning());
    S.OnUnlinked();
    CHECK(S.ShouldStartAdvertising());    // ...and resume the moment it drops
    CHECK(S.ShouldStartScanning());
}

// Serving is tracked separately: the GATT service is published once and outlives discovery, but it
// does NOT outlive the adapter. A phone that lost its service while thinking it still had one
// accepts no connection and reports health — the #203 shape.
static void TestServingIsIndependentOfDiscoveryButNotOfTheAdapter() {
    BleRadioState S;
    S.OnAdapterOn();
    S.OnServing();
    S.OnLinked();
    CHECK(S.IsServing());                 // a link does not unpublish the service
    CHECK(!S.ShouldStartServing());
    S.OnAdapterOff();
    CHECK(!S.IsServing());
    S.OnAdapterOn();
    CHECK(S.ShouldStartServing());        // republish: the adapter took it with it
}

int main() {
    TestAdapterOffInvalidatesEveryStartedState();
    TestStartIsAllowedAgainAfterAPowerCycle();
    TestNoDoubleStartWhileAlreadyUp();
    TestFailedStartLeavesUsWillingToRetry();
    TestNothingStartsWhileTheAdapterIsOff();
    TestLinkedSuppressesDiscoveryStarts();
    TestServingIsIndependentOfDiscoveryButNotOfTheAdapter();

    if (GFailures == 0) std::printf("BleRadioState tests: all passed\n");
    return GFailures == 0 ? 0 : 1;
}
