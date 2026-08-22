// Tests for Lur::DevGui::FlatList — the dev console's fold-aware row list, promoted out of
// Rps::GameView (#121, #201).
//
// The property worth defending is that ONE array feeds painting, hit-testing and the anchored
// editors. A console where the row you tap is not the row you see is the classic dev-UI bug, and it
// arises precisely when the hit-test recomputes layout independently of the draw. So the headline test
// asserts that RowAtScreenY agrees with the ContentY a painter would have used, for every row, at
// several scroll offsets.
#include <cstdio>
#include <string>
#include <unordered_set>
#include <vector>

#include "Lur/DevGui/FlatList.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

using Lur::DevGui::BuildCategoryTree;
using Lur::DevGui::ClampScroll;
using Lur::DevGui::FlatRow;
using Lur::DevGui::FlattenTree;
using Lur::DevGui::RowAtScreenY;
using Lur::DevGui::RowScreenY;

using Leaf = const char*;
using Row = FlatRow<Leaf>;

// Deliberately far apart. The shipping console uses 20 vs 22, where a swapped-height hit-test
// shifts a band by 2px and a midpoint probe still lands in the right row — the sabotage passed
// until these were separated AND the probes moved to the row EDGES.
constexpr float LeafH = 20.0f;
constexpr float CatH = 44.0f;

// "a.b.c"-style names, split on '.', exactly as the CVar console does.
static Lur::DevGui::CatNode<Leaf> Tree() {
    std::vector<std::pair<std::string, Leaf>> Items = {
        {"boid.sep", "boid.sep"},
        {"boid.coh", "boid.coh"},
        {"boid.noise.amp", "boid.noise.amp"},
        {"boid.noise.hz", "boid.noise.hz"},
        {"unit.rock.hp", "unit.rock.hp"},
        {"", "loose"},          // no category at all -> a root leaf
    };
    return BuildCategoryTree(Items, '.');
}

static std::unordered_set<std::string> NoFolds;
static auto FoldSet(const std::unordered_set<std::string>& S) {
    return [&S](const std::string& P) { return S.find(P) != S.end(); };
}

// ---- Root leaves come FIRST and at depth 0 ----
// They belong to no category, so there is no header to fold them under; if they were emitted inside
// the tree walk they could be hidden with no way to reach them.
static void TestRootLeavesAlwaysFirstAndVisible() {
    auto Root = Tree();
    std::vector<Row> Rows;
    const float H = FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows);
    CHECK(!Rows.empty());
    // Guarded, not just CHECKed: a category row's Item is null, and CHECK does not early-return, so
    // an unguarded std::string(Item) turns a clean assertion failure into a crash. A sabotage that
    // crashes is still detected, but it names nothing.
    CHECK(!Rows.empty() && !Rows[0].IsCategory);
    if (!Rows.empty() && !Rows[0].IsCategory) {
        CHECK(std::string(Rows[0].Item) == "loose");
        CHECK(Rows[0].Depth == 0);
        CHECK(Rows[0].ContentY == 0.0f);
    }
    CHECK(H > 0.0f);
    // Folding every category cannot hide it.
    std::unordered_set<std::string> All = {"boid", "boid.noise", "unit", "unit.rock"};
    std::vector<Row> Folded;
    FlattenTree(Root, FoldSet(All), LeafH, CatH, Folded);
    bool Found = false;
    for (const Row& R : Folded)
        if (!R.IsCategory && R.Item != nullptr && std::string(R.Item) == "loose") Found = true;
    CHECK(Found);
}

// ---- ContentY advances by the row's OWN height, never a mixed one ----
// Categories are taller than leaves. Using one height for both is the drift that makes the bottom of
// a long list un-tappable.
static void TestContentYUsesPerKindHeights() {
    auto Root = Tree();
    std::vector<Row> Rows;
    const float H = FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows);
    float Expect = 0.0f;
    for (const Row& R : Rows) {
        CHECK(R.ContentY == Expect);
        Expect += R.IsCategory ? CatH : LeafH;
    }
    CHECK(H == Expect);   // the returned height is exactly where the last row ended
}

