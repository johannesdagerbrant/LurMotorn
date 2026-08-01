// Unit tests for the dev-GUI Numpad utility — key layout, the shared KeyRect geometry
// used by both render + hit-test, buffer accumulation, the one-dot guard, and Enter.
#include <cstdio>
#include <string>

#include <utility>
#include <vector>

#include "Lur/DevGui/CategoryTree.h"
#include "Lur/DevGui/Numpad.h"
#include "Lur/DevGui/Popover.h"

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
    // Bottom digit row: sign LEFT of 0, dot RIGHT of it.
    CHECK(std::string(Numpad::Label(3, 0)) == "+/-");
    CHECK(std::string(Numpad::Label(3, 1)) == "0");
    CHECK(std::string(Numpad::Label(3, 2)) == ".");
    // Enter is the row below, spanning it entirely.
    CHECK(Numpad::IsEnter(Numpad::EnterRow, 0) && !Numpad::IsEnter(0, 0));
    CHECK(!Numpad::IsEnter(3, 2));  // the dot key is NOT part of the Enter strip
}

static void TestPressBuildsBuffer() {
    Numpad N;
    N.Press(0, 0);  // 1
    N.Press(3, 1);  // 0
    N.Press(3, 2);  // .
    N.Press(1, 1);  // 5
    CHECK(N.Buffer() == "10.5");
    CHECK(!N.TakeEnter());
    N.Press(3, 2);  // second '.' ignored
    CHECK(N.Buffer() == "10.5");
    N.Backspace();
    CHECK(N.Buffer() == "10.");
    N.Press(Numpad::EnterRow, 0);  // Enter
    CHECK(N.TakeEnter());
    CHECK(!N.TakeEnter());  // one-shot
    N.Clear();
    CHECK(N.Buffer().empty());
}

// The sign key is what makes a negative value expressible at all. Tapping "+/-" and typing
// '-' must be the same action, or the pad and the keyboard disagree about one widget.
static void TestSignKey() {
    Numpad N;
    N.Press(3, 0);  // +/- on an EMPTY buffer: leaves a lone '-' so "sign then digits" works
    CHECK(N.Buffer() == "-");
    N.Press(1, 1);  // 5
    N.Press(3, 2);  // .
    N.Press(0, 0);  // 1
    CHECK(N.Buffer() == "-5.1");

    N.Press(3, 0);  // toggles back off — the minus is removed, the digits are untouched
    CHECK(N.Buffer() == "5.1");
    N.Press(3, 0);
    CHECK(N.Buffer() == "-5.1");

    // The sign lives at the FRONT however it was reached, so backspace still eats digits.
    N.Backspace();
    CHECK(N.Buffer() == "-5.");

    // '-' typed on the keyboard toggles rather than appends: a blind append would make "5-"
    // reachable by keyboard and not by pad.
    Numpad K;
    for (char C : std::string("5.1")) CHECK(K.Press(C));
    CHECK(K.Press('-'));
    CHECK(K.Buffer() == "-5.1");
    CHECK(K.Press('-'));
    CHECK(K.Buffer() == "5.1");
}

// Enter is one key spanning the whole bottom row: every column commits, and RowRect covers
// the full width so the renderer draws it once. Label carries the text on the first cell only.
static void TestEnterSpansBottomRow() {
    const float X = 100, Y = 200, W = 300, H = 400, Gap = 10;
    for (int C = 0; C < Numpad::Cols; ++C) {
        CHECK(Numpad::IsEnter(Numpad::EnterRow, C));
        Numpad Probe;
        Probe.Press(Numpad::EnterRow, C);
        CHECK(Probe.TakeEnter());          // any column of the strip commits
        CHECK(Probe.Buffer().empty());     // and none of them appends a label to the buffer
    }
    CHECK(std::string(Numpad::Label(Numpad::EnterRow, 0)) == "Enter");
    CHECK(std::string(Numpad::Label(Numpad::EnterRow, 1)).empty());  // drawn once, not thrice

    // RowRect spans every column, and shares the grid's row geometry.
    float Rx, Ry, Rw, Rh, Kx, Ky, Kw, Kh;
    Numpad::RowRect(X, Y, W, H, Gap, Numpad::EnterRow, Rx, Ry, Rw, Rh);
    Numpad::KeyRect(X, Y, W, H, Gap, Numpad::EnterRow, 0, Kx, Ky, Kw, Kh);
    CHECK(Rw == W);
    CHECK(Rx == Kx && Ry == Ky && Rh == Kh);
}

// The tap hit-test must resolve to the same key the renderer would draw at that rect.
// #119: typing on a physical keyboard must land in the SAME buffer the on-screen keys fill,
// so the two input devices can never produce different values for one widget.
static void TestPressCharMatchesTappedKeys() {
    Numpad Typed, Tapped;
    for (char C : std::string("10.5")) CHECK(Typed.Press(C));
    Tapped.Press(0, 0);  // 1
    Tapped.Press(3, 1);  // 0
    Tapped.Press(3, 2);  // .
    Tapped.Press(1, 1);  // 5
    CHECK(Typed.Buffer() == Tapped.Buffer());
    CHECK(Typed.Buffer() == "10.5");

    // The one-dot guard is the buffer's, not the pad geometry's — it must hold for typing too.
    CHECK(!Typed.Press('.'));
    CHECK(Typed.Buffer() == "10.5");

    // Keys with no pad equivalent are declined so the caller can let them fall through.
    CHECK(!Typed.Press('a'));
    CHECK(!Typed.Press(' '));
    CHECK(Typed.Buffer() == "10.5");

    // Enter is NOT a character: it arrives as a VK and the caller commits. Typing never arms it.
    CHECK(!Typed.TakeEnter());
}

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
            else if (std::string(Numpad::Label(R, C)) == "+/-") CHECK(Probe.Buffer() == "-");
            else CHECK(Probe.Buffer() == Numpad::Label(R, C));
        }
    // A tap outside the pad misses.
    CHECK(!N.Tap(X, Y, W, H, Gap, X - 20, Y - 20));
    CHECK(N.Buffer().empty());
}

// Hierarchical category tree: split on '|', nest, sort children, count the subtree.
static void TestCategoryTree() {
    using Lur::DevGui::BuildCategoryTree;
    using Item = std::pair<std::string, int>;
    // Feed items sorted by (path, leaf) as the console does; leaf = a stand-in id.
    std::vector<Item> Items = {
        {"Combat", 1},          // flat category, one leaf at its own node
        {"Units|Miner", 2},
        {"Units|Rock", 3},
        {"Units|Rock", 4},      // two leaves under the same leaf-category
        {"Boid|Noise", 5},
        {"", 6},                // empty path -> lands on the root
        {"Units", 7},           // a leaf directly on the "Units" parent (not a child)
    };
    auto Root = BuildCategoryTree(Items);
    CHECK(Root.Segment.empty() && Root.Path.empty());
    CHECK(Root.TotalLeaves == 7);         // every leaf counted once
    CHECK(Root.Leaves.size() == 1 && Root.Leaves[0] == 6);  // the empty-path leaf
    // Children sorted by segment: Boid, Combat, Units.
    CHECK(Root.Children.size() == 3);
    CHECK(Root.Children[0].Segment == "Boid");
    CHECK(Root.Children[1].Segment == "Combat");
    CHECK(Root.Children[2].Segment == "Units");
    // Combat: a leaf directly on it, no children.
    const auto& Combat = Root.Children[1];
    CHECK(Combat.Path == "Combat" && Combat.TotalLeaves == 1 && Combat.Children.empty());
    CHECK(Combat.Leaves.size() == 1 && Combat.Leaves[0] == 1);
    // Units: one direct leaf (7) + children Miner, Rock; Rock holds two leaves (3,4).
    const auto& Units = Root.Children[2];
    CHECK(Units.Path == "Units" && Units.TotalLeaves == 4);
    CHECK(Units.Leaves.size() == 1 && Units.Leaves[0] == 7);
    CHECK(Units.Children.size() == 2);
    CHECK(Units.Children[0].Segment == "Miner" && Units.Children[0].Path == "Units|Miner");
    CHECK(Units.Children[1].Segment == "Rock" && Units.Children[1].Path == "Units|Rock");
    CHECK(Units.Children[1].Leaves.size() == 2);
    CHECK(Units.Children[1].Leaves[0] == 3 && Units.Children[1].Leaves[1] == 4);
    // Empty/doubled separators collapse: "A||B" == "A|B", trailing '|' ignored.
    std::vector<Item> Sloppy = {{"A||B", 1}, {"A|", 2}};
    auto R2 = BuildCategoryTree(Sloppy);
    CHECK(R2.Children.size() == 1 && R2.Children[0].Segment == "A");
    CHECK(R2.Children[0].Leaves.size() == 1 && R2.Children[0].Leaves[0] == 2);  // "A|" -> leaf on A
    CHECK(R2.Children[0].Children.size() == 1 && R2.Children[0].Children[0].Segment == "B");
}

// Popover placement: prefer below the row, flip above when it would overflow the bottom.
static void TestPopoverPlacement() {
    using Lur::DevGui::PlaceBelowOrAbove;
    const float ScreenH = 1000, Gap = 8;
    // Row near the top with lots of room below -> placed below.
    CHECK(PlaceBelowOrAbove(/*Ay*/100, /*Ah*/20, /*Ph*/300, Gap, ScreenH) == 100 + 20 + Gap);
    // Row near the bottom, no room below -> flips above.
    {
        const float Ay = 900, Ah = 20, Ph = 300;
        const float Y = PlaceBelowOrAbove(Ay, Ah, Ph, Gap, ScreenH);
        CHECK(Y == Ay - Gap - Ph);
        CHECK(Y >= 0 && Y + Ph <= ScreenH);   // fully on-screen
    }
    // Popover taller than either gap -> clamped on-screen (never negative).
    {
        const float Y = PlaceBelowOrAbove(/*Ay*/500, /*Ah*/20, /*Ph*/1200, Gap, ScreenH);
        CHECK(Y == 0);
    }
}

int main() {
    TestLayout();
    TestPressBuildsBuffer();
    TestSignKey();
    TestEnterSpansBottomRow();
    TestPressCharMatchesTappedKeys();
    TestTapHitTest();
    TestCategoryTree();
    TestPopoverPlacement();
    if (GFailures == 0) { std::printf("devgui_tests: ALL PASS\n"); return 0; }
    std::printf("devgui_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
