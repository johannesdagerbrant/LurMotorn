# LurMotorn host build — VS-free (standalone CMake + Ninja + MinGW-w64 g++).
# Builds and unit-tests the shared C++ core. No Visual Studio required.
#
#   Run:  powershell -ExecutionPolicy Bypass -File build.ps1
#
# One-time toolchain install (winget):
#   winget install Kitware.CMake
#   winget install Ninja-build.Ninja
#   winget install BrechtSanders.WinLibs.POSIX.UCRT      # MinGW-w64 GCC (UCRT)
$ErrorActionPreference = 'Stop'

# Make winget-installed tools visible even in a fresh shell (PATH may not have
# refreshed since install).
$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [System.Environment]::GetEnvironmentVariable('Path', 'User')

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
$gxx   = (Get-Command g++   -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw 'cmake not found. Install: winget install Kitware.CMake' }
if (-not $gxx)   { throw 'g++ not found. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT' }
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'

$root  = Split-Path $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'

# Cook game content -> runtime-ready embedded headers. Incremental: a no-op (needing no
# cook tools) unless a game's Content/ changed since the last cook. See Cook/Cook.ps1.
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'Cook\Cook.ps1')
if ($LASTEXITCODE) { throw "cook failed ($LASTEXITCODE)" }

# The host unit-test loop wants fast COMPILES, not fast code: LUR_FAST pins -O0
# (EngineFlags couples optimization to LUR_CONFIG otherwise). LUR_CONFIG stays the
# default Development, so asserts are on.
& $cmake -S $root -B $build -G Ninja -DLUR_FAST=ON -DCMAKE_CXX_COMPILER="$gxx"
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
& $cmake --build $build
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }
& $ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE) { throw "tests failed ($LASTEXITCODE)" }

Write-Host "`nLurMotorn core: build + tests green (g++ $(& $gxx -dumpversion), VS-free)." -ForegroundColor Green
