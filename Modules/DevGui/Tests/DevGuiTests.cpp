// Unit tests for the dev-GUI Numpad utility — key layout, the shared KeyRect geometry
// used by both render + hit-test, buffer accumulation, the one-dot guard, and Enter.
#include <cstdio>
#include <string>

#include <utility>
#include <vector>

#include "Lur/DevGui/CategoryTree.h"
#include "Lur/DevGui/ColorMath.h"
#include "Lur/DevGui/ColorPicker.h"
#include "Lur/DevGui/Numpad.h"
#include "Lur/DevGui/Popover.h"
#include "Lur/DevGui/Widgets.h"

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

// #113/#116: the slider's two mappings must be exact inverses, or grabbing a knob makes it jump
// — the classic slider bug, and the one that makes a tuning control feel untrustworthy.
static void TestSliderRoundTrips() {
    using Lur::DevGui::Slider;
    const float X = 50, W = 200, KnobW = 20, Min = 0.0f, Max = 1.0f;

    for (int I = 0; I <= 10; ++I) {
        const float V = static_cast<float>(I) / 10.0f;
        const float Kx = Slider::KnobX(X, W, KnobW, V, Min, Max);
        const float Back = Slider::ValueAt(X, W, KnobW, Kx, Min, Max);
        CHECK(Back > V - 0.001f && Back < V + 0.001f);
    }

    // The knob's CENTRE spans exactly [Min,Max], so the knob never hangs off either end.
    CHECK(Slider::KnobX(X, W, KnobW, Min, Min, Max) == X + KnobW * 0.5f);
    CHECK(Slider::KnobX(X, W, KnobW, Max, Min, Max) == X + W - KnobW * 0.5f);

    // Out of range clamps rather than drawing outside the widget — the console warns-but-allows
    // an out-of-range value, so the slider has to cope with one.
    CHECK(Slider::KnobX(X, W, KnobW, -5.0f, Min, Max) == X + KnobW * 0.5f);
    CHECK(Slider::KnobX(X, W, KnobW, 99.0f, Min, Max) == X + W - KnobW * 0.5f);
    CHECK(Slider::ValueAt(X, W, KnobW, X - 500, Min, Max) == Min);
    CHECK(Slider::ValueAt(X, W, KnobW, X + 500, Min, Max) == Max);

    // A degenerate range must not divide by zero or NaN the knob off-screen.
    const float Kx = Slider::KnobX(X, W, KnobW, 5.0f, 5.0f, 5.0f);
    CHECK(Kx >= X && Kx <= X + W);

    // Non-unit ranges (an int knob with min/max) map the same way.
    CHECK(Slider::ValueAt(X, W, KnobW, Slider::KnobX(X, W, KnobW, 75.0f, 50.0f, 100.0f),
                          50.0f, 100.0f) > 74.9f);
}

