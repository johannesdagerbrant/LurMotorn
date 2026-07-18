# dev-rig.ps1 — the one-command Windows<->Android BLE dev loop (issue #58).
#
# Loopback has no radio; two phones have no debugger. This rig has both: it drives
# the engine's REAL wire protocol over REAL Bluetooth between a Windows endpoint
# (under Lur::Log + the flight recorder) and the UNMODIFIED Android app, and wraps
# the whole two-endpoint cycle — link-death chaos included — into one command an
# agent can run, read the captured evidence from, and iterate.
#
#   powershell -ExecutionPolicy Bypass -File Tools\BleDevRig\dev-rig.ps1 [options]
#
# What it does, in order:
#   1. Builds the Windows endpoint (onlychess_desktop) + the C# radio (BleRadio.exe).
#   2. (Re)connects wireless adb, installs + launches the Android app, and forces it
#      to advertise as a peripheral (wipe cached peer/role, grant BLE permissions).
#   3. Launches the desktop --ble endpoint; it scans, connects, and plays.
#   4. Optionally forces >=1 reconnect mid-run via `svc bluetooth disable/enable`
#      (the exact link-death bug class, on demand).
#   5. Tails both logs to files under Tools\BleDevRig\.logs\ (and echoes to console).
#   6. On exit, pulls the flight-recorder evidence.
#
# See operational notes (adapter power management, LMP version) in scripts\README.md.
[CmdletBinding()]
param(
    [string]$Serial,                 # pin ANDROID_SERIAL (two transports can point at one phone)
    [switch]$SkipBuild,              # reuse existing desktop + radio binaries
    [switch]$SkipInstall,            # don't reinstall the APK (still resets identity + launches)
    [int]$Reconnects = 1,            # forced link-death cycles during the run
    [int]$ReconnectAfterSec = 20,    # first reconnect this long after link-up; then spaced out
    [int]$DurationSec = 0            # auto-stop after N s (0 = run until the desktop window closes)
)
$ErrorActionPreference = 'Stop'
$pkg  = 'com.lurmotorn.onlychess'
$rig  = Split-Path $MyInvocation.MyCommand.Path                                      # Tools\BleDevRig
$root = (Resolve-Path (Join-Path $rig '..\..')).Path                                 # repo root
$logs = Join-Path $rig '.logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$desktopLog = Join-Path $logs 'desktop.log'
$logcatLog  = Join-Path $logs 'logcat.log'

$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path','User')

function Say($m)  { Write-Host "[dev-rig] $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "[dev-rig] $m" -ForegroundColor Yellow }

# --- adb helpers -----------------------------------------------------------------
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) { throw 'adb not found on PATH. Run scripts\setup-android.bat once (installs platform-tools).' }
function Adb { & $adb @args }

# Wireless adb drops when the phone idles; rediscover + reconnect (the port changes).
function Ensure-Device {
    if ($Serial) {
        $env:ANDROID_SERIAL = $Serial
        Adb connect $Serial *> $null
    }
    $devs = (Adb devices) -split "`n" | Where-Object { $_ -match "`tdevice$" }
    if (-not $devs) {
        Say 'no device — rediscovering wireless adb via mdns...'
        $svc = (Adb mdns services) -split "`n" | Where-Object { $_ -match '_adb-tls-connect' } | Select-Object -First 1
        if ($svc) {
            $addr = ($svc -split '\s+')[-1]
            Say "reconnecting to $addr"
            Adb connect $addr *> $null
            $devs = (Adb devices) -split "`n" | Where-Object { $_ -match "`tdevice$" }
        }
    }
    if (-not $devs) { throw 'no Android device reachable (usb or wireless). Connect the phone and retry.' }
    if (-not $Serial) {
        $Serial = (($devs | Select-Object -First 1) -split '\s+')[0]
        $env:ANDROID_SERIAL = $Serial
    }
    Say "device: $Serial"
}

# --- 1) build --------------------------------------------------------------------
if (-not $SkipBuild) {
    Say 'building the C# BLE radio (BleRadio.exe)...'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $rig 'build.ps1') -Source BleRadio.cs
    if ($LASTEXITCODE) { throw "radio build failed ($LASTEXITCODE)" }

    Say 'building the Windows endpoint (onlychess_desktop)...'
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\desktop-build.ps1')
    if ($LASTEXITCODE) { throw "desktop build failed ($LASTEXITCODE)" }
}
$desktopExe = Join-Path $root 'build-desktop\Games\Chess\Desktop\onlychess_desktop.exe'
$radioExe   = Join-Path $rig 'BleRadio.exe'
if (-not (Test-Path $desktopExe)) { throw "desktop endpoint missing: $desktopExe (drop -SkipBuild)" }
if (-not (Test-Path $radioExe))   { throw "radio missing: $radioExe (drop -SkipBuild)" }

