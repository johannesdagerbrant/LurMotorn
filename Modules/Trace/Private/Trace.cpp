// Lur::Trace registry — fixed-capacity, lock-free on the hot path (issue #101).
#include "Lur/Trace/Trace.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace Lur::Trace {
namespace {

struct Slot {
    std::atomic<const char*> Name{nullptr};  // set once at registration
    std::atomic<uint64_t>    TotalNs{0};
    std::atomic<uint32_t>    Count{0};
    std::atomic<uint64_t>    MaxNs{0};
};

Slot             g_Slots[MaxScopes];
std::atomic<int> g_Count{0};

// Registration is rare (once per call-site); guard it with a mutex. The hot path
// (AddSample) never takes this lock.
std::mutex& RegMutex() {
    static std::mutex m;
    return m;
}

}  // namespace

uint64_t NowNs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

ScopeId Register(const char* Name) {
    if (Name == nullptr) return InvalidScope;
    std::lock_guard<std::mutex> Lk(RegMutex());
    const int N = g_Count.load(std::memory_order_relaxed);
    for (int I = 0; I < N; ++I) {
        const char* Existing = g_Slots[I].Name.load(std::memory_order_relaxed);
        if (Existing != nullptr && std::strcmp(Existing, Name) == 0)
            return static_cast<ScopeId>(I);
    }
    if (N >= MaxScopes) return InvalidScope;  // bump MaxScopes if this ever trips
    g_Slots[N].Name.store(Name, std::memory_order_relaxed);
    g_Count.store(N + 1, std::memory_order_release);
    return static_cast<ScopeId>(N);
}

void AddSample(ScopeId Id, uint64_t Ns) {
    if (Id >= static_cast<ScopeId>(g_Count.load(std::memory_order_acquire))) return;
    Slot& S = g_Slots[Id];
    S.TotalNs.fetch_add(Ns, std::memory_order_relaxed);
    S.Count.fetch_add(1, std::memory_order_relaxed);
    uint64_t Prev = S.MaxNs.load(std::memory_order_relaxed);
    while (Ns > Prev && !S.MaxNs.compare_exchange_weak(Prev, Ns, std::memory_order_relaxed)) {
        // Prev reloaded by compare_exchange_weak on failure; retry.
    }
}

int Snapshot(ScopeStat* Out, int MaxOut) {
    if (Out == nullptr || MaxOut <= 0) return 0;
    const int N = g_Count.load(std::memory_order_acquire);
    int Written = 0;
    for (int I = 0; I < N && Written < MaxOut; ++I) {
        Slot& S = g_Slots[I];
        ScopeStat St;
        St.Name    = S.Name.load(std::memory_order_relaxed);
        St.Count   = S.Count.load(std::memory_order_relaxed);
        St.TotalNs = S.TotalNs.load(std::memory_order_relaxed);
        St.MaxNs   = S.MaxNs.load(std::memory_order_relaxed);
        Out[Written++] = St;
    }
    return Written;
}

void Reset() {
    const int N = g_Count.load(std::memory_order_acquire);
    for (int I = 0; I < N; ++I) {
        g_Slots[I].TotalNs.store(0, std::memory_order_relaxed);
        g_Slots[I].Count.store(0, std::memory_order_relaxed);
        g_Slots[I].MaxNs.store(0, std::memory_order_relaxed);
    }
}

int FormatLineAndReset(char* Buf, int BufSize) {
    if (Buf == nullptr || BufSize <= 0) return 0;
    const int N = g_Count.load(std::memory_order_acquire);
    int Len = 0;
    Buf[0] = '\0';
    for (int I = 0; I < N; ++I) {
        Slot& S = g_Slots[I];
        const uint32_t C = S.Count.load(std::memory_order_relaxed);
        if (C == 0) continue;  // skip scopes with no samples this window
        const char*    Nm    = S.Name.load(std::memory_order_relaxed);
        const uint64_t Total = S.TotalNs.load(std::memory_order_relaxed);
        const uint64_t Max   = S.MaxNs.load(std::memory_order_relaxed);
        const double   AvgMs = (static_cast<double>(Total) / C) / 1.0e6;
        const double   MaxMs = static_cast<double>(Max) / 1.0e6;
        const int      Wrote = std::snprintf(Buf + Len, static_cast<size_t>(BufSize - Len),
                                             "%s%s=%.3f/%.3f", (Len > 0 ? " " : ""), Nm, AvgMs, MaxMs);
        if (Wrote <= 0 || Wrote >= BufSize - Len) {  // truncated — stop cleanly
            Buf[Len] = '\0';
            break;
        }
        Len += Wrote;
    }
    Reset();
    return Len;
}

}  // namespace Lur::Trace
