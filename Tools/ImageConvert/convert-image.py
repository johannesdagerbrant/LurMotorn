#!/usr/bin/env python3
"""Convert images the engine's texture cook can't accept into the channel formats it
needs.

The runtime ships raw byte streams (a host-side cook embeds them; no image decoder in
the app), and each stream has a fixed channel layout the source PNG must already match:

  --channels 2  ->  grayscale+alpha ("LA") PNG   -> feeds an R8G8   cook (R=shade, G=coverage)
  --channels 4  ->  RGBA PNG                      -> feeds an R8G8B8A8 cook (full colour+alpha)

Source art is usually the wrong format (RGB with no alpha, palette, a different channel
count, an SVG, ...), so normalise it here first. Pillow is a dev-host-only content tool
— never linked into the app or its build.

For the 2-channel (RG8) case the SHADE channel (R) is selectable:
  --shade luma   (default)  Rec.601 grayscale of the RGB
  --shade r|g|b             use that single source channel as the shade
The second channel is always the source alpha (coverage); if the source has no alpha it
is treated as fully opaque (255).

Usage:
  python convert-image.py --channels 2 [--shade luma|r|g|b] [--size N] [--out DIR] IMAGE [IMAGE ...]
  python convert-image.py --channels 4 [--size N] [--out DIR] IMAGE [IMAGE ...]
  # preset: (re)author the chess piece art (fetch rhosgfx CC0 SVGs -> 2ch shade+coverage PNGs):
  python convert-image.py --preset chess-pieces [--size 96]
"""
import argparse
import io
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent  # Tools/ImageConvert/ -> repo root


def to_two_channel(img, shade):
    """RGBA image -> 2-channel LA, where L is the chosen shade and A is the coverage."""
    rgba = img.convert("RGBA")
    r, g, b, a = rgba.split()
    if shade == "luma":
        L = rgba.convert("L")          # Rec.601 luma of RGB (alpha ignored)
    elif shade == "r":
        L = r
    elif shade == "g":
        L = g
    elif shade == "b":
        L = b
    else:
        raise SystemExit(f"--shade must be luma|r|g|b (got {shade})")
    return Image.merge("LA", (L, a))


def normalize(img, channels, shade, size):
    if size:
        img = img.convert("RGBA")
        if img.size != (size, size):
            img = img.resize((size, size), Image.LANCZOS)
    if channels == 2:
        return to_two_channel(img, shade)
    if channels == 4:
        return img.convert("RGBA")
    raise SystemExit("--channels must be 2 or 4")


def save(out_img, dst, channels):
    dst.parent.mkdir(parents=True, exist_ok=True)
    out_img.save(dst, "PNG")
    shown = dst.relative_to(ROOT) if ROOT in dst.parents else dst
    print(f"  {shown}  ({out_img.width}x{out_img.height}, {channels}ch)")


def convert_files(inputs, channels, shade, size, out_dir):
    for p in inputs:
        src = Path(p)
        dst = ((Path(out_dir) / src.name) if out_dir else src).with_suffix(".png")
        save(normalize(Image.open(src), channels, shade, size), dst, channels)


CHESS_PIECES = ["wP", "wN", "wB", "wR", "wQ", "wK"]  # Chess::EPieceType order


def preset_chess_pieces(size):
    """Fetch the rhosgfx (CC0) white piece SVGs via the images.weserv.nl proxy (no local
    SVG rasteriser on the host) and convert each to a 2-channel shade+coverage PNG — the
    source for the R8G8 piece cook (Tools/ChessPieceCook/gen-piece-masks.ps1)."""
    out = ROOT / "Games" / "Chess" / "Content" / "Pieces"
    ua = "LurMotorn/0.1 (johannesdagerbrant@gmail.com)"
    for piece in CHESS_PIECES:
        svg = f"raw.githubusercontent.com/lichess-org/lila/master/public/piece/rhosgfx/{piece}.svg"
        url = ("https://images.weserv.nl/?url=" + urllib.parse.quote(svg, safe="")
               + f"&w={size}&h={size}&fit=contain&output=png")
        req = urllib.request.Request(url, headers={"User-Agent": ua})
        with urllib.request.urlopen(req, timeout=30) as resp:
            data = resp.read()
        save(normalize(Image.open(io.BytesIO(data)), 2, "luma", size), out / f"{piece}.png", 2)
    print(f"Wrote {len(CHESS_PIECES)} 2ch PNGs to {out.relative_to(ROOT)}.")
    print("Next: the build cooks them into PieceMasks.h (Cook/Cook.ps1, driven by the")
    print("      LUR_COOK reference in Games/Chess/View/Private/BoardView.cpp).")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--channels", type=int, choices=(2, 4))
    ap.add_argument("--shade", choices=("luma", "r", "g", "b"), default="luma",
                    help="2ch only: how to derive the shade (R) channel")
    ap.add_argument("--size", type=int, default=0, help="resize to SIZE x SIZE (0 = keep)")
    ap.add_argument("--out", help="output directory (default: alongside each input)")
    ap.add_argument("--preset", choices=("chess-pieces",))
    ap.add_argument("inputs", nargs="*")
    a = ap.parse_args()

    if a.preset == "chess-pieces":
        preset_chess_pieces(a.size or 96)
        return
    if not a.channels or not a.inputs:
        ap.error("give --channels {2|4} and one or more IMAGE inputs (or --preset chess-pieces)")
    convert_files(a.inputs, a.channels, a.shade, a.size or 0, a.out)


if __name__ == "__main__":
    main()
