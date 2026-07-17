#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

// Lur::Core::FlightRecorder — record everything, always, so a bug becomes a FILE that
// replays (Review #2 §4.2). The architecture's deepest property is that state is
// derived from the input stream; the recorder captures that stream — every datagram
// in/out, link transition, and local input, timestamped — into a bounded ring. Feed a
// recording back through the loopback transport on the desktop and the exact failure
// re-happens in a debugger, every time.
//
// At chess/RTS datagram rates this is bytes/sec — there is no cost argument. The ring
// bounds memory for a long soak; the wire format is a flat [count][events...] blob.
namespace Lur::Core {

enum class EFlightEvent : uint8_t {
    DatagramIn  = 0,  // a datagram arrived from the peer
    DatagramOut = 1,  // a datagram we sent
    LinkUp      = 2,
    LinkDown    = 3,
    Input       = 4,  // a local input (e.g. a tap-derived move)
};

class FlightRecorder {
public:
    struct Event {
        EFlightEvent         Kind = EFlightEvent::Input;
        uint64_t             TimeNs = 0;
        std::vector<uint8_t> Data;
    };

    explicit FlightRecorder(std::size_t Capacity = 65536) : Cap(Capacity) {}

    // Append an event. Payloads are tiny; keep the last Cap events (drop-oldest ring).
    void Record(EFlightEvent Kind, uint64_t TimeNs, const uint8_t* Data, std::size_t Size) {
        Event E;
        E.Kind = Kind;
        E.TimeNs = TimeNs;
        if (Size > 0 && Data != nullptr) E.Data.assign(Data, Data + Size);
        if (Events_.size() >= Cap) { Events_.erase(Events_.begin()); Dropped_ = true; }
        Events_.push_back(std::move(E));
    }

    const std::vector<Event>& Events() const { return Events_; }
    std::size_t Count() const { return Events_.size(); }
    bool Dropped() const { return Dropped_; }

    // Flat wire form: [u32 count]{ [u8 kind][u64 le time][u16 le size][bytes] }.
    std::vector<uint8_t> Serialize() const {
        std::vector<uint8_t> Out;
        PutU32(Out, static_cast<uint32_t>(Events_.size()));
        for (const Event& E : Events_) {
            Out.push_back(static_cast<uint8_t>(E.Kind));
            PutU64(Out, E.TimeNs);
            PutU16(Out, static_cast<uint16_t>(E.Data.size()));
            Out.insert(Out.end(), E.Data.begin(), E.Data.end());
        }
        return Out;
    }

    // Parse a serialized recording. Returns false on a truncated/corrupt blob.
    static bool Parse(const uint8_t* Data, std::size_t Size, std::vector<Event>& Out) {
        Out.clear();
        std::size_t P = 0;
        uint32_t Count = 0;
        if (!GetU32(Data, Size, P, Count)) return false;
        for (uint32_t i = 0; i < Count; ++i) {
            if (P >= Size) return false;
            Event E;
            E.Kind = static_cast<EFlightEvent>(Data[P++]);
            uint64_t T = 0;
            uint16_t N = 0;
            if (!GetU64(Data, Size, P, T) || !GetU16(Data, Size, P, N)) return false;
            E.TimeNs = T;
            if (P + N > Size) return false;
            E.Data.assign(Data + P, Data + P + N);
            P += N;
            Out.push_back(std::move(E));
        }
        return true;
    }

    bool WriteFile(const char* Path) const {
        const std::vector<uint8_t> Blob = Serialize();
        std::FILE* F = std::fopen(Path, "wb");
        if (F == nullptr) return false;
        const std::size_t Wrote = Blob.empty() ? 0 : std::fwrite(Blob.data(), 1, Blob.size(), F);
        std::fclose(F);
        return Wrote == Blob.size();
    }

    static bool ReadFile(const char* Path, std::vector<Event>& Out) {
        std::FILE* F = std::fopen(Path, "rb");
        if (F == nullptr) return false;
        std::fseek(F, 0, SEEK_END);
        const long Len = std::ftell(F);
        std::fseek(F, 0, SEEK_SET);
        std::vector<uint8_t> Blob(Len > 0 ? static_cast<std::size_t>(Len) : 0);
        const std::size_t Read = Blob.empty() ? 0 : std::fread(Blob.data(), 1, Blob.size(), F);
        std::fclose(F);
        if (Read != Blob.size()) return false;
        return Parse(Blob.data(), Blob.size(), Out);
    }

private:
    static void PutU16(std::vector<uint8_t>& O, uint16_t V) {
        O.push_back(static_cast<uint8_t>(V & 0xFF));
        O.push_back(static_cast<uint8_t>((V >> 8) & 0xFF));
    }
    static void PutU32(std::vector<uint8_t>& O, uint32_t V) {
        for (int i = 0; i < 4; ++i) O.push_back(static_cast<uint8_t>((V >> (8 * i)) & 0xFF));
    }
    static void PutU64(std::vector<uint8_t>& O, uint64_t V) {
        for (int i = 0; i < 8; ++i) O.push_back(static_cast<uint8_t>((V >> (8 * i)) & 0xFF));
    }
    static bool GetU16(const uint8_t* D, std::size_t S, std::size_t& P, uint16_t& V) {
        if (P + 2 > S) return false;
        V = static_cast<uint16_t>(D[P] | (D[P + 1] << 8));
        P += 2;
        return true;
    }
    static bool GetU32(const uint8_t* D, std::size_t S, std::size_t& P, uint32_t& V) {
        if (P + 4 > S) return false;
        V = 0;
        for (int i = 0; i < 4; ++i) V |= static_cast<uint32_t>(D[P + i]) << (8 * i);
        P += 4;
        return true;
    }
    static bool GetU64(const uint8_t* D, std::size_t S, std::size_t& P, uint64_t& V) {
        if (P + 8 > S) return false;
        V = 0;
        for (int i = 0; i < 8; ++i) V |= static_cast<uint64_t>(D[P + i]) << (8 * i);
        P += 8;
        return true;
    }

    std::vector<Event> Events_;
    std::size_t        Cap;
    bool               Dropped_ = false;
};

}  // namespace Lur::Core
