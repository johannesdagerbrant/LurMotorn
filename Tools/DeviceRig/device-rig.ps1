# device-rig.ps1 — game-agnostic on-device run + debug rig for LurMotorn apps.
#
# An ENGINE dev instrument (hand-run, never shipped, not tied to any game): it drives
# a LurMotorn app on real phones to arm the dev autoplayer, launch, tail the engine
# log, screenshot, and summarize the engine's SAME-FRAME reply metric — over adb
# (Android) and pymobiledevice3 (iOS). It knows nothing about any particular game; it
# speaks only engine terms (peer, link, autoplay, datagram, same-frame, match) and
# parses only engine log lines (`AUTOPLAY …`, `MATCH END …`). Point it at an app via
# the $App block; a different game reuses it verbatim with a different $App.
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
    [string]$AppleId,              # iOS 'install': Apple ID for Sideloadly (once; then a token is cached)
    [string]$Ipa,                  # iOS 'install': .ipa to sign+install (default: dist\ below)
    [int]$Matches = 3,              # 'run': stop after this many completed matches (0 = until Ctrl-C)
    [int]$DurationSec = 0,          # 'run': hard cap in seconds (0 = none)
    [int]$SettleSec = 12            # 'run': grace for the two peers to discover + link
)
$ErrorActionPreference = 'Stop'

# --- App under test — the ONLY app-specific config. The rig body stays game-agnostic. ---
$App = @{
    LogTag         = 'OnlyChess'                          # engine log tag the app emits
    AndroidPackage = 'com.lurmotorn.onlychess'
    IosBundleId    = 'com.lurmotorn.onlychess.L5XBWVZ7N3' # sideload appends the signer suffix
    AutoplayProp   = 'debug.lur.autoplay'                 # Android: engine autoplay toggle (setprop)
    AutoplayMarker = 'autoplay'                           # iOS: Documents/<marker> engine autoplay toggle
}

$root = (Resolve-Path (Join-Path (Split-Path $MyInvocation.MyCommand.Path) '..\..')).Path
$Sideloadly = Join-Path $env:LOCALAPPDATA 'Sideloadly\sideloadly.exe'   # signs+installs a .ipa headlessly (cached token)
if (-not $Ipa) { $Ipa = Join-Path $root 'dist\OnlyChess-unsigned.ipa' } # CI artifact (see CLAUDE.md)

$rig  = Split-Path $MyInvocation.MyCommand.Path
$logs = Join-Path $rig '.logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path','User')

