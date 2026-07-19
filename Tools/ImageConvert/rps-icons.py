# Tools/ImageConvert/rps-icons.py — hand-run sanitizer (never linked into the app).
# Rasterize the RPS design-lock glyphs (#85) into 128x128 grayscale+alpha PNGs the
# rg8-shade-coverage cook accepts: shade = luma (white -> 255), coverage = alpha.
# Remote SVGs render via the images.weserv.nl proxy (the same sanctioned service the
# chess piece art used — no local SVG rasterizer exists on the host, by design).
# The custom "bold pick" (ours) is drawn analytically with PIL at 8x and downsampled.
import io, math, os, urllib.parse, urllib.request
from PIL import Image, ImageDraw

SIZE = 128
OUT = r"C:\Games\LurMotorn\Games\RocksPapersScissors\Content\Icons"
os.makedirs(OUT, exist_ok=True)

REMOTE = {
    # cook order 0..3 = EUnit order (the miner is the SPLIT ore cart, below), then
    # gold/mine/swords/camp/pointer + the ore-load overlay
    "rock":     "raw.githubusercontent.com/game-icons/icons/master/lorc/rock.svg",
    "paper":    "raw.githubusercontent.com/game-icons/icons/master/lorc/scroll-unfurled.svg",
    "scissors": "raw.githubusercontent.com/FortAwesome/Font-Awesome/6.x/svgs/solid/scissors.svg",
    "gold":     "raw.githubusercontent.com/game-icons/icons/master/delapouite/token.svg",
    "mine":     "raw.githubusercontent.com/game-icons/icons/master/delapouite/stone-pile.svg",
    "swords":   "raw.githubusercontent.com/game-icons/icons/master/lorc/crossed-swords.svg",
    "camp":     "raw.githubusercontent.com/game-icons/icons/master/delapouite/barracks-tent.svg",
    "pointer":  "raw.githubusercontent.com/FortAwesome/Font-Awesome/6.x/svgs/solid/hand-pointer.svg",
}

def save_gray_alpha(name, coverage):
    # coverage: L-mode image, 0..255. Output: white glyph + coverage alpha, padded to
    # a SIZE x SIZE square (non-square SVGs keep aspect; the cook needs uniform cells).
    if coverage.size != (SIZE, SIZE):
        sq = Image.new("L", (SIZE, SIZE), 0)
        sq.paste(coverage, ((SIZE - coverage.width) // 2, (SIZE - coverage.height) // 2))
        coverage = sq
    img = Image.merge("LA", (Image.new("L", coverage.size, 255), coverage))
    img.save(os.path.join(OUT, name + ".png"))
    print("wrote", name, coverage.size)

def fetch(name, path):
    url = "https://images.weserv.nl/?url=" + urllib.parse.quote(path, safe="") + "&w=%d&h=%d" % (SIZE, SIZE)
    data = urllib.request.urlopen(url, timeout=60).read()
    img = Image.open(io.BytesIO(data)).convert("RGBA")
    r, g, b, a = img.split()
    luma = img.convert("L")
    # game-icons SVGs carry a black bg square + white glyph (alpha ~ everywhere):
    # coverage = luma. FA SVGs are a black glyph on transparency: coverage = alpha.
    amin, amax = a.getextrema()
    coverage = a if amin < 250 else luma
    save_gray_alpha(name, coverage)

def bezier_q(p0, c, p1, n=64):
    return [(
        (1-t)**2*p0[0] + 2*(1-t)*t*c[0] + t**2*p1[0],
        (1-t)**2*p0[1] + 2*(1-t)*t*c[1] + t**2*p1[1]) for t in (i/(n-1) for i in range(n))]

def make_cart():
    # Playtest 2026-07-19: the miner IS an ore cart — visibly EMPTY heading out,
    # FULL heading home. Split delapouite/mine-wagon at the cart's top rail:
    # below = the empty cart (miner.png), above = the ore heap (oreload.png), drawn
    # as a second gold-tinted instance on top of loaded carts at runtime.
    import urllib.request as rq
    url = "https://images.weserv.nl/?url=" + urllib.parse.quote(
        "raw.githubusercontent.com/game-icons/icons/master/delapouite/mine-wagon.svg",
        safe="") + "&w=%d&h=%d" % (SIZE, SIZE)
    img = Image.open(io.BytesIO(rq.urlopen(url, timeout=60).read())).convert("RGBA")
    coverage = img.convert("L")  # black bg + white glyph -> luma is the mask
    cut = int(SIZE * 158 / 512)  # the rail line in the 512 viewBox, scaled
    cart = coverage.copy()
    cart.paste(0, (0, 0, SIZE, cut))
    ore = coverage.copy()
    ore.paste(0, (0, cut, SIZE, SIZE))
    # Playtest: the load must read BIG but stay SEATED — enlarge the heap around its
    # bottom-centre anchor (the rail line), taller than wide so it never outgrows the
    # cart. Baked here so the runtime draws cart and load at the same instance size.
    bb = ore.getbbox()
    if bb:
        x0, y0, x1, y1 = bb
        heap = ore.crop(bb)
        nw = int(heap.width * 1.12)
        nh = int(heap.height * 1.55)
        heap = heap.resize((nw, nh), Image.LANCZOS)
        cx = (x0 + x1) // 2
        big = Image.new("L", (SIZE, SIZE), 0)
        px, py = cx - nw // 2, max(0, y1 - nh)
        big.paste(heap, (px, py))
        big.paste(0, (0, cut, SIZE, SIZE))  # never below the rail
        ore = big
    save_gray_alpha("miner", cart)
    save_gray_alpha("oreload", ore)

for n, p in REMOTE.items():
    fetch(n, p)
make_cart()
print("OK")
