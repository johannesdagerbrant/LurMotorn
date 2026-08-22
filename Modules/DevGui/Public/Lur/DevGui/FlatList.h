#pragma once
// Lur::DevGui::FlatList — flatten a CategoryTree into the linear, scrollable row list a dev console
// actually draws, honouring which categories are folded.
//
// Promoted out of Rps::GameView's console block (#121, #201).
//
// ---- WHY THIS IS THE PART THAT MOVES ----
// #201 sizes "the dev console" at ~690 lines and points them all at Modules/DevGui. Most of those
// lines are PAINTING, and by this module's own convention painting stays in the game: every DevGui
// widget so far (Numpad, ColorPicker) is "pure logic + shared geometry" with no renderer in sight,
// precisely so the geometry that the renderer draws and the geometry that the hit-test uses are the
// SAME call and cannot drift. The tree itself was already promoted (CategoryTree). What was left
// stranded is this: the fold-aware flatten, the scroll clamp, and the row lookup the anchored popovers
// need. That is the reusable core; the ~200 lines of quad-and-text calls around it are the game's.
//
// ---- ONE ARRAY, THREE READERS ----
// Scroll, culling, hit-testing and the anchored numpad/toaster all read the SAME flattened array with
// the same per-row content-Y. That is the invariant worth having in one place: a console where the row
// you tap is not the row you see is the classic dev-UI bug, and it arises exactly when hit-testing
// recomputes layout independently of drawing.
//
// Generic over the leaf type (no Core dependency), so it stays host-testable with a dummy leaf; the
// CVar console instantiates it with Lur::Core::ICVar*.
//
// The recursion is a plain function template rather than a std::function lambda — the original
// allocated one closure per frame while the overlay was open, for a walk that never needed the
// indirection.
#include <vector>

#include "Lur/DevGui/CategoryTree.h"

