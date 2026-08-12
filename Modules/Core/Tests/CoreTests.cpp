// Dependency-free unit tests for Modules/Core (the Lur::Log seam). No framework:
// each CHECK records a failure and the process exits non-zero if any failed.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fstream>

#include "Lur/Core/Assert.h"
#include "Lur/Core/CVar.h"
#include "Lur/Core/CVarConfig.h"
#include "Lur/Core/DevCommand.h"
#include "Lur/Core/FlightRecorder.h"
#include "Lur/Core/FromString.h"
#include "Lur/Core/Hash.h"
#include "Lur/Core/Log.h"
// Nudge's Fixed step is the one branch Core alone can't reach. FixedString.h supplies the
// ADL FromString/ToString overloads CVar<Fixed> needs — Fixed.h alone leaves it unprintable.
#include "Lur/Sim/Fixed.h"
#include "Lur/Sim/FixedString.h"
// #116: CVar<Color> — the ADL overloads live in Lur::Render, test-only here as with Fixed.
#include "Lur/Render/ColorString.h"

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

namespace {
std::string GLast;
bool        GLastError = false;
int         GCalls = 0;
void CaptureSink(bool Error, const char* Line, void* User) {
    *static_cast<int*>(User) += 1;  // user pointer is threaded through
    GLast = Line;
    GLastError = Error;
    ++GCalls;
}
}  // namespace

// Info/Error format their args and route to the installed sink, with the error flag
// and the user pointer threaded through.
static void TestLogRoutesToSink() {
    int UserCounter = 0;
    Lur::Log::Init(&CaptureSink, "Test", &UserCounter);

    Lur::Log::Info("hello %d + %d = %d", 2, 3, 5);
    CHECK(GCalls == 1);
    CHECK(!GLastError);
    CHECK(GLast == "hello 2 + 3 = 5");

    Lur::Log::Error("bad: %s", "oops");
    CHECK(GCalls == 2);
    CHECK(GLastError);
    CHECK(GLast == "bad: oops");
    CHECK(UserCounter == 2);  // the void* user was passed to the sink each time

    Lur::Log::Init(nullptr, "Lur");  // restore the default writer
}

// A failed assert's MESSAGE must reach the installed sink, not stderr.
//
// This is not a style point. On a phone stderr is nowhere — nothing drains a sideloaded app's
// stdio, and on iOS Lur::App::Platform::UnblockStdio deliberately makes those writes DROP rather
// than wedge the app (a blocking write to a full pipe once got the process killed with
// 0x8BADF00D). So an assert that printed to stderr trapped silently on device, leaving only a
// backtrace: "deafening in Development" was true on the desktop and false where it mattered. A
// 2026-07-20 plan even listed `logcat | grep "LUR_ASSERT failed:"` as its evidence step for
// classifying crashes — a grep that could never have matched.
//
// Report() is called directly, without the trap: the macro would take the process down, and what
// needs proving is the ROUTING, which is the half that was broken.
//
// Guarded because Report itself is compiled out with the asserts (a Shipping test build) — the
// same capability gate the macro keys on, never the config name.
#if LUR_ASSERTS_ENABLED
static void TestAssertMessageRoutesToSink() {
    int UserCounter = 0;
    GCalls = 0;
    Lur::Log::Init(&CaptureSink, "Test", &UserCounter);

    Lur::Assert::Detail::Report("X == 1", "Some/File.cpp", 42, "context %d", 7);
    CHECK(GCalls == 1);
    CHECK(GLastError);  // an assert is an error, so it must take the sink's error path
    // The expression, the file:line and the message all survive, on ONE line — os_log and logcat
    // are line-oriented, and reading device logs means grepping a filtered capture.
    CHECK(GLast.find("X == 1") != std::string::npos);
    CHECK(GLast.find("Some/File.cpp:42") != std::string::npos);
    CHECK(GLast.find("context 7") != std::string::npos);
    CHECK(GLast.find('\n') == std::string::npos);

    Lur::Log::Init(nullptr, "Lur");
}
#endif

