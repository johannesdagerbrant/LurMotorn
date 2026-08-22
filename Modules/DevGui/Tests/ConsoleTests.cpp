// Lur::DevGui::Console — the dev console as an engine facility (#201).
//
// The console used to be ~650 lines inside Rps::GameView, reachable only by running the game and
// tapping, so nothing about it was ever asserted. It now has three consumers (RPS, chess, and game 3
// by the standing certain-consumer ruling), which makes the behaviours below shared contract rather
// than one game's UI.
//
// What is worth testing is NOT the pixels. It is the INTERACTION MODEL, because every bug the console
// has actually had was in that: an editor bound to the wrong row, an edit that did not commit, a
// press that reached both the console and the game, a hue handle that snapped to red mid-drag. Each
// test below drives Tap/Key/Draw exactly as a hand would and asserts on the state the paint reads —
// so a passing test cannot describe a console that draws something else.
#include "Lur/DevGui/Console.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "Lur/DevGui/ColorPicker.h"
#include "Lur/Render/ColorString.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::DevGui::ColorPicker;
using Lur::DevGui::Console;
using Lur::Render::Color;

// ---- The CVars under test ----------------------------------------------------------------------
// One of each kind the console treats differently, because "which editor does this type get" is the
// rule most likely to regress. Names share a prefix so they land in one category.
LUR_CVAR(CvNumber, "test.console.number", 5.0f, ::Lur::Core::CVarFlagNone,
         "a number, so this row gets the numpad");
LUR_CVAR(CvFlag, "test.console.flag", false, ::Lur::Core::CVarFlagNone,
         "a bool, so this row toggles in place");
LUR_CVAR(CvPaint, "test.console.paint", (Color{0.0f, 0.0f, 1.0f, 1.0f}), ::Lur::Core::CVarFlagNone,
         "a colour, so this row gets the picker");
// No tooltip: the "i" button must be inert for this one, which is the other half of that rule.
LUR_CVAR(CvQuiet, "test.console.quiet", 1.0f, ::Lur::Core::CVarFlagNone, nullptr);

namespace {

constexpr float ScreenW = 800.0f;
constexpr float ScreenH = 1200.0f;

// Counts what the console asks the renderer to do; every method is otherwise a stub handing back
// plausible distinct handles.
class RecordingRenderer final : public Lur::Render::IRenderer {
public:
    int Meshes = 0;   // DrawMesh calls
    int Glyphs = 0;   // DrawGlyphs calls (text)

    void DrawMesh(Lur::Render::MeshHandle, Lur::Render::MaterialHandle,
                  const Lur::Math::Mat4&) override {
        ++Meshes;
    }
    void DrawGlyphs(const Lur::Render::Vertex*, uint32_t, const uint32_t*, uint32_t,
                    Lur::Render::MaterialHandle, float) override {
        ++Glyphs;
    }
    int Total() const { return Meshes + Glyphs; }
    void Reset() { Meshes = 0; Glyphs = 0; }

    bool Init(void*) override { return true; }
    void Resize(int, int) override {}
    void Shutdown() override {}
    Lur::Render::MeshHandle CreateMesh(const Lur::Render::Vertex*, uint32_t, const uint32_t*,
                                       uint32_t) override {
        return ++NextMesh;
    }
    Lur::Render::TextureHandle LoadTexture(const uint8_t*, int, int,
                                           Lur::Render::ETextureFormat) override {
        return ++NextTex;
    }
    Lur::Render::MaterialHandle CreateMaterial(const Lur::Render::MaterialDesc&) override {
        return ++NextMat;
    }
    void BeginFrame(const Lur::Render::Camera&) override {}
    void EndFrame() override {}

private:
    Lur::Render::MeshHandle NextMesh = 0;
    Lur::Render::TextureHandle NextTex = 0;
    Lur::Render::MaterialHandle NextMat = 0;
};

// Records every commit so a test can tell "the value changed" from "the value changed AND the app was
// told". Those are different bugs: the second one loses the edit at the next launch, and on a phone
// it is also the one that fails to reach the peer.
struct CommitLog {
    std::vector<std::string> Names;
    static void Hook(void* Ctx, Lur::Core::ICVar& Cv) {
        static_cast<CommitLog*>(Ctx)->Names.emplace_back(Cv.Name());
    }
    bool Saw(const char* Name) const {
        for (const std::string& N : Names)
            if (N == Name) return true;
        return false;
    }
    void Clear() { Names.clear(); }
};

Lur::Core::ICVar* Find(const char* Name) { return Lur::Core::CVarRegistry::Find(Name); }

// Open a console with everything wired, its rows unfolded, and one frame already drawn so the row
// geometry exists. Returns by out-params because Console is not copyable.
void Bring(Console& C, RecordingRenderer& R, CommitLog& Log) {
    C.CreateResources(&R);
    C.SetCommitHook(&CommitLog::Hook, &Log);
    C.SetOpen(true);
    C.Draw(&R, ScreenW, ScreenH);
    Log.Clear();
}

// Tap the middle of a CVar's row. Fails the test rather than silently doing nothing if the row is not
// on screen — a test that taps into empty space and then asserts "no editor opened" would pass for
// entirely the wrong reason.
bool TapRow(Console& C, RecordingRenderer& R, const Lur::Core::ICVar* Cv) {
    float X = 0, Y = 0, W = 0, H = 0;
    if (!C.RowRect(Cv, ScreenW, ScreenH, X, Y, W, H)) return false;
    // Left of the value column and right of the "i" button: the row body, which is the "select this"
    // target. A tap on either end means something else entirely.
    C.Tap(X + W * 0.45f, Y + H * 0.5f);
    C.Draw(&R, ScreenW, ScreenH);
    return true;
}

bool Near(float A, float B, float Eps = 0.02f) { return std::fabs(A - B) <= Eps; }

}  // namespace

