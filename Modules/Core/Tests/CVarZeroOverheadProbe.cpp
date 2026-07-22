// Representative "hot" tunable read, compiled two ways under SHIPPING codegen, to prove
// the zero-overhead contract (dev-console-cvar-tech-spec.md §1, §7): reading a CVar in a
// shipping build must emit machine code IDENTICAL to reading the old constexpr — the one
// `#if` inside CVar::Get() melts the class to its raw value, no registry/branch/wrapper.
//
// This file is NEVER part of the CMake tree (no add_executable references it). Instead
// scripts/check-cvar-zero-overhead.ps1 compiles it TWICE at -O2 -DLUR_SHIPPING=1 — once
// with -DUSE_CVAR=1 (the CVar path), once without (the plain constexpr path) — and diffs
// the disassembly of Hot(). Any divergence fails the build's test pass. Keep Hot()'s two
// definitions of Tune() value-identical, or the check would (correctly) fail.
#include "Lur/Core/CVar.h"

#ifdef USE_CVAR
LUR_CVAR(CvTune, "probe.tune", 42, 0);
static inline int Tune() { return CvTune.Get(); }
#else
static constexpr int Tune() { return 42; }
#endif

// A non-trivial expression (several reads + arithmetic) so the comparison is meaningful
// rather than a single folded load. extern "C" keeps the symbol name stable across both
// compiles so the diff targets the same `Hot`.
extern "C" int Hot(int X) {
    return X * Tune() + Tune() - (X >> 1) * Tune();
}