// FNV-1a is deterministic and sensitive to any byte change (the desync-hash property).
static void TestHashDeterministicAndSensitive() {
    const uint8_t A[] = {1, 2, 3, 4, 5};
    uint8_t B[] = {1, 2, 3, 4, 5};
    CHECK(Lur::Core::Fnv1a64(A, sizeof(A)) == Lur::Core::Fnv1a64(B, sizeof(B)));
    B[2] = 0x33;  // flip one byte
    CHECK(Lur::Core::Fnv1a64(A, sizeof(A)) != Lur::Core::Fnv1a64(B, sizeof(B)));
}

// A recording serializes and parses back byte-identically (kind, time, payload).
static void TestFlightRecorderRoundtrip() {
    using Lur::Core::EFlightEvent;
    Lur::Core::FlightRecorder Rec;
    const uint8_t D1[] = {0xAB};
    const uint8_t D2[] = {0x01, 0x02, 0x03};
    Rec.Record(EFlightEvent::LinkUp, 100, nullptr, 0);
    Rec.Record(EFlightEvent::DatagramIn, 200, D1, sizeof(D1));
    Rec.Record(EFlightEvent::DatagramOut, 300, D2, sizeof(D2));

    const std::vector<uint8_t> Blob = Rec.Serialize();
    std::vector<Lur::Core::FlightRecorder::Event> Back;
    CHECK(Lur::Core::FlightRecorder::Parse(Blob.data(), Blob.size(), Back));
    CHECK(Back.size() == 3);
    CHECK(Back[0].Kind == EFlightEvent::LinkUp && Back[0].TimeNs == 100 && Back[0].Data.empty());
    CHECK(Back[1].Kind == EFlightEvent::DatagramIn && Back[1].TimeNs == 200);
    CHECK(Back[1].Data.size() == 1 && Back[1].Data[0] == 0xAB);
    CHECK(Back[2].Data.size() == 3 && Back[2].Data[2] == 0x03);

    // A truncated blob is rejected, not read as garbage.
    std::vector<Lur::Core::FlightRecorder::Event> Junk;
    CHECK(!Lur::Core::FlightRecorder::Parse(Blob.data(), Blob.size() - 1, Junk));
}

// The ring is bounded: past capacity it drops the oldest and flags it.
static void TestFlightRecorderRingBounded() {
    Lur::Core::FlightRecorder Rec(/*Capacity*/ 4);
    for (int i = 0; i < 10; ++i) {
        const uint8_t B = static_cast<uint8_t>(i);
        Rec.Record(Lur::Core::EFlightEvent::Input, static_cast<uint64_t>(i), &B, 1);
    }
    CHECK(Rec.Count() == 4);
    CHECK(Rec.Dropped());
    CHECK(Rec.Events().front().Data[0] == 6);  // oldest kept is #6 (0..5 dropped)
    CHECK(Rec.Events().back().Data[0] == 9);
}

// ---- FromString / ToString (the generic bool/int/float/enum codec) ----
enum class ETestMode : uint8_t { Off = 0, On = 1, Auto = 2 };

static void TestFromStringGeneric() {
    using Lur::Core::FromString;
    using Lur::Core::ToString;

    bool B = false;
    CHECK(FromString("true", B) && B);
    CHECK(FromString(" yes ", B) && B);        // trimmed
    CHECK(FromString("0", B) && !B);
    CHECK(!FromString("maybe", B));            // malformed -> false, B untouched

    int32_t I = 0;
    CHECK(FromString("42", I) && I == 42);
    CHECK(FromString("  -7 ", I) && I == -7);
    CHECK(!FromString("3x", I));               // trailing junk
    CHECK(!FromString("", I));                 // empty
    CHECK(ToString(I) == "-7");

    int8_t Small = 0;
    CHECK(FromString("120", Small) && Small == 120);
    CHECK(!FromString("200", Small));          // out of int8 range

    float F = 0.0f;
    CHECK(FromString("1.5", F) && F == 1.5f);
    CHECK(!FromString("abc", F));

    ETestMode M = ETestMode::Off;
    CHECK(FromString("2", M) && M == ETestMode::Auto);   // enum by ordinal
    CHECK(ToString(ETestMode::On) == "1");
}

