#pragma once
// Absorbs a rollback correction into a per-slot visual offset and decays it away, so a corrected
// entity reads as fast motion instead of a teleport.
//
// Promoted out of Rps::GameView (#201), whose header already said "this is the piece that transfers to
// the physics game". Three things are being promoted, and only one of them is the arithmetic.
//
// ---- 1. What it does ----
// In normal play, snapshot N's Pos equals snapshot N+1's Prev, because the sim sets Prev = Pos each
// step — the interpolation endpoints CHAIN. A rollback breaks the chain: the corrected timeline puts
// the entity somewhere else, so Prev jumps. Absorb that jump into an error offset, apply the offset to
// BOTH interpolation endpoints (so the whole segment slides), and decay it toward zero over a few
// frames. Cost is nil when nothing rolled back — the discontinuity is then exactly zero.
//
// This is NOT extrapolation. The renderer still interpolates between two real sim states; the offset
// only shifts where that segment is drawn. (CLAUDE.md: the renderer interpolates, never predicts.)
//
// ---- 2. A SLOT INDEX IS NOT AN IDENTITY ----
// The load-bearing part, and the one that shipped a bug. Smoothing may only happen when the slot still
// holds the SAME ENTITY, tested by a per-creation serial that is never reused — not by (type, team)
// plus a distance heuristic.
//
// Why the heuristic cannot work: a rollback re-runs every allocation made inside the resim window, so
// ONE extra entity in the corrected timeline slides every later spawn down one slot. Slot I then holds
// its former neighbour — same team, same type, about one world unit away — which is indistinguishable
// from a real correction. The old test called that "the same unit" and eased each new spawn in from
// its neighbour's position; with real motion on top it read as a swinging arc on everything freshly
// built. A serial mismatch means NEW OCCUPANT, and a new occupant must SNAP.
//
// Observe() returns that verdict, because the caller almost always has more per-slot view state with
// the same problem — held facing, a deposit-edge latch, a trail — and all of it must be reset on the
// same edge. In RPS, forgetting the facing made a fresh soldier point wherever its predecessor last
// ran; forgetting the carry latch made a new cart bank gold it never mined.
//
// ---- 3. The cap is a sanity check, not the identity test ----
// It used to be doing double duty. A real correction is about one world unit (roughly one resim tick of
// a slow unit, measured); a SAME-ENTITY jump bigger than MaxJump is something we do not understand,
// and snapping is the safe reading, because easing a cross-map jump is a visible fly-in.
#include <cmath>
#include <cstdint>

namespace Lur::Render {

template <int MaxSlots>
class CorrectionSmoother {
public:
    struct Config {
        // Halflife of the visual error, in real seconds. ~4 frames at 60 Hz.
        float HalflifeSec = 0.07f;
        // Largest same-entity discontinuity still treated as a smoothable correction (world units).
        float MaxJump = 8.0f;
        // Residue below this is snapped to zero so the offset actually reaches rest.
        float Epsilon = 0.001f;
    };

    explicit CorrectionSmoother(Config C = {}) : Cfg(C) {}

    // Call once per slot per PUBLISHED snapshot (not per frame).
    //
    // SegStart is where this snapshot says the entity's interpolation segment begins (its Prev), and
    // SegEnd is where it ends (its Pos). Serial must be the entity's per-creation id; 0 means empty.
    //
    // Returns TRUE when this slot's occupant changed — a new entity, or a slot that was dead. The
    // caller must reset its own per-slot view state on that edge; see the note above.
    bool Observe(int32_t Slot, bool Alive, uint32_t Serial, float SegStartX, float SegStartY,
                 float SegEndX, float SegEndY) {
        if (Slot < 0 || Slot >= MaxSlots) return false;
        const bool SameEntity = Alive && PrevAlive[Slot] && PrevSerial[Slot] == Serial;
        if (SameEntity) {
            // Discontinuity: where we last had this entity vs where this snapshot says it begins.
            const float Dx = PrevPosX[Slot] - SegStartX;
            const float Dy = PrevPosY[Slot] - SegStartY;
            if (Dx * Dx + Dy * Dy <= Cfg.MaxJump * Cfg.MaxJump) {
                ErrX_[Slot] += Dx;   // add to the error so the DISPLAYED position does not jump
                ErrY_[Slot] += Dy;
            } else {
                ErrX_[Slot] = 0.0f;  // too big to be a correction -> snap
                ErrY_[Slot] = 0.0f;
            }
        } else {
            ErrX_[Slot] = 0.0f;      // new or replaced occupant -> snap, never ease in
            ErrY_[Slot] = 0.0f;
        }
        PrevPosX[Slot] = SegEndX;
        PrevPosY[Slot] = SegEndY;
        PrevSerial[Slot] = Serial;
        PrevAlive[Slot] = Alive;
        return !SameEntity;
    }

    // Call once per RENDER FRAME. Exponential decay in real time, so the result is frame-rate
    // independent: 60 Hz and 30 Hz reach the same offset after the same elapsed seconds.
    void Decay(float DtSec, int32_t Count = MaxSlots) {
        if (DtSec <= 0.0f) return;
        const float K = std::pow(0.5f, DtSec / Cfg.HalflifeSec);
        const int32_t N = Count < MaxSlots ? Count : MaxSlots;
        for (int32_t I = 0; I < N; ++I) {
            ErrX_[I] *= K;
            ErrY_[I] *= K;
            if (std::fabs(ErrX_[I]) < Cfg.Epsilon) ErrX_[I] = 0.0f;
            if (std::fabs(ErrY_[I]) < Cfg.Epsilon) ErrY_[I] = 0.0f;
        }
    }

    // The offset to add to BOTH interpolation endpoints for this slot.
    float ErrX(int32_t Slot) const { return Slot >= 0 && Slot < MaxSlots ? ErrX_[Slot] : 0.0f; }
    float ErrY(int32_t Slot) const { return Slot >= 0 && Slot < MaxSlots ? ErrY_[Slot] : 0.0f; }

    // Forget everything — a fresh match, or a hard reload.
    void Reset() {
        for (int32_t I = 0; I < MaxSlots; ++I) {
            ErrX_[I] = ErrY_[I] = 0.0f;
            PrevPosX[I] = PrevPosY[I] = 0.0f;
            PrevSerial[I] = 0;
            PrevAlive[I] = false;
        }
    }

private:
    Config   Cfg;
    float    ErrX_[MaxSlots] = {};
    float    ErrY_[MaxSlots] = {};
    float    PrevPosX[MaxSlots] = {};
    float    PrevPosY[MaxSlots] = {};
    uint32_t PrevSerial[MaxSlots] = {};
    bool     PrevAlive[MaxSlots] = {};
};

}  // namespace Lur::Render
