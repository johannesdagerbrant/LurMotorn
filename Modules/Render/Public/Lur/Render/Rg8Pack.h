#pragma once
// Interleave two single-channel 8-bit masks into the RG8 layout the sprite shader samples:
// R = SHADE (the source art's tones), G = COVERAGE (the silhouette). The shader multiplies the
// material tint by the shade, so the tint supplies the colour while the art's highlights, mid-tones
// and dark outline survive instead of flattening to a solid blob (#30).
//
// Promoted out of the games (Phase 5, #201) because it is the one item on that list with a SECOND
// consumer already in the tree, which is what the promotion rule asks for:
//   * chess packs one PieceMaskSize^2 pair per piece type into its own texture;
//   * RPS packs GlyphCount masks SIDE BY SIDE into one wide atlas.
// Same primitive, two destination layouts — so the parameter that matters is the destination stride,
// and once that is a parameter both call sites are one line. The RPS copy computed
// `2 * (Y * (GlyphCount * S) + G * S + X)` inline, which is exactly the arithmetic worth having in one
// tested place rather than two.
//
// Deliberately NOT in Modules/Math: the layout exists because ETextureFormat::Rg8 exists, and the
// channel meanings are a contract with the sprite shader. That makes it a Render concept.
#include <cstddef>
#include <cstdint>

namespace Lur::Render {

// Write a W x H tile of interleaved (shade, coverage) into Dst at pixel (DstX, DstY).
//
// Dst is an RG8 image DstWidthPx pixels wide (2 bytes per pixel); the caller sizes and owns it.
// Shade and Coverage are tightly packed W x H single-channel sources. For a whole-image pack, pass
// DstWidthPx = W and DstX = DstY = 0; for a tile inside an atlas, pass the atlas width and the tile's
// top-left. No allocation, no clamping: writing outside Dst is a caller bug, and the assert-free
// signature keeps it usable from a hot upload path.
inline void PackRg8(uint8_t* Dst, int DstWidthPx, int DstX, int DstY, const uint8_t* Shade,
                    const uint8_t* Coverage, int W, int H) {
    if (Dst == nullptr || Shade == nullptr || Coverage == nullptr) return;
    for (int Y = 0; Y < H; ++Y) {
        const std::size_t Row = static_cast<std::size_t>(DstY + Y) * static_cast<std::size_t>(DstWidthPx);
        std::size_t D = 2u * (Row + static_cast<std::size_t>(DstX));
        std::size_t S = static_cast<std::size_t>(Y) * static_cast<std::size_t>(W);
        for (int X = 0; X < W; ++X, D += 2u, ++S) {
            Dst[D + 0] = Shade[S];
            Dst[D + 1] = Coverage[S];
        }
    }
}

}  // namespace Lur::Render
