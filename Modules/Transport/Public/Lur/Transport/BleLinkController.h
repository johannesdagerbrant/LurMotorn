#pragma once
#include <cstdint>

#include "Lur/Transport/BleDiscoveryTimers.h"
#include "Lur/Transport/BleRadioState.h"
#include "Lur/Transport/BleStartRetry.h"

namespace Lur::Transport {

// The whole unlinked-radio policy behind ONE object. A driver holds this, reports what the radio
// did, and applies what it is told. It owns no platform types and touches no radio.
//
// WHY IT EXISTS. After the #197 cutover both drivers held four policy objects and composed them by
// hand — and the two compositions had already begun to differ:
//
//   Android   OnRadioLinked()  { Timers.OnLinked(); RadioState.OnLinked();
//                                AdvRetry.Cancel(); ScanRetry.Cancel(); }
//   iOS       onLinked         { _Timers.OnLinked(); _RadioState.OnLinked(); }
//
// Both defensible — iOS has no start-failure edge, so it has no retries to cancel — but that is a
// judgement being re-made in each platform file, in a language no host test can reach. That is
// precisely the shape that produced 650 lines of drift and the one-sided #194 fix, and it is what
// the shared-first doctrine exists to prevent: *if a line chooses, it is engine C++.*
//
// A pleasant consequence: "BleStartRetry is Android-only" stops being a special case. iOS simply
// never reports a start failure, because CoreBluetooth does not have that edge. One policy, two
// platforms, different events — rather than two policies.
//
// TIMING is nanoseconds fed to Tick(), never a platform timer, so every deadline is host-testable
// and identical on both platforms. Call Tick() once per transport pump.
class BleLinkController {
public:
    // What the driver should DO this pump. Several can be true at once: a long stall (an app
    // suspended and resumed) leaves every deadline overdue, and the driver needs each of them, not
    // just the first.
    struct Actions {
        bool StartAdvertising = false;  // a start-failure backoff came due
        bool StartScanning    = false;
        bool GoSymmetric      = false;  // discovery watchdog: drop cached-role gates, advertise+scan
        bool AbortConnect     = false;  // connect watchdog: tear the stalled attempt down
        bool Rescan           = false;  // the post-collision rescan delay elapsed
    };

    // --- Facts. None of these decide anything; none of them touch a radio. ---

    void OnAdapterOn() { Radio_.OnAdapterOn(); Timers_.OnUnlinked(); }

    // The radio went away. Everything it held went with it, and any pending backoff was scheduled
    // against a radio that no longer exists — firing it would earn an error and, on some stacks,
    // leave a registration behind for the next process to collide with (#194).
    void OnAdapterOff() {
        Radio_.OnAdapterOff();
        AdvRetry_.Cancel();
        ScanRetry_.Cancel();
    }

    // The link is up. Stop every deadline and abandon both backoffs: firing into a live link is
    // churn, and churn is what degraded the radio in #163.
    void OnLinked() {
        Radio_.OnLinked();
        Timers_.OnLinked();
        AdvRetry_.Cancel();
        ScanRetry_.Cancel();
        // The escalation is per unlinked-stretch. Carrying it across a link would mean every later
        // reconnect began in the distrusting, mutually-connecting state we are avoiding.
        SymmetricRounds_ = 0;
    }

    // The link dropped. Re-arms discovery FROM ZERO, so time spent linked is never banked against
    // the next unlinked stretch.
    void OnUnlinked() { Radio_.OnUnlinked(); Timers_.OnUnlinked(); }

    // Shutting down. Quiesces exactly like a link-up, but says so honestly — "linked" would be a lie
    // at the one moment someone reading a shutdown log can least afford one.
    void OnStopped() {
        Timers_.OnLinked();     // the "no deadline is armed" state
        AdvRetry_.Cancel();
        ScanRetry_.Cancel();
    }

    void OnAdvertising()      { Radio_.OnAdvertising(); }
    void OnScanning()         { Radio_.OnScanning(); }
    void OnServing()          { Radio_.OnServing(); }
    void OnAdvertiseStopped() { Radio_.OnAdvertiseStopped(); }
    void OnScanStopped()      { Radio_.OnScanStopped(); }

    // A start SUCCEEDED: clear the backoff so a failure an hour from now starts from the shortest
    // delay again.
    void OnAdvertiseStarted() { Radio_.OnAdvertising(); AdvRetry_.OnStarted(); }
    void OnScanStarted()      { Radio_.OnScanning();    ScanRetry_.OnStarted(); }