// ---- Folding a PARENT hides the whole subtree, nested categories included ----
static void TestFoldingParentHidesNestedSubtree() {
    auto Root = Tree();
    std::vector<Row> Open, Shut;
    FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Open);
    std::unordered_set<std::string> F = {"boid"};
    const float ShutH = FlattenTree(Root, FoldSet(F), LeafH, CatH, Shut);
    CHECK(Shut.size() < Open.size());
    for (const Row& R : Shut) {
        // "boid" itself must still be present (it is what you tap to unfold)...
        if (R.IsCategory) CHECK(R.Node->Path != "boid.noise");
        // ...but nothing from under it.
        else CHECK(R.Item != nullptr && std::string(R.Item).rfind("boid.", 0) != 0);
    }
    bool HasBoidHeader = false;
    for (const Row& R : Shut)
        if (R.IsCategory && R.Node->Path == "boid") HasBoidHeader = true;
    CHECK(HasBoidHeader);
    CHECK(ShutH > 0.0f);
}

// ---- Folding a CHILD hides only that child ----
static void TestFoldingChildIsIndependent() {
    auto Root = Tree();
    std::unordered_set<std::string> F = {"boid.noise"};
    std::vector<Row> Rows;
    FlattenTree(Root, FoldSet(F), LeafH, CatH, Rows);
    bool HasSep = false, HasAmp = false, HasNoiseHeader = false;
    for (const Row& R : Rows) {
        if (R.IsCategory && R.Node->Path == "boid.noise") HasNoiseHeader = true;
        if (!R.IsCategory && std::string(R.Item) == "boid.sep") HasSep = true;
        if (!R.IsCategory && std::string(R.Item) == "boid.noise.amp") HasAmp = true;
    }
    CHECK(HasNoiseHeader);
    CHECK(HasSep);     // sibling content under "boid" is untouched
    CHECK(!HasAmp);    // the folded child's leaves are gone
}

// ---- THE HEADLINE: the hit-test agrees with the paint, at every scroll offset ----
// RowAtScreenY must return exactly the row whose ContentY a painter would have used. If these two ever
// disagree, tapping row N edits row M.
static void TestHitTestAgreesWithPaintAtEveryScroll() {
    auto Root = Tree();
    std::vector<Row> Rows;
    const float ContentH = FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows);
    const float ViewTop = 100.0f, ViewH = 90.0f, ViewBot = ViewTop + ViewH;
    for (float S = 0.0f; S <= ContentH; S += 7.0f) {
        float Scroll = S;
        ClampScroll(Scroll, ContentH, ViewH);
        for (std::size_t I = 0; I < Rows.size(); ++I) {
            const float Top = ViewTop - Scroll + Rows[I].ContentY;
            const float Hgt = Rows[I].IsCategory ? CatH : LeafH;
            // Probe the TOP EDGE, the middle and just inside the BOTTOM edge. The edges are what
            // catch a wrong row height: a midpoint stays inside a band that is a few px too tall or
            // too short, while a point 0.5px above the true bottom lands in the NEXT row the moment
            // the height is wrong.
            const float Probes[3] = {Top, Top + Hgt * 0.5f, Top + Hgt - 0.5f};
            for (float P : Probes) {
                const int Hit = RowAtScreenY(Rows, P, ViewTop, ViewBot, Scroll, LeafH, CatH);
                if (P >= ViewTop && P < ViewBot) CHECK(Hit == static_cast<int>(I));
                else CHECK(Hit == -1);   // scrolled out of the band: not hittable
            }
        }
    }
}

// ---- Taps outside the viewport band never hit, however the arithmetic lands ----
static void TestTapsOutsideTheBandMiss() {
    auto Root = Tree();
    std::vector<Row> Rows;
    FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows);
    const float ViewTop = 100.0f, ViewBot = 190.0f;
    CHECK(RowAtScreenY(Rows, 99.9f, ViewTop, ViewBot, 0.0f, LeafH, CatH) == -1);
    CHECK(RowAtScreenY(Rows, 190.0f, ViewTop, ViewBot, 0.0f, LeafH, CatH) == -1);
    CHECK(RowAtScreenY(Rows, -500.0f, ViewTop, ViewBot, 0.0f, LeafH, CatH) == -1);
    CHECK(RowAtScreenY(Rows, ViewTop, ViewTop, ViewBot, 0.0f, LeafH, CatH) == 0);  // the band is inclusive at the top
}

// ---- Scroll clamps, and content shorter than the view pins to zero ----
// The pin is what stops a fold from stranding the view past the end of the content, showing blank
// space with no hint that you must scroll UP.
static void TestScrollClamp() {
    float S = 0.0f;
    CHECK(ClampScroll(S, /*ContentH*/ 500.0f, /*ViewH*/ 100.0f) == 400.0f);
    S = 1000.0f;
    ClampScroll(S, 500.0f, 100.0f);
    CHECK(S == 400.0f);
    S = -50.0f;
    ClampScroll(S, 500.0f, 100.0f);
    CHECK(S == 0.0f);
    // Content that fits: max is 0 and any offset pins to 0.
    S = 250.0f;
    CHECK(ClampScroll(S, 40.0f, 100.0f) == 0.0f);
    CHECK(S == 0.0f);
    // Exactly filling the viewport is not scrollable.
    S = 5.0f;
    CHECK(ClampScroll(S, 100.0f, 100.0f) == 0.0f);
    CHECK(S == 0.0f);
}

// ---- RowScreenY finds a visible row, and falls back when the row is folded away ----
// An editor stays open across a fold; anchoring it to a stale Y parks it off-screen with no way back.
static void TestRowScreenYFallsBackWhenFolded() {
    auto Root = Tree();
    std::vector<Row> Rows;
    FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows);
    Leaf Amp = nullptr;
    for (const Row& R : Rows)
        if (!R.IsCategory && std::string(R.Item) == "boid.noise.amp") Amp = R.Item;
    CHECK(Amp != nullptr);
    const float Y = RowScreenY(Rows, Amp, /*ViewTop*/ 100.0f, /*Scroll*/ 0.0f, /*Fallback*/ -999.0f);
    CHECK(Y != -999.0f);
    // Scroll shifts it by exactly the offset.
    const float Y2 = RowScreenY(Rows, Amp, 100.0f, 30.0f, -999.0f);
    CHECK(Y2 == Y - 30.0f);
    // Fold its category: the row is gone, so the fallback is used.
    std::unordered_set<std::string> F = {"boid.noise"};
    std::vector<Row> Folded;
    FlattenTree(Root, FoldSet(F), LeafH, CatH, Folded);
    CHECK(RowScreenY(Folded, Amp, 100.0f, 0.0f, -999.0f) == -999.0f);
}

// ---- An empty tree is a valid, empty list ----
static void TestEmptyTree() {
    std::vector<std::pair<std::string, Leaf>> None;
    auto Root = BuildCategoryTree(None, '.');
    std::vector<Row> Rows;
    CHECK(FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows) == 0.0f);
    CHECK(Rows.empty());
    CHECK(RowAtScreenY(Rows, 120.0f, 100.0f, 190.0f, 0.0f, LeafH, CatH) == -1);
    CHECK(RowScreenY(Rows, Leaf{"x"}, 100.0f, 0.0f, -7.0f) == -7.0f);
    float S = 9.0f;
    CHECK(ClampScroll(S, 0.0f, 100.0f) == 0.0f);
}

// ---- Out is CLEARED, so reflattening every frame does not accumulate ----
static void TestFlattenClearsOut() {
    auto Root = Tree();
    std::vector<Row> Rows;
    const float A = FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows);
    const std::size_t N = Rows.size();
    const float B = FlattenTree(Root, FoldSet(NoFolds), LeafH, CatH, Rows);
    CHECK(Rows.size() == N);
    CHECK(A == B);
}

int main() {
    TestRootLeavesAlwaysFirstAndVisible();
    TestContentYUsesPerKindHeights();
    TestFoldingParentHidesNestedSubtree();
    TestFoldingChildIsIndependent();
    TestHitTestAgreesWithPaintAtEveryScroll();
    TestTapsOutsideTheBandMiss();
    TestScrollClamp();
    TestRowScreenYFallsBackWhenFolded();
    TestEmptyTree();
    TestFlattenClearsOut();
    if (GFailures == 0) std::printf("devgui_flatlist_tests: ALL PASS\n");
    else std::printf("devgui_flatlist_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
