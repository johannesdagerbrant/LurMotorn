#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace Lur::Pairing {

// A nearby device advertising the LurMotorn BLE service.
struct Peer {
    std::string Id;           // opaque platform handle (BLE identifier)
    std::string DisplayName;  // what the user sees in the device list
};

// Cross-platform pairing flow, BLE-only (NFC is unavailable iOS<->Android):
//   1. Both phones advertise + scan for the LurMotorn service.
//   2. Each shows the peers it found; the user picks one.
//   3. A short numeric code derived from the session key is shown on BOTH phones;
//      the user confirms they match (guards against connecting to the wrong device).
//   4. On confirm, roles are assigned (one peripheral, one central) and an
//      ITransport is produced for the net/game layers.
class IPairing {
public:
    virtual ~IPairing() = default;

    virtual void StartAdvertising(const std::string& DisplayName) = 0;
    virtual void StartScanning() = 0;

    using PeerFound = std::function<void(const Peer&)>;
    virtual void SetPeerFound(PeerFound OnFound) = 0;

    // Begin connecting to a chosen peer. The 6-digit confirmation code both sides
    // should display is delivered via OnConfirmCode.
    using ConfirmCode = std::function<void(uint32_t SixDigitCode)>;
    virtual void Connect(const Peer& Target, ConfirmCode OnConfirmCode) = 0;

    virtual void Stop() = 0;
};

} // namespace Lur::Pairing
