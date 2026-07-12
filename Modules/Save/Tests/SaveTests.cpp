// Dependency-free unit tests for Lur::Save (issue #17, Phase A): the key -> blob
// Store (round-trip, absent-key, overwrite, atomicity) and LoadOrCreateDeviceId
// (generated once, then stable). No framework: each CHECK records a failure and the
// process exits non-zero if any failed, which CTest reports. Mirrors net_tests.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "Lur/Save/DeviceId.h"
#include "Lur/Save/Store.h"

using Lur::Save::LoadOrCreateDeviceId;
using Lur::Save::Store;
namespace fs = std::filesystem;

static int GFailures = 0;

#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

static std::vector<uint8_t> Bytes(std::initializer_list<uint8_t> Init) {
    return std::vector<uint8_t>(Init);
}

// A fresh, empty scratch directory unique to this run (removed if it lingered from
// a previous run), so tests never see stale state and never touch the repo.
static std::string ScratchDir() {
    const fs::path Dir = fs::temp_directory_path() / "lur_save_tests";
    std::error_code Ec;
    fs::remove_all(Dir, Ec);
    return Dir.string();
}

// Save then Load returns the identical bytes; a never-written key returns empty.
static void TestRoundTripAndAbsent() {
    Store S(ScratchDir());
    CHECK(S.Load("missing").empty());  // absent -> empty, before anything is written

    const std::vector<uint8_t> Payload = Bytes({0x00, 0x01, 0xFF, 0x7F, 0x00});  // embedded NUL/high bytes
    CHECK(S.Save("blob", Payload.data(), Payload.size()));
    CHECK(S.Load("blob") == Payload);
    CHECK(S.Load("other").empty());  // a different key is still absent
}

// Saving the same key again replaces the value; the store never appends or tears.
static void TestOverwrite() {
    Store S(ScratchDir());
    const std::vector<uint8_t> First  = Bytes({1, 2, 3, 4, 5, 6});
    const std::vector<uint8_t> Second = Bytes({9, 9});  // shorter, to catch leftover tail bytes
    CHECK(S.Save("k", First.data(), First.size()));
    CHECK(S.Save("k", Second.data(), Second.size()));
    CHECK(S.Load("k") == Second);
}

// Values persist across Store instances pointed at the same directory (this is what
// "survives an app restart" reduces to on the host).
static void TestPersistsAcrossInstances() {
    const std::string Dir = ScratchDir();
    const std::vector<uint8_t> Payload = Bytes({0xDE, 0xAD, 0xBE, 0xEF});
    { Store Writer(Dir); CHECK(Writer.Save("x", Payload.data(), Payload.size())); }
    { Store Reader(Dir); CHECK(Reader.Load("x") == Payload); }
}

// Keys with filesystem-unsafe characters are handled (escaped, not rejected) and do
// not collide with a differently-unsafe key.
static void TestUnsafeKeys() {
    Store S(ScratchDir());
    const std::vector<uint8_t> A = Bytes({0xAA});
    const std::vector<uint8_t> B = Bytes({0xBB});
    CHECK(S.Save("a/b:c", A.data(), A.size()));      // slash + colon
    CHECK(S.Save("a\\b*c", B.data(), B.size()));     // backslash + star (distinct key)
    CHECK(S.Load("a/b:c") == A);
    CHECK(S.Load("a\\b*c") == B);                    // no collision between the two
}

// The device id is created on first call and identical on every later call — the
// stability the stable-BLE-role fix depends on.
static void TestDeviceIdStableWithinInstance() {
    Store S(ScratchDir());
    const std::string First  = LoadOrCreateDeviceId(S);
    const std::string Second = LoadOrCreateDeviceId(S);
    CHECK(First.size() == Lur::Save::DeviceIdHexLen);
    CHECK(First == Second);
}

// A brand-new Store over the SAME directory (i.e. an app restart) reads back the
// same id rather than minting a new one — this is the actual reconnect fix.
static void TestDeviceIdStableAcrossRestart() {
    const std::string Dir = ScratchDir();
    std::string First, Second;
    { Store S(Dir); First  = LoadOrCreateDeviceId(S); }
    { Store S(Dir); Second = LoadOrCreateDeviceId(S); }
    CHECK(First == Second);
}

// Two independent installs mint DIFFERENT ids (the 128-bit space makes a collision
// negligible), and each id is 32 lowercase hex chars.
static void TestDeviceIdsAreDistinctAndHex() {
    std::string A, B;
    { Store S(ScratchDir()); A = LoadOrCreateDeviceId(S); }
    { Store S(ScratchDir()); B = LoadOrCreateDeviceId(S); }
    CHECK(A != B);
    CHECK(A.size() == 32 && B.size() == 32);
    for (char C : A) {
        const bool Hex = (C >= '0' && C <= '9') || (C >= 'a' && C <= 'f');
        CHECK(Hex);
    }
}

int main() {
    TestRoundTripAndAbsent();
    TestOverwrite();
    TestPersistsAcrossInstances();
    TestUnsafeKeys();
    TestDeviceIdStableWithinInstance();
    TestDeviceIdStableAcrossRestart();
    TestDeviceIdsAreDistinctAndHex();

    if (GFailures == 0) {
        std::printf("All save tests passed.\n");
        return 0;
    }
    std::printf("%d save test(s) failed.\n", GFailures);
    return 1;
}