# --- 2) get the phone advertising as a peripheral --------------------------------
Ensure-Device

if (-not $SkipInstall) {
    Say 'installing the Android app (assembleDebug + installDebug)...'
    & powershell -NoProfile -ExecutionPolicy Bypass -Command `
        "Set-Location '$($root)\Games\Chess\Android'; if (Test-Path .\gradlew.bat) { .\gradlew.bat installDebug } else { gradle installDebug }"
    if ($LASTEXITCODE) { Warn "gradle installDebug returned $LASTEXITCODE — continuing with the installed build" }
}

# WIPE cached peer/role — else the app stays CENTRAL and won't advertise. Then grant
# every BLE permission so it can advertise + serve headlessly (no manual taps).
Say 'resetting app identity + granting BLE permissions (so it advertises as peripheral)...'
Adb shell pm clear $pkg *> $null
foreach ($p in 'BLUETOOTH_SCAN','BLUETOOTH_ADVERTISE','BLUETOOTH_CONNECT','ACCESS_FINE_LOCATION') {
    Adb shell pm grant $pkg "android.permission.$p" *> $null
}
Adb shell input keyevent KEYCODE_WAKEUP *> $null
Adb shell monkey -p $pkg -c android.intent.category.LAUNCHER 1 *> $null
Say 'app launched — it should now log "no cached peer — full discovery" + "BLE up: serving + advertising"'

# --- 5) tail the phone log to a file (background job) -----------------------------
Adb logcat -c *> $null   # clear so the log starts at this run
Remove-Item $logcatLog, $desktopLog -ErrorAction SilentlyContinue
Say "tailing logcat -> $logcatLog"
$logcatJob = Start-Job -ScriptBlock {
    param($adb, $serial, $out)
    & $adb -s $serial logcat -s OnlyChess:* | Tee-Object -FilePath $out
} -ArgumentList $adb, $Serial, $logcatLog

# --- 3) launch the desktop --ble endpoint ----------------------------------------
Say "launching the Windows endpoint --ble (log -> $desktopLog)"
$desktop = Start-Process -FilePath $desktopExe `
    -ArgumentList @('--ble', "`"$radioExe`"") `
    -WorkingDirectory $root -PassThru `
    -RedirectStandardOutput $desktopLog -RedirectStandardError "$desktopLog.err"

# --- 4) link-death chaos + wait --------------------------------------------------
$start = Get-Date
$doneReconnects = 0
try {
    while (-not $desktop.HasExited) {
        Start-Sleep -Seconds 1
        $elapsed = ((Get-Date) - $start).TotalSeconds

        if ($doneReconnects -lt $Reconnects -and $elapsed -ge ($ReconnectAfterSec * ($doneReconnects + 1))) {
            $doneReconnects++
            Warn "CHAOS $doneReconnects/$Reconnects — forcing a reconnect (svc bluetooth disable/enable)"
            Adb shell svc bluetooth disable *> $null
            Start-Sleep -Seconds 3
            Adb shell svc bluetooth enable  *> $null
            # After a BT bounce the app must re-advertise; relaunch it to be safe.
            Adb shell monkey -p $pkg -c android.intent.category.LAUNCHER 1 *> $null
        }

        if ($DurationSec -gt 0 -and $elapsed -ge $DurationSec) {
            Say "duration $DurationSec s reached — stopping"
            break
        }
    }
} finally {
    # --- 6) collect evidence + clean up ------------------------------------------
    if (-not $desktop.HasExited) { $desktop | Stop-Process -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 300
    if ($logcatJob) { Stop-Job $logcatJob -ErrorAction SilentlyContinue; Receive-Job $logcatJob -ErrorAction SilentlyContinue *> $null; Remove-Job $logcatJob -ErrorAction SilentlyContinue }

    $rec = Join-Path $root '.lur-desktop-save\ble.flightrec'
    if (Test-Path $rec) {
        $dest = Join-Path $logs 'ble.flightrec'
        Copy-Item $rec $dest -Force
        Say "flight recorder (desktop endpoint) -> $dest"
    } else {
        Warn "no desktop flight recording at $rec (did the endpoint link?)"
    }
    # The Android app has no flight recorder yet (#55 is desktop-only). When it lands
    # writing into the app's Download/, this is the pull:
    #   adb pull /sdcard/Download/onlychess.flightrec (Tools\BleDevRig\.logs)
    Adb pull /sdcard/Download/onlychess.flightrec (Join-Path $logs 'phone.flightrec') 2>$null | Out-Null

    Say "logs: $desktopLog  |  $logcatLog"
    Say 'done.'
}
