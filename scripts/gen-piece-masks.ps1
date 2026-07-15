# Cooks Games/Chess/View/Private/PieceMasks.h from the prepped PNG content in
# Games/Chess/Content/Pieces/ (w*.png). This is the LurMotorn asset "cook": a
# host-side decode of committed Content/ into runtime-ready raw bytes, embedded so
# no image decoder ships in the app.
#
# The cook reads ONLY the committed local PNGs -- it never touches the network. It
# DEMANDS the content honours the convention (raises otherwise): each white piece
# is present as w<X>.png, square, with an alpha channel, and all six share one
# resolution. That resolution becomes PieceMaskSize. To (re)author the PNGs from
# the rhosgfx (CC0) SVGs, run scripts/gen-piece-pngs.py first, then this cook.
#
# We cook TWO single-byte channels per piece TYPE (see issue #30):
#   * COVERAGE (alpha)   -- the silhouette cutout, straight-alpha composited.
#   * SHADE (luminance)  -- the source art's internal tones (dark outline, cream
#                           body, shadow bands), 0..255.
# Piece colour (white vs black) still comes from the material tint at draw time,
# so 6 mask pairs cover all 12 pieces (the "tint trick"). At draw the shader
# multiplies tint * (shade/255), so the tint supplies the colour while the shade
# preserves the art's tonal detail instead of flattening it to a solid blob. The
# app ships only raw bytes -- no SVG/PNG decoder at runtime.
#
# Usage:  powershell -ExecutionPolicy Bypass -File scripts/gen-piece-masks.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$Root      = Split-Path -Parent $PSScriptRoot
$AssetsDir = Join-Path $Root 'Games\Chess\Content\Pieces'
$OutHeader = Join-Path $Root 'Games\Chess\View\Private\PieceMasks.h'

# White variants, in Chess::EPieceType order: Pawn, Knight, Bishop, Rook, Queen, King.
$pieces = 'wP','wN','wB','wR','wQ','wK'

# Read the coverage (alpha) and shade (Rec.601 luminance) bytes, row-major, from a
# committed PNG, asserting the content convention. Returns a hashtable with .Size,
# .Coverage and .Shade.
function Get-MaskAndShade([string]$png) {
    if (-not (Test-Path $png)) {
        throw "Missing content PNG: $png (run scripts/gen-piece-pngs.py to author it)."
    }
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        if ($bmp.Width -ne $bmp.Height) {
            throw "$png must be square (got $($bmp.Width)x$($bmp.Height))."
        }
        if (-not [System.Drawing.Image]::IsAlphaPixelFormat($bmp.PixelFormat)) {
            throw "$png must have an alpha channel (the silhouette coverage)."
        }
        $size = $bmp.Width
        $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                              [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
        try {
            $stride = $data.Stride
            $buf = New-Object byte[] ($stride * $bmp.Height)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
            $coverage = New-Object byte[] ($size * $size)
            $shade    = New-Object byte[] ($size * $size)
            for ($y = 0; $y -lt $size; $y++) {
                for ($x = 0; $x -lt $size; $x++) {
                    # Format32bppArgb is straight (non-premultiplied) BGRA in memory,
                    # so B,G,R,A are bytes 0..3 and RGB is the true colour even on the
                    # anti-aliased edge (where coverage is partial).
                    $o = $y * $stride + $x * 4
                    $b = $buf[$o]; $g = $buf[$o + 1]; $r = $buf[$o + 2]
                    $i = $y * $size + $x
                    $coverage[$i] = $buf[$o + 3]
                    # Rec.601 luma; the source tones become the tint's shade at draw.
                    $shade[$i] = [byte][math]::Round(0.299 * $r + 0.587 * $g + 0.114 * $b)
                }
            }
            return @{ Size = $size; Coverage = $coverage; Shade = $shade }
        } finally { $bmp.UnlockBits($data) }
    } finally { $bmp.Dispose() }
}

# Emit one `{ ... }` initialiser block of $bytes as a comma-separated C array body.
function Add-ByteBlock($sb, [string]$label, [byte[]]$bytes) {
    [void]$sb.AppendLine("  { // $label")
    $line = "    "
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $line += "$($bytes[$i]),"
        if ($line.Length -ge 110) { [void]$sb.AppendLine($line); $line = "    " }
    }
    if ($line.Trim().Length -gt 0) { [void]$sb.AppendLine($line) }
    [void]$sb.AppendLine("  },")
}

# Extract every piece first, so both arrays can be emitted in order. The first
# piece sets the resolution; the rest must match it (one shared PieceMaskSize).
$masks = @{}
$Size = 0
foreach ($p in $pieces) {
    $m = Get-MaskAndShade (Join-Path $AssetsDir "$p.png")
    if ($Size -eq 0) { $Size = $m.Size }
    elseif ($m.Size -ne $Size) {
        throw "All piece PNGs must share one resolution; $p is $($m.Size) but expected $Size."
    }
    $masks[$p] = $m
    Write-Host "  $p : coverage + shade extracted ($Size x $Size)"
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// Generated by scripts/gen-piece-masks.ps1 -- do not edit by hand.")
[void]$sb.AppendLine("// Two single-byte channels per piece TYPE, extracted from the rhosgfx (CC0) art:")
[void]$sb.AppendLine("//   PieceCoverage -- silhouette alpha (straight-alpha cutout).")
[void]$sb.AppendLine("//   PieceShade    -- Rec.601 luminance of the source tones (dark outline, cream")
[void]$sb.AppendLine("//                    body, shadow bands), multiplied by the material tint at draw.")
[void]$sb.AppendLine("// Piece colour (white vs black) is the tint, so 6 pairs cover all 12 pieces; the")
[void]$sb.AppendLine("// shade keeps the art's tonal detail instead of a flat blob (issue #30). Uploaded")
[void]$sb.AppendLine("// as an R8G8 texture (R=shade, G=coverage). See Games/Chess/Content/Pieces/NOTICE.md.")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("namespace ChessArt {")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("constexpr int PieceMaskSize = $Size;  // ${Size}x${Size}, 1 byte/pixel per channel")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("// Both arrays are indexed by Chess::EPieceType: Pawn, Knight, Bishop, Rook, Queen, King.")
[void]$sb.AppendLine("inline constexpr unsigned char PieceCoverage[6][$Size * $Size] = {")
foreach ($p in $pieces) { Add-ByteBlock $sb $p $masks[$p].Coverage }
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("inline constexpr unsigned char PieceShade[6][$Size * $Size] = {")
foreach ($p in $pieces) { Add-ByteBlock $sb $p $masks[$p].Shade }
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("} // namespace ChessArt")

Set-Content -LiteralPath $OutHeader -Value $sb.ToString() -Encoding ascii
Write-Host "Wrote $OutHeader"