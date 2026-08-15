#pragma once

namespace Lur::Transport {

// What the radio is ACTUALLY doing — and therefore what it is worth asking it to do next.
//
// Found on hardware (Galaxy A14, 2026-08-15): toggle the adapter off and on under a running app and
// it never advertises or scans again. Invisible and deaf for the life of the process, while a fresh
// launch seconds later came up fine. The radio was healthy; the app's memory of it was not.
//
// The mechanism is worth stating in full, because both halves are individually correct:
//
//   * #194 made the starts idempotent — `if (advertising) return` — because asking twice earns
//     ALREADY_STARTED, and that churn is what wedged the radio to begin with.
//   * The flag behind that guard was set when we ASKED and cleared only by our own stop or our own
//     failure callback.
//
// An adapter power cycle is neither. It takes the registration away without telling us, so the flag
// stays true, the guard suppresses every future attempt, and the 8 s discovery watchdog fires
// forever into a function that returns immediately. On the device that showed as the watchdog line
// repeating with no radio-state line after it — nothing had "changed", because the flags still
// claimed health.
//
// So: the same disease as #194 (permanently invisible after a transient fault) reached by a
// different route, and the same disease as #203 (a flag reporting what we HOPED, not what IS).
//
// This is engine C++ rather than a platform bool because "am I advertising" is a fact both
// platforms need and both got wrong the same way — iOS has the identical hole through
// CBManagerState. It is also the seed of the BleLinkController this phase is heading for: link
// state and radio state are the two things that controller has to own, and this is the half with a
// hardware-found bug behind it. Deliberately NOT a general state machine yet — it answers only the
// questions something actually asks.
//
// The rule it encodes: A RADIO POWER CYCLE INVALIDATES EVERY STARTED-STATE. Nothing we asked for
// before the radio went away survives it.
class BleRadioState {
public:
    // --- Facts, reported by the driver. None of these decide anything. ---

    // The adapter came up (Android ACTION_STATE_CHANGED -> STATE_ON, iOS CBManagerStatePoweredOn).
    void OnAdapterOn() { AdapterOn_ = true; }

    // The adapter went away. Everything it was holding went with it — including the GATT service,
    // which is why Serving_ is cleared here and nowhere else automatic.
    void OnAdapterOff() {
        AdapterOn_ = false;
        Advertising_ = false;
        Scanning_ = false;
        Serving_ = false;
    }

    void OnAdvertising()      { Advertising_ = true; }
    void OnScanning()         { Scanning_ = true; }
    void OnServing()          { Serving_ = true; }

    // A start failed, or we stopped on purpose. Either way we are not doing it, so the next attempt
    // must be allowed through — that is the edge BleStartRetry schedules against.
    void OnAdvertiseStopped() { Advertising_ = false; }
    void OnScanStopped()      { Scanning_ = false; }

    void OnLinked()   { Linked_ = true;  Advertising_ = false; Scanning_ = false; }
    void OnUnlinked() { Linked_ = false; }

    // --- Questions the driver asks before touching the radio. ---

    // Worth asking the radio to start? No if the adapter is gone (the attempt errors, and on some
    // stacks leaves a registration behind for the next process to collide with), no if we are
    // already doing it (ALREADY_STARTED, #194), no while linked (churn on a live link is what
    // degraded the radio in #163).
    bool ShouldStartAdvertising() const { return AdapterOn_ && !Advertising_ && !Linked_; }
    bool ShouldStartScanning()    const { return AdapterOn_ && !Scanning_    && !Linked_; }
    // Serving is NOT gated on Linked_: the GATT service is published once and outlives discovery.
    // It does not outlive the adapter.
    bool ShouldStartServing()     const { return AdapterOn_ && !Serving_; }

    bool IsAdapterOn()   const { return AdapterOn_; }
    bool IsAdvertising() const { return Advertising_; }
    bool IsScanning()    const { return Scanning_; }
    bool IsServing()     const { return Serving_; }
    bool IsLinked()      const { return Linked_; }

private:
    // Adapter state starts UNKNOWN, and unknown must read as off: starting into a radio we have not
    // been told is up is the attempt that leaves a stale registration behind.
    bool AdapterOn_   = false;
    bool Advertising_ = false;
    bool Scanning_    = false;
    bool Serving_     = false;
    bool Linked_      = false;
};

}  // namespace Lur::Transport
