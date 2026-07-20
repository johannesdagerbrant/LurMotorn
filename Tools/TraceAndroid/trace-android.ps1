# TraceAndroid — capture real performance numbers from the installed app on the Android
# device while it plays a live match against the PC (issue #101). The discipline: run
# this BEFORE and AFTER every perf task so each change has a real before/after.
#
# It (optionally) launches the PC as a BLE opponent, then over a capture window pulls
# from the phone: our own TRACE scope line (sim/render/latency, #101 B/D), gfxinfo
# (rendered fps, jank %, frame-time percentiles), thermal zones, and memory (PSS). It
# writes runs\<ts>\report.md and appends a row to runs\history.csv, tagged with the git
# SHA + LUR_CONFIG, so a diff is one glance.
#
# Game-agnostic like DeviceRig: the $App table below is the only per-game knob.
#
# Examples:
#   Tools\TraceAndroid\trace-android.bat -Label after-perf2 -DurationSec 60
#   Tools\TraceAndroid\trace-android.bat -App rps -NoPeer -DurationSec 30   # capture only
[CmdletBinding()]
param(
    [ValidateSet('rps','chess')] [string]$App = 'rps',
    [string]$Serial = '',
    [int]$DurationSec = 60,
    [string]$Label = '',
    [switch]$NoPeer,             # don't launch the PC opponent (a match is already running)
    [switch]$Fresh,             # pm clear + re-grant BLE perms before launching (fresh advertising)
    [string]$RadioExe = 'Tools\BleDevRig\BleRadio.exe',
    [string]$PeerExe = 'build-desktop\Games\RocksPapersScissors\Desktop\onlyrps_desktop.exe'
)
# 'Continue', not 'Stop': native tools (adb/monkey) write progress to stderr, which PS
# 5.1 would otherwise turn into terminating errors. Real preconditions use explicit throw.
$ErrorActionPreference = 'Continue'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
Set-Location $RepoRoot

# ---- Per-game config (the only per-game knob, like DeviceRig's $App block) ----
$Apps = @{
    rps   = @{ Pkg = 'com.lurmotorn.onlyrps';   LogTag = 'OnlyRps'   }
    chess = @{ Pkg = 'com.lurmotorn.onlychess'; LogTag = 'OnlyChess' }
}
$Cfg = $Apps[$App]
$Pkg = $Cfg.Pkg
$LogTag = $Cfg.LogTag

# Resolve adb once (it isn't always on PowerShell's PATH — see CLAUDE.md).
$AdbExe = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $AdbExe) { $AdbExe = Join-Path $env:LOCALAPPDATA 'Android\Sdk\platform-tools\adb.exe' }
if (-not (Test-Path $AdbExe)) { throw "adb not found (tried PATH + $AdbExe)" }

function RunAdb { param([Parameter(ValueFromRemainingArguments=$true)]$Args)
    if ($Serial) { & $AdbExe -s $Serial @Args } else { & $AdbExe @Args }
}

# ---- Resolve the device (prefer an explicit -Serial; else the single ip:port device) ----
if (-not $Serial) {
    $devs = (& $AdbExe devices) -split "`n" | Where-Object { $_ -match '^\S+\s+device$' } |
            ForEach-Object { ($_ -split '\s+')[0] }
    $ip = $devs | Where-Object { $_ -match ':\d+$' } | Select-Object -First 1
    if ($ip) { $Serial = $ip } elseif ($devs.Count -ge 1) { $Serial = $devs[0] }
    if (-not $Serial) { throw 'No adb device found. Connect the phone (see CLAUDE.md wireless-ADB notes).' }
}
Write-Host "TraceAndroid: device=$Serial app=$App pkg=$Pkg window=${DurationSec}s" -ForegroundColor Cyan

$installed = (RunAdb shell pm list packages $Pkg) -match [regex]::Escape($Pkg)
if (-not $installed) { throw "$Pkg is not installed. Build+install it first (gradlew installDebug)." }

# ---- Fresh start (fresh advertising identity) or just launch ----
if ($Fresh) {
    RunAdb shell pm clear $Pkg | Out-Null
    foreach ($p in 'BLUETOOTH_ADVERTISE','BLUETOOTH_CONNECT','BLUETOOTH_SCAN') {
        RunAdb shell pm grant $Pkg "android.permission.$p" 2>$null | Out-Null
    }
}
# Launch precisely via the resolved launcher activity (am start), not monkey — monkey
# is flaky and its result is opaque. Fall back to monkey only if resolve fails.
$launchComp = ((RunAdb shell cmd package resolve-activity --brief -c android.intent.category.LAUNCHER $Pkg) -split "`n" |
               ForEach-Object { $_.Trim() } | Where-Object { $_ -match "^$([regex]::Escape($Pkg))/" } | Select-Object -First 1)
