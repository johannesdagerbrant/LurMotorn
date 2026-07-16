#include "Chess/MatchMeta.h"

#include <chrono>

namespace Chess {

namespace {
std::string MetaKey(const std::string& Guid) { return "meta-" + Guid; }
}  // namespace

MatchMeta LoadMatchMeta(const Lur::Save::Store& Store, const std::string& Guid) {
    MatchMeta M;
    const std::vector<uint8_t> B = Store.Load(MetaKey(Guid));
    if (B.size() >= 8) {
        // Little-endian u64, explicit (no reinterpret) so it's portable across ABIs.
        std::uint64_t V = 0;
        for (int i = 0; i < 8; ++i) V |= static_cast<std::uint64_t>(B[i]) << (8 * i);
        M.LastMoveMs = V;
    }
    return M;
}

bool SaveMatchMeta(Lur::Save::Store& Store, const std::string& Guid, const MatchMeta& Meta) {
    uint8_t B[8];
    for (int i = 0; i < 8; ++i) B[i] = static_cast<uint8_t>((Meta.LastMoveMs >> (8 * i)) & 0xFF);
    return Store.Save(MetaKey(Guid), B, sizeof(B));
}

std::uint64_t NowMillisUtc() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

} // namespace Chess