// ---- Closed: invisible AND inert ---------------------------------------------------------------
// Both halves matter and they fail differently. A console that draws while closed sits on top of the
// game forever; one that claims input makes the game unplayable. This is also what lets the tool exist
// in a build someone plays.
static void TestClosedIsInvisibleAndInert() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    C.CreateResources(&R);
    C.SetCommitHook(&CommitLog::Hook, &Log);

    CHECK(!C.IsOpen());
    R.Reset();
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(R.Total() == 0);

    // A key is NOT claimed while closed — the game must still get it.
    CHECK(!C.Key(0x0D));
    CHECK(!C.Key(0x26));

    // It paints once opened.
    C.SetOpen(true);
    R.Reset();
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(R.Total() > 0);

    // ---- Input queued while CLOSED is discarded, not banked ----
    // Otherwise a tap that landed before the console opened fires into a layout it never saw.
    // The tap has to be somewhere that WOULD select a row, or "nothing got selected" is true
    // whether or not the stale tap fired.
    Lur::Core::ICVar* Num = Find("test.console.number");
    CHECK(Num != nullptr);
    if (Num == nullptr) return;
    float Rx = 0, Ry = 0, Rw = 0, Rh = 0;
    CHECK(C.RowRect(Num, ScreenW, ScreenH, Rx, Ry, Rw, Rh));
    // Prove the coordinate is live: tapping there while open really does select the row.
    C.Tap(Rx + Rw * 0.45f, Ry + Rh * 0.5f);
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.SelectedCvar() == Num);

    // Now the real check: the same tap, queued while closed, must be gone by the time it reopens.
    // Note closing dismisses the EDITORS, not the selection — the row you were last on stays marked
    // deliberately, so the assertion has to be about the editor. A stale tap re-firing would reopen
    // the numpad on this row, which is exactly what must not happen.
    C.SetOpen(false);
    CHECK(!C.NumpadOpen());
    C.Draw(&R, ScreenW, ScreenH);
    C.Tap(Rx + Rw * 0.45f, Ry + Rh * 0.5f);
    C.Draw(&R, ScreenW, ScreenH);   // closed: consumed and thrown away
    C.SetOpen(true);
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(!C.NumpadOpen());
}

// ---- ...and a scroll queued while closed is discarded too --------------------------------------
// Split out because it needs a viewport the content actually OVERFLOWS. On a tall screen this
// fixture's few rows fit, so ClampScroll pins the offset to 0 regardless and the assertion cannot
// fail — the vacuous version of this check survived a sabotage that deleted the discard outright.
static void TestScrollQueuedWhileClosedIsDiscarded() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    C.CreateResources(&R);
    C.SetCommitHook(&CommitLog::Hook, &Log);

    // Short viewport, so the rows overflow and scrolling has somewhere to go.
    constexpr float ShortH = 260.0f;
    C.SetOpen(true);
    C.Draw(&R, ScreenW, ShortH);

    // First establish that scrolling CAN move the offset here. Without this the test below would
    // pass on a console that simply never scrolls.
    C.Scroll(40.0f);
    C.Draw(&R, ScreenW, ShortH);
    const float Moved = C.ScrollOffset();
    CHECK(Moved > 0.0f);

    // Back to the top, close, spin the wheel, reopen: the offset must be where we left it.
    C.Scroll(-1000.0f);
    C.Draw(&R, ScreenW, ShortH);
    CHECK(C.ScrollOffset() == 0.0f);
    C.SetOpen(false);
    C.Draw(&R, ScreenW, ShortH);
    C.Scroll(40.0f);
    C.Draw(&R, ScreenW, ShortH);   // closed: discarded
    C.SetOpen(true);
    C.Draw(&R, ScreenW, ShortH);
    CHECK(C.ScrollOffset() == 0.0f);
}

