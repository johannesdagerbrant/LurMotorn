#pragma once
// Lur::DevGui::CategoryTree — turn a flat list of (category-path, leaf) pairs into a nested
// tree, splitting each path on a separator (default '|'). It's how the dev console renders
// HIERARCHICAL CVar categories: a CVar's Category() like "Units|Rock" or "Boid|Noise" becomes
// a recursively-collapsible section under "Units" / "Boid".
//
// GENERIC over the leaf type (no Core dependency) so it stays pure logic + host-testable with
// a dummy leaf; the CVar view instantiates it with Lur::Core::ICVar*. Deterministic: children
// are sorted by segment name and each node's TotalLeaves (whole-subtree count) is computed, so
// the layout is stable frame-to-frame. Leaves keep the caller's order — feed them pre-sorted
// (e.g. by CVar name) and each node's leaves come out sorted.
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace Lur::DevGui {

template <class Leaf>
struct CatNode {
    std::string           Segment;      // this level's name ("Rock"); empty on the root
    std::string           Path;         // full path to here ("Units|Rock"); the collapse key
    std::vector<CatNode>  Children;     // sub-categories, sorted by Segment
    std::vector<Leaf>     Leaves;       // CVars directly in this category, in caller order
    int                   TotalLeaves = 0;  // leaves in this whole subtree (for the header count)
};

namespace Detail {
// Find-or-create the direct child named Segment (Path = parent path joined by Sep).
template <class Leaf>
CatNode<Leaf>& ChildFor(CatNode<Leaf>& Node, const std::string& Segment, char Sep) {
    for (auto& C : Node.Children)
        if (C.Segment == Segment) return C;
    CatNode<Leaf> Fresh;
    Fresh.Segment = Segment;
    Fresh.Path = Node.Path.empty() ? Segment : (Node.Path + Sep + Segment);
    Node.Children.push_back(std::move(Fresh));
    return Node.Children.back();
}

template <class Leaf>
int Finalize(CatNode<Leaf>& Node) {
    std::sort(Node.Children.begin(), Node.Children.end(),
              [](const CatNode<Leaf>& A, const CatNode<Leaf>& B) { return A.Segment < B.Segment; });
    int Total = static_cast<int>(Node.Leaves.size());
    for (auto& C : Node.Children) Total += Finalize(C);
    Node.TotalLeaves = Total;
    return Total;
}
}  // namespace Detail

// Build the tree. Each item is (categoryPath, leaf); an empty path drops the leaf at the root.
// A path splits on Sep into segments ("Units|Rock" -> {"Units","Rock"}); empty segments (a
// leading/trailing/doubled Sep) are skipped, so "Units||Rock" == "Units|Rock".
template <class Leaf>
CatNode<Leaf> BuildCategoryTree(const std::vector<std::pair<std::string, Leaf>>& Items,
                                char Sep = '|') {
    CatNode<Leaf> Root;
    for (const auto& [Path, Leaf_] : Items) {
        CatNode<Leaf>* Node = &Root;
        std::string Seg;
        for (size_t I = 0; I <= Path.size(); ++I) {
            if (I == Path.size() || Path[I] == Sep) {
                if (!Seg.empty()) { Node = &Detail::ChildFor(*Node, Seg, Sep); Seg.clear(); }
            } else {
                Seg.push_back(Path[I]);
            }
        }
        Node->Leaves.push_back(Leaf_);
    }
    Detail::Finalize(Root);
    return Root;
}

}  // namespace Lur::DevGui
