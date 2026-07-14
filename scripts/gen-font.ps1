# Cooks an OFL font into Modules/Text/Private/Cooked/FontAtlas_<Name>.h — an MSDF
# atlas + per-glyph metrics embedded as raw bytes, so NO font/image decoder ships in
# the app. This is the LurMotorn text-asset "cook" step (companion to gen-piece-masks.ps1).
#
# The heavy lifting (rasterise outlines -> multi-channel signed distance field) is done
# by msdf-atlas-gen — a SANCTIONED OFFLINE BUILD TOOL (MIT). It is downloaded on demand
# to tools/ (gitignored), run here on the dev host / CI, and NEVER linked into the app
# or its CMake build: only its output (this header) is committed. See CLAUDE.md.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/gen-font.ps1 `
#       -Name Inter -Font Content/Fonts/Inter/Inter.ttf [-Size 32] [-PxRange 4]
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Font,
    [int]$Size = 32,
    [int]$PxRange = 4
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$Root      = Split-Path -Parent $PSScriptRoot
$FontPath  = if ([IO.Path]::IsPathRooted($Font)) { $Font } else { Join-Path $Root $Font }
$OutDir    = Join-Path $Root 'Modules\Text\Private\Cooked'
$OutHeader = Join-Path $OutDir "FontAtlas_$Name.h"
$Tmp       = Join-Path $env:TEMP "lur_font_cook_$Name"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
New-Item -ItemType Directory -Force -Path $Tmp    | Out-Null
if (-not (Test-Path $FontPath)) { throw "Missing font: $FontPath" }

# --- Locate (or fetch) the pinned msdf-atlas-gen build tool -----------------------
$ToolVersion = '1.4'
$ToolDir     = Join-Path $Root 'tools\msdf-atlas-gen'
$ToolExe     = Get-ChildItem -Path $ToolDir -Recurse -Filter 'msdf-atlas-gen.exe' -ErrorAction SilentlyContinue |
               Select-Object -First 1 -ExpandProperty FullName
if (-not $ToolExe) {
    $Zip = Join-Path $ToolDir "msdf-atlas-gen-$ToolVersion-win64.zip"
    $Url = "https://github.com/Chlumsky/msdf-atlas-gen/releases/download/v$ToolVersion/msdf-atlas-gen-$ToolVersion-win64.zip"
    New-Item -ItemType Directory -Force -Path $ToolDir | Out-Null
    Write-Host "msdf-atlas-gen not found; downloading pinned v$ToolVersion ..."
    Invoke-WebRequest -Uri $Url -OutFile $Zip -TimeoutSec 60
    Expand-Archive -LiteralPath $Zip -DestinationPath $ToolDir -Force
    $ToolExe = Get-ChildItem -Path $ToolDir -Recurse -Filter 'msdf-atlas-gen.exe' |
               Select-Object -First 1 -ExpandProperty FullName
}
if (-not $ToolExe) { throw "Could not obtain msdf-atlas-gen" }
Write-Host "Using tool: $ToolExe"

