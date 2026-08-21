// Tests for Lur::Sim::CsrBuckets — the deterministic broadphase index promoted out of RPS (#201).
//
// The assertions that matter are the determinism contract, not "the buckets contain the right items":
// ascending-source-order within a cell, fixed bin order, and a grid query that visits EXACTLY the set
// brute force would. Those are the properties whose violation shows up as two phones diverging with
// nothing on the wire to blame.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Lur/Sim/CsrBuckets.h"

static int GFailures = 0;
#define CHECK(Cond)                                                     \
    do {                                                                \
        if (!(Cond)) {                                                  \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #Cond); \
            ++GFailures;                                                \
        }                                                               \
    } while (0)

namespace {

constexpr int MaxItems = 64;
constexpr int Cols = 4, Rows = 3, Cells = Cols * Rows;

struct Item { int X, Y; bool Alive; };

int CellOf(const Item& It) { return It.Y * Cols + It.X; }

using Buckets = Lur::Sim::CsrBuckets<MaxItems, Cells>;

std::vector<Item> MakeItems() {
    // Deliberately unsorted by cell, with holes, so a scatter bug shows up.
    return {
        {1, 1, true},   // 0 -> cell 5
        {0, 0, true},   // 1 -> cell 0
        {1, 1, true},   // 2 -> cell 5
        {3, 2, false},  // 3 -> excluded
        {0, 0, true},   // 4 -> cell 0
        {2, 0, true},   // 5 -> cell 2
        {1, 1, true},   // 6 -> cell 5
        {0, 0, false},  // 7 -> excluded
    };
}

}  // namespace

// ---- CSR shape: Start is non-decreasing, spans exactly the included items ----
static void TestCsrShape() {
    const std::vector<Item> Items = MakeItems();
    Buckets B;
    B.Build(static_cast<int32_t>(Items.size()), Cells,
            [&](int32_t I) { return CellOf(Items[I]); },
            [&](int32_t I) { return Items[I].Alive; });
    CHECK(B.Start[0] == 0);
    for (int C = 0; C < Cells; ++C) CHECK(B.Start[C] <= B.Start[C + 1]);
    int Included = 0;
    for (const Item& It : Items) if (It.Alive) ++Included;
    CHECK(B.Start[Cells] == Included);
}

// ---- THE TIE-BREAK: ids ascend within every cell ----
// This is the property a grid query's determinism rests on. Scattering in descending order, or with a
// shared cursor, still produces a "valid-looking" index that visits neighbours in a different order.
static void TestAscendingWithinEachCell() {
    const std::vector<Item> Items = MakeItems();
    Buckets B;
    B.Build(static_cast<int32_t>(Items.size()), Cells,
            [&](int32_t I) { return CellOf(Items[I]); },
            [&](int32_t I) { return Items[I].Alive; });
    for (int C = 0; C < Cells; ++C)
        for (int P = B.Start[C] + 1; P < B.Start[C + 1]; ++P)
            CHECK(B.Order[P - 1] < B.Order[P]);
    // Cell 5 holds items 0, 2, 6 in exactly that order.
    const int C5 = 1 * Cols + 1;
    CHECK(B.Start[C5 + 1] - B.Start[C5] == 3);
    CHECK(B.Order[B.Start[C5] + 0] == 0);
    CHECK(B.Order[B.Start[C5] + 1] == 2);
    CHECK(B.Order[B.Start[C5] + 2] == 6);
}

// ---- Excluded items appear nowhere ----
static void TestExcludedItemsAbsent() {
    const std::vector<Item> Items = MakeItems();
    Buckets B;
    B.Build(static_cast<int32_t>(Items.size()), Cells,
            [&](int32_t I) { return CellOf(Items[I]); },
            [&](int32_t I) { return Items[I].Alive; });
    for (int P = 0; P < B.Start[Cells]; ++P) {
        CHECK(Items[static_cast<size_t>(B.Order[P])].Alive);
        CHECK(B.Order[P] != 3 && B.Order[P] != 7);
    }
}

