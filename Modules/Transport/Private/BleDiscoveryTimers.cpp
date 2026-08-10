#include "Lur/Transport/BleDiscoveryTimers.h"

namespace Lur::Transport {

void BleDiscoveryTimers::OnLinked() {
    Linked_ = true;
    WatchdogArmed_ = false;
    ConnectArmed_ = false;
    RescanArmed_ = false;
    WatchdogNs_ = ConnectNs_ = RescanNs_ = 0;
}

void BleDiscoveryTimers::OnUnlinked() {
    Linked_ = false;
    WatchdogArmed_ = true;
    WatchdogNs_ = 0;      // from zero: time spent linked is not banked against this stretch
}

void BleDiscoveryTimers::OnConnectStarted() {
    ConnectArmed_ = true;
    ConnectNs_ = 0;
}

void BleDiscoveryTimers::OnConnectResolved() {
    ConnectArmed_ = false;
    ConnectNs_ = 0;
}

void BleDiscoveryTimers::ScheduleRescan() {
    if (Linked_) return;      // scanning on top of a live link is churn
    RescanArmed_ = true;
    RescanNs_ = 0;            // a re-request supersedes the pending one rather than stacking
}

BleDiscoveryTimers::Actions BleDiscoveryTimers::Tick(uint64_t ElapsedNs) {
    Actions A;

    if (WatchdogArmed_) {
        WatchdogNs_ += ElapsedNs;
        if (WatchdogNs_ >= DiscoveryWatchdogNs) {
            A.GoSymmetric = true;
            WatchdogNs_ = 0;      // PERIODIC: re-arm, because an unlinked phone keeps trying
        }
    }

    if (ConnectArmed_) {
        ConnectNs_ += ElapsedNs;
        if (ConnectNs_ >= ConnectWatchdogNs) {
            A.AbortConnect = true;
            ConnectArmed_ = false;   // one-shot: the caller tears down and decides what follows
            ConnectNs_ = 0;
        }
    }

    if (RescanArmed_) {
        RescanNs_ += ElapsedNs;
        if (RescanNs_ >= RescanDelayNs) {
            A.Rescan = true;
            RescanArmed_ = false;    // one-shot per request
            RescanNs_ = 0;
        }
    }

    return A;
}

}  // namespace Lur::Transport