// ---- Open: it paints, and a key is claimed ----------------------------------------------------
static void TestOpenPaintsAndClaimsKeys() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    R.Reset();
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(R.Meshes > 0);   // panel, rows, plates
    CHECK(R.Glyphs > 0);   // labels and values

    // Claimed while open, so the caller must not also give it to the game.
    CHECK(C.Key(0x0D));
}

// ---- THE type rule: a colour gets the picker, a number gets the numpad, a bool gets neither ----
// This is the rule most worth pinning. Get it wrong and a colour row opens a numeric keypad you are
// meant to type "0.88 0.31 0.22 1" into, which is unusable but looks like it works.
static void TestEditorPerType() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Num = Find("test.console.number");
    Lur::Core::ICVar* Paint = Find("test.console.paint");
    Lur::Core::ICVar* Flag = Find("test.console.flag");
    CHECK(Num != nullptr && Paint != nullptr && Flag != nullptr);
    if (Num == nullptr || Paint == nullptr || Flag == nullptr) return;

    CHECK(TapRow(C, R, Num));
    CHECK(C.SelectedCvar() == Num);
    CHECK(C.NumpadOpen());
    CHECK(!C.PickerOpen());

    CHECK(TapRow(C, R, Paint));
    CHECK(C.SelectedCvar() == Paint);
    CHECK(C.PickerOpen());
    CHECK(!C.NumpadOpen());   // exactly one editor: two popovers would fight for the same space

    // A bool has no editor to open — one tap flips it in place and commits.
    const int32_t Was = Flag->RawValue();
    Log.Clear();
    CHECK(TapRow(C, R, Flag));
    CHECK(Flag->RawValue() != Was);
    CHECK(Log.Saw("test.console.flag"));   // it PERSISTED, not just changed in memory
    CHECK(!C.NumpadOpen());
    CHECK(!C.PickerOpen());
    Flag->Reset();
}

// ---- The numpad commits on Enter and discards on Escape ----------------------------------------
// A tuner types a number and presses Enter; if the commit hook is skipped the value is right until
// the next launch, which is the kind of bug you diagnose as "the console does not save".
static void TestNumpadCommitAndCancel() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Num = Find("test.console.number");
    CHECK(Num != nullptr);
    if (Num == nullptr) return;
    const std::string Was = Num->ValueString();

    CHECK(TapRow(C, R, Num));
    CHECK(C.NumpadOpen());
    C.Key('4');   // VK_4 == '4'
    C.Key('2');
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.NumpadBuffer() == "42");
    CHECK(Num->ValueString() == Was);   // nothing written before Enter

    Log.Clear();
    C.Key(0x0D);   // VK_RETURN
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(!C.NumpadOpen());
    CHECK(Num->ValueF() > 41.5f && Num->ValueF() < 42.5f);
    CHECK(Log.Saw("test.console.number"));

    // Escape discards: the buffer is dropped and the value is untouched.
    CHECK(TapRow(C, R, Num));
    C.Key('7');
    C.Draw(&R, ScreenW, ScreenH);
    Log.Clear();
    C.Key(0x1B);   // VK_ESCAPE
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(!C.NumpadOpen());
    CHECK(Num->ValueF() > 41.5f && Num->ValueF() < 42.5f);   // still 42, not 7
    CHECK(Log.Names.empty());

    Num->Reset();
}