// ---- OnPlaced fires once per placed item, at its Order slot ----
static void TestOnPlacedPacksInOrder() {
    const std::vector<Item> Items = MakeItems();
    Buckets B;
    int Packed[MaxItems];
    for (int& V : Packed) V = -1;
    int Calls = 0;
    B.Build(static_cast<int32_t>(Items.size()), Cells,
            [&](int32_t I) { return CellOf(Items[I]); },
            [&](int32_t I) { return Items[I].Alive; },
            [&](int32_t P, int32_t I) { Packed[P] = I; ++Calls; });
    CHECK(Calls == B.Start[Cells]);
    for (int P = 0; P < B.Start[Cells]; ++P) CHECK(Packed[P] == B.Order[P]);
}

// ---- GRID == BRUTE FORCE, including visit ORDER ----
// The real guard. A 3x3-cell neighbourhood query must visit exactly the items brute force would, in
// the same sequence — same set AND same order, because a sim that sums in a different order can
// diverge even when the set matches.
static void TestGridQueryEqualsBruteForce() {
    std::vector<Item> Items;
    for (int I = 0; I < 40; ++I)
        Items.push_back({(I * 7) % Cols, (I * 5) % Rows, (I % 5) != 0});  // pseudo-scattered, some dead
    Buckets B;
    B.Build(static_cast<int32_t>(Items.size()), Cells,
            [&](int32_t I) { return CellOf(Items[I]); },
            [&](int32_t I) { return Items[I].Alive; });

    for (int Qy = 0; Qy < Rows; ++Qy)
        for (int Qx = 0; Qx < Cols; ++Qx) {
            // Grid walk: cells in fixed index order, items in Order order.
            std::vector<int> ViaGrid;
            for (int Cy = Qy - 1; Cy <= Qy + 1; ++Cy) {
                if (Cy < 0 || Cy >= Rows) continue;
                for (int Cx = Qx - 1; Cx <= Qx + 1; ++Cx) {
                    if (Cx < 0 || Cx >= Cols) continue;
                    const int C = Cy * Cols + Cx;
                    for (int P = B.Start[C]; P < B.Start[C + 1]; ++P) ViaGrid.push_back(B.Order[P]);
                }
            }
            // Brute force, visiting cells in the SAME fixed order and items ascending — which is what
            // the grid must reproduce.
            std::vector<int> ViaBrute;
            for (int Cy = Qy - 1; Cy <= Qy + 1; ++Cy) {
                if (Cy < 0 || Cy >= Rows) continue;
                for (int Cx = Qx - 1; Cx <= Qx + 1; ++Cx) {
                    if (Cx < 0 || Cx >= Cols) continue;
                    for (int I = 0; I < static_cast<int>(Items.size()); ++I)
                        if (Items[static_cast<size_t>(I)].Alive && Items[static_cast<size_t>(I)].X == Cx &&
                            Items[static_cast<size_t>(I)].Y == Cy)
                            ViaBrute.push_back(I);
                }
            }
            CHECK(ViaGrid == ViaBrute);
        }
}

// ---- Rebuilding is idempotent: same input, same index, no residue from the previous tick ----
static void TestRebuildIsClean() {
    std::vector<Item> Items = MakeItems();
    Buckets B;
    auto Go = [&] {
        B.Build(static_cast<int32_t>(Items.size()), Cells,
                [&](int32_t I) { return CellOf(Items[I]); },
                [&](int32_t I) { return Items[I].Alive; });
    };
    Go();
    const int32_t FirstTotal = B.Start[Cells];
    std::vector<int32_t> FirstOrder(B.Order, B.Order + FirstTotal);
    // A busy tick in between: everything alive, then back to the original.
    for (Item& It : Items) It.Alive = true;
    Go();
    Items = MakeItems();
    Go();
    CHECK(B.Start[Cells] == FirstTotal);
    for (int32_t P = 0; P < FirstTotal; ++P) CHECK(B.Order[P] == FirstOrder[static_cast<size_t>(P)]);
}

int main() {
    TestCsrShape();
    TestAscendingWithinEachCell();
    TestExcludedItemsAbsent();
    TestOnPlacedPacksInOrder();
    TestGridQueryEqualsBruteForce();
    TestRebuildIsClean();
    if (GFailures == 0) std::printf("csr_buckets_tests: ALL PASS\n");
    else std::printf("csr_buckets_tests: %d FAILURE(S)\n", GFailures);
    return GFailures == 0 ? 0 : 1;
}
