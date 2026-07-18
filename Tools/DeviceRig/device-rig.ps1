# device-rig.ps1 - game-agnostic on-device run + debug rig for LurMotorn apps.
#
# An ENGINE dev instrument (hand-run, never shipped, not tied to any game): it drives a
# LurMotorn app on real phones to arm the dev autoplayer, launch, tail the engine log,
# screenshot, and summarize the engine's SAME-FRAME reply metric - over adb (Android)
# and pymobiledevice3 / Sideloadly (iOS). It knows nothing about any particular game; it
# speaks only engine terms (peer, link, autoplay, datagram, same-frame, match) and parses
# only engine log lines (AUTOPLAY ..., MATCH END ...). Point it at an app via the $App
# block; a different game reuses it verbatim with a different $App.
#
# Autoplay + same-frame instrumentation are #if LUR_INTERNAL in the app (a Development
# build); this rig just toggles and observes them. See Tools/DeviceRig/README.md.
#
#   powershell -File Tools\DeviceRig\device-rig.ps1 -Action run -Matches 3
#   powershell -File Tools\DeviceRig\device-rig.ps1 -Action tail  -Peer ios
#   powershell -File Tools\DeviceRig\device-rig.ps1 -Action arm   -Peer both
[CmdletBinding()]
param(
    [ValidateSet('run','arm','disarm','reset','install','launch','tail','shot','status')]
    [string]$Action = 'run',
    [ValidateSet('android','ios','both')]
    [string]$Peer = 'both',
    [string]$AndroidSerial,         # pin when several transports point at one phone
    [string]$AppleId,               # iOS install: Apple ID for Sideloadly (once; then a token is cached)
    [string]$Ipa,                   # iOS install: .ipa to sign+install (default: dist\ below)
    [int]$Matches = 3,              # run: stop after this many completed matches (0 = until Ctrl-C)
    [int]$DurationSec = 0,          # run: hard cap in seconds (0 = none)
    [int]$SettleSec = 12            # run: grace for the two peers to discover + link
)
$ErrorActionPreference = 'Stop'

# --- App under test - the ONLY app-specific config. The rig body stays game-agnostic. ---
$App = @{
    LogTag         = 'OnlyChess'                          # engine log tag the app emits
    AndroidPackage = 'com.lurmotorn.onlychess'
    IosBundleId    = 'com.lurmotorn.onlychess.L5XBWVZ7N3' # sideload appends the signer suffix
    AutoplayProp   = 'debug.lur.autoplay'                 # Android: engine autoplay toggle (setprop)
    AutoplayMarker = 'autoplay'                           # iOS: Documents/<marker> engine autoplay toggle
}

$root = (Resolve-Path (Join-Path (Split-Path $MyInvocation.MyCommand.Path) '..\..')).Path
$Sideloadly = Join-Path $env:LOCALAPPDATA 'Sideloadly\sideloadly.exe'
if (-not $Ipa) { $Ipa = Join-Path $root 'dist\OnlyChess-unsigned.ipa' }

$rig  = Split-Path $MyInvocation.MyCommand.Path
$logs = Join-Path $rig '.logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path','User')

