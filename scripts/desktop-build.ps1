# LurMotorn desktop build — ONE script for every game (#198).
#
# The desktop harness is the fast iteration loop: two instances in one process over loopback or a
# real BLE radio, with a real Win32 window + Vulkan surface. VS-free, MinGW-w64 g++, links the
# installed Vulkan SDK.
#
#   powershell -ExecutionPolicy Bypass -File scripts\desktop-build.ps1 [-Game chess|rps] [-Run] [args...]
#
# -Game defaults to chess (the historical behaviour of this script's name). Anything after the named
# parameters is passed STRAIGHT THROUGH to the exe when -Run is given, so a game's own flags need no
# script change:
#
#   ... -Game rps -Run --solo --ai easy
#   ... -Game rps -Run --aivs
#
# This replaces desktop-build.ps1 + rps-desktop-build.ps1, which were 35 and 43 lines with an
# IDENTICAL build half — same toolchain probe, same configure line, same target-and-check. The only
# real differences were the game's paths and its run flags, i.e. a table and a passthrough. The third
# game would have made a third copy, and per-game build scripts are one of the three boilerplate axes
# #198 exists to remove.
#
# Requires the host toolchain (see build.ps1) plus the Vulkan SDK (VULKAN_SDK set).
param(
    [ValidateSet('chess', 'rps')] [string]$Game = 'chess',
    [switch]$Run,
    [Parameter(ValueFromRemainingArguments = $true)] $GameArgs
)
$ErrorActionPreference = 'Stop'

# The per-game table: everything that differs. A new game adds one row.
$Games = @{
    chess = @{ Target = 'onlychess_desktop'; Dir = 'Chess';                Label = 'Chess (Workbench)' }
    rps   = @{ Target = 'onlyrps_desktop';   Dir = 'RocksPapersScissors';  Label = 'RPS' }
}
$G = $Games[$Game]

$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [System.Environment]::GetEnvironmentVariable('Path', 'User')

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
$gxx   = (Get-Command g++   -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw 'cmake not found. Install: winget install Kitware.CMake' }
if (-not $gxx)   { throw 'g++ not found. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT' }
if (-not $env:VULKAN_SDK) { throw 'VULKAN_SDK not set. Install the LunarG Vulkan SDK.' }

$root  = Split-Path (Split-Path $MyInvocation.MyCommand.Path)   # scripts\.. = repo root
$build = Join-Path $root 'build-desktop'

# No -DCMAKE_BUILD_TYPE: EngineFlags derives it from LUR_CONFIG (default Development ->
# RelWithDebInfo / optimized), so desktop perf numbers are real. Pass -DLUR_FAST=ON for a quick -O0
# build, or -DLUR_CONFIG=Debugging to debug. Hardcoding CMAKE_BUILD_TYPE here is the #89 bug: every
# driver used to force Debug, so no build was ever actually optimized.
& $cmake -S $root -B $build -G Ninja -DCMAKE_CXX_COMPILER="$gxx" `
         -DLUR_DESKTOP=ON -DLUR_BUILD_TESTS=OFF
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
& $cmake --build $build --target $G.Target
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

$exe = Join-Path $build "Games/$($G.Dir)/Desktop/$($G.Target).exe"
Write-Host "`n$($G.Label) desktop build green: $exe" -ForegroundColor Green

# Kill the old exe before relinking is the caller's job (a running exe holds the file open) — but
# running it is ours.
if ($Run) {
    if ($GameArgs) { & $exe @GameArgs } else { & $exe }
}
