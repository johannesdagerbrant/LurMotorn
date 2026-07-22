// Dependency-free unit tests for the dev console MODEL — completion ranking, MRU,
// command vs CVar dispatch, gameplay-set routing, arrow navigation, scrollback.
#include <cstdio>
#include <string>

#include "Lur/Core/CVar.h"
#include "Lur/Core/DevCommand.h"
#include "Lur/DevConsole/ConsoleModel.h"
#include "Lur/Sim/FixedString.h"  // Fixed codec for a CVar<Fixed> (ADL)

using Lur::DevConsole::ConsoleModel;

static int GFailures = 0;
#define CHECK(Cond)                                                       \
    do {                                                                  \
        if (!(Cond)) {                                                    \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond);   \
            ++GFailures;                                                  \
        }                                                                 \
    } while (0)

// Test fixtures: a local int CVar, a gameplay Fixed CVar, and a dev-command that counts
// its invocations via its context pointer.
LUR_CVAR(CvConMax, "con.max", 10, ::Lur::Core::CVarFlagNone);
LUR_CVAR(CvConSpeed, "con.speed", ::Lur::Sim::Fixed{::Lur::Sim::Fixed::One / 2},
         ::Lur::Core::CVarFlagAffectsGameplay);

static int GCmdRuns = 0;
static std::string GCmdLastArg;
static void RestartHandler(const Lur::Core::DevArgs& Args, std::string& Out, void* Ctx) {
    *static_cast<int*>(Ctx) += 1;
    GCmdLastArg = Args.Arg(0);
    Out = "restarted";
}
static Lur::Core::DevCommand GRestartCmd{"con.restart", "restart the match", &RestartHandler,
                                         &GCmdRuns, "Test"};

// A gameplay set must route through the hook (not touch the CVar locally).
static int GGameplayRoutes = 0;
static std::string GRoutedValue;
static void GameplayHook(void*, Lur::Core::ICVar& Cv, const char* Value) {
    ++GGameplayRoutes;
    GRoutedValue = std::string(Cv.Name()) + "=" + Value;
}

static void TestCvarSetAndPrint() {
    ConsoleModel M;
    M.SetInput("con.max 42");
    M.Execute();
    CHECK(CvConMax.Get() == 42);
    CHECK(!M.Scrollback().empty() && M.Scrollback().back() == "con.max = 42");

    M.SetInput("con.max");  // no value -> print
    M.Execute();
    CHECK(M.Scrollback().back() == "con.max = 42");

    M.SetInput("con.max notanumber");
    M.Execute();
    CHECK(M.Scrollback().back().rfind("bad value", 0) == 0);
    CvConMax.Reset();
}

static void TestCommandDispatch() {
    ConsoleModel M;
    const int Before = GCmdRuns;
    M.SetInput("con.restart now");
    M.Execute();
    CHECK(GCmdRuns == Before + 1);
    CHECK(GCmdLastArg == "now");
    CHECK(M.Scrollback().back() == "restarted");

    M.SetInput("con.nope");
    M.Execute();
    CHECK(M.Scrollback().back().rfind("unknown", 0) == 0);
}

static void TestGameplaySetRoutesThroughHook() {
    ConsoleModel M;
    M.SetGameplayHook(&GameplayHook, nullptr);
    const int Before = GGameplayRoutes;
    M.SetInput("con.speed 0.9");
    M.Execute();
    CHECK(GGameplayRoutes == Before + 1);          // routed, not applied locally...
    CHECK(GRoutedValue == "con.speed=0.9");
    CHECK(!CvConSpeed.Overridden());               // ...so the CVar itself is untouched
}

static void TestCompletionAndMru() {
    ConsoleModel M;
    M.SetInput("con.");
    // All three fixtures share the "con." prefix.
    const auto& C = M.Completions();
    auto Has = [&](const char* N) {
        for (const auto& S : C) if (S == N) return true;
        return false;
    };
    CHECK(Has("con.max") && Has("con.speed") && Has("con.restart"));

    // Use con.restart -> it should rank to the BOTTOM (newest-at-bottom) on the next filter.
    M.SetInput("con.restart");
    M.Execute();
    M.SetInput("con.");
    const auto& C2 = M.Completions();
    CHECK(!C2.empty() && C2.back() == "con.restart");
}

static void TestArrowNav() {
    ConsoleModel M;
    M.SetInput("con.ma");             // stash = "con.ma"
    M.ArrowUp();                       // enter the list at the bottom
    CHECK(M.Highlight() >= 0);
    CHECK(M.Input() == M.Completions()[static_cast<size_t>(M.Highlight())]);
    M.ArrowDown();                     // past the (single) entry -> restore the stash
    CHECK(M.Highlight() == -1 && M.Input() == "con.ma");
}

int main() {
    Lur::Core::CVarEnterMain();
    TestCvarSetAndPrint();
    TestCommandDispatch();
    TestGameplaySetRoutesThroughHook();
    TestCompletionAndMru();
    TestArrowNav();

    if (GFailures == 0) { std::printf("devconsole_tests: ALL PASS\n"); return 0; }
    std::printf("devconsole_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
