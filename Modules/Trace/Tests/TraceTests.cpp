// Host unit tests for lur::trace (issue #101). Plain executable: returns non-zero on
// failure, registered with CTest. Built with LUR_TRACE=1 (Development), so the macros
// and registry are live.
#include "Lur/Trace/Trace.h"

#include <cstdio>
#include <cstring>
#include <thread>

using namespace Lur::Trace;

static int g_Failures = 0;
#define CHECK(Cond)                                                              \
    do {                                                                         \
        if (!(Cond)) {                                                           \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);          \
            ++g_Failures;                                                        \
        }                                                                        \
    } while (0)

static const ScopeStat* Find(const ScopeStat* Stats, int N, const char* Name) {
    for (int I = 0; I < N; ++I)
        if (Stats[I].Name != nullptr && std::strcmp(Stats[I].Name, Name) == 0) return &Stats[I];
    return nullptr;
}

int main() {
    Reset();

    // --- Registration is idempotent by name ---
    const ScopeId A1 = Register("sim.step");
    const ScopeId A2 = Register("sim.step");
    const ScopeId B  = Register("render.build");
    CHECK(A1 != InvalidScope);
    CHECK(A1 == A2);   // same name -> same id
    CHECK(B != A1);    // different name -> different id

    // --- Aggregation: count / total / max / avg ---
    AddSample(A1, 100);
    AddSample(A1, 300);   // avg 200, max 300, count 2
    AddSample(B, 1000);

    ScopeStat Stats[MaxScopes];
    int N = Snapshot(Stats, MaxScopes);
    CHECK(N >= 2);
    const ScopeStat* SA = Find(Stats, N, "sim.step");
    const ScopeStat* SB = Find(Stats, N, "render.build");
    CHECK(SA != nullptr && SB != nullptr);
    if (SA) {
        CHECK(SA->Count == 2);
        CHECK(SA->TotalNs == 400);
        CHECK(SA->MaxNs == 300);
        CHECK(SA->AvgNs() == 200.0);
    }
    if (SB) CHECK(SB->Count == 1 && SB->MaxNs == 1000);

    // --- RAII ScopeTimer records exactly one non-zero sample ---
    Reset();
    {
        LUR_TRACE_SCOPE("raii.block");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    N = Snapshot(Stats, MaxScopes);
    const ScopeStat* SR = Find(Stats, N, "raii.block");
    CHECK(SR != nullptr);
    if (SR) {
        CHECK(SR->Count == 1);
        CHECK(SR->MaxNs > 0);  // some time elapsed
    }

    // --- Latency records now - start ---
    Reset();
    const uint64_t Start = NowNs();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    LUR_TRACE_LATENCY("lat.span", Start);
    N = Snapshot(Stats, MaxScopes);
    const ScopeStat* SL = Find(Stats, N, "lat.span");
    CHECK(SL != nullptr && SL->Count == 1 && SL->MaxNs > 0);

    // --- FormatLineAndReset emits active scopes then zeroes them ---
    Reset();
    AddSample(A1, 2'000'000);   // 2.000 ms
    AddSample(A1, 4'000'000);   // -> avg 3.000 / max 4.000
    char Line[512];
    const int Len = FormatLineAndReset(Line, sizeof(Line));
    CHECK(Len > 0);
    CHECK(std::strstr(Line, "sim.step=3.000/4.000") != nullptr);
    // Reset happened: no samples remain.
    N = Snapshot(Stats, MaxScopes);
    SA = Find(Stats, N, "sim.step");
    CHECK(SA != nullptr && SA->Count == 0);

    // --- Empty window formats to an empty line ---
    Reset();
    char Empty[64];
    CHECK(FormatLineAndReset(Empty, sizeof(Empty)) == 0);
    CHECK(Empty[0] == '\0');

    if (g_Failures == 0) std::printf("lur::trace: all tests passed\n");
    return g_Failures == 0 ? 0 : 1;
}