    // A start FAILED. Clears the state (so the retry is not suppressed by the idempotence guard)
    // and schedules the backoff. Returns whether this failure deserves a LOUD log line — true for
    // the first of a run, false for the repeats, so a retry loop cannot bury the one that matters.
    bool OnAdvertiseStartFailed() {
        Radio_.OnAdvertiseStopped();
        return AdvRetry_.OnStartFailed();
    }
    bool OnScanStartFailed() {
        Radio_.OnScanStopped();
        return ScanRetry_.OnStartFailed();
    }

    void OnConnectStarted()  { Timers_.OnConnectStarted(); }
    void OnConnectResolved() { Timers_.OnConnectResolved(); }
    void ScheduleRescan()    { Timers_.ScheduleRescan(); }

    // The discovery watchdog fired and we dropped the cached-role gates. Counted, because trusting
    // the cached peer id forever is what #79 exists to prevent — see ShouldConnectOut.
    void OnWentSymmetric() { ++SymmetricRounds_; }

    // How many fruitless symmetric rounds before the cached peer id stops being trusted. Three is
    // ~24 s at the 8 s watchdog cadence: long enough that a healthy pair links first (measured
    // 2-4.5 s when one side is already central), short enough that a genuinely re-rolled peer is
    // not left deaf for long.
    static constexpr int SymmetricRoundsBeforeDistrust = 3;

    // MAY WE CONNECT OUT? This is #206's fix, and it is the one decision the watchdog got wrong.
    //
    // #79's gate-drop put BOTH peers into connect-out mode on the same 8 s cadence. Two BLE devices
    // share ONE LE link, so when the side the tie-break elects peripheral defers and disconnects,
    // it tears down the peer's in-flight incoming attempt as well. Measured on the pair
    // (2026-08-16): three wasted rounds per recovery, resolving only when #146's fruitless-defer
    // breaker fired — while a phone already in the central role recovered in 2-4.5 s because it
    // never entered the dance.
    //
    // So discovery stays symmetric (both advertise, both scan — that is what finds a peer at all),
    // but INITIATION is one-sided, chosen by the tie-break both phones can compute from the two
    // ids. No negotiation, no new wire state: DecideBleRole is already symmetric and already
    // shared.
    //
    // #79's guarantee is preserved as an escalation rather than as the default: after enough
    // fruitless rounds the cached id may belong to a peer that re-rolled its GUID, so it stops
    // being trusted and anyone may initiate. #146's breaker remains the backstop underneath.
    bool ShouldConnectOut(bool HaveCachedPeer, bool TieBreakSaysCentral) const {
        if (!HaveCachedPeer) return true;   // first pairing: no tie-break to apply, both must try
        if (SymmetricRounds_ >= SymmetricRoundsBeforeDistrust) return true;   // #79 escape hatch
        return TieBreakSaysCentral;
    }

    // --- Questions the driver asks before touching the radio. ---
    bool ShouldStartAdvertising() const { return Radio_.ShouldStartAdvertising(); }
    bool ShouldStartScanning()    const { return Radio_.ShouldStartScanning(); }
    bool ShouldStartServing()     const { return Radio_.ShouldStartServing(); }

    bool IsLinked()             const { return Radio_.IsLinked(); }
    bool IsAdapterOn()          const { return Radio_.IsAdapterOn(); }
    bool HasPendingStartRetry() const { return AdvRetry_.IsPending() || ScanRetry_.IsPending(); }

    // Advance every deadline. Once per transport pump, with the nanoseconds since the previous call.
    Actions Tick(uint64_t ElapsedNs) {
        Actions Out;
        Out.StartAdvertising = AdvRetry_.Tick(ElapsedNs);
        Out.StartScanning    = ScanRetry_.Tick(ElapsedNs);

        const BleDiscoveryTimers::Actions T = Timers_.Tick(ElapsedNs);
        Out.GoSymmetric  = T.GoSymmetric;
        Out.AbortConnect = T.AbortConnect;
        Out.Rescan       = T.Rescan;
        return Out;
    }

private:
    BleRadioState      Radio_;
    BleDiscoveryTimers Timers_;
    // Separate backoffs on purpose: advertise and scan are separate registrations with separate
    // failure modes, and one failing must not delay the other's recovery.
    BleStartRetry      AdvRetry_;
    BleStartRetry      ScanRetry_;
    int                SymmetricRounds_ = 0;
};

}  // namespace Lur::Transport
