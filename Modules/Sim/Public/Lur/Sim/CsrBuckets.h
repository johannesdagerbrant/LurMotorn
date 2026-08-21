#pragma once
// Deterministic counting-sort bucketing into CSR (compressed-sparse-row) form: the broadphase index
// a fixed-timestep sim rebuilds every tick.
//
// Promoted out of Rps::Sim's `Grid` and `MineGrid` (#201), which had the same four-step counting sort
// written out twice. It is the physics game's broadphase, but the reason it belongs in the engine is
// the DETERMINISM CONTRACT below, not the speed.
//
// ---- The contract, which is the whole point ----
//   * ZERO allocation. Fixed arrays sized by the template parameters; rebuilt in place each tick.
//   * FIXED bin-iteration order. Cells are visited by index, so a query walks the same cells in the
//     same order on every peer.
//   * ASCENDING SOURCE ORDER within each cell. This is the tie-break: two items in one cell are
//     always visited lowest-id first. It falls out of scattering in ascending `I` with a per-cell
//     cursor, and it is the property that makes a grid query bit-identical to brute force.
//   * TRANSIENT. This is scratch, never simulation state — it must not be hashed or snapshotted.
//     Cell size is therefore a pure performance knob: any value gives identical results.
//
// Get any of those wrong and the two phones diverge with nothing on the wire to point at, which is
// why the tests assert grid-equals-brute-force rather than just "the buckets look right".
//
// The caller supplies the cell mapping and the include predicate, so items can be excluded (a dead
// unit, a depleted mine) without a second pass and without the engine knowing what an item is.
#include <cstdint>

namespace Lur::Sim {

template <int MaxItems, int MaxCells>
class CsrBuckets {
public:
    // Cell C's items are Order[Start[C] .. Start[C + 1]).
    int32_t Start[MaxCells + 1] = {};
    int32_t Order[MaxItems] = {};

    // Rebuild for `Count` candidate items over `Cells` cells (Cells <= MaxCells).
    //
    //   CellOf(I)     -> cell index in [0, Cells) for item I
    //   Include(I)    -> whether item I participates this tick
    //   OnPlaced(P,I) -> called as item I lands at Order[P], in Order order. This is the hook for
    //                    packing a cache-local payload alongside the index; pass a no-op lambda if
    //                    there is nothing to pack.
    //
    // OnPlaced runs in the scatter pass on purpose: the scattered reads of item I's fields happen
    // ONCE here, amortised over the O(items x neighbours) queries that follow, instead of once per
    // neighbour visit.
    template <class CellOfFn, class IncludeFn, class OnPlacedFn>
    void Build(int32_t Count, int32_t Cells, CellOfFn CellOf, IncludeFn Include, OnPlacedFn OnPlaced) {
        for (int32_t C = 0; C <= Cells; ++C) Start[C] = 0;
        // Count into Start[cell + 1], then prefix-sum so Start[cell] is that cell's bucket offset.
        for (int32_t I = 0; I < Count; ++I)
            if (Include(I)) ++Start[CellOf(I) + 1];
        for (int32_t C = 1; C <= Cells; ++C) Start[C] += Start[C - 1];
        // Scatter in ASCENDING item order, so ids stay ascending inside each cell (the tie-break).
        int32_t Cursor[MaxCells];
        for (int32_t C = 0; C < Cells; ++C) Cursor[C] = Start[C];
        for (int32_t I = 0; I < Count; ++I)
            if (Include(I)) {
                const int32_t P = Cursor[CellOf(I)]++;
                Order[P] = I;
                OnPlaced(P, I);
            }
    }

    // Convenience overload for an index with no packed payload.
    template <class CellOfFn, class IncludeFn>
    void Build(int32_t Count, int32_t Cells, CellOfFn CellOf, IncludeFn Include) {
        Build(Count, Cells, CellOf, Include, [](int32_t, int32_t) {});
    }
};

}  // namespace Lur::Sim