// #174: HSV <-> RGB must round-trip everywhere hue is DEFINED. Where it is not (grey, black,
// white) the conversion is lossy by nature — which is exactly why the picker keeps its own H,S,V
// while open instead of re-deriving each frame.
static void TestColorMathRoundTrip() {
    using namespace Lur::DevGui::ColorMath;
    for (int Hi = 0; Hi < 12; ++Hi)
        for (int Si = 1; Si <= 4; ++Si)
            for (int Vi = 1; Vi <= 4; ++Vi) {
                const float H = static_cast<float>(Hi) / 12.0f;
                const float S = static_cast<float>(Si) / 4.0f;
                const float V = static_cast<float>(Vi) / 4.0f;
                float R = 0, G = 0, B = 0, H2 = 0, S2 = 0, V2 = 0;
                HsvToRgb(H, S, V, R, G, B);
                RgbToHsv(R, G, B, H2, S2, V2);
                CHECK(S2 > S - 0.01f && S2 < S + 0.01f);
                CHECK(V2 > V - 0.01f && V2 < V + 0.01f);
                // Hue wraps, so 0 and 1 are the same angle.
                const float D = (H2 > H) ? (H2 - H) : (H - H2);
                CHECK(D < 0.01f || D > 0.99f);
            }

    // The known primaries, so a sign error in a sector cannot hide behind a round-trip.
    float R = 0, G = 0, B = 0;
    HsvToRgb(0.0f, 1.0f, 1.0f, R, G, B);        CHECK(R == 1.0f && G == 0.0f && B == 0.0f);
    HsvToRgb(1.0f / 3.0f, 1.0f, 1.0f, R, G, B); CHECK(G == 1.0f && R < 0.01f && B < 0.01f);
    HsvToRgb(2.0f / 3.0f, 1.0f, 1.0f, R, G, B); CHECK(B == 1.0f && R < 0.01f && G < 0.01f);

    // Hue WRAPS rather than clamping, so dragging the strip past either end keeps going.
    float R2 = 0, G2 = 0, B2 = 0;
    HsvToRgb(1.25f, 1.0f, 1.0f, R, G, B);
    HsvToRgb(0.25f, 1.0f, 1.0f, R2, G2, B2);
    CHECK(R == R2 && G == G2 && B == B2);
    HsvToRgb(-0.25f, 1.0f, 1.0f, R, G, B);
    HsvToRgb(0.75f, 1.0f, 1.0f, R2, G2, B2);
    CHECK(R == R2 && G == G2 && B == B2);

    // S and V clamp; a grey has no hue and must report 0 rather than garbage.
    float H3 = 0, S3 = 0, V3 = 0;
    RgbToHsv(0.5f, 0.5f, 0.5f, H3, S3, V3);
    CHECK(S3 == 0.0f && H3 == 0.0f && V3 == 0.5f);
    RgbToHsv(0.0f, 0.0f, 0.0f, H3, S3, V3);
    CHECK(S3 == 0.0f && V3 == 0.0f);
}

// #174: the SV square's mapping must be an exact inverse, and its axes the conventional way up
// (bright at the TOP, black along the bottom) — an inverted V axis is the kind of thing that
// looks plausible in code and wrong the instant you drag it.
static void TestColorPickerSquare() {
    using Lur::DevGui::ColorPicker;
    const float X = 20, Y = 40, W = 200, H = 140;
    float S = -1, V = -1;

    ColorPicker::SvAt(X, Y, W, H, X, Y, S, V);              // top-left
    CHECK(S == 0.0f && V == 1.0f);                          // unsaturated, full brightness = white
    ColorPicker::SvAt(X, Y, W, H, X + W, Y, S, V);          // top-right
    CHECK(S == 1.0f && V == 1.0f);                          // full hue
    ColorPicker::SvAt(X, Y, W, H, X, Y + H, S, V);          // bottom-left
    CHECK(S == 0.0f && V == 0.0f);                          // black
    ColorPicker::SvAt(X, Y, W, H, X + W * 0.5f, Y + H * 0.5f, S, V);
    CHECK(S > 0.49f && S < 0.51f && V > 0.49f && V < 0.51f);

    // A drag that leaves the square PINS to the edge rather than jumping or going negative.
    ColorPicker::SvAt(X, Y, W, H, X - 500, Y + 900, S, V);
    CHECK(S == 0.0f && V == 0.0f);
    ColorPicker::SvAt(X, Y, W, H, X + 500, Y - 900, S, V);
    CHECK(S == 1.0f && V == 1.0f);

    // SvPoint is SvAt's inverse — the reticle sits where the press selected.
    for (int I = 0; I <= 4; ++I)
        for (int J = 0; J <= 4; ++J) {
            const float Ws = static_cast<float>(I) / 4.0f, Wv = static_cast<float>(J) / 4.0f;
            float Px = 0, Py = 0, S2 = 0, V2 = 0;
            ColorPicker::SvPoint(X, Y, W, H, Ws, Wv, Px, Py);
            ColorPicker::SvAt(X, Y, W, H, Px, Py, S2, V2);
            CHECK(S2 > Ws - 0.01f && S2 < Ws + 0.01f);
            CHECK(V2 > Wv - 0.01f && V2 < Wv + 0.01f);
        }
}

