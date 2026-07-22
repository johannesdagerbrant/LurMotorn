// Dependency-free unit tests for Modules/Core (the Lur::Log seam). No framework:
// each CHECK records a failure and the process exits non-zero if any failed.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <fstream>

#include "Lur/Core/CVar.h"
#include "Lur/Core/CVarConfig.h"
#include "Lur/Core/FlightRecorder.h"
#include "Lur/Core/FromString.h"
#include "Lur/Core/Hash.h"
#include "Lur/Core/Log.h"

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
LUR_CVAR(CvTestInt, "test.int", 7, ::Lur::Core::CVarFlagNone);
LUR_CVAR(CvTestBool, "test.bool", false, ::Lur::Core::CVarFlagNone);
LUR_CVAR(CvTestMode, "test.mode", ETestMode::Auto, ::Lur::Core::CVarFlagAffectsGameplay);

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
    TestHashDeterministicAndSensitive();
    TestFlightRecorderRoundtrip();
    TestFlightRecorderRingBounded();
    TestFromStringGeneric();
    TestCVarMechanism();
    TestCVarRegistry();
    TestCVarConfig();

    if (GFailures == 0) {
        std::printf("All core tests passed.\n");
        return 0;
    }
    std::printf("%d core test(s) failed.\n", GFailures);
    return 1;
}