// ---- CVar<T>: default/get/set/reset/overridden + registry (dev shape) ----
LUR_CVAR(CvTestInt, "test.int", 7, ::Lur::Core::CVarFlagNone, "Test fixture: int CVar");
LUR_CVAR(CvTestBool, "test.bool", false, ::Lur::Core::CVarFlagNone, "Test fixture: bool CVar");
LUR_CVAR(CvTestMode, "test.mode", ETestMode::Auto, ::Lur::Core::CVarFlagAffectsGameplay,
         "Test fixture: enum CVar");

// ---- #119: arrow-key scrubbing. The step size is per-type and chosen inside CVar<T>,
//      because ICVar is type-erased and the console has no idea what T is. ----
LUR_CVAR(CvNudgeInt, "nudge.int", 5, ::Lur::Core::CVarFlagNone, "Test fixture: int nudge");
LUR_CVAR(CvNudgeBool, "nudge.bool", false, ::Lur::Core::CVarFlagNone,
         "Test fixture: bool nudge");
LUR_CVAR(CvNudgeFixed, "nudge.fixed", ::Lur::Sim::Fixed::FromInt(2),
         ::Lur::Core::CVarFlagAffectsGameplay, "Test fixture: Fixed nudge");
LUR_CVAR(CvNudgeFloat, "nudge.float", 1.0f, ::Lur::Core::CVarFlagNone,
         "Test fixture: float nudge");

static void TestCVarNudge() {
    // int: one unit per step, scaling linearly with the step count.
    CHECK(CvNudgeInt.Nudge(+1) && CvNudgeInt.Get() == 6);
    CHECK(CvNudgeInt.Nudge(-1) && CvNudgeInt.Get() == 5);
    CHECK(CvNudgeInt.Nudge(+4) && CvNudgeInt.Get() == 9);
    CvNudgeInt.Reset();

    // Zero steps succeeds and changes nothing — it must NOT mark the CVar overridden, or a
    // stray key event would light the console's "R" button on an untouched knob.
    CHECK(CvNudgeInt.Nudge(0) && CvNudgeInt.Get() == 5 && !CvNudgeInt.Overridden());

    // bool FLIPS. This is the case the ordering inside Nudge exists for: bool is integral, so
    // an "increment" would reach true and then stay there, and Down would never turn it off.
    CHECK(CvNudgeBool.Nudge(+1) && CvNudgeBool.Get() == true);
    CHECK(CvNudgeBool.Nudge(+1) && CvNudgeBool.Get() == false);
    CHECK(CvNudgeBool.Nudge(-1) && CvNudgeBool.Get() == true);  // a toggle ignores direction
    CvNudgeBool.Reset();

    // Fixed: 1/64 of a unit, asserted in RAW units so the check can't be satisfied by a
    // float-rounded near-miss. This is the type the balance knobs actually use.
    const int32_t Step = ::Lur::Sim::Fixed::One / 64;
    const int32_t Base = ::Lur::Sim::Fixed::FromInt(2).Raw;
    CHECK(CvNudgeFixed.Nudge(+1) && CvNudgeFixed.Get().Raw == Base + Step);
    CHECK(CvNudgeFixed.Nudge(-2) && CvNudgeFixed.Get().Raw == Base - Step);
    CvNudgeFixed.Reset();

    // float: a 0.01 step, sized for the 0..1 knobs (theme colours, non-gameplay).
    CHECK(CvNudgeFloat.Nudge(+1));
    CHECK(CvNudgeFloat.Get() > 1.009f && CvNudgeFloat.Get() < 1.011f);
    CvNudgeFloat.Reset();

    // enum: DECLINED, value untouched. This layer cannot know the valid range, so scrubbing
    // off the end would produce a value that is not a member of the enumeration.
    const ETestMode Before = CvTestMode.Get();
    CHECK(!CvTestMode.Nudge(+1));
    CHECK(CvTestMode.Get() == Before);

    // A real nudge marks the CVar overridden — that is what lights the console's reset button.
    CHECK(CvNudgeInt.Nudge(+1) && CvNudgeInt.Overridden());
    CvNudgeInt.Reset();
    CHECK(!CvNudgeInt.Overridden());
}

