// Unit tests for the dev-GUI Numpad utility — key layout, the shared KeyRect geometry
// used by both render + hit-test, buffer accumulation, the one-dot guard, and Enter.
#include <cstdio>
#include <string>

#include "Lur/DevGui/Numpad.h"

using Lur::DevGui::Numpad;

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

static void TestLayout() {
    CHECK(std::string(Numpad::Label(0, 0)) == "1");
    CHECK(std::string(Numpad::Label(2, 2)) == "9");
    CHECK(std::string(Numpad::Label(3, 0)) == ".");
    CHECK(std::string(Numpad::Label(3, 1)) == "0");
    CHECK(std::string(Numpad::Label(3, 2)) == "Enter");
    CHECK(Numpad::IsEnter(3, 2) && !Numpad::IsEnter(0, 0));
}

static void TestPressBuildsBuffer() {
    Numpad N;
    N.Press(0, 0);  // 1
    N.Press(3, 1);  // 0
    N.Press(3, 0);  // .
    N.Press(1, 1);  // 5
    CHECK(N.Buffer() == "10.5");
    CHECK(!N.TakeEnter());
    N.Press(3, 0);  // second '.' ignored
    CHECK(N.Buffer() == "10.5");
    N.Backspace();
    CHECK(N.Buffer() == "10.");
    N.Press(3, 2);  // Enter
    CHECK(N.TakeEnter());
    CHECK(!N.TakeEnter());  // one-shot
    N.Clear();
    CHECK(N.Buffer().empty());
}

// The tap hit-test must resolve to the same key the renderer would draw at that rect.
static void TestTapHitTest() {
    Numpad N;
    const float X = 100, Y = 200, W = 300, H = 400, Gap = 10;
    // Aim at the centre of each key via KeyRect; Tap must apply that key.
    for (int R = 0; R < Numpad::Rows; ++R)
        for (int C = 0; C < Numpad::Cols; ++C) {
            float Kx, Ky, Kw, Kh;
            Numpad::KeyRect(X, Y, W, H, Gap, R, C, Kx, Ky, Kw, Kh);
            Numpad Probe;
            const bool Hit = Probe.Tap(X, Y, W, H, Gap, Kx + Kw * 0.5f, Ky + Kh * 0.5f);
            CHECK(Hit);
            if (Numpad::IsEnter(R, C)) CHECK(Probe.TakeEnter());
            else CHECK(Probe.Buffer() == Numpad::Label(R, C));
        }
    // A tap outside the pad misses.
    CHECK(!N.Tap(X, Y, W, H, Gap, X - 20, Y - 20));
    CHECK(N.Buffer().empty());
}

int main() {
    TestLayout();
    TestPressBuildsBuffer();
    TestTapHitTest();
    if (GFailures == 0) { std::printf("devgui_tests: ALL PASS\n"); return 0; }
    std::printf("devgui_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
