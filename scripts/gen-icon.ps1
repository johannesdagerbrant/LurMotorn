# gen-icon.ps1 — turn an SVG into a cook-ready silhouette PNG (the sibling of gen-font.ps1 /
# gen-shaders.ps1). Rasterises with HEADLESS CHROMIUM (Chrome or Edge — always present on a
# Windows dev box; no ImageMagick/Inkscape/rsvg needed) and cleans up with scripts/icon_clean.py
# (Pillow) into a square white-on-transparent PNG that the `rg8-shade-coverage` cooker
# (Cook/CookRg8ShadeCoverage.ps1) packs into an icon atlas (white shade => the material tint is the
# fill, alpha => the cutout).
#
# Source is EITHER a game-icons.net icon ("author/name", CC BY 3.0 — attribute the author!) fetched
# from the game-icons GitHub mirror, OR a local .svg path. game-icons SVGs are a black background
# rect + a white icon path; we strip the bg rect and force white fill, so only the silhouette lands.
#
#   Run:  powershell -ExecutionPolicy Bypass -File scripts\gen-icon.ps1 -Src delapouite/gold-mine -Name minecamp
#         powershell -ExecutionPolicy Bypass -File scripts\gen-icon.ps1 -Src art\my.svg -Name thing -Size 128
#
# After generating, add the PNG to the game's `// LUR_COOK rg8-shade-coverage src=...` marker + the
# matching glyph enum (same order), then build.ps1 re-cooks the atlas. Deps: a Chromium browser +
# Python with Pillow (both already used by the toolchain).
param(
    [Parameter(Mandatory = $true)][string]$Src,   # "author/name" (game-icons) OR a path to a .svg
    [Parameter(Mandatory = $true)][string]$Name,  # output icon name (<Name>.png)
    [string]$OutDir = "Games\RocksPapersScissors\Content\Icons",
    [int]$Size = 128,           # square output edge (match the game's IconSize)
    [double]$Pad = 0.12         # transparent margin around the cropped icon, fraction of the icon
)
$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $MyInvocation.MyCommand.Path)  # scripts\.. = repo root

$browser = @(
    "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
    "${env:ProgramFiles(x86)}\Google\Chrome\Application\chrome.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $browser) { throw "No Chromium browser found (Chrome/Edge) for headless SVG rasterisation." }
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { throw "python not found (needed with Pillow for the icon cleanup)." }

$work = Join-Path $env:TEMP "lur-gen-icon"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# --- resolve the SVG source (game-icons author/name, or a local file) ---
$svgPath = Join-Path $work "$Name.src.svg"
if ($Src -match '^[\w-]+/[\w-]+$') {
    $url = "https://raw.githubusercontent.com/game-icons/icons/master/$Src.svg"
    Write-Host "fetching game-icons: $Src  (CC BY 3.0 - credit the author)" -ForegroundColor Cyan
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12  # PS 5.1 default is too old
    try { Invoke-WebRequest -Uri $url -OutFile $svgPath -UseBasicParsing }
    catch { & curl.exe -sSL --fail --max-time 30 -o $svgPath $url; if ($LASTEXITCODE) { throw "fetch failed: $Src (is it a real game-icons author/name?)" } }
    if (-not (Test-Path $svgPath) -or (Get-Item $svgPath).Length -lt 50) { throw "fetch produced no SVG for '$Src' (404? check the author/name)." }
} elseif (Test-Path $Src) {
    Copy-Item -LiteralPath $Src -Destination $svgPath -Force
} else {
    throw "Src '$Src' is neither an author/name game-icons id nor an existing .svg path."
}

# --- strip the full-canvas bg rect, force white fill, wrap in a transparent-bg HTML page ---
$svg = Get-Content -Raw -LiteralPath $svgPath
$svg = [regex]::Replace($svg, '<path d="M0 0h512v512H0z"/>', '', 1)   # game-icons black bg square
$svg = [regex]::Replace($svg, '<svg ', '<svg style="width:512px;height:512px" ', 1)
$style = 'html,body{margin:0;padding:0;background:transparent}svg *{fill:#fff !important;stroke:none}'
$html = '<!doctype html><html><head><meta charset="utf-8"><style>' + $style + '</style></head><body>' + $svg + '</body></html>'
$htmlPath = Join-Path $work "$Name.html"
Set-Content -LiteralPath $htmlPath -Value $html -Encoding utf8
$rawPng = Join-Path $work "$Name.raw.png"
if (Test-Path $rawPng) { Remove-Item $rawPng -Force }

# --- rasterise: headless Chromium, transparent background, 512x512. Chrome prints its success
#     line to STDERR, which PS 5.1 would turn into a terminating error under -Stop; drop to
#     Continue around the call and judge success by the output file, not the exit/stderr. ---
$prevEap = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
& $browser --headless --disable-gpu --hide-scrollbars --force-device-scale-factor=1 `
    --default-background-color=00000000 --window-size=512,512 `
    "--screenshot=$rawPng" "$htmlPath" 2>$null | Out-Null
$ErrorActionPreference = $prevEap
if (-not (Test-Path $rawPng)) { throw "headless rasterisation produced no image (browser: $browser)." }

# --- clean up (crop/pad/resize -> grayscale+alpha) via the standalone Pillow helper ---
$outPng = Join-Path $root (Join-Path $OutDir "$Name.png")
$cleaner = Join-Path $root "scripts\icon_clean.py"
& $python $cleaner $rawPng $outPng $Size $Pad
if ($LASTEXITCODE) { throw "icon cleanup failed ($LASTEXITCODE)" }
Write-Host "gen-icon: $Src -> $outPng ($Size x $Size grayscale+alpha)" -ForegroundColor Green
Write-Host "next: add Icons\$Name.png to the game's LUR_COOK marker + glyph enum, then build.ps1 re-cooks." -ForegroundColor DarkGray