if ($launchComp) {
    Write-Host "launching $launchComp" -ForegroundColor Cyan
    RunAdb shell am start -n $launchComp | Out-Null
} else {
    Write-Warning "could not resolve a launcher activity for $Pkg; falling back to monkey"
    RunAdb shell monkey -p $Pkg -c android.intent.category.LAUNCHER 1 2>$null | Out-Null
}
Start-Sleep -Seconds 3   # let the app reach the field + start advertising

# ---- Launch the PC opponent over BLE (unless -NoPeer) ----
$peer = $null
if (-not $NoPeer) {
    if (-not (Test-Path $PeerExe)) {
        Write-Warning "PC peer exe not found ($PeerExe). Build it: scripts\rps-desktop-build.ps1. Continuing capture-only."
    } elseif (-not (Test-Path $RadioExe)) {
        Write-Warning "BLE radio not found ($RadioExe). Build it: Tools\BleDevRig\build.ps1 -Source BleRadio.cs. Continuing capture-only."
    } else {
        $stdout = Join-Path $PSScriptRoot 'peer.out.log'
        $peer = Start-Process -FilePath $PeerExe -ArgumentList @('--ble', $RadioExe, '--auto') `
                    -RedirectStandardOutput $stdout -RedirectStandardError (Join-Path $PSScriptRoot 'peer.err.log') `
                    -PassThru -WindowStyle Hidden
        Write-Host "PC opponent launched (pid $($peer.Id)); linking over BLE..." -ForegroundColor Cyan
        Start-Sleep -Seconds 6   # BLE scan+connect+handshake (~2-3s on the A14 per the metrics)
    }
}

# ---- t0: reset the frame stats + clear the log, snapshot thermal ----
function ThermalC {
    $raw = (RunAdb shell "cat /sys/class/thermal/thermal_zone*/temp" 2>$null) -split "`n" |
           ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^\d+$' } | ForEach-Object { [double]$_ }
    # values are milli-C on this device (e.g. 45000); normalise
    $raw | ForEach-Object { if ($_ -gt 1000) { $_/1000 } else { $_ } }
}
RunAdb shell dumpsys gfxinfo $Pkg reset | Out-Null
RunAdb logcat -c 2>$null | Out-Null
$thermal0 = ThermalC
Write-Host "Capturing for ${DurationSec}s..." -ForegroundColor Cyan
Start-Sleep -Seconds $DurationSec

# ---- t1: pull everything ----
$gfx = (RunAdb shell dumpsys gfxinfo $Pkg) -join "`n"
$mem = (RunAdb shell dumpsys meminfo $Pkg) -join "`n"
$log = (RunAdb logcat -d -s "$LogTag`:*") -join "`n"
$thermal1 = ThermalC
if ($peer) { try { Stop-Process -Id $peer.Id -Force -ErrorAction SilentlyContinue } catch {}
             Get-Process -Name 'BleRadio' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue }

# ---- Parse ----
function Match1 { param($Text,$Pattern) $m = [regex]::Match($Text,$Pattern); if ($m.Success) { $m.Groups[1].Value } else { $null } }

$totalFrames = [int]([regex]::Match($gfx,'Total frames rendered:\s*(\d+)').Groups[1].Value)
$jankFrames  = Match1 $gfx 'Janky frames:\s*(\d+)'
$jankPct     = Match1 $gfx 'Janky frames:\s*\d+\s*\(([\d.]+)'
$p50 = Match1 $gfx '50th percentile:\s*(\d+)ms'
$p90 = Match1 $gfx '90th percentile:\s*(\d+)ms'
$p95 = Match1 $gfx '95th percentile:\s*(\d+)ms'
$p99 = Match1 $gfx '99th percentile:\s*(\d+)ms'
$missedVsync = Match1 $gfx 'Number Missed Vsync:\s*(\d+)'
$renderedFps = if ($DurationSec -gt 0 -and $totalFrames -gt 0) { [math]::Round($totalFrames / $DurationSec, 1) } else { 0 }

