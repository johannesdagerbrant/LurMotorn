# Build a Tools/BleDevRig C# tool with the .NET Framework csc.exe (VS-FREE — csc ships
# with Windows). The tools consume WinRT (Windows.Devices.Bluetooth) at runtime via the
# Windows SDK reference winmds; no Visual Studio, no SDK install, no license.
#
#   powershell -ExecutionPolicy Bypass -File Tools\BleDevRig\build.ps1 [-Source BleConnect.cs] [-Run]
#
# Requires the Windows SDK (10.0.x) — present on this dev machine at Windows Kits\10.
param([string]$Source = 'BleConnect.cs', [switch]$Run)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

$csc = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if (-not (Test-Path $csc)) { throw "csc.exe not found ($csc) - needs .NET Framework 4.x" }

# Pick the newest installed Windows SDK reference winmds.
$refRoot = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\References' -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending | Select-Object -First 1
if (-not $refRoot) { throw 'Windows SDK References not found under Windows Kits\10\References' }
function Get-Winmd([string]$contract) {
    $d = Get-ChildItem (Join-Path $refRoot.FullName $contract) -Directory | Sort-Object Name -Descending | Select-Object -First 1
    (Get-ChildItem $d.FullName -Filter "$contract.winmd" | Select-Object -First 1).FullName
}
$uni  = Get-Winmd 'Windows.Foundation.UniversalApiContract'
$fnd  = Get-Winmd 'Windows.Foundation.FoundationContract'
$srwr = (Get-ChildItem 'C:\Windows\Microsoft.NET\assembly\GAC_MSIL\System.Runtime.WindowsRuntime' -Recurse -Filter System.Runtime.WindowsRuntime.dll | Select-Object -First 1).FullName
$facades = 'C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8\Facades'

$out = Join-Path $here ([IO.Path]::ChangeExtension($Source, 'exe'))
$args = @(
    '/nologo', '/platform:x64', "/out:$out",
    "/r:$uni", "/r:$fnd", "/r:$srwr",
    "/r:$facades\System.Runtime.dll", "/r:$facades\System.Runtime.InteropServices.WindowsRuntime.dll",
    (Join-Path $here $Source)
)
& $csc @args 2>&1 | Where-Object { $_ -notmatch 'CS1701|related to previous' }
if ($LASTEXITCODE) { throw "csc failed ($LASTEXITCODE)" }
Write-Host "built: $out" -ForegroundColor Green
if ($Run) { & $out }
