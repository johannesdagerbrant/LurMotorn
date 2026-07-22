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
    TestTapHitTest();
    TestCategoryTree();
    TestPopoverPlacement();
    if (GFailures == 0) { std::printf("devgui_tests: ALL PASS\n"); return 0; }
    std::printf("devgui_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
