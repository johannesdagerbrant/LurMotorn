// The 16 tunables promoted from the Galaxy's persisted overrides to compiled defaults (2026-08-22,
// owner's call: "android's cvar overrides should be the new defaults").
//
// ---- WHY THIS TEST EXISTS ----
// The overrides lived in rps-cvars.cfg as DECIMAL TEXT, and the compiled defaults are Fixed literals.
// Those two go through different arithmetic:
//
//   * the .cfg parser (Lur::Sim::FromString) ROUNDS to nearest: (frac * 65536 + den/2) / den
//   * F(Num, Den) TRUNCATES:                                     (Num << 16) / Den
//
// So "0.6" parses to raw 39322 while F(3, 5) is 39321. Baking a decimal with F instead of FRound
// therefore shifts the value by one raw unit — invisible in the source, and a genuine change to a
// deterministic sim's behaviour. That is exactly the class of silent difference this codebase keeps
// getting bitten by, so the mapping is asserted mechanically here rather than trusted to arithmetic
// done by hand.
//
// Each case pins: the compiled default == FromString(<the exact text that was in the .cfg>).
// If anyone re-tunes one of these knobs, the corresponding line here fails and has to be updated
// deliberately — which is the point. It is not asserting that these are GOOD values, only that the
// baked constant is the same number the phone was actually running.
#include <cstdio>

#include "Lur/Core/CVar.h"
#include "Lur/Sim/FixedString.h"
#include "Rps/Tunables.h"

using Lur::Sim::Fixed;
using Lur::Sim::FromString;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

// The compiled default must equal what the .cfg text parsed to, RAW for RAW.
static void ExpectFixed(const char* Name, Fixed Compiled, const char* CfgText) {
    Fixed Parsed{0};
    if (!FromString(CfgText, Parsed)) {
        std::printf("FAIL %s: cfg text '%s' does not parse\n", Name, CfgText);
        ++GFailures;
        return;
    }
    if (Compiled.Raw != Parsed.Raw) {
        std::printf("FAIL %s: compiled raw %d != cfg '%s' raw %d (off by %d)\n", Name, Compiled.Raw,
                    CfgText, Parsed.Raw, Compiled.Raw - Parsed.Raw);
        ++GFailures;
    }
}

static void ExpectInt(const char* Name, int32_t Compiled, int32_t Cfg) {
    if (Compiled != Cfg) {
        std::printf("FAIL %s: compiled %d != cfg %d\n", Name, Compiled, Cfg);
        ++GFailures;
    }
}

// ---- The DECIMAL ones: these are the cases where F vs FRound actually differs ----
static void TestDecimalDefaultsMatchTheCfgText() {
    ExpectFixed("rps.unit.paper.speed",       Rps::CvPaperSpeed.Get(),      "0.6");
    ExpectFixed("rps.unit.scissor.speed",     Rps::CvScissorSpeed.Get(),    "0.65");
    ExpectFixed("rps.boid.w_predator_flee",   Rps::CvWPredatorFlee.Get(),   "0.9");
    ExpectFixed("rps.boid.flock_damping",     Rps::CvFlockDamping.Get(),    "0.95");
    ExpectFixed("rps.boid.inrange_damping",   Rps::CvInRangeDamping.Get(),  "0.7");
}

// ---- The WHOLE-NUMBER Fixed ones: F is exact here, but pin them anyway ----
// A whole number cannot be off by a rounding unit, so what these guard is a plain transcription slip
// — the far likelier error in a batch of sixteen.
static void TestWholeNumberFixedDefaults() {
    ExpectFixed("rps.build.repel_radius",     Rps::CvBuildingRepelRadius.Get(), "3");
    ExpectFixed("rps.boid.sep_radius",        Rps::CvSepRadius.Get(),           "5");
    ExpectFixed("rps.boid.sep_strength",      Rps::CvSeparationStrength.Get(),  "1");
    ExpectFixed("rps.boid.coh_same_radius",   Rps::CvCohSameRadius.Get(),       "8");
    ExpectFixed("rps.boid.coh_all_radius",    Rps::CvCohAllRadius.Get(),        "4");
    ExpectFixed("rps.boid.align_radius",      Rps::CvAlignRadius.Get(),         "15");
    ExpectFixed("rps.boid.interpose_radius",  Rps::CvInterposeRadius.Get(),     "3");
}

// ---- The INTEGER cvars ----
static void TestIntegerDefaults() {
    ExpectInt("rps.unit.rock.build_time",    Rps::CvRockBuild.Get(),    10);
    ExpectInt("rps.unit.scissor.build_time", Rps::CvScissorBuild.Get(), 10);
    ExpectInt("rps.base.home_hp",            Rps::CvHomeBaseHp.Get(),   2000);
}

// ---- The trap itself, asserted directly ----
// If F and FRound ever stop differing on these inputs, the reasoning above is void and this test's
// whole premise needs re-checking. Cheaper to state it than to rediscover it.
static void TestFAndFRoundReallyDoDiffer() {
    using Lur::Sim::F;
    using Lur::Sim::FRound;
    CHECK(F(3, 5).Raw == 39321);        // truncates 39321.6
    CHECK(FRound(3, 5).Raw == 39322);   // rounds it
    CHECK(F(3, 5).Raw != FRound(3, 5).Raw);
    Fixed P{0};
    CHECK(FromString("0.6", P));
    CHECK(P.Raw == FRound(3, 5).Raw);   // the parser agrees with FRound, NOT with F
}

int main() {
    Lur::Core::CVarEnterMain();  // CVars may not be read before main() (spec §1.1)
    TestDecimalDefaultsMatchTheCfgText();
    TestWholeNumberFixedDefaults();
    TestIntegerDefaults();
    TestFAndFRoundReallyDoDiffer();
    if (GFailures == 0) std::printf("rps_baked_defaults_tests: ALL PASS\n");
    else std::printf("rps_baked_defaults_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
