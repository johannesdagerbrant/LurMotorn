# check-cvar-zero-overhead.ps1 — the SHIPPING zero-overhead proof for CVars
# (dev-console-cvar-tech-spec.md §1, §7; issue #111).
#
# Compiles Modules/Core/Tests/CVarZeroOverheadProbe.cpp TWICE at -O2 -DLUR_SHIPPING=1 —
# once reading a plain `constexpr` (Tune()==42), once reading a CVar<int> with the same
# default — and asserts the emitted machine code for Hot() is IDENTICAL. If reading a CVar
# in a shipping build ever costs an instruction more than the old constexpr, this fails.
#
# Wired as the `cvar_zero_overhead` CTest test, so `build.ps1` runs it every pass. It
# compiles the probe independently (its own -O2 shipping flags), so it is unaffected by
# the host build's LUR_FAST=-O0.
$ErrorActionPreference = 'Stop'

# Same VS-free toolchain discovery as build.ps1 (PATH may be stale in a fresh shell).
$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [System.Environment]::GetEnvironmentVariable('Path', 'User')
$gxx     = (Get-Command g++     -ErrorAction SilentlyContinue).Source
$objdump = (Get-Command objdump -ErrorAction SilentlyContinue).Source
if (-not $gxx)     { throw 'g++ not found (winget install BrechtSanders.WinLibs.POSIX.UCRT)' }
if (-not $objdump) { throw 'objdump not found (ships with the MinGW-w64 binutils)' }

$root  = Split-Path $PSScriptRoot                      # repo root (scripts/..)
$probe = Join-Path $root 'Modules\Core\Tests\CVarZeroOverheadProbe.cpp'
$inc   = Join-Path $root 'Modules\Core\Public'
$tmp   = Join-Path ([System.IO.Path]::GetTempPath()) 'lur-cvar-asm'
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$plainObj = Join-Path $tmp 'plain.o'
$cvarObj  = Join-Path $tmp 'cvar.o'

$flags = @('-std=c++20', '-O2', '-DLUR_SHIPPING=1', '-I', $inc, '-c', $probe, '-o')
& $gxx @flags $plainObj
if ($LASTEXITCODE) { throw "probe (constexpr) failed to compile ($LASTEXITCODE)" }
& $gxx @flags $cvarObj '-DUSE_CVAR=1'
if ($LASTEXITCODE) { throw "probe (CVar) failed to compile ($LASTEXITCODE)" }

# Extract Hot()'s disassembly, dropping the per-instruction address column (offsets shift
# only if the code differs, which is exactly what we are testing) so we compare the pure
# mnemonic+operand stream. objdump separates functions with a blank line.
function Get-HotDisasm([string]$obj) {
    $lines = & $objdump -d --no-show-raw-insn $obj
    $out = New-Object System.Collections.Generic.List[string]
    $inHot = $false
    foreach ($l in $lines) {
        if ($l -match '<Hot>:') { $inHot = $true; continue }
        if ($inHot) {
            if ($l.Trim() -eq '') { break }
            # "   7:\timul   $0x2a,%eax,%eax" -> "imul   $0x2a,%eax,%eax"
            $out.Add(($l -replace '^\s*[0-9a-f]+:\s*', '').Trim())
        }
    }
    return $out -join "`n"
}

$plain = Get-HotDisasm $plainObj
$cvar  = Get-HotDisasm $cvarObj

if ([string]::IsNullOrWhiteSpace($plain)) { throw 'could not find Hot() in the constexpr disassembly' }

if ($plain -ne $cvar) {
    Write-Host "CVar zero-overhead FAILED: Hot() codegen differs between constexpr and CVar." -ForegroundColor Red
    Write-Host "--- constexpr ---`n$plain`n--- CVar ---`n$cvar"
    exit 1
}

Write-Host "CVar zero-overhead: OK (Hot() machine code identical, constexpr == CVar under -O2 shipping)." -ForegroundColor Green
Write-Host "--- Hot() ---`n$plain"
exit 0