// ---- #116: a declared [Min,Max] is ENFORCED on every commit path, not just advisory ----
LUR_CVAR_RANGE(CvRangedInt, "range.int", 5, 0, 10, ::Lur::Core::CVarFlagNone,
               "Test fixture: ranged int");
LUR_CVAR_RANGE(CvRangedFixed, "range.fixed", ::Lur::Sim::Fixed::FromInt(1),
               ::Lur::Sim::Fixed::FromInt(0), ::Lur::Sim::Fixed::FromInt(2),
               ::Lur::Core::CVarFlagAffectsGameplay, "Test fixture: ranged Fixed");

static void TestCVarRange() {
    CHECK(CvRangedInt.HasRange());
    CHECK(!CvTestInt.HasRange());   // unranged is the default and stays that way
    CHECK(CvRangedInt.RangeMinF() == 0.0f && CvRangedInt.RangeMaxF() == 10.0f);

    // Typed input clamps rather than being rejected: a tuner entering 1000 wants "as high as it
    // goes". SetFromString still returns TRUE — the value was accepted, just bounded.
    CHECK(CvRangedInt.SetFromString("1000"));
    CHECK(CvRangedInt.Get() == 10);
    CHECK(CvRangedInt.SetFromString("-40"));
    CHECK(CvRangedInt.Get() == 0);
    CHECK(CvRangedInt.SetFromString("7") && CvRangedInt.Get() == 7);
    CvRangedInt.Reset();

    // The keyboard scrub (#119) goes through the same clamp — walking Up off the top must stop
    // at the bound, not run away.
    CvRangedInt.Set(9);
    CHECK(CvRangedInt.Nudge(+5) && CvRangedInt.Get() == 10);
    CHECK(CvRangedInt.Nudge(+1) && CvRangedInt.Get() == 10);   // already at the top: no-op
    CvRangedInt.Set(1);
    CHECK(CvRangedInt.Nudge(-9) && CvRangedInt.Get() == 0);
    CvRangedInt.Reset();

    // Set() clamps too, so no code path can plant an out-of-range value behind the UI's back.
    CvRangedInt.Set(99);
    CHECK(CvRangedInt.Get() == 10);
    CvRangedInt.Reset();

    // Fixed ranges work through the same operator< path, and ValueF divides out the Q16.16
    // scale so a slider sees 0.75 rather than 49152.
    CHECK(CvRangedFixed.HasRange());
    CHECK(CvRangedFixed.RangeMaxF() == 2.0f);
    CHECK(CvRangedFixed.SetFromString("9.5"));
    CHECK(CvRangedFixed.Get() == ::Lur::Sim::Fixed::FromInt(2));
    CHECK(CvRangedFixed.SetFromString("0.75"));
    CHECK(CvRangedFixed.ValueF() > 0.749f && CvRangedFixed.ValueF() < 0.751f);
    CvRangedFixed.Reset();
}

// ---- #116: CVar<Color>. Found by ADL in Lur::Render, exactly like Fixed's overloads, so
//      Modules/Core still depends on neither Render nor Sim. ----
LUR_CVAR(CvTestColor, "color.tint", (::Lur::Render::Color{0.5f, 0.25f, 0.125f, 1.0f}),
         ::Lur::Core::CVarFlagNone, "Test fixture: colour CVar");

