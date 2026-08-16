#pragma once
#include <cstdint>

#include "Rps/Tunables.h"   // brings Rps::Fixed (= Lur::Sim::Fixed) plus WorldWidth/WorldHeight

// The four pieces of screen<->world arithmetic every RPS main needs (issue #43, section D).
//
// Each of these was defined separately in DesktopMain.cpp, the Android main and the iOS main —
// character-for-character identical apart from a parameter name, and in one case a whole call site
// that simply never got the newest of them. That is the shape this section exists to remove: not
// clever code, just code that has to agree in four places and has no mechanism forcing it to.
//
// WorldToFixed is the one that is not merely untidy. It produces the value a Place event carries
// ACROSS THE WIRE into a deterministic fixed-point sim, so if two of its three copies ever rounded
// differently the peers would place a building at different coordinates and diverge — a desync whose
// cause is a duplicated one-line cast, three files away from anything netcode-shaped.
//
// Header-only and free functions rather than methods: they depend on nothing but the tunables, and a
// main should be able to call them without holding a view.

namespace Rps {

// Pixels per world unit for a viewport `WidthPx` wide. The world's WIDTH is what maps to the screen
// width; height then follows from the aspect ratio, which is why there is no Ppu-by-height.
inline float Ppu(float WidthPx) {
    return WidthPx / (static_cast<float>(WorldWidth.Raw) / static_cast<float>(Fixed::One));
}

// The world's height in float world units — the scroll extent the camera clamps against.
inline float WorldHeightF() {
    return static_cast<float>(WorldHeight.Raw) / static_cast<float>(Fixed::One);
}

// A float world coordinate as the Fixed the wire carries. Clamps negatives to zero and rounds to
// nearest (the +0.5 before truncation) rather than truncating toward zero.
//
// Keep this the ONLY definition. See the header note: it is a wire-format decision wearing the
// clothes of a cast, and it used to exist three times.
inline Fixed WorldToFixed(float W) {
    if (W < 0.0f) W = 0.0f;
    return Fixed{static_cast<int32_t>(W * static_cast<float>(Fixed::One) + 0.5f)};
}

// How far up-left of the finger to lift a dragged building ghost, in pixels (#1).
//
// A thumb covers roughly the thing it is placing, so the ghost is offset by about half a footprint
// and the SAME offset feeds the ghost draw, the validity read and the drop — the building lands
// where you SEE it, not under your thumb. All three of those must use one number, which is most of
// why this is a function.
//
// `FootprintRaw` is Tunables' BuildingFootprint.Raw; passed in rather than read from a global
// because it is a live CVar sampled from a snapshot, and which snapshot is the caller's business.
inline float GhostOffsetPx(int32_t FootprintRaw, float PpuValue) {
    return (static_cast<float>(FootprintRaw) / static_cast<float>(Fixed::One)) * 0.5f * PpuValue;
}

}  // namespace Rps
