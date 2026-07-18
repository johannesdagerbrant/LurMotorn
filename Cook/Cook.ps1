# LurMotorn content cook -- a build-activated, GAME-AGNOSTIC process (Unreal-cook style),
# NOT a hand-run tool. The build script calls this; it derives what to cook, and how,
# from how the gameplay code REFERENCES its content.
#
# Reference-driven: gameplay source declares each content dependency inline, next to the
# code that uses it, with a marker:
#
#   // LUR_COOK <format> src=<paths> out=<header> <format-specific key=value ...>
#
# The driver finds every marker under Games/, derives that game's Content/ root from the
# marker file's own location (.../Games/<Game>/...), resolves the src paths RELATIVE TO
# that Content/ folder (partial addresses), resolves out relative to the game root, and
# dispatches to the cooker for <format> (the format = how the code uses the content).
#
# Incremental: each generated header carries a `// cook-source-hash:` stamp of its
# ordered sources. A recipe is up-to-date iff the hash matches -- robust across git
# checkouts and needing no cook tools. Cooked outputs are committed, so a normal build
# with unchanged content is a no-op.
#
#   powershell -ExecutionPolicy Bypass -File Cook/Cook.ps1 [-Force]
#
# Add a content type by writing a cooker beside this file and adding a case to Dispatch.
param([switch]$Force)
$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot   # Cook/ -> repo root

. (Join-Path $PSScriptRoot 'CookRg8ShadeCoverage.ps1')

# Walk up from a marker's file to its owning game root (Games/<Game>) and Content/.
# Resolve relative to the repo root, so a repo that itself sits under a "...\Games\..."
# path doesn't confuse the search.
function Resolve-GameRoots([string]$MarkerFile) {
    $abs = (Resolve-Path $MarkerFile).Path
    $rel = $abs.Substring($Root.Length).TrimStart('\', '/')
    $parts = $rel -split '[\\/]'
    if ($parts.Count -lt 2 -or $parts[0] -ne 'Games') {
        throw "LUR_COOK marker not under a Games/<Game> tree: $MarkerFile"
    }
    $gameRoot = Join-Path (Join-Path $Root $parts[0]) $parts[1]
    return @{ GameRoot = $gameRoot; ContentRoot = (Join-Path $gameRoot 'Content') }
}

# Parse a marker's "key=value key=value" tail into a hashtable.
function Parse-Kv([string]$tail) {
    $kv = @{}
    foreach ($tok in ($tail -split '\s+' | Where-Object { $_ })) {
        $eq = $tok.IndexOf('=')
        if ($eq -gt 0) { $kv[$tok.Substring(0, $eq)] = $tok.Substring($eq + 1) }
    }
    return $kv
}

function Get-SourceHash([string[]]$Files) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $ms  = New-Object System.IO.MemoryStream
    foreach ($f in $Files) {   # ordered: order is part of the identity
        $name = [System.Text.Encoding]::UTF8.GetBytes((Split-Path $f -Leaf) + "`0")
        $ms.Write($name, 0, $name.Length)
        $bytes = [System.IO.File]::ReadAllBytes($f)
        $ms.Write($bytes, 0, $bytes.Length)
    }
    -join (($sha.ComputeHash($ms.ToArray())) | ForEach-Object { $_.ToString('x2') })
}

function Get-StampedHash([string]$Output) {
    if (-not (Test-Path $Output)) { return '' }
    $line = Get-Content $Output -TotalCount 3 | Where-Object { $_ -match 'cook-source-hash:' } | Select-Object -First 1
    if ($line -match 'cook-source-hash:\s*([0-9a-f]+)') { return $Matches[1] } else { return '' }
}

# Run the cooker for a parsed marker. Returns the ordered source paths (for hashing).
function Dispatch([string]$Format, [hashtable]$Kv, [string]$ContentRoot, [string]$Output, [string[]]$Sources) {
    switch ($Format) {
        'rg8-shade-coverage' {
            Invoke-CookRg8ShadeCoverage -Sources $Sources -Output $Output `
                -Namespace $Kv['ns'] -SizeVar $Kv['size'] -CoverageVar $Kv['coverage'] -ShadeVar $Kv['shade']
        }
        default { throw "unknown LUR_COOK format '$Format'" }
    }
}

# --- Scan gameplay source for markers, cook the stale ones. ---
$gamesDir = Join-Path $Root 'Games'
$markerFiles = Get-ChildItem $gamesDir -Recurse -File -Include *.cpp, *.h, *.hpp, *.mm -ErrorAction SilentlyContinue |
               Select-String -Pattern '//\s*LUR_COOK\s' -List | Select-Object -ExpandProperty Path -Unique

$rebuilt = 0; $seen = 0
foreach ($file in $markerFiles) {
    foreach ($m in (Select-String -Path $file -Pattern '//\s*LUR_COOK\s+(\S+)\s+(.*)$')) {
        $seen++
        $format = $m.Matches[0].Groups[1].Value
        $kv = Parse-Kv $m.Matches[0].Groups[2].Value
        $roots = Resolve-GameRoots $file
        $sources = @($kv['src'] -split ',' | Where-Object { $_ } | ForEach-Object { Join-Path $roots.ContentRoot $_ })
        $output  = Join-Path $roots.GameRoot $kv['out']
        $name = "$format <- $(Split-Path $file -Leaf)"

        $srcHash = Get-SourceHash $sources
        if (-not $Force -and $srcHash -eq (Get-StampedHash $output)) { Write-Host "cook: $name up-to-date"; continue }
        Write-Host "cook: $name -- content changed, cooking..."
        Dispatch $format $kv $roots.ContentRoot $output $sources
        $body = Get-Content -LiteralPath $output -Raw
        Set-Content -LiteralPath $output -Value ("// cook-source-hash: $srcHash`r`n" + $body) -Encoding ascii
        $rebuilt++
    }
}
Write-Host "cook: done ($seen reference(s), $rebuilt rebuilt)"