static void TestCVarColor() {
    using ::Lur::Render::Color;
    CHECK(!CvTestColor.Overridden());
    CHECK(CvTestColor.ValueString() == "0.5 0.25 0.125 1");

    CHECK(CvTestColor.SetFromString("1 0 0 1"));
    CHECK(CvTestColor.Get().R == 1.0f && CvTestColor.Get().G == 0.0f);
    CHECK(CvTestColor.Overridden());

    // Alpha is optional and defaults to 1 — most tuned colours are opaque, and typing the
    // trailing 1 every time is friction.
    CHECK(CvTestColor.SetFromString("0.2 0.4 0.6"));
    CHECK(CvTestColor.Get().A == 1.0f && CvTestColor.Get().B == 0.6f);

    // A malformed component leaves the value ENTIRELY untouched — a typo must not half-apply a
    // colour (three channels set, one stale), which is the failure that would look like a
    // rendering bug rather than a parse error.
    const Color Before = CvTestColor.Get();
    CHECK(!CvTestColor.SetFromString("0.1 nope 0.3"));
    CHECK(CvTestColor.Get() == Before);
    CHECK(!CvTestColor.SetFromString("0.1 0.2"));           // too few components
    CHECK(!CvTestColor.SetFromString("0.1 0.2 0.3 0.4 0.5"));  // too many
    CHECK(CvTestColor.Get() == Before);

    // Not clamped on parse: a tint used as a multiplier can legitimately exceed 1.
    CHECK(CvTestColor.SetFromString("2 0 0 1") && CvTestColor.Get().R == 2.0f);

    // A colour has no single scalar and no ordering, so it offers neither a range nor a nudge.
    CHECK(!CvTestColor.HasRange());
    CHECK(!CvTestColor.Nudge(+1));
    CHECK(!CvTestColor.IsBool());

    // Channel helpers: the console's `.r/.g/.b/.a` and the picker's four sliders must agree
    // about which letter is which slot.
    CHECK(::Lur::Render::ColorChannelIndex('r') == 0);
    CHECK(::Lur::Render::ColorChannelIndex('A') == 3);
    CHECK(::Lur::Render::ColorChannelIndex('x') == -1);
    Color C{0.1f, 0.2f, 0.3f, 0.4f};
    CHECK(::Lur::Render::GetColorChannel(C, 2) == 0.3f);
    ::Lur::Render::SetColorChannel(C, 1, 0.9f);
    CHECK(C.G == 0.9f);

    CvTestColor.Reset();
    CHECK(!CvTestColor.Overridden());
}

// ---- #116: dev commands. The registry had ZERO entries before this, so "commands as buttons"
//      would have rendered an empty strip and proved nothing. ----
static void TestDevCommands() {
    using Lur::Core::DevCommandRegistry;
    CHECK(DevCommandRegistry::Find("dev.reset_cvars") != nullptr);
    CHECK(DevCommandRegistry::Find("dev.list_overrides") != nullptr);
    CHECK(DevCommandRegistry::Find("dev.nope") == nullptr);

    CvTestInt.Set(4242);
    CHECK(CvTestInt.Overridden());

    std::string Out;
    CHECK(DevCommandRegistry::Dispatch("dev.list_overrides", Out));
    CHECK(Out.find("test.int") != std::string::npos);
    CHECK(Out.find("4242") != std::string::npos);
    CHECK(Out.find("(default 7)") != std::string::npos);   // both values, so the report is usable

    Out.clear();
    CHECK(DevCommandRegistry::Dispatch("dev.reset_cvars", Out));
    CHECK(!CvTestInt.Overridden() && CvTestInt.Get() == 7);
    CHECK(Out.find("reset") != std::string::npos);

    // A second reset finds nothing left to do and says so rather than lying about a count.
    Out.clear();
    CHECK(DevCommandRegistry::Dispatch("dev.reset_cvars", Out));
    CHECK(Out.find("reset 0") != std::string::npos);
    Out.clear();
    CHECK(DevCommandRegistry::Dispatch("dev.list_overrides", Out));
    CHECK(Out.find("no cvars overridden") != std::string::npos);

    // An unknown name is declined (false, no output) so a caller can try other interpretations.
    Out.clear();
    CHECK(!DevCommandRegistry::Dispatch("not.a.command", Out));
    CHECK(Out.empty());

    // Every registered command carries a category, which is what groups its button.
    DevCommandRegistry::ForEach([&](Lur::Core::DevCommand* C) {
        CHECK(C->Category()[0] != '\0');
        CHECK(C->Help()[0] != '\0');
    });
}

