// Dependency-free unit tests for Lur::Sim (TickClock catch-up clamp + Fixed edges).
// No framework: each CHECK records a failure and the process exits non-zero if any
// failed, which CTest reports.
#include <cstdint>
#include <cstdio>

#include "Lur/Core/CVar.h"
#include "Lur/Sim/Fixed.h"
#include "Lur/Sim/FixedString.h"
#include "Lur/Sim/Tick.h"

using Lur::Sim::Fixed;
using Lur::Sim::TickClock;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// Steady real-time pacing: whole steps become ticks, remainder is kept for interp.
static void TestTickClockSteady() {
    TickClock C(100);                        // 10 ms per tick
    CHECK(C.Advance(25'000'000ull) == 2);    // 25 ms -> 2 ticks (5 ms remainder)
    CHECK(C.Advance(5'000'000ull) == 1);     // + 5 ms = 10 ms -> 1 tick
    CHECK(C.Advance(1'000'000ull) == 0);     // 1 ms -> no whole tick yet
}

// A huge elapsed time (a debugger pause / backgrounded app) is clamped to MaxCatchup
// and the backlog is discarded — no burst of hundreds of catch-up ticks.
static void TestTickClockCatchupClamp() {
    TickClock C(60);                                     // ~16.67 ms per tick
    const uint32_t Ticks = C.Advance(10'000'000'000ull); // 10 s -> would be ~600 ticks
    CHECK(Ticks == TickClock::MaxCatchup);               // clamped
    CHECK(C.GetInterpolationAlpha() < 1.0f);             // backlog dropped, remainder < a step
    // Not stuck: after the clamp, normal pacing resumes.
    CHECK(C.Advance(20'000'000ull) >= 1);
}

// Fixed: round-trips, arithmetic, and the divide-by-zero guard (saturates, no trap).
static void TestFixedEdges() {
    CHECK(Fixed::FromInt(5).ToInt() == 5);
    CHECK(Fixed::FromInt(-100).ToInt() == -100);              // negative round-trips
    CHECK((Fixed::FromInt(3) * Fixed::FromInt(4)).ToInt() == 12);
    CHECK((Fixed::FromInt(6) / Fixed::FromInt(2)).ToInt() == 3);
    CHECK((Fixed::FromInt(7) / Fixed{0}).Raw == 0);          // div-by-zero -> 0, not a CPU trap
}

// Fixed <-> decimal string: exact round-trip through the CVar/console codec. The
// generic bool/int overloads come from Lur::Core; the Fixed overload from Lur::Sim,
// selected by ADL (both surface through the CVar<Fixed> path).
static void TestFixedString() {
    using Lur::Sim::FromString;
    using Lur::Sim::ToString;

    Fixed V{};
    CHECK(FromString("0.7", V) && V.Raw == (7 << 16) / 10);   // matches a truncating F(7,10)
    CHECK(ToString(V) == "0.7");                              // shortest decimal, trimmed
    CHECK(FromString("6", V) && V == Fixed::FromInt(6) && ToString(V) == "6");
    CHECK(FromString(" -2.5 ", V) && ToString(V) == "-2.5");  // sign + trim
    CHECK(!FromString("1.2.3", V));                           // malformed
    CHECK(!FromString("", V));
    CHECK(!FromString("40000", V));                           // outside Q16.16 integer range

    // Round-trip a spread of raw values through decimal and back — must be identity.
    const int32_t Raws[] = {0, 1, -1, 45875, 65536, -65536, 100000, -100000, 32767 << 16};
    for (int32_t R : Raws) {
        Fixed Back{};
        CHECK(FromString(ToString(Fixed{R}).c_str(), Back) && Back.Raw == R);
    }
}

// CVar<Fixed>: a Sim-typed CVar parses/formats via the Fixed overload through ICVar.
// Default 0.5 (= One/2, an EXACT Q16.16 value) so ValueString is unambiguous — unlike
// an F(6,10) literal, which truncates to raw 39321 (0.59999) — a real gotcha the codec exposes.
LUR_CVAR(CvTestSpeed, "test.speed", Fixed{Fixed::One / 2}, ::Lur::Core::CVarFlagAffectsGameplay,
         "Test fixture: Fixed gameplay CVar");

static void TestCVarFixed() {
    CHECK(CvTestSpeed.Get() == Fixed{Fixed::One / 2});
    CHECK(CvTestSpeed.AffectsGameplay());
    CHECK(CvTestSpeed.ValueString() == "0.5");
    CHECK(CvTestSpeed.SetFromString("0.9") && CvTestSpeed.ValueString() == "0.9");
    CHECK(!CvTestSpeed.SetFromString("fast"));   // parse fail leaves it intact
    CHECK(CvTestSpeed.ValueString() == "0.9");
    CvTestSpeed.Reset();
    CHECK(CvTestSpeed.Get() == Fixed{Fixed::One / 2} && !CvTestSpeed.Overridden());
}

// ---- #201: F() / FRound(), promoted here from Rps/Tunables.h ----
// The reason FRound exists at all is a one-raw-unit bug (#155) that surfaced only as a StateHash
// difference between two phones, so the contract is pinned here rather than left to the games:
// F truncates, FRound rounds to nearest, and FRound must agree with what the decimal codec parses.
static void TestFixedBuilders() {
    using Lur::Sim::F;
    using Lur::Sim::FRound;
    CHECK(F(3).Raw == (3 << 16));
    CHECK(F(-2).Raw == -(2 << 16));
    CHECK(F(0).Raw == 0);

    // Exact division: the two agree.
    CHECK(F(1, 2).Raw == FRound(1, 2).Raw);
    CHECK(F(1, 2).Raw == (1 << 16) / 2);

    // Inexact: F truncates DOWN, FRound goes to nearest — and FRound is the one that round-trips
    // through FromString, which is the whole point.
    CHECK(F(1, 10).Raw == 6553);
    CHECK(FRound(1, 10).Raw == 6554);
    Lur::Sim::Fixed Parsed{};
    CHECK(Lur::Sim::FromString("0.1", Parsed));
    CHECK(Parsed.Raw == FRound(1, 10).Raw);   // #155: the property that was violated
    CHECK(Parsed.Raw != F(1, 10).Raw);        // and the one-unit gap that made it invisible

    // Negative rationals: FRound's `+ Den/2` must not turn -0.5 into a positive step.
    CHECK(F(-1, 10).Raw < 0 && FRound(-1, 10).Raw < 0);

    // constexpr, not just const — these are used as CVar defaults and array sizes.
    static_assert(F(2).Raw == (2 << 16), "F must be constexpr");
    static_assert(FRound(1, 10).Raw == F(1, 10).Raw + 1, "FRound must round up on 1/10");
}

int main() {
    Lur::Core::CVarEnterMain();

    TestTickClockSteady();
    TestTickClockCatchupClamp();
    TestFixedEdges();
    TestFixedString();
    TestCVarFixed();
    TestFixedBuilders();

    if (GFailures == 0) {
        std::printf("All sim tests passed.\n");
        return 0;
    }
    std::printf("%d sim test(s) failed.\n", GFailures);
    return 1;
}