// #174: the three interactive regions must not overlap, and each must resolve to ITS OWN kind of
// value — a press on the hue strip that reported a saturation would be silently wrong.
static void TestColorPickerRegions() {
    using Lur::DevGui::ColorPicker;
    using EHit = ColorPicker::EHit;
    const float X = 10, Y = 20, W = 200, SwH = 26, SqH = 140, StH = 18, RoH = 16, Gap = 6;
    const float Knob = 10;
    const float PanelH = ColorPicker::PanelH(SwH, SqH, StH, RoH, Gap);

    auto Centre = [&](void (*Fn)(float, float, float, float, float, float, float,
                                 float&, float&, float&, float&), float& Cx, float& Cy) {
        float Rx, Ry, Rw, Rh;
        Fn(X, Y, W, SwH, SqH, StH, Gap, Rx, Ry, Rw, Rh);
        Cx = Rx + Rw * 0.5f; Cy = Ry + Rh * 0.5f;
    };
    float A = -1, B = -1, Cx = 0, Cy = 0;

    // Square.
    {
        float Rx, Ry, Rw, Rh;
        ColorPicker::SquareRect(X, Y, W, SwH, SqH, Gap, Rx, Ry, Rw, Rh);
        CHECK(Ry >= Y + SwH);                       // below the swatch
        CHECK(ColorPicker::Hit(X, Y, W, SwH, SqH, StH, Gap, Knob,
                               Rx + Rw * 0.5f, Ry + Rh * 0.5f, A, B) == EHit::SvSquare);
        CHECK(A > 0.49f && A < 0.51f && B > 0.49f && B < 0.51f);
    }
    // Hue strip.
    Centre(&ColorPicker::HueRect, Cx, Cy);
    CHECK(ColorPicker::Hit(X, Y, W, SwH, SqH, StH, Gap, Knob, Cx, Cy, A, B) == EHit::HueStrip);
    CHECK(A > 0.49f && A < 0.51f);
    // Alpha strip.
    Centre(&ColorPicker::AlphaRect, Cx, Cy);
    CHECK(ColorPicker::Hit(X, Y, W, SwH, SqH, StH, Gap, Knob, Cx, Cy, A, B) == EHit::AlphaStrip);

    // The swatch and the readout row are DISPLAY, not controls — pressing them does nothing.
    CHECK(ColorPicker::Hit(X, Y, W, SwH, SqH, StH, Gap, Knob,
                           X + W * 0.5f, Y + SwH * 0.5f, A, B) == EHit::None);
    float Ox, Oy, Ow, Oh;
    ColorPicker::ReadoutRect(X, Y, W, SwH, SqH, StH, RoH, Gap, Ox, Oy, Ow, Oh);
    CHECK(ColorPicker::Hit(X, Y, W, SwH, SqH, StH, Gap, Knob,
                           Ox + Ow * 0.5f, Oy + Oh * 0.5f, A, B) == EHit::None);
    CHECK(Oy + Oh <= Y + PanelH + 0.01f);   // everything fits the height the caller reserved
    CHECK(ColorPicker::Hit(X, Y, W, SwH, SqH, StH, Gap, Knob, X - 50, Y - 50, A, B) == EHit::None);

    CHECK(std::string(ColorPicker::ChannelLabel(0)) == "R");
    CHECK(std::string(ColorPicker::ChannelLabel(3)) == "A");
    CHECK(std::string(ColorPicker::ChannelLabel(9)).empty());
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
    TestSliderRoundTrips();
    TestColorMathRoundTrip();
    TestColorPickerSquare();
    TestColorPickerRegions();
    if (GFailures == 0) { std::printf("devgui_tests: ALL PASS\n"); return 0; }
    std::printf("devgui_tests: %d FAILURE(S)\n", GFailures);
    return 1;
}