static void TestCVarMechanism() {
    CHECK(CvTestInt.Get() == 7);
    CHECK(int(CvTestInt) == 7);                // operator T
    CHECK(!CvTestInt.Overridden());

    CHECK(CvTestInt.SetFromString("10"));
    CHECK(CvTestInt.Get() == 10 && CvTestInt.Overridden());
    CHECK(CvTestInt.ValueString() == "10" && CvTestInt.DefaultString() == "7");

    CHECK(!CvTestInt.SetFromString("garbage"));  // parse fail leaves value intact
    CHECK(CvTestInt.Get() == 10);
    CvTestInt.Reset();
    CHECK(CvTestInt.Get() == 7 && !CvTestInt.Overridden());

    // Flags: an enum CVar tagged AffectsGameplay reports it; a plain one does not.
    CHECK(CvTestMode.AffectsGameplay());
    CHECK(!CvTestBool.AffectsGameplay());
    CHECK(CvTestMode.Get() == ETestMode::Auto);
}

static void TestCVarRegistry() {
    using Lur::Core::CVarRegistry;
    Lur::Core::ICVar* Found = CVarRegistry::Find("test.int");
    CHECK(Found != nullptr);
    CHECK(Found && Found->SetFromString("99"));
    CHECK(CvTestInt.Get() == 99);              // registry set reaches the typed CVar
    CvTestInt.Reset();
    CHECK(CVarRegistry::Find("does.not.exist") == nullptr);

    // Every one of our three test CVars is enumerable.
    int Seen = 0;
    CVarRegistry::ForEach([&](Lur::Core::ICVar* C) {
        if (std::strncmp(C->Name(), "test.", 5) == 0) ++Seen;
    });
    CHECK(Seen == 3);
}

// ---- cvars.cfg: human-readable persistence round-trip (Addendum B) ----
static void TestCVarConfig() {
    const char* Path = "test_cvars.cfg";
    std::remove(Path);

    CvTestInt.Set(55);
    CvTestInt.SetEditWallMs(123456);  // C.4: the edit-timestamp column must round-trip
    CHECK(CvTestBool.SetFromString("true"));
    CHECK(Lur::Core::SaveCVarConfig(Path));

    // Wipe in memory, then reload from disk.
    CvTestInt.Reset();
    CvTestInt.SetEditWallMs(0);
    CHECK(CvTestBool.SetFromString("false"));
    CHECK(Lur::Core::LoadCVarConfig(Path) == 2);
    CHECK(CvTestInt.Get() == 55 && CvTestBool.Get() == true);
    CHECK(CvTestInt.EditWallMs() == 123456);  // timestamp survived the round-trip

    // A stale/unknown name (renamed or removed CVar) is warned + skipped, not fatal.
    { std::ofstream A(Path, std::ios::app); A << "no.such.cvar = 3\n"; }
    CHECK(Lur::Core::LoadCVarConfig(Path) == 2);  // still just our two resolve

    // reset_all clears every override; nothing persists (reloading applies 0). We assert
    // the invariant rather than file-absence: std::remove succeeds but this toolchain's
    // ifstream::good() is unreliable immediately after a delete.
    Lur::Core::ResetAllCVars(Path);
    CHECK(!CvTestInt.Overridden() && !CvTestBool.Overridden());
    CHECK(Lur::Core::LoadCVarConfig(Path) == 0);
    std::remove(Path);
}

int main() {
    Lur::Core::CVarEnterMain();  // CVars may not be read before main() (spec §1.1)

    TestLogRoutesToSink();
#if LUR_ASSERTS_ENABLED
    TestAssertMessageRoutesToSink();
#endif
    TestHashDeterministicAndSensitive();
    TestFlightRecorderRoundtrip();
    TestFlightRecorderRingBounded();
    TestFromStringGeneric();
    TestCVarMechanism();
    TestCVarNudge();
    TestCVarRange();
    TestCVarColor();
    TestDevCommands();
    TestCVarRegistry();
    TestCVarConfig();

    if (GFailures == 0) {
        std::printf("All core tests passed.\n");
        return 0;
    }
    std::printf("%d core test(s) failed.\n", GFailures);
    return 1;
}