function Say($m)  { Write-Host "[device-rig] $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "[device-rig] $m" -ForegroundColor Yellow }

# --- Android peer (adb) ------------------------------------------------------------
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
function Adb { & $adb @args }
function Ensure-Android {
    if (-not $adb) { throw 'adb not found on PATH (Android platform-tools).' }
    if ($AndroidSerial) { $env:ANDROID_SERIAL = $AndroidSerial }
    $dev = (Adb devices) -split "`n" | Where-Object { $_ -match "`tdevice$" }
    if (-not $dev) { throw 'no Android device (usb/wireless). Connect it and retry.' }
    if (-not $AndroidSerial) { $script:AndroidSerial = (($dev | Select-Object -First 1) -split '\s+')[0]; $env:ANDROID_SERIAL = $script:AndroidSerial }
}
function Reset-Android {
    Say "android: reset identity + grant link permissions ($($App.AndroidPackage))"
    Adb shell pm clear $App.AndroidPackage | Out-Null
    foreach ($p in 'BLUETOOTH_SCAN','BLUETOOTH_ADVERTISE','BLUETOOTH_CONNECT','ACCESS_FINE_LOCATION') {
        Adb shell pm grant $App.AndroidPackage "android.permission.$p"
    }
}
function Arm-Android    { Say "android: arm autoplay ($($App.AutoplayProp)=1)"; Adb shell setprop $App.AutoplayProp 1 }
function Disarm-Android { Say 'android: disarm autoplay'; Adb shell setprop $App.AutoplayProp 0; Adb shell am force-stop $App.AndroidPackage }
function Launch-Android {
    Say 'android: wake + launch'
    Adb shell input keyevent KEYCODE_WAKEUP | Out-Null
    Adb shell monkey -p $App.AndroidPackage -c android.intent.category.LAUNCHER 1 2>&1 | Out-Null
}
function Shot-Android($path) {
    Adb shell screencap -p /sdcard/_devrig.png | Out-Null
    Adb pull /sdcard/_devrig.png $path 2>&1 | Out-Null
    Adb shell rm /sdcard/_devrig.png | Out-Null
    Say "android: screenshot -> $path"
}

# --- iOS peer (pymobiledevice3 + Sideloadly) ---------------------------------------
# Free-sideloaded + no RemoteXPC tunnel: arm (container file push) and tail (syslog) are
# automatable; launch/screenshot need a tunnel, and install needs Sideloadly - see below.
function Pmd { & python -m pymobiledevice3 @args }
function Ensure-Ios {
    $j = (Pmd usbmux list 2>$null) | Out-String
    if (-not ($j -match 'iPhone|iPad|DeviceClass')) { throw 'no iOS device reachable via pymobiledevice3 (USB).' }
}
function Arm-Ios {
    Say "ios: arm autoplay (push Documents/$($App.AutoplayMarker))"
    $marker = Join-Path $logs $App.AutoplayMarker
    Set-Content -Path $marker -Value '1' -NoNewline
    # VendContainer push (container-relative Documents/<marker>). Do NOT pass --documents
    # (VendDocuments), which fails on iOS 26 with InstallationLookupFailed.
    Pmd apps push $App.IosBundleId $marker "Documents/$($App.AutoplayMarker)" 2>&1 | Out-Null
    Say 'ios: marker pushed (app arms within ~1s if running)'
}
function Disarm-Ios { Warn 'ios: to disarm, relaunch the app without the marker (a running app stays armed for the session).' }

# Kick off a Sideloadly sign+install of the .ipa. HONEST LIMITATION: assisted, not fully
# headless. Sideloadly CLI flags are ignored while an instance holds localhost:28811
# (always); the only headless path is POSTing a plist to its private /enqueue API with a
# cached credential token, which is fragile + credential-sensitive, so we do not automate
# it. The Sideloadly daemon auto-refreshes the installed build before the 7-day cert
# expiry, so a re-install is only needed when the .ipa changes. This opens Sideloadly at
# the ipa to make that quick. See Tools/DeviceRig/README.md.
function Install-Ios {
    if (-not (Test-Path $Sideloadly)) { throw "Sideloadly not found: $Sideloadly" }
    if (-not (Test-Path $Ipa)) { throw "ipa not found: $Ipa" }
    Say ('ios: opening Sideloadly for ' + (Split-Path $Ipa -Leaf) + ' - finish sign+install in its window.')
    Start-Process $Sideloadly -ArgumentList @('-i', $Ipa)
    Warn 'ios: install is assisted via the Sideloadly GUI, not headless. Daemon auto-refreshes existing builds; re-install only on a new ipa.'
}

# Headless launch via a RemoteXPC tunnel. Needs pymobiledevice3 remote tunneld running
# under admin - a one-time-per-boot step you start, since I cannot elevate. With it up,
# this launches without touching the phone. Returns $true on success.
function Launch-Ios {
    try {
        Pmd developer dvt launch $App.IosBundleId 2>&1 | Out-Null
        Say 'ios: launched via tunnel'; return $true
    } catch {
        Warn 'ios: launch needs a tunnel. Start once (admin, persists): sudo python -m pymobiledevice3 remote tunneld'
        Warn 'ios: until then, launch the app by hand (foreground + allow Bluetooth on first run).'
        return $false
    }
}
function Shot-Ios($path) {
    try { Pmd developer dvt screenshot $path 2>&1 | Out-Null; Say "ios: screenshot -> $path" }
    catch { Warn 'ios: screenshot needs a RemoteXPC tunnel (sudo pymobiledevice3 remote tunneld) - skipped.' }
}

# --- same-frame summary (engine log lines, game-agnostic) --------------------------
function Summarize($file, $label) {
    if (-not (Test-Path $file)) { Warn "$label : no log captured"; return }
    $auto = Get-Content $file | Select-String 'AUTOPLAY ' | Select-Object -Last 1
    $ends = (Get-Content $file | Select-String 'MATCH END').Count
    $line = if ($auto) { ($auto.Line -replace '.*AUTOPLAY','AUTOPLAY') } else { '(no AUTOPLAY line)' }
    Say "$label : $line ; matches ended=$ends"
}

# --- actions -----------------------------------------------------------------------
$doAndroid = $Peer -in @('android','both')
$doIos     = $Peer -in @('ios','both')

switch ($Action) {
    'reset'   { if ($doAndroid) { Ensure-Android; Reset-Android }; if ($doIos) { Warn 'ios: reset = re-sideload for a fresh identity (no pm clear).' } }
    'install' { if ($doIos) { Ensure-Ios; Install-Ios }; if ($doAndroid) { Ensure-Android; & $adb install -r (Join-Path $root 'Games\Chess\Android\app\build\outputs\apk\debug\app-debug.apk') } }
    'arm'     { if ($doAndroid) { Ensure-Android; Arm-Android }; if ($doIos) { Ensure-Ios; Arm-Ios } }
    'disarm'  { if ($doAndroid) { Ensure-Android; Disarm-Android }; if ($doIos) { Ensure-Ios; Disarm-Ios } }
    'launch'  { if ($doAndroid) { Ensure-Android; Launch-Android }; if ($doIos) { Ensure-Ios; [void](Launch-Ios) } }
    'shot'    { if ($doAndroid) { Ensure-Android; Shot-Android (Join-Path $logs 'android.png') }; if ($doIos) { Ensure-Ios; Shot-Ios (Join-Path $logs 'ios.png') } }
    'tail'    {
        if ($doIos -and -not $doAndroid) { Say 'ios: tailing engine log (Ctrl-C to stop)'; & python -m pymobiledevice3 syslog live 2>$null | Select-String -Pattern $App.LogTag | ForEach-Object { $_.Line } }
        elseif ($doAndroid -and -not $doIos) { Ensure-Android; Say 'android: tailing engine log (Ctrl-C to stop)'; Adb logcat -s "$($App.LogTag):*" }
        else { Warn 'tail: choose one -Peer (android or ios)' }
    }
    'status'  { Summarize (Join-Path $logs 'android.log') 'android'; Summarize (Join-Path $logs 'ios.log') 'ios' }
    'run'     {
        $andLog = Join-Path $logs 'android.log'; $iosLog = Join-Path $logs 'ios.log'
        Remove-Item $andLog,$iosLog -ErrorAction SilentlyContinue
        if ($doIos) {
            Ensure-Ios
            if (-not (Launch-Ios)) { Read-Host 'Press Enter once the iPhone app is foregrounded + Bluetooth allowed' }
            Arm-Ios
        }
        if ($doAndroid) { Ensure-Android; Reset-Android; Arm-Android; Adb logcat -c | Out-Null; Launch-Android }
        $jobs = @()
        if ($doAndroid) { $jobs += Start-Job -Name and -ScriptBlock { param($a,$s,$t,$o) & $a -s $s logcat -s "$t`:*" | Out-File -FilePath $o -Encoding utf8 } -ArgumentList $adb,$AndroidSerial,$App.LogTag,$andLog }
        if ($doIos)     { $jobs += Start-Job -Name ios -ScriptBlock { param($t,$o) & python -m pymobiledevice3 syslog live 2>$null | Select-String -Pattern $t | ForEach-Object { $_.Line } | Out-File -FilePath $o -Encoding utf8 } -ArgumentList $App.LogTag,$iosLog }
        Say "linking... (settling ${SettleSec}s)"; Start-Sleep -Seconds $SettleSec
        $start = Get-Date
        try {
            while ($true) {
                Start-Sleep -Seconds 3
                $ended = 0
                if ($doAndroid -and (Test-Path $andLog)) { $ended = [math]::Max($ended, (Get-Content $andLog | Select-String 'MATCH END').Count) }
                if ($doIos     -and (Test-Path $iosLog)) { $ended = [math]::Max($ended, (Get-Content $iosLog | Select-String 'MATCH END').Count) }
                $el = ((Get-Date) - $start).TotalSeconds
                Say ("progress: matches ended={0} elapsed={1:N0}s" -f $ended, $el)
                if ($Matches -gt 0 -and $ended -ge $Matches) { Say "reached $Matches matches"; break }
                if ($DurationSec -gt 0 -and $el -ge $DurationSec) { Say 'duration cap reached'; break }
            }
        } finally {
            foreach ($j in $jobs) { Stop-Job $j -ErrorAction SilentlyContinue; Remove-Job $j -Force -ErrorAction SilentlyContinue }
            if ($doAndroid) { Disarm-Android }
            Say '--- same-frame report ---'
            Summarize $andLog 'android'; Summarize $iosLog 'ios'
            Say "logs: $andLog  |  $iosLog"
        }
    }
}
