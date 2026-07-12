#include "Lur/Save/DeviceId.h"

#include <cstdint>
#include <random>

namespace Lur::Save {

std::string LoadOrCreateDeviceId(Store& S) {
    // Reuse an existing, well-formed id verbatim — the whole point is stability
    // across restarts.
    const std::vector<uint8_t> Existing = S.Load(DeviceIdKey);
    if (Existing.size() == DeviceIdHexLen) {
        bool AllHex = true;
        for (uint8_t C : Existing) {
            const bool Hex = (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
            if (!Hex) { AllHex = false; break; }
        }
        if (AllHex) return std::string(Existing.begin(), Existing.end());
    }

    // Generate 128 random bits from the platform entropy source (std::random_device
    // is a non-deterministic hardware/OS source on Android and iOS alike — this is
    // identity generation, NOT gameplay sim, so non-determinism is correct here).
    std::random_device Rd;
    uint8_t Bytes[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t Word = Rd();
        Bytes[i + 0] = static_cast<uint8_t>(Word);
        Bytes[i + 1] = static_cast<uint8_t>(Word >> 8);
        Bytes[i + 2] = static_cast<uint8_t>(Word >> 16);
        Bytes[i + 3] = static_cast<uint8_t>(Word >> 24);
    }

    static constexpr char Hex[] = "0123456789abcdef";
    std::string Id;
    Id.reserve(DeviceIdHexLen);
    for (uint8_t B : Bytes) {
        Id.push_back(Hex[B >> 4]);
        Id.push_back(Hex[B & 0x0F]);
    }

    S.Save(DeviceIdKey, reinterpret_cast<const uint8_t*>(Id.data()), Id.size());
    return Id;
}

} // namespace Lur::Save
