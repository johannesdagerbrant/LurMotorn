// Dependency-free unit tests for Modules/Core (the Lur::Log seam). No framework:
// each CHECK records a failure and the process exits non-zero if any failed.
#include <cstdio>
#include <cstring>
#include <string>

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

int main() {
    TestLogRoutesToSink();

    if (GFailures == 0) {
        std::printf("All core tests passed.\n");
        return 0;
    }
    std::printf("%d core test(s) failed.\n", GFailures);
    return 1;
}