// ---- The keyboard cursor moves, and Enter acts on what it landed on ----------------------------
// Arrows MOVE the highlight; Left/Right scrub the value. Conflating the two (an early version had
// Up/Down scrubbing) makes the list impossible to navigate without a mouse.
static void TestKeyboardCursor() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    CHECK(C.HighlightedCvar() == nullptr && C.HighlightedCategory().empty());
    C.Key(0x28);   // VK_DOWN
    C.Draw(&R, ScreenW, ScreenH);
    // The first row is the root category header, so the cursor lands on a category, not a CVar.
    const bool OnSomething =
        C.HighlightedCvar() != nullptr || !C.HighlightedCategory().empty();
    CHECK(OnSomething);

    // Walk down to a CVar row and scrub it. Bounded: the tree is small, and an unbounded loop here
    // would hang rather than fail if StepIndex ever stopped advancing.
    Lur::Core::ICVar* Num = Find("test.console.number");
    CHECK(Num != nullptr);
    if (Num == nullptr) return;
    const float Before = Num->ValueF();
    bool Reached = false;
    for (int I = 0; I < 400 && !Reached; ++I) {
        C.Key(0x28);
        C.Draw(&R, ScreenW, ScreenH);
        Reached = (C.HighlightedCvar() == Num);
    }
    CHECK(Reached);
    if (!Reached) return;

    Log.Clear();
    C.Key(0x27);   // VK_RIGHT — scrub up
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(Num->ValueF() > Before);
    CHECK(Log.Saw("test.console.number"));
    // ...and no editor was opened by scrubbing, which is the difference from Enter.
    CHECK(!C.NumpadOpen());

    C.Key(0x0D);   // VK_RETURN promotes the highlight to the editor
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.SelectedCvar() == Num);
    CHECK(C.NumpadOpen());

    Num->Reset();
}

// ---- Folding a category hides its rows ---------------------------------------------------------
static void TestFoldHidesRows() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Num = Find("test.console.number");
    CHECK(Num != nullptr);
    if (Num == nullptr) return;

    float X = 0, Y = 0, W = 0, H = 0;
    CHECK(C.RowRect(Num, ScreenW, ScreenH, X, Y, W, H));   // visible to start with

    // Tap the header of the category it lives in. Its path is the name minus the leaf.
    const std::string Path = "test.console";
    CHECK(!C.IsFolded(Path));
    // Find the header row by tapping just above the first CVar row of that category... rather than
    // guess at pixels, drive the fold through the keyboard: arrow onto the category and press Enter.
    bool OnCat = false;
    for (int I = 0; I < 400 && !OnCat; ++I) {
        C.Key(0x28);
        C.Draw(&R, ScreenW, ScreenH);
        OnCat = (C.HighlightedCategory() == Path);
    }
    CHECK(OnCat);
    if (!OnCat) return;
    C.Key(0x0D);
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.IsFolded(Path));
    CHECK(!C.RowRect(Num, ScreenW, ScreenH, X, Y, W, H));   // folded away: not drawn, not tappable

    C.Key(0x0D);   // unfold again, so the rest of the suite sees the tree it expects
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(!C.IsFolded(Path));
    CHECK(C.RowRect(Num, ScreenW, ScreenH, X, Y, W, H));
}

// ---- The picker writes RGBA and commits LIVE ---------------------------------------------------
// "Live" is the whole point: a colour you must press Enter to see is a colour you cannot tune. So a
// single press inside the hue strip must both change the CVar and fire the commit hook.
static void TestPickerWritesLive() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Paint = Find("test.console.paint");
    CHECK(Paint != nullptr);
    if (Paint == nullptr) return;
    Paint->Reset();

    CHECK(TapRow(C, R, Paint));
    CHECK(C.PickerOpen());

    Console::PickerGeom PG{};
    CHECK(C.PickerGeometry(ScreenW, ScreenH, PG));
    if (!C.PickerGeometry(ScreenW, ScreenH, PG)) return;

    float Hx, Hy, Hw, Hh;
    ColorPicker::HueRect(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.StripH, PG.Gap, Hx, Hy, Hw,
                        Hh);
    float Before[4] = {0, 0, 0, 0};
    Paint->GetColorChannels(Before);

    Log.Clear();
    C.Tap(Hx + Hw * 0.5f, Hy + Hh * 0.5f);   // mid-strip: hue ~0.5 (cyan)
    C.Draw(&R, ScreenW, ScreenH);

    float After[4] = {0, 0, 0, 0};
    Paint->GetColorChannels(After);
    const bool Moved = After[0] != Before[0] || After[1] != Before[1] || After[2] != Before[2];
    CHECK(Moved);
    CHECK(Log.Saw("test.console.paint"));       // committed, not just mutated
    CHECK(Near(C.PickerHue(), 0.5f, 0.08f));    // the working hue is what was pressed

    Paint->Reset();
}