# --- Cook: MSDF atlas (RGB PNG) + JSON metrics for printable ASCII ----------------
$Charset = Join-Path $Tmp 'charset.txt'
Set-Content -LiteralPath $Charset -Value '[0x20, 0x7E]' -Encoding ascii
$Png  = Join-Path $Tmp 'atlas.png'
$Json = Join-Path $Tmp 'atlas.json'
# -yorigin top: atlas Y grows downward (v=0 = top row), matching how System.Drawing
# reads the PNG row-major and how the renderer uploads/samples it (Vulkan v=0 = top).
# So the normalised UVs map straight onto the uploaded texture with no V-flip.
& $ToolExe -font $FontPath -charset $Charset -type msdf -format png `
           -imageout $Png -json $Json -size $Size -pxrange $PxRange -yorigin top
if ($LASTEXITCODE -ne 0) { throw "msdf-atlas-gen failed ($LASTEXITCODE)" }

$meta = Get-Content -LiteralPath $Json -Raw | ConvertFrom-Json
$W = [int]$meta.atlas.width
$H = [int]$meta.atlas.height

# --- Read the atlas as RGB bytes (System.Drawing 24bpp is BGR in memory) ----------
function Get-Rgb([string]$png, [int]$w, [int]$h) {
    $bmp = [System.Drawing.Bitmap]::FromFile($png)
    try {
        $rect = New-Object System.Drawing.Rectangle 0, 0, $bmp.Width, $bmp.Height
        $data = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                              [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
        try {
            $stride = $data.Stride
            $buf = New-Object byte[] ($stride * $bmp.Height)
            [System.Runtime.InteropServices.Marshal]::Copy($data.Scan0, $buf, 0, $buf.Length)
            $rgb = New-Object byte[] ($w * $h * 3)
            for ($y = 0; $y -lt $h; $y++) {
                for ($x = 0; $x -lt $w; $x++) {
                    $s = $y * $stride + $x * 3
                    $d = ($y * $w + $x) * 3
                    $rgb[$d + 0] = $buf[$s + 2]  # R (from B-G-R)
                    $rgb[$d + 1] = $buf[$s + 1]  # G
                    $rgb[$d + 2] = $buf[$s + 0]  # B
                }
            }
            return $rgb
        } finally { $bmp.UnlockBits($data) }
    } finally { $bmp.Dispose() }
}
$rgb = Get-Rgb $Png $W $H

# --- Emit the generated header ----------------------------------------------------
function F([double]$v) {
    $s = $v.ToString('R', [System.Globalization.CultureInfo]::InvariantCulture)
    if ($s -notmatch '[.eE]') { $s += '.0' }   # C++ float literals need a decimal point (not "0f")
    return $s + 'f'
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// Generated by scripts/gen-font.ps1 -- do not edit by hand.")
[void]$sb.AppendLine("// MSDF atlas + per-glyph metrics for `"$Name`", cooked from an OFL font via the")
[void]$sb.AppendLine("// offline msdf-atlas-gen tool (size=$Size, pxrange=$PxRange). See CLAUDE.md.")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine('#include "Lur/Text/CookedFont.h"')
[void]$sb.AppendLine("")
[void]$sb.AppendLine("namespace Lur::Text::Cooked {")
[void]$sb.AppendLine("")

# Glyph table (sorted by codepoint).
$glyphs = $meta.glyphs | Sort-Object unicode
[void]$sb.AppendLine("inline constexpr CookedGlyph ${Name}_Glyphs[] = {")
foreach ($g in $glyphs) {
    $adv = F $g.advance
    if ($null -ne $g.planeBounds) {
        $pl = F $g.planeBounds.left; $pb = F $g.planeBounds.bottom
        $pr = F $g.planeBounds.right; $pt = F $g.planeBounds.top
        $ul = F ($g.atlasBounds.left   / $W); $ub = F ($g.atlasBounds.bottom / $H)
        $ur = F ($g.atlasBounds.right  / $W); $ut = F ($g.atlasBounds.top    / $H)
    } else {
        $pl = '0.0f'; $pb = '0.0f'; $pr = '0.0f'; $pt = '0.0f'
        $ul = '0.0f'; $ub = '0.0f'; $ur = '0.0f'; $ut = '0.0f'
    }
    [void]$sb.AppendLine("  { $($g.unicode)u, $adv, $pl,$pb,$pr,$pt, $ul,$ub,$ur,$ut },")
}
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")

# Atlas bytes.
[void]$sb.AppendLine("inline constexpr unsigned char ${Name}_Atlas[$($W * $H * 3)] = {")
$line = "  "
for ($i = 0; $i -lt $rgb.Length; $i++) {
    $line += "$($rgb[$i]),"
    if ($line.Length -ge 110) { [void]$sb.AppendLine($line); $line = "  " }
}
if ($line.Trim().Length -gt 0) { [void]$sb.AppendLine($line) }
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")

# The descriptor.
[void]$sb.AppendLine("inline constexpr CookedFont $Name = {")
[void]$sb.AppendLine("  $W, $H, 3,")
[void]$sb.AppendLine("  $(F $meta.metrics.emSize), $(F $meta.metrics.lineHeight), $(F $meta.metrics.ascender), $(F $meta.metrics.descender), $(F $meta.atlas.distanceRange),")
[void]$sb.AppendLine("  ${Name}_Glyphs, $($glyphs.Count), ${Name}_Atlas,")
[void]$sb.AppendLine("};")
[void]$sb.AppendLine("")
[void]$sb.AppendLine("} // namespace Lur::Text::Cooked")

Set-Content -LiteralPath $OutHeader -Value $sb.ToString() -Encoding ascii
Write-Host "Wrote $OutHeader  ($($glyphs.Count) glyphs, ${W}x${H} MSDF atlas)"