namespace Lur::DevGui {

// One row of the flattened view. A row is either a category header or a leaf, never both.
template <class Leaf>
struct FlatRow {
    bool IsCategory = false;
    const CatNode<Leaf>* Node = nullptr;   // set when IsCategory
    Leaf Item{};                           // set when !IsCategory
    int Depth = 0;                         // indent level; root-level leaves are 0
    float ContentY = 0.0f;                 // offset into the SCROLLABLE content, not the screen
};

namespace Detail {
template <class Leaf, class CollapsedFn>
void FlattenNode(const CatNode<Leaf>& N, int Depth, CollapsedFn& IsCollapsed, float LeafH,
                 float CatH, float& Cy, std::vector<FlatRow<Leaf>>& Out) {
    FlatRow<Leaf> Header;
    Header.IsCategory = true;
    Header.Node = &N;
    Header.Depth = Depth;
    Header.ContentY = Cy;
    Out.push_back(Header);
    Cy += CatH;
    // Folded: the header still occupies a row (it is what you tap to unfold), but the body is
    // skipped entirely — including nested categories, so folding a parent hides the whole subtree.
    if (IsCollapsed(N.Path)) return;
    for (const Leaf& L : N.Leaves) {
        FlatRow<Leaf> Row;
        Row.Item = L;
        Row.Depth = Depth + 1;
        Row.ContentY = Cy;
        Out.push_back(Row);
        Cy += LeafH;
    }
    for (const CatNode<Leaf>& Ch : N.Children)
        FlattenNode(Ch, Depth + 1, IsCollapsed, LeafH, CatH, Cy, Out);
}
}  // namespace Detail

// Flatten Root into Out (cleared first) and return the total content height.
//
// Root's OWN leaves come first, at depth 0: they belong to no category, so there is no header to fold
// them under and they must always be visible. Then each child subtree, in the tree's order (which
// CategoryTree already sorted).
//
// IsCollapsed(const std::string& Path) -> bool. Called per category, keyed on the FULL path, so
// folding "Boid" and folding "Units|Boid" are different states.
template <class Leaf, class CollapsedFn>
float FlattenTree(const CatNode<Leaf>& Root, CollapsedFn IsCollapsed, float LeafH, float CatH,
                  std::vector<FlatRow<Leaf>>& Out) {
    Out.clear();
    float Cy = 0.0f;
    for (const Leaf& L : Root.Leaves) {
        FlatRow<Leaf> Row;
        Row.Item = L;
        Row.Depth = 0;
        Row.ContentY = Cy;
        Out.push_back(Row);
        Cy += LeafH;
    }
    for (const CatNode<Leaf>& Ch : Root.Children)
        Detail::FlattenNode(Ch, 0, IsCollapsed, LeafH, CatH, Cy, Out);
    return Cy;
}

// Clamp a scroll offset into range and return the maximum. Content shorter than the viewport pins to
// 0 — without that, folding a category while scrolled down leaves the view stranded past the end,
// showing empty space with no way to know you must scroll UP to find the content.
inline float ClampScroll(float& ScrollY, float ContentH, float ViewH) {
    const float MaxScroll = ContentH > ViewH ? ContentH - ViewH : 0.0f;
    if (ScrollY < 0.0f) ScrollY = 0.0f;
    if (ScrollY > MaxScroll) ScrollY = MaxScroll;
    return MaxScroll;
}

// Screen Y of the row holding Item, or Fallback when that row is not currently visible (its category
// is folded, or it is gone from the registry). The anchored editors — a numpad, a tooltip — sit
// relative to their row, and a fold can remove that row while the editor is still open; anchoring to
// a stale Y would park the pad off-screen with no way back to it.
template <class Leaf>
float RowScreenY(const std::vector<FlatRow<Leaf>>& Rows, const Leaf& Item, float ViewTop,
                 float ScrollY, float Fallback) {
    for (const FlatRow<Leaf>& R : Rows)
        if (!R.IsCategory && R.Item == Item) return ViewTop - ScrollY + R.ContentY;
    return Fallback;
}

// The row at screen Y, or -1 for none. Uses the SAME ContentY the caller drew from, which is the
// whole point of flattening once: tap and paint cannot disagree. Rows scrolled outside [ViewTop,
// ViewBot) are not hittable even though their arithmetic would place them there.
template <class Leaf>
int RowAtScreenY(const std::vector<FlatRow<Leaf>>& Rows, float Y, float ViewTop, float ViewBot,
                 float ScrollY, float LeafH, float CatH) {
    if (Y < ViewTop || Y >= ViewBot) return -1;
    for (std::size_t I = 0; I < Rows.size(); ++I) {
        const float Top = ViewTop - ScrollY + Rows[I].ContentY;
        const float Bot = Top + (Rows[I].IsCategory ? CatH : LeafH);
        if (Y >= Top && Y < Bot) return static_cast<int>(I);
    }
    return -1;
}

// ---- The keyboard CURSOR over the flattened list -----------------------------------------------
// A dev console gets arrow keys on desktop and none on a phone, so the cursor is separate state from
// the open editor: arrows move the cursor, Enter promotes it. What follows is the part that is the
// same for any list.

// Index of the row holding Item, or -1. The cursor is stored as a KEY, never an index, and these are
// how it is resolved each frame — because the flattened list is rebuilt every frame and its indices
// shift the moment any category folds. An index would quietly start pointing at a different row after
// an expand/collapse, which reads as the cursor teleporting.
template <class Leaf>
int FindLeafRow(const std::vector<FlatRow<Leaf>>& Rows, const Leaf& Item) {
    for (std::size_t I = 0; I < Rows.size(); ++I)
        if (!Rows[I].IsCategory && Rows[I].Item == Item) return static_cast<int>(I);
    return -1;
}

template <class Leaf>
int FindCategoryRow(const std::vector<FlatRow<Leaf>>& Rows, const std::string& Path) {
    if (Path.empty()) return -1;
    for (std::size_t I = 0; I < Rows.size(); ++I)
        if (Rows[I].IsCategory && Rows[I].Node != nullptr && Rows[I].Node->Path == Path)
            return static_cast<int>(I);
    return -1;
}

// Step a cursor index by Delta and clamp it into the list.
//
// Current < 0 means "nothing highlighted yet", and then the step enters from the NEAR END: a downward
// move starts at the top, an upward move starts at the bottom. Without that, the first arrow press
// after opening the console does nothing visible, because a clamped -1 + 1 lands on the same row a
// fresh 0 would and the user cannot tell whether the key registered.
//
// Returns -1 only for an empty list.
inline int StepIndex(int Current, int Delta, int Count) {
    if (Count <= 0) return -1;
    int I = Current;
    if (I < 0) I = (Delta > 0) ? -1 : Count;   // enter from the near end
    I += Delta;
    if (I < 0) I = 0;
    if (I >= Count) I = Count - 1;
    return I;
}

// Scroll the MINIMUM amount that brings [RowContentY, RowContentY + RowH) fully into a ViewH-tall
// viewport. Already-visible rows do not move the view at all, which is what keeps arrowing through the
// middle of a long list from jittering.
//
// The bug this fixes: without it, Down walks the cursor off the bottom of the clip band and keeps
// going INVISIBLY — the list looks frozen, and then jumps several rows the moment you press Enter.
inline void ScrollToReveal(float RowContentY, float RowH, float& ScrollY, float ViewH,
                           float MaxScroll) {
    if (RowContentY < ScrollY) ScrollY = RowContentY;
    else if (RowContentY + RowH > ScrollY + ViewH) ScrollY = RowContentY + RowH - ViewH;
    if (ScrollY < 0.0f) ScrollY = 0.0f;
    if (ScrollY > MaxScroll) ScrollY = MaxScroll;
}

}  // namespace Lur::DevGui
