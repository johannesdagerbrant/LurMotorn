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
    [ValidateSet('run','cycle','arm','disarm','reset','install','launch','tail','shot','status')]
    [string]$Action = 'run',
    [ValidateSet('android','ios','both')]
    [string]$Peer = 'both',
    [string]$AndroidSerial,         # pin when several transports point at one phone
    [string]$AppleId,               # iOS install: Apple ID for Sideloadly (once; then a token is cached)
    [string]$Ipa,                   # iOS install: unsigned .ipa from CI (default: dist\ below)
    [string]$SignedIpa,             # iOS install: a pre-signed .ipa -> headless `apps install` (no GUI)
    [string]$ZsignP12,              # iOS install: zsign a fresh cert (.p12) to sign $Ipa headlessly
    [string]$ZsignProfile,          # iOS install: zsign mobileprovision (device UDID embedded)
    [string]$ZsignPassword = '',    # iOS install: password for the .p12 (blank if none)
    [switch]$Fetch,                 # cycle: `gh run download` the latest CI .ipa into dist\ first
    [string]$RunId,                 # cycle -Fetch: specific CI run id (default: latest successful)
    [int]$Matches = 3,              # run: stop after this many completed matches (0 = until Ctrl-C)
    [int]$DurationSec = 0,          # run: hard cap in seconds (0 = none)
    [int]$SettleSec = 12,           # run: grace for the two peers to discover + link
    [int]$Iterations = 1            # cycle: repeat the loop this many times back-to-back
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
# Free-sideloaded, NO admin: arm (container push) and tail (syslog) ride plain lockdown;
# launch/screenshot use the iOS 17+ *userspace* tunnel (`--userspace`), a pure-Python
# network stack that needs NO root/admin - so the old `sudo remote tunneld` step is gone.
# Install of a NEW binary is the one Apple gate (code signing) - see Install-Ios.
function Pmd    { & python -m pymobiledevice3 @args }
# A `developer dvt` call that stands up its own userspace tunnel in-process (no admin).
function PmdDev { & python -m pymobiledevice3 developer dvt @args --userspace }
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

# Install a NEW build of the app. Code signing is the one Apple gate that cannot be
# automated away without handling Apple-ID credentials (which this project deliberately
# does not do). We pick the most-headless route the caller has enabled, in order:
#   1. -SignedIpa <path>   : already signed -> `apps install` (fully headless, no GUI).
#   2. -ZsignP12 + -ZsignProfile : zsign the unsigned CI .ipa with a persisted free cert,
#      then `apps install` (fully headless for the 7-day life of that cert; re-run the
#      one-time cert setup weekly). zsign is a dev-only Tool, never linked into the app.
#   3. otherwise           : open Sideloadly at the .ipa (ASSISTED - the one human touch).
# Returns $true if the install path was headless (no human needed), else $false.
function Install-Ios {
    Ensure-Ios
    # (1) Pre-signed ipa -> headless apps install.
    if ($SignedIpa) {
        if (-not (Test-Path $SignedIpa)) { throw "signed ipa not found: $SignedIpa" }
        Say ('ios: headless install (signed) ' + (Split-Path $SignedIpa -Leaf))
        Pmd apps install $SignedIpa
        Say 'ios: installed'; return $true
    }
    # (2) zsign the unsigned CI ipa with a persisted free cert, then apps install.
    if ($ZsignP12 -and $ZsignProfile) {
        $zsign = (Get-Command zsign -ErrorAction SilentlyContinue).Source
        if (-not $zsign) { throw 'zsign not on PATH. Install it (dev-only Tool) or use -SignedIpa.' }
        if (-not (Test-Path $Ipa)) { throw "unsigned ipa not found: $Ipa" }
        if (-not (Test-Path $ZsignP12)) { throw "p12 not found: $ZsignP12" }
        if (-not (Test-Path $ZsignProfile)) { throw "mobileprovision not found: $ZsignProfile" }
        $signed = Join-Path $logs 'signed.ipa'
        Say 'ios: zsign the unsigned CI ipa with the persisted free cert'
        & $zsign -k $ZsignP12 -p $ZsignPassword -m $ZsignProfile -o $signed -z 5 $Ipa
        if ($LASTEXITCODE -ne 0) { throw 'zsign failed (cert/profile expired? re-run the weekly one-time setup).' }
        Say 'ios: headless install (zsigned)'
        Pmd apps install $signed
        Say 'ios: installed'; return $true
    }
    # (3) Assisted Sideloadly - the honest single human touch (first run also does the
    # Apple-ID login + 2FA; thereafter the daemon auto-refreshes until the cert expires).
    if (-not (Test-Path $Sideloadly)) { throw "Sideloadly not found: $Sideloadly" }
    if (-not (Test-Path $Ipa)) { throw "ipa not found: $Ipa" }
    Say ('ios: opening Sideloadly for ' + (Split-Path $Ipa -Leaf) + ' - finish sign+install in its window.')
    Start-Process $Sideloadly -ArgumentList @('-i', $Ipa)
    Warn 'ios: install is ASSISTED (Sideloadly GUI). For headless installs pass -SignedIpa or -ZsignP12/-ZsignProfile.'
    return $false
}

# Headless launch via the iOS 17+ userspace tunnel (no admin). Returns $true on success.
function Launch-Ios {
    try {
        PmdDev launch $App.IosBundleId 2>&1 | Out-Null
        Say 'ios: launched (userspace tunnel, no admin)'; return $true
    } catch {
        Warn 'ios: launch failed. The app must be installed + Developer Mode on; first run needs a one-time Bluetooth allow.'
        return $false
    }
}
function Shot-Ios($path) {
    try { PmdDev screenshot $path 2>&1 | Out-Null; Say "ios: screenshot -> $path" }
    catch { Warn 'ios: screenshot failed (userspace tunnel) - is the device unlocked + Developer Mode on?' }
}

