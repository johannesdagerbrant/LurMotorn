#pragma once
// Lur::Trace — named CPU scopes, Unreal-Insights / Handmade `TIMED_BLOCK` style
// (issue #101). One line instruments a code region; the tool reads the aggregate.
//
//   { LUR_TRACE_SCOPE("sim.step"); ... }          // duration of a block
//   LUR_TRACE_LATENCY("input.toApply", ingressNs) // elapsed since an external stamp
//
// Two primitives, one fixed-capacity registry. Per named scope we keep count, total,
// and max (avg = total/count), reset each emit window. `sim.step` and `input.toApply`
// both live here — a *scope* times a region it brackets; a *latency* measures the gap
// from a timestamp taken elsewhere (another thread / an inbound datagram) to now.
//
// DETERMINISM: the timers read a wall clock (NowNs) and write only to this side
// registry — the values are NEVER read back into sim state. A pure observer, so it
// cannot perturb the fixed-point lockstep. (This is exactly why a profiler is allowed
// inside a bit-deterministic sim: the measurement lives outside `Fixed`/sim state.)
//
// GATING: follows the capability macro LUR_TRACE (cmake/EngineFlags.cmake, from
// LUR_CONFIG): on in Development/Debugging, off in Shipping. Force with
// LUR_TRACE_ENABLED=1/0. Default OFF when neither is defined, so a stray host tool
// that never saw EngineFlags doesn't silently trace. When off, the macros expand to
// nothing (zero cost, zero footprint) — the "profiling = shipping + stats" off-ladder
// build the capability system was designed for is `-DLUR_CONFIG=Shipping -DLUR_TRACE=1`.
#include <cstdint>

#if !defined(LUR_TRACE_ENABLED)
    #if defined(LUR_TRACE)
        #define LUR_TRACE_ENABLED LUR_TRACE
    #else
        #define LUR_TRACE_ENABLED 0
    #endif
#endif

namespace Lur::Trace {

using ScopeId = uint16_t;
inline constexpr ScopeId InvalidScope = 0xFFFF;
inline constexpr int      MaxScopes   = 128;  // fixed capacity; no heap in the hot path

// steady_clock nanoseconds — the trace clock. Observational only, never sim time.
uint64_t NowNs();

// Register a named scope. Idempotent by name (same string -> same id), thread-safe.
// Called once per call-site via the macros' function-local static. Returns
// InvalidScope if MaxScopes is exhausted (AddSample then safely ignores it).
ScopeId Register(const char* Name);

// Add one nanosecond sample to a scope. Lock-free; safe on any thread.
void AddSample(ScopeId Id, uint64_t Ns);

struct ScopeStat {
    const char* Name    = nullptr;
    uint32_t    Count   = 0;
    uint64_t    TotalNs = 0;
    uint64_t    MaxNs   = 0;
    double AvgNs() const { return Count ? static_cast<double>(TotalNs) / Count : 0.0; }
};

// Copy the currently-registered scopes' aggregates into Out (up to MaxOut). Returns
// the number written. Does not reset.
int Snapshot(ScopeStat* Out, int MaxOut);

// Zero every accumulator (registrations are kept, ids stay stable).
void Reset();

// Format the active scopes as one compact line for the periodic log, then Reset():
//   "sim.step=1.800/4.200 render.build=2.100/6.000 input.toApply=12.000/48.000"
// (each token is name=avgMs/maxMs). Scopes with zero samples this window are skipped.
// Writes at most BufSize-1 chars + NUL; returns chars written (excluding NUL).
int FormatLineAndReset(char* Buf, int BufSize);

#if LUR_TRACE_ENABLED
class ScopeTimer {
public:
    explicit ScopeTimer(ScopeId Id) : Id_(Id), Start_(NowNs()) {}
    ~ScopeTimer() { AddSample(Id_, NowNs() - Start_); }
    ScopeTimer(const ScopeTimer&)            = delete;
    ScopeTimer& operator=(const ScopeTimer&) = delete;
private:
    ScopeId  Id_;
    uint64_t Start_;
};
#endif

}  // namespace Lur::Trace

#if LUR_TRACE_ENABLED
    #define LUR_TRACE_CONCAT_(a, b) a##b
    #define LUR_TRACE_CONCAT(a, b)  LUR_TRACE_CONCAT_(a, b)

    // Bind the scope id ONCE (function-local static -> registered on first hit, like
    // SCOPE_CYCLE_COUNTER), so the hot path is just NowNs() x2 + an atomic add.
    #define LUR_TRACE_SCOPE(Name) LUR_TRACE_SCOPE_(Name, __COUNTER__)
    #define LUR_TRACE_SCOPE_(Name, Ctr)                                                 \
        static const ::Lur::Trace::ScopeId LUR_TRACE_CONCAT(lurTraceId_, Ctr) =          \
            ::Lur::Trace::Register(Name);                                                \
        ::Lur::Trace::ScopeTimer LUR_TRACE_CONCAT(lurTraceScope_, Ctr)(                  \
            LUR_TRACE_CONCAT(lurTraceId_, Ctr))

    #define LUR_TRACE_LATENCY(Name, StartNs) LUR_TRACE_LATENCY_(Name, StartNs, __COUNTER__)
    #define LUR_TRACE_LATENCY_(Name, StartNs, Ctr)                                      \
        do {                                                                            \
            static const ::Lur::Trace::ScopeId LUR_TRACE_CONCAT(lurLatId_, Ctr) =        \
                ::Lur::Trace::Register(Name);                                            \
            ::Lur::Trace::AddSample(LUR_TRACE_CONCAT(lurLatId_, Ctr),                    \
                                    ::Lur::Trace::NowNs() - (StartNs));                  \
        } while (0)
#else
    #define LUR_TRACE_SCOPE(Name)            do {} while (0)
    #define LUR_TRACE_LATENCY(Name, StartNs) do { (void)sizeof(StartNs); } while (0)
#endif