function Say($m)  { Write-Host "[device-rig] $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "[device-rig] $m" -ForegroundColor Yellow }

# ── Android peer (adb) ───────────────────────────────────────────────────────
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
function Arm-Android   { Say "android: arm autoplay ($($App.AutoplayProp)=1)"; Adb shell setprop $App.AutoplayProp 1 }
function Disarm-Android{ Say "android: disarm autoplay"; Adb shell setprop $App.AutoplayProp 0; Adb shell am force-stop $App.AndroidPackage }
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

# ── iOS peer (pymobiledevice3) ───────────────────────────────────────────────
# Free-sideloaded + no RemoteXPC tunnel: arm (container file push) and tail (syslog)
# are automatable; launch/screenshot need a human / a sudo tunnel, so they PROMPT.
function Pmd { & python -m pymobiledevice3 @args }
function Ensure-Ios {
    $j = (Pmd usbmux list 2>$null) | Out-String
    if (-not ($j -match 'iPhone|iPad|DeviceClass')) { throw 'no iOS device reachable via pymobiledevice3 (USB).' }
}
function Arm-Ios {
    Say "ios: arm autoplay (push Documents/$($App.AutoplayMarker))"
    $marker = Join-Path $logs $App.AutoplayMarker
    Set-Content -Path $marker -Value '1' -NoNewline
    # VendContainer push (container-relative Documents/<marker>); VendDocuments/--documents
    # fails on iOS 26 with InstallationLookupFailed, so DON'T pass --documents.
    Pmd apps push $App.IosBundleId $marker "Documents/$($App.AutoplayMarker)" 2>&1 | Out-Null
    Say 'ios: marker pushed (app arms within ~1s if running)'
}
function Disarm-Ios { Warn 'ios: to disarm, relaunch the app without the marker (running app stays armed for the session).' }

# Headless sign+install of a .ipa via Sideloadly's CLI, reusing its cached credential
# token (localhost:28811 /enqueue). The FIRST run needs an interactive Apple-ID login
# + 2FA in the Sideloadly GUI; after that the token is cached and installs are silent,
# and the Sideloadly daemon auto-refreshes before the 7-day free-cert expiry.
function Install-Ios {
    if (-not (Test-Path $Sideloadly)) { throw "Sideloadly not found: $Sideloadly" }
    if (-not (Test-Path $Ipa)) { throw "ipa not found: $Ipa (download the CI artifact to dist\ first)" }
    $a = @('--silent','--enqueue','-i', $Ipa)
    if ($AppleId) { $a += @('-a', $AppleId) }
    Say "ios: enqueue sign+install of $(Split-Path $Ipa -Leaf) via Sideloadly (cached token; first login is interactive)"
    & $Sideloadly @a 2>&1 | Where-Object { $_ -match 'enqueue|Task|error|fail|login|token' } | ForEach-Object { Write-Host "  sideloadly: $_" }
    Say 'ios: enqueued — watch the Sideloadly window on first login; the daemon reports install status.'
}

# Headless launch via a RemoteXPC tunnel. Needs `pymobiledevice3 remote tunneld` running
# (ADMIN — a one-time-per-boot step you start; I can't elevate). With it up, this launches
# without touching the phone.
function Launch-Ios {
    try {
        Pmd developer dvt launch $App.IosBundleId 2>&1 | Out-Null
        Say 'ios: launched via tunnel'; return $true
    } catch {
        Warn 'ios: launch needs a tunnel. Start it once (admin, persists): sudo python -m pymobiledevice3 remote tunneld'
        Warn 'ios: (until then, launch the app by hand — foreground + allow Bluetooth on first run).'
        return $false
    }
}
function Shot-Ios($path) {
    try { Pmd developer dvt screenshot $path 2>&1 | Out-Null; Say "ios: screenshot -> $path" }
    catch { Warn 'ios: screenshot needs a RemoteXPC tunnel (sudo pymobiledevice3 remote tunneld) — skipped.' }
}
# Tail the engine log, filtered to the app's tag. Android: logcat tag filter. iOS: broad
# syslog + grep (the `-m` message match does NOT filter on iOS 26).
function Tail-Ios($outFile, $seconds) {
    $tag = $App.LogTag
    if ($seconds -gt 0) {
        & python -m pymobiledevice3 syslog live 2>$null | Select-String -Pattern $tag |
            ForEach-Object { $_.Line } | Tee-Object -FilePath $outFile
    }
}

# ── same-frame summary (engine log lines, game-agnostic) ─────────────────────
function Summarize($file, $label) {
    if (-not (Test-Path $file)) { Warn "$label : no log captured"; return }
    $auto = Get-Content $file | Select-String 'AUTOPLAY ' | Select-Object -Last 1
    $ends = (Get-Content $file | Select-String 'MATCH END').Count
    Say "$label : $($auto.Line -replace '.*AUTOPLAY','AUTOPLAY') ; matches ended=$ends"
}

# ── actions ──────────────────────────────────────────────────────────────────
$doAndroid = $Peer -in @('android','both')
$doIos     = $Peer -in @('ios','both')

switch ($Action) {
    'reset'   { if ($doAndroid) { Ensure-Android; Reset-Android }; if ($doIos) { Warn 'ios: reset = re-sideload for a fresh identity (no pm clear).' } }
    'install' { if ($doIos) { Ensure-Ios; Install-Ios }; if ($doAndroid) { Ensure-Android; Say 'android: use android-install.bat (adb install -r)'; & $adb install -r (Join-Path $root 'Games\Chess\Android\app\build\outputs\apk\debug\app-debug.apk') } }
    'arm'     { if ($doAndroid) { Ensure-Android; Arm-Android }; if ($doIos) { Ensure-Ios; Arm-Ios } }
    'disarm'  { if ($doAndroid) { Ensure-Android; Disarm-Android }; if ($doIos) { Ensure-Ios; Disarm-Ios } }
    'launch'  { if ($doAndroid) { Ensure-Android; Launch-Android }; if ($doIos) { Launch-Ios } }
    'shot'    { if ($doAndroid) { Ensure-Android; Shot-Android (Join-Path $logs 'android.png') }; if ($doIos) { Ensure-Ios; Shot-Ios (Join-Path $logs 'ios.png') } }
    'tail'    {
        if ($doIos -and -not $doAndroid) { Say "ios: tailing engine log (Ctrl-C to stop)"; Tail-Ios (Join-Path $logs 'ios.log') 999999 }
        elseif ($doAndroid -and -not $doIos) { Ensure-Android; Say 'android: tailing engine log (Ctrl-C to stop)'; Adb logcat -s "$($App.LogTag):*" }
        else { Warn 'tail: choose one -Peer (android or ios)' }
    }
    'status'  { Summarize (Join-Path $logs 'android.log') 'android'; Summarize (Join-Path $logs 'ios.log') 'ios' }
    'run'     {
        # A full measurement session: arm both peers, (re)launch, capture both engine
        # logs, and summarize the same-frame reply metric until N matches complete.
        $andLog = Join-Path $logs 'android.log'; $iosLog = Join-Path $logs 'ios.log'
        Remove-Item $andLog,$iosLog -ErrorAction SilentlyContinue
        if ($doIos) {
            Ensure-Ios
            if (-not (Launch-Ios)) { Read-Host 'Press Enter once the iPhone app is foregrounded + Bluetooth allowed' }
            Arm-Ios
        }
        if ($doAndroid) { Ensure-Android; Reset-Android; Arm-Android; Adb logcat -c | Out-Null; Launch-Android }
        # background log capture per peer
        $jobs = @()
        if ($doAndroid) { $jobs += Start-Job -Name and -ScriptBlock { param($a,$s,$t,$o) & $a -s $s logcat -s "$t`:*" | Out-File -FilePath $o -Encoding utf8 } -ArgumentList $adb,$AndroidSerial,$App.LogTag,$andLog }
        if ($doIos)     { $jobs += Start-Job -Name ios -ScriptBlock { param($t,$o) & python -m pymobiledevice3 syslog live 2>$null | Select-String -Pattern $t | ForEach-Object { $_.Line } | Out-File -FilePath $o -Encoding utf8 } -ArgumentList $App.LogTag,$iosLog }
        Say "linking… (settling ${SettleSec}s)"; Start-Sleep -Seconds $SettleSec
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
