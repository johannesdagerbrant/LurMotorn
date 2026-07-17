# LurMotorn desktop Workbench build (roadmap Phase 0.5) — VS-free, MinGW-w64 g++,
# links the installed Vulkan SDK. Builds the shared engine + chess with a real Win32
# window + Vulkan surface, so chess runs on the PC (the fast iteration loop).
#
#   Run:  powershell -ExecutionPolicy Bypass -File scripts\desktop-build.ps1 [-Run]
#
# Requires the host toolchain (see build.ps1) plus the Vulkan SDK (VULKAN_SDK set).
param([switch]$Run)
$ErrorActionPreference = 'Stop'

$env:Path = [System.Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [System.Environment]::GetEnvironmentVariable('Path', 'User')

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
$gxx   = (Get-Command g++   -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw 'cmake not found. Install: winget install Kitware.CMake' }
if (-not $gxx)   { throw 'g++ not found. Install: winget install BrechtSanders.WinLibs.POSIX.UCRT' }
if (-not $env:VULKAN_SDK) { throw 'VULKAN_SDK not set. Install the LunarG Vulkan SDK.' }

$root  = Split-Path (Split-Path $MyInvocation.MyCommand.Path)   # scripts\.. = repo root
$build = Join-Path $root 'build-desktop'

& $cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="$gxx" `
         -DLUR_DESKTOP=ON -DLUR_BUILD_TESTS=OFF
if ($LASTEXITCODE) { throw "configure failed ($LASTEXITCODE)" }
& $cmake --build $build --target onlychess_desktop
if ($LASTEXITCODE) { throw "build failed ($LASTEXITCODE)" }

$exe = Join-Path $build 'Games/Chess/Desktop/onlychess_desktop.exe'
Write-Host "`nDesktop Workbench build green: $exe" -ForegroundColor Green

if ($Run) { & $exe }