// ---- THE working-state rule: our own write must NOT re-derive HSV ------------------------------
// RGB->HSV cannot recover a hue when saturation or value is 0 — every grey, black and white has an
// undefined hue. So a picker that re-derives from the CVar every frame snaps the handle to red the
// instant a drag reaches the black edge of the square, and the drag becomes unusable exactly where
// you need it most. This test drags INTO black, sets a hue while sitting there, then comes back out:
// the colour that returns must be the hue that was chosen, not red.
static void TestHuesSurviveTheBlackEdge() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Paint = Find("test.console.paint");
    CHECK(Paint != nullptr);
    if (Paint == nullptr) return;
    Paint->Reset();

    CHECK(TapRow(C, R, Paint));
    CHECK(C.PickerOpen());

    auto Geom = [&](Console::PickerGeom& G) { return C.PickerGeometry(ScreenW, ScreenH, G); };
    Console::PickerGeom PG{};
    CHECK(Geom(PG));
    if (!Geom(PG)) return;

    float Qx, Qy, Qw, Qh;
    ColorPicker::SquareRect(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.Gap, Qx, Qy, Qw, Qh);
    float Hx, Hy, Hw, Hh;
    ColorPicker::HueRect(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.StripH, PG.Gap, Hx, Hy, Hw,
                        Hh);

    // 1. Drag to the very BOTTOM EDGE of the square: value EXACTLY 0, so the CVar becomes pure
    //    black and its RGB carries no hue information at all.
    //
    //    "Exactly" is load-bearing and was got wrong once. Tapping one pixel inside the edge gives
    //    V ~= 0.003, and RGB (0.001, 0.001, 0.003) still round-trips to the right hue — the ratios
    //    survive at tiny-but-nonzero values, so RgbToHsv recovers 0.667 perfectly and the degenerate
    //    case never happens. A tolerance-based "it is black now" check let that through and the
    //    whole test passed against a console that re-derived every frame. SvAt clamps and HitRect is
    //    inclusive, so the bottom edge itself is a hit with V == 0.
    C.Tap(Qx + Qw * 0.5f, Qy + Qh);
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.PickerVal() == 0.0f);
    float Black[4] = {1, 1, 1, 1};
    Paint->GetColorChannels(Black);
    // Exact, not near: HsvToRgb with V == 0 must produce exact zeros, and it is precisely the
    // exactness that destroys the hue.
    CHECK(Black[0] == 0.0f && Black[1] == 0.0f && Black[2] == 0.0f);

    // 2. Choose a hue while sitting on black. The CVar cannot change (black is black at any hue),
    //    so ONLY the working state records this. If the console re-derived HSV from the CVar it
    //    would immediately forget.
    CHECK(Geom(PG));
    C.Tap(Hx + Hw * 0.5f, Hy + Hh * 0.5f);
    C.Draw(&R, ScreenW, ScreenH);
    const float ChosenHue = C.PickerHue();
    CHECK(Near(ChosenHue, 0.5f, 0.08f));

    // 3. Come back out to full brightness. The colour that appears must be the hue chosen in step 2.
    //    Cyan is G and B high, R low; red (the snap-back failure) is the exact opposite.
    C.Tap(Qx + Qw, Qy);   // the top-right CORNER: S == 1, V == 1 exactly
    C.Draw(&R, ScreenW, ScreenH);
    float Out[4] = {0, 0, 0, 0};
    Paint->GetColorChannels(Out);
    CHECK(Near(C.PickerHue(), ChosenHue, 0.02f));
    CHECK(Out[1] > 0.8f);           // green high
    CHECK(Out[2] > 0.8f);           // blue high
    CHECK(Out[0] < 0.2f);           // red LOW — this is the assertion a re-deriving picker fails

    Paint->Reset();
}

// ---- An EXTERNAL edit does re-derive ----------------------------------------------------------
// The other half of the same rule, and it has to be tested separately or "never re-derive" would
// pass the test above while leaving the handle stuck wherever it was when someone typed a new value
// into the row or pressed its reset button.
static void TestExternalEditRederives() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Paint = Find("test.console.paint");
    CHECK(Paint != nullptr);
    if (Paint == nullptr) return;

    CHECK(Paint->SetFromString("0 0 1 1"));   // blue
    CHECK(TapRow(C, R, Paint));
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.PickerOpen());
    CHECK(Near(C.PickerHue(), 2.0f / 3.0f, 0.02f));   // blue sits at hue 2/3

    // Someone edits the value from outside the picker (a reset, a config reload, another tool).
    CHECK(Paint->SetFromString("1 1 0 1"));           // yellow, hue 1/6
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(Near(C.PickerHue(), 1.0f / 6.0f, 0.02f));

    Paint->Reset();
}

