# Cooks the engine's GLSL shaders to committed SPIR-V includes
# (Modules/Render/Private/Vulkan/Shaders/*.inc). The renderer #includes these as
# uint32 arrays, so NEITHER the Android NDK build NOR the macOS iOS build needs a
# shader compiler -- the SPIR-V is cooked once, here, and committed (part of the
# asset cook pipeline, issue #13).
#
# Uses glslc from the Android NDK's shader-tools (already installed for Android).
# Re-run when a .vert/.frag changes.
#
# Usage:  powershell -ExecutionPolicy Bypass -File scripts/gen-shaders.ps1
$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$ShaderDir = Join-Path $Root 'Modules\Render\Private\Vulkan\Shaders'
$Ndk       = Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk\27.2.12479018'
$Glslc     = Join-Path $Ndk 'shader-tools\windows-x86_64\glslc.exe'
if (-not (Test-Path $Glslc)) { throw "glslc not found at $Glslc (install the Android NDK)." }

foreach ($stage in 'Sprite.vert', 'Sprite.frag') {
    $src = Join-Path $ShaderDir $stage
    $inc = Join-Path $ShaderDir "$stage.inc"
    & $Glslc -mfmt=num $src -o $inc
    if ($LASTEXITCODE -ne 0) { throw "glslc failed on $stage" }
    Write-Host "  $stage -> $stage.inc"
}
Write-Host "Cooked shaders into $ShaderDir"