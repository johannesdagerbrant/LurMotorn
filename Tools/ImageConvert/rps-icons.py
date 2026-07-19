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
    # cook order 0..3 = EUnit order (miner is custom, below), then gold/mine/swords/camp
    "rock":     "raw.githubusercontent.com/game-icons/icons/master/lorc/rock.svg",
    "paper":    "raw.githubusercontent.com/game-icons/icons/master/lorc/scroll-unfurled.svg",
    "scissors": "raw.githubusercontent.com/FortAwesome/Font-Awesome/6.x/svgs/solid/scissors.svg",
    "gold":     "raw.githubusercontent.com/game-icons/icons/master/delapouite/token.svg",
    "mine":     "raw.githubusercontent.com/game-icons/icons/master/delapouite/mine-wagon.svg",
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

def make_bold_pick():
    # The locked custom miner glyph: rotate(42deg) about centre of
    #   handle: rounded rect x=234 y=64 w=44 h=400 r=22 (viewBox 512)
    #   head:   path M48 172 Q256 8 464 172 Q256 92 48 172 Z
    S = 8 * SIZE
    k = S / 512.0
    img = Image.new("L", (S, S), 0)
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([234*k, 64*k, 278*k, 464*k], radius=22*k, fill=255)
    top = bezier_q((48, 172), (256, 8), (464, 172))
    bot = bezier_q((464, 172), (256, 92), (48, 172))
    d.polygon([(x*k, y*k) for x, y in top + bot], fill=255)
    img = img.rotate(-42, center=(S/2, S/2), resample=Image.BICUBIC)
    img = img.resize((SIZE, SIZE), Image.LANCZOS)
    save_gray_alpha("miner", img)

for n, p in REMOTE.items():
    fetch(n, p)
make_bold_pick()
print("OK")
