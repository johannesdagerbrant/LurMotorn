#!/usr/bin/env python3
"""One-off CONTENT AUTHORING step -- NOT part of the repeatable asset cook.

Rasterises the rhosgfx (CC0) white piece SVGs into committed square grayscale+alpha
PNGs in Games/Chess/Content/Pieces/. Those PNGs are the prepped content that the
engine's cook (scripts/gen-piece-masks.ps1) consumes: the cook reads ONLY these
local files and asserts they meet the convention (square, alpha, one shared
resolution) -- it never touches the network. Re-run this only when the piece art or
its resolution changes, then re-run the cook.

Output is 2-channel grayscale+alpha ("LA") because that is exactly the piece
packaging plan: the runtime R8G8 texture packs R = shade (luminance) and
G = coverage (alpha). The RGB colour is not stored -- piece colour comes from the
material tint at draw time -- so luminance+alpha is the whole, lossless source.

There is no local SVG rasteriser on the dev host, so authoring uses the
images.weserv.nl proxy (already the sanctioned way we rasterise piece art) to
render each SVG, then Pillow normalises the result to a square RGBA PNG. Pillow is
a dev-host-only content tool -- it is never linked into the app or its build.

Usage:  python scripts/gen-piece-pngs.py [size]     (default size 96)
"""
import io
import sys
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image

SIZE = int(sys.argv[1]) if len(sys.argv) > 1 else 96
ROOT = Path(__file__).resolve().parent.parent.parent  # Tools/ChessPieceArt/ -> repo root
OUT = ROOT / "Games" / "Chess" / "Content" / "Pieces"
UA = "LurMotorn/0.1 (johannesdagerbrant@gmail.com)"

# White variants, in Chess::EPieceType order: Pawn, Knight, Bishop, Rook, Queen, King.
PIECES = ["wP", "wN", "wB", "wR", "wQ", "wK"]

for piece in PIECES:
    src = ("raw.githubusercontent.com/lichess-org/lila/master/"
           f"public/piece/rhosgfx/{piece}.svg")
    url = ("https://images.weserv.nl/?url=" + urllib.parse.quote(src, safe="")
           + f"&w={SIZE}&h={SIZE}&fit=contain&output=png")
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as resp:
        data = resp.read()

    img = Image.open(io.BytesIO(data)).convert("RGBA")
    if img.size != (SIZE, SIZE):
        img = img.resize((SIZE, SIZE), Image.LANCZOS)
    # Collapse to luminance + alpha to match the R8G8 packaging plan (shade+coverage).
    img = img.convert("LA")

    dst = OUT / f"{piece}.png"
    img.save(dst, "PNG")
    print(f"  {piece} -> {dst.relative_to(ROOT)} ({img.width}x{img.height} grayscale+alpha)")

print(f"Wrote {len(PIECES)} PNGs to {OUT.relative_to(ROOT)}.")
print("Next: run scripts/gen-piece-masks.ps1 to cook PieceMasks.h.")