// ---- The "i" button opens a toaster, and only when there is something to say -------------------
static void TestTooltipToaster() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Num = Find("test.console.number");
    Lur::Core::ICVar* Quiet = Find("test.console.quiet");
    CHECK(Num != nullptr && Quiet != nullptr);
    if (Num == nullptr || Quiet == nullptr) return;

    float X = 0, Y = 0, W = 0, H = 0;
    CHECK(C.RowRect(Num, ScreenW, ScreenH, X, Y, W, H));
    CHECK(C.ToastText().empty());
    C.Tap(X + H * 0.4f, Y + H * 0.5f);   // the "i" sits at the row's left edge, a square of ~H
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(!C.ToastText().empty());
    CHECK(C.ToastText() == std::string(Num->Tooltip()));
    // The row must NOT also have been selected — the "i" is hit-tested before the row body, or
    // asking what a knob does would edit it.
    CHECK(C.SelectedCvar() != Num);

    // A visible toaster is modal-lite: the next tap anywhere dismisses it and is CONSUMED, so it
    // cannot also trigger the row underneath.
    CHECK(C.RowRect(Num, ScreenW, ScreenH, X, Y, W, H));
    C.Tap(X + W * 0.45f, Y + H * 0.5f);
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.ToastText().empty());
    CHECK(C.SelectedCvar() != Num);   // dismissal ate the press

    // A tooltip-less row's "i" is inert: no toaster.
    CHECK(C.RowRect(Quiet, ScreenW, ScreenH, X, Y, W, H));
    C.Tap(X + H * 0.4f, Y + H * 0.5f);
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.ToastText().empty());
}

// ---- Closing dismisses the editors -----------------------------------------------------------
// A numpad still bound to a row you can no longer see reappears mid-edit over an unrelated CVar the
// next time the console opens.
static void TestClosingDismissesEditors() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Num = Find("test.console.number");
    CHECK(Num != nullptr);
    if (Num == nullptr) return;

    CHECK(TapRow(C, R, Num));
    CHECK(C.NumpadOpen());
    C.Key('9');
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(C.NumpadBuffer() == "9");

    C.SetOpen(false);
    CHECK(!C.NumpadOpen());
    CHECK(!C.PickerOpen());
    CHECK(C.NumpadBuffer().empty());   // the half-typed value is gone, not banked

    C.SetOpen(true);
    R.Reset();
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(!C.NumpadOpen());
    Num->Reset();
}

// ---- Reset puts a row back to its default and commits -----------------------------------------
static void TestRowResetCommits() {
    RecordingRenderer R;
    Console C;
    CommitLog Log;
    Bring(C, R, Log);

    Lur::Core::ICVar* Num = Find("test.console.number");
    CHECK(Num != nullptr);
    if (Num == nullptr) return;

    CHECK(Num->SetFromString("99"));
    CHECK(Num->Overridden());

    float X = 0, Y = 0, W = 0, H = 0;
    CHECK(C.RowRect(Num, ScreenW, ScreenH, X, Y, W, H));
    Log.Clear();
    // The "R" button is the rightmost square in the row.
    C.Tap(X + W - H * 0.4f, Y + H * 0.5f);
    C.Draw(&R, ScreenW, ScreenH);
    CHECK(!Num->Overridden());
    CHECK(Log.Saw("test.console.number"));   // a reset persists too, or it un-resets on relaunch
}

int main() {
    Lur::Core::CVarEnterMain();   // CVars may not be read before main()
    TestClosedIsInvisibleAndInert();
    TestScrollQueuedWhileClosedIsDiscarded();
    TestOpenPaintsAndClaimsKeys();
    TestEditorPerType();
    TestNumpadCommitAndCancel();
    TestKeyboardCursor();
    TestFoldHidesRows();
    TestPickerWritesLive();
    TestHuesSurviveTheBlackEdge();
    TestExternalEditRederives();
    TestTooltipToaster();
    TestClosingDismissesEditors();
    TestRowResetCommits();
    if (GFailures == 0) std::printf("devgui_console_tests: ALL PASS\n");
    else std::printf("devgui_console_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
