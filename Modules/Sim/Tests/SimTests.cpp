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
    CHECK(FromString("0.7", V) && V.Raw == (7 << 16) / 10);   // matches Rps::F(7,10)
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
// Rps::F(6,10) which truncates to raw 39321 (0.59999), a real gotcha the codec exposes.
LUR_CVAR(CvTestSpeed, "test.speed", Fixed{Fixed::One / 2}, ::Lur::Core::CVarFlagAffectsGameplay);

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

int main() {
    Lur::Core::CVarEnterMain();

    TestTickClockSteady();
    TestTickClockCatchupClamp();
    TestFixedEdges();
    TestFixedString();
    TestCVarFixed();

    if (GFailures == 0) {
        std::printf("All sim tests passed.\n");
        return 0;
    }
    std::printf("%d sim test(s) failed.\n", GFailures);
    return 1;
}
