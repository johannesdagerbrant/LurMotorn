# icon_clean.py — crop a headless-rasterised icon PNG to its content, pad square, resize, and
# save as a white silhouette + alpha coverage (grayscale+alpha) PNG the rg8-shade-coverage cooker
# accepts. Used by scripts/gen-icon.ps1; kept a standalone file so the PowerShell wrapper never has
# to embed Python. Deps: Pillow.
#
#   python scripts/icon_clean.py <raw.png> <out.png> <size> <pad-fraction>
import sys
from PIL import Image

raw, out, size, pad = sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
im = Image.open(raw).convert("RGBA")
alpha = im.split()[3]
bbox = alpha.getbbox()
if not bbox or sum(1 for p in alpha.get_flattened_data() if p > 16) < 100:
    raise SystemExit("icon looks blank after rasterisation")
ic = im.crop(bbox)
w, h = ic.size
s = max(w, h)
margin = int(s * pad)
side = s + 2 * margin
canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
canvas.paste(ic, ((side - w) // 2, (side - h) // 2), ic)
canvas = canvas.resize((size, size), Image.LANCZOS)
white = Image.new("L", (size, size), 255)  # white silhouette -> shade 255; alpha is the cutout
Image.merge("LA", (white, canvas.split()[3])).save(out)
print("wrote", out)