$pssKb = Match1 $mem 'TOTAL PSS:\s*(\d+)'
if (-not $pssKb) { $pssKb = Match1 $mem 'TOTAL:\s*(\d+)' }
$pssMb = if ($pssKb) { [math]::Round([int]$pssKb / 1024.0, 1) } else { $null }

# LOCKSTEP tick + presented (last line of the window) -> presented-fps needs two samples.
$lockLines = [regex]::Matches($log, 'LOCKSTEP tick=(\d+).*?presented=(\d+)')
$presFps = $null; $finalTick = $null; $desync = $null
if ($lockLines.Count -ge 2) {
    $first = $lockLines[0]; $last = $lockLines[$lockLines.Count-1]
    $dp = [int]$last.Groups[2].Value - [int]$first.Groups[2].Value
    # LOCKSTEP prints ~every 2s; approximate the span from the count.
    $spanSec = [math]::Max(2, ($lockLines.Count - 1) * 2)
    if ($dp -gt 0) { $presFps = [math]::Round($dp / $spanSec, 1) }
    $finalTick = [int]$last.Groups[1].Value
}
$desync = if ($log -match 'desync=1') { 'YES' } else { 'no' }
# TRACE scope lines: take the last one in the window (freshest aggregate).
$traceMatches = [regex]::Matches($log, 'TRACE (.+)$', 'Multiline')
$traceLine = if ($traceMatches.Count -gt 0) { $traceMatches[$traceMatches.Count-1].Groups[1].Value.Trim() } else { '(none — is this a Development/LUR_TRACE build, and did a match run?)' }

$peakT0 = if ($thermal0) { [math]::Round(($thermal0 | Measure-Object -Maximum).Maximum,1) } else { $null }
$peakT1 = if ($thermal1) { [math]::Round(($thermal1 | Measure-Object -Maximum).Maximum,1) } else { $null }

$sha = (& git rev-parse --short HEAD).Trim()
$cfg = 'Development'  # phone installDebug default; -PlurConfig would change it
$ts  = Get-Date -Format 'yyyy-MM-dd_HH-mm-ss'
$runDir = Join-Path $PSScriptRoot "runs\$ts"
New-Item -ItemType Directory -Force -Path $runDir | Out-Null

$report = @"
# TraceAndroid run $ts

- **Label:** $Label
- **App / pkg:** $App / $Pkg
- **Device:** $Serial
- **Window:** ${DurationSec}s
- **Git:** $sha   **Config:** $cfg   **Peer:** $(if($NoPeer){'none (capture-only)'}else{'onlyrps_desktop --ble --auto'})

## Frame pacing (gfxinfo)
| metric | value |
|---|---|
| rendered fps (frames/window) | $renderedFps |
| presented fps (LOCKSTEP) | $presFps |
| total frames | $totalFrames |
| janky frames | $jankFrames ($jankPct%) |
| frame ms p50 / p90 / p95 / p99 | $p50 / $p90 / $p95 / $p99 |
| missed vsync | $missedVsync |

## App scopes (TRACE — sim/render/latency, ms avg/max)
```
$traceLine
```

## Thermal / memory / liveness
| metric | value |
|---|---|
| peak zone C (start -> end) | $peakT0 -> $peakT1 |
| TOTAL PSS (MB) | $pssMb |
| final lockstep tick | $finalTick |
| desync | $desync |
"@
Set-Content -Path (Join-Path $runDir 'report.md') -Value $report -Encoding utf8
$gfx | Set-Content -Path (Join-Path $runDir 'gfxinfo.txt') -Encoding utf8
$log | Set-Content -Path (Join-Path $runDir 'logcat.txt') -Encoding utf8

$histFile = Join-Path $PSScriptRoot 'runs\history.csv'
if (-not (Test-Path $histFile)) {
    'ts,label,app,sha,config,durSec,renderedFps,presFps,jankPct,p50,p90,p95,p99,missedVsync,pssMb,peakC,finalTick,desync' |
        Set-Content -Path $histFile -Encoding utf8
}
"$ts,$Label,$App,$sha,$cfg,$DurationSec,$renderedFps,$presFps,$jankPct,$p50,$p90,$p95,$p99,$missedVsync,$pssMb,$peakT1,$finalTick,$desync" |
    Add-Content -Path $histFile -Encoding utf8

Write-Host "`n===== report ($runDir\report.md) =====" -ForegroundColor Green
Write-Host $report
Write-Host "`nappended: $histFile" -ForegroundColor Green