# --- same-frame summary (engine log lines, game-agnostic) --------------------------
function Summarize($file, $label) {
    if (-not (Test-Path $file)) { Warn "$label : no log captured"; return }
    $auto = Get-Content $file | Select-String 'AUTOPLAY ' | Select-Object -Last 1
    $ends = (Get-Content $file | Select-String 'MATCH END').Count
    $line = if ($auto) { ($auto.Line -replace '.*AUTOPLAY','AUTOPLAY') } else { '(no AUTOPLAY line)' }
    Say "$label : $line ; matches ended=$ends"
}

# --- cycle helpers (fetch + install-skip) ------------------------------------------
# Pull the latest CI .ipa artifact so an edit->build(CI)->... loop needs no manual download.
function Fetch-Ipa {
    $gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
    if (-not $gh) { throw 'gh (GitHub CLI) not on PATH - cannot -Fetch the CI ipa.' }
    $rid = $RunId
    if (-not $rid) {
        Say 'ios: finding latest successful CI run with an ipa artifact'
        $rid = (& $gh run list --workflow macos-ci.yml --status success -L 1 --json databaseId --jq '.[0].databaseId')
        if (-not $rid) { throw 'no successful macos-ci run found.' }
    }
    $dist = Join-Path $root 'dist'
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    Say "ios: downloading ipa from CI run $rid"
    & $gh run download $rid -n OnlyChess-unsigned-ipa -D $dist
    if ($LASTEXITCODE -ne 0) { throw "gh run download failed for run $rid." }
}

# The app binary only needs re-installing when the .ipa actually changes: the Sideloadly
# daemon keeps an unchanged build's signature fresh, so re-running the loop is zero-touch.
# We stamp the installed ipa's hash and skip install when it matches.
function Ipa-Changed {
    $src = if ($SignedIpa) { $SignedIpa } else { $Ipa }
    if (-not (Test-Path $src)) { return $true }
    $stamp = Join-Path $logs 'installed.hash'
    $now = (Get-FileHash $src -Algorithm SHA256).Hash
    $was = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { '' }
    return ($now -ne $was)
}
function Stamp-Ipa {
    $src = if ($SignedIpa) { $SignedIpa } else { $Ipa }
    if (Test-Path $src) { (Get-FileHash $src -Algorithm SHA256).Hash | Set-Content -Path (Join-Path $logs 'installed.hash') -NoNewline }
}

# --- actions -----------------------------------------------------------------------
$doAndroid = $Peer -in @('android','both')
$doIos     = $Peer -in @('ios','both')

# One measured run: (re)launch + arm both peers, capture both engine logs, stop after
# $Matches matches (or $DurationSec), then summarize the same-frame tally. $Interactive
# controls the one place a human might be needed - the iOS Bluetooth-allow prompt on a
# fresh install; `cycle` passes $false so the loop never blocks.
function Invoke-Run([bool]$Interactive) {
    $andLog = Join-Path $logs 'android.log'; $iosLog = Join-Path $logs 'ios.log'
    Remove-Item $andLog,$iosLog -ErrorAction SilentlyContinue
    if ($doIos) {
        Ensure-Ios
        if (-not (Launch-Ios)) {
            if ($Interactive) { Read-Host 'Press Enter once the iPhone app is foregrounded + Bluetooth allowed' }
            else { Warn 'ios: launch failed and running non-interactively - continuing; log may be empty.' }
        }
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

switch ($Action) {
    'reset'   { if ($doAndroid) { Ensure-Android; Reset-Android }; if ($doIos) { Warn 'ios: reset = re-sideload for a fresh identity (no pm clear).' } }
    'install' { if ($doIos) { [void](Install-Ios) }; if ($doAndroid) { Ensure-Android; & $adb install -r (Join-Path $root 'Games\Chess\Android\app\build\outputs\apk\debug\app-debug.apk') } }
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
    'run'     { Invoke-Run $true }
    'cycle'   {
        # The full autonomous loop: (fetch ->) install-if-changed -> [run -> analyze] xN,
        # with ZERO human interaction per iteration after the one-time setup (README).
        # Only a NEW app binary needs the one assisted step (code signing); an unchanged
        # ipa is skipped, so re-running experiments back-to-back is entirely hands-off.
        if ($Fetch) { Fetch-Ipa }
        if ($doIos) {
            if (Ipa-Changed) {
                Say 'ios: ipa changed -> installing new build'
                $headless = Install-Ios
                Stamp-Ipa
                if (-not $headless) {
                    Read-Host 'Press Enter once Sideloadly has finished installing (assisted, one-time per new build)'
                }
                # A fresh install re-triggers the one-time Bluetooth (TCC) allow: launch once
                # and let the human tap Allow, then the loop is hands-off. Skipped when the
                # install was headless AND the app was already granted (unchanged bundle id).
            } else {
                Say 'ios: ipa unchanged -> skipping install (daemon keeps signature fresh)'
            }
        }
        if ($doAndroid -and (Test-Path (Join-Path $root 'Games\Chess\Android\app\build\outputs\apk\debug\app-debug.apk'))) {
            Ensure-Android; & $adb install -r (Join-Path $root 'Games\Chess\Android\app\build\outputs\apk\debug\app-debug.apk') | Out-Null
        }
        for ($i = 1; $i -le $Iterations; $i++) {
            Say "=== cycle iteration $i / $Iterations ==="
            Invoke-Run $false
        }
        Say "=== cycle complete: $Iterations iteration(s) ==="
    }
}
