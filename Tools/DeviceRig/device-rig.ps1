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
#   powershell -File Tools\DeviceRig\device-rig.ps1 -Action pullrec -Game rps   # both peers' .rec + diff
[CmdletBinding()]
param(
    [ValidateSet('run','cycle','arm','disarm','reset','clearhistory','install','uninstall','launch','tail','shot','status','pullrec')]
    [string]$Action = 'run',
    [ValidateSet('auto','central','peripheral')]
    [string]$AndroidRole = 'auto',  # dev BLE role override (LUR_INTERNAL): pin this peer's role
    [ValidateSet('auto','central','peripheral')]
    [string]$IosRole = 'auto',      # set the two COMPLEMENTARY (or both auto) or they never link
    [ValidateSet('android','ios','both')]
    [string]$Peer = 'both',
    [string]$AndroidSerial,         # pin when several transports point at one phone
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
    [int]$LinkTimeoutSec = 35,      # run: abort LOUDLY if no peer reports READY by then
    [int]$Iterations = 1,           # cycle: repeat the loop this many times back-to-back
    [int]$InstallTimeoutSec = 240,  # install: hard bound on `apps install` (never hang silently)
    [switch]$ForceUninstall,        # install: DESTRUCTIVE fallback - uninstall first, for the day
                                    #   SIGKILL stops working. Stop-IosApp is the normal path.
                                    #   Wipes the container: device GUID, opponent history, and any
                                    #   .rec flight recordings not yet pulled. Off by default.
    [switch]$NoReset,               # run: DON'T pm-clear Android or clear logcat - arm the
                                    #   already-running/linked apps + keep the buffered
                                    #   handshake. Use when peers are already paired live.
    [switch]$Fresh,                 # run: force-stop + relaunch BOTH (fresh handshake) WITHOUT
    [string]$RecDir,                # pullrec: where the .rec files land (default: dist\rec\<peer>\)
    [string]$Game = 'chess'         # which game's app under test: 'chess' or 'rps'
)                                   #   pm-clear (identity kept), and clear the log window.
                                    #   The clean-measurement mode: fresh link, no churn.
$ErrorActionPreference = 'Stop'

# --- App under test - the ONLY app-specific config. The rig body stays game-agnostic.
#     Selected by -Game (chess|rps); the two games share the engine (and BLE UUID), so
#     the rig serves both — the second consumer that makes this config a real switch. ---
if ($Game -eq 'rps') {
    $App = @{
        LogTag         = 'OnlyRps'
        AndroidPackage = 'com.lurmotorn.onlyrps'
        IosBundleId    = 'com.lurmotorn.onlyrps.L5XBWVZ7N3'  # sideload appends the same signer suffix
        IosBundleBase  = 'com.lurmotorn.onlyrps'
        AutoplayProp   = 'debug.lur.autoplay'                # RPS dev build auto-plays already; harmless
        AutoplayMarker = 'autoplay'
        RoleProp       = 'debug.lur.role'
        RoleMarker     = 'role'
        ClearMarker    = 'clearsave'
        # Flight recordings (pullrec). The rig body stays game-agnostic: it knows only
        # "this app writes recordings HERE, matching THIS glob, and THIS tool pairs them".
        RecGlob        = 'rps-*.rec'                         # everything the recorder writes
        RecPairGlob    = 'rps-vs-*.rec'                      # ...of which THESE are two-peer captures
        RecDiffExe     = 'build-desktop\Games\RocksPapersScissors\Desktop\onlyrps_desktop.exe'
        RecDiffArg     = '--recdiff'
        AndroidApk     = 'Games\RocksPapersScissors\Android\app\build\outputs\apk\debug\app-debug.apk'
    }
} else {
    $App = @{
        LogTag         = 'OnlyChess'                          # engine log tag the app emits
        AndroidPackage = 'com.lurmotorn.onlychess'
        IosBundleId    = 'com.lurmotorn.onlychess.L5XBWVZ7N3' # sideload appends the signer suffix
        IosBundleBase  = 'com.lurmotorn.onlychess'            # uninstall sweeps every ...<SIGNER> variant
        AutoplayProp   = 'debug.lur.autoplay'                 # Android: engine autoplay toggle (setprop)
        AutoplayMarker = 'autoplay'                           # iOS: Documents/<marker> engine autoplay toggle
        RoleProp       = 'debug.lur.role'                     # Android: dev BLE role override (setprop)
        RoleMarker     = 'role'                               # iOS: Documents/<marker> dev BLE role override
        ClearMarker    = 'clearsave'                          # iOS: Documents/<marker> one-shot history wipe
        RecGlob        = $null                                # chess has no flight recorder (pullrec no-ops)
        AndroidApk     = 'Games\Chess\Android\app\build\outputs\apk\debug\app-debug.apk'
    }
}

$root = (Resolve-Path (Join-Path (Split-Path $MyInvocation.MyCommand.Path) '..\..')).Path
$Sideloadly = Join-Path $env:LOCALAPPDATA 'Sideloadly\sideloadly.exe'
if (-not $Ipa) {
    $IpaName = if ($Game -eq 'rps') { 'OnlyRps-unsigned.ipa' } else { 'OnlyChess-unsigned.ipa' }
    $Ipa = Join-Path $root (Join-Path 'dist' $IpaName)
}

$rig  = Split-Path $MyInvocation.MyCommand.Path
$logs = Join-Path $rig '.logs'
New-Item -ItemType Directory -Force -Path $logs | Out-Null
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path','User')

function Say($m)  { Write-Host "[device-rig] $m" -ForegroundColor Cyan }
function Warn($m) { Write-Host "[device-rig] $m" -ForegroundColor Yellow }

# --- THE NO-HANG INVARIANT ----------------------------------------------------------
# EVERY call out to a device — adb, pymobiledevice3, zsign — goes through Invoke-Bounded.
# Nothing in this script may wait on an external service without a bound.
#
# Why this is a rule and not a nicety: a remote device can always stall, and a stall that
# never returns is INDISTINGUISHABLE from a crash. The caller cannot tell "busy" from
# "broken", so the only way out has been a human noticing and swiping the app away. That has
# been the single biggest time sink in iOS work on this project. #168 bounded the install
# case after it cost a session; #179 found `dvt screenshot` doing exactly the same thing
# months later. Fixing instances does not fix the class — this does.
#
# A REAL pid, not Start-Job: on timeout we must kill the whole TREE. The direct child is
# python or adb, and orphaning it leaves the device's service socket held open — which makes
# the NEXT invocation hang too, turning one stall into a dead rig until someone reboots
# something. Killing the tree is what makes a timeout recoverable rather than contagious.
$script:TimedOut = @()
function Invoke-Bounded {
    param(
        [Parameter(Mandatory)][string]$What,      # human description, used in the timeout message
        [Parameter(Mandatory)][int]$TimeoutSec,
        [Parameter(Mandatory)][string]$Exe,
        [string[]]$CmdArgs = @(),
        [switch]$Quiet,                           # discard output (stderr-benign calls)
        [switch]$NoThrow                          # return $null on timeout instead of throwing
    )
    if (-not $Exe) { throw "Invoke-Bounded: no executable for '$What'" }
    $so = [System.IO.Path]::GetTempFileName()
    $se = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $CmdArgs -NoNewWindow -PassThru `
                           -RedirectStandardOutput $so -RedirectStandardError $se
        if (-not $p.WaitForExit($TimeoutSec * 1000)) {
            # /T kills descendants too — see the tree note above.
            & taskkill.exe /PID $p.Id /T /F 2>&1 | Out-Null
            $script:TimedOut += $What
            $msg = "TIMEOUT after ${TimeoutSec}s: $What - killed it and its children rather than hanging."
            if ($NoThrow) { Warn $msg; return $null }
            throw $msg
        }
        if ($Quiet) { return $null }
        return (Get-Content $so -Raw -ErrorAction SilentlyContinue)
    } finally {
        Remove-Item $so, $se -Force -ErrorAction SilentlyContinue
    }
}
# Per-class default bounds. Generous enough that a healthy call never trips one, short enough
# that a stalled call is noticed in seconds rather than never.
$script:TmoAdb    = 30    # any adb round-trip (install has its own, below)
$script:TmoPmd    = 60    # lockdown-level pymobiledevice3 (usbmux, apps push/rm)
$script:TmoPmdDev = 45    # `developer dvt` over the userspace tunnel

# --- Android peer (adb) ------------------------------------------------------------
$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
function Adb { Invoke-Bounded -What "adb $($args -join ' ')" -TimeoutSec $script:TmoAdb -Exe $adb -CmdArgs $args }
# For adb calls whose native stderr is benign (monkey/force-stop): PS 5.1 turns ANY native
# stderr into a fatal NativeCommandError under ErrorActionPreference=Stop, so relax it here.
function AdbQuiet {
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'SilentlyContinue'
    try { Invoke-Bounded -What "adb $($args -join ' ')" -TimeoutSec $script:TmoAdb -Exe $adb -CmdArgs $args -Quiet -NoThrow | Out-Null }
    finally { $ErrorActionPreference = $prev }
}
# A bounded adb round-trip: `adb devices` LIES about half-dead wireless transports (the
# cached entry stays "device" while every real command hangs forever - the silent-stall
# class). Only an actual echo through the device proves it's alive.
function Test-AndroidAlive([string]$Serial) {
    $job = Start-Job -ScriptBlock { param($a, $s) & $a -s $s shell echo RT-OK 2>$null } -ArgumentList $adb, $Serial
    $ok = (Wait-Job $job -Timeout 6) -and ((Receive-Job $job) -match 'RT-OK')
    Stop-Job $job -ErrorAction SilentlyContinue; Remove-Job $job -Force -ErrorAction SilentlyContinue
    return [bool]$ok
}
function Ensure-Android {
    if (-not $adb) { throw 'adb not found on PATH (Android platform-tools).' }
    if (-not $AndroidSerial) {
        $dev = (Adb devices) -split "`n" | Where-Object { $_ -match "`tdevice$" }
        if ($dev) { $script:AndroidSerial = (($dev | Select-Object -First 1) -split '\s+')[0] }
    }
    if ($AndroidSerial -and (Test-AndroidAlive $AndroidSerial)) { $env:ANDROID_SERIAL = $AndroidSerial; return }
    # Dead or missing: rediscover over mDNS (the wireless port CHANGES between drops).
    Warn "android: '$AndroidSerial' not responding - rediscovering over mDNS..."
    AdbQuiet disconnect
    $ep = ((Adb mdns services) -split "`n" | Where-Object { $_ -match '_adb-tls-connect' } |
           Select-Object -First 1) -replace '.*\s(\S+:\d+)\s*$', '$1'
    if ($ep -and $ep -match ':\d+$') {
        Say "android: reconnecting to $ep"
        AdbQuiet connect $ep
        if (Test-AndroidAlive $ep) { $script:AndroidSerial = $ep; $env:ANDROID_SERIAL = $ep; Say 'android: reconnected + round-trip OK'; return }
    }
    throw "android: UNREACHABLE - wireless debugging is off/dropped on the phone. Toggle Settings > Developer options > Wireless debugging, then retry. (adb devices lies about dead transports; this check does a real round-trip.)"
}
function Reset-Android {
    Say "android: reset identity + grant link permissions ($($App.AndroidPackage))"
    Adb shell pm clear $App.AndroidPackage | Out-Null
    foreach ($p in 'BLUETOOTH_SCAN','BLUETOOTH_ADVERTISE','BLUETOOTH_CONNECT','ACCESS_FINE_LOCATION') {
        Adb shell pm grant $App.AndroidPackage "android.permission.$p"
    }
}
function Arm-Android    { Say "android: arm autoplay ($($App.AutoplayProp)=1)"; Adb shell setprop $App.AutoplayProp 1 }
# Pin/clear the dev BLE role override (read by the app per role decision).
function Role-Android {
    $v = if ($AndroidRole -eq 'auto') { '""' } else { $AndroidRole }
    Say "android: BLE role override = $AndroidRole"
    AdbQuiet shell setprop $App.RoleProp $v
}
# Wipe opponent history: per-opponent records (32-hex files), their meta sidecars, and
# the cached peer-id — the DEVICE-ID IS KEPT (stable identity/colour). Fresh pairing state.
function ClearHistory-Android {
    Say 'android: clear opponent history (records + meta + peer-id; device-id kept)'
    Kill-Android
    # run-as starts in the app data dir; sh -c so the DEVICE shell expands the globs.
    # A record file's name is exactly 32 hex chars -> the 32-'?' glob (device-id is 9).
    $wipe = 'rm -f files/peer-id files/meta-* files/' + ('?' * 32)
    AdbQuiet shell "run-as $($App.AndroidPackage) sh -c '$wipe'"
}
function Disarm-Android { Say 'android: disarm autoplay'; AdbQuiet shell setprop $App.AutoplayProp 0; AdbQuiet shell am force-stop $App.AndroidPackage }
function Launch-Android {
    Say 'android: wake + launch'
    AdbQuiet shell input keyevent KEYCODE_WAKEUP
    AdbQuiet shell monkey -p $App.AndroidPackage -c android.intent.category.LAUNCHER 1
    # VERIFY the process actually came up - monkey fails silently (locked profile,
    # missing app, disabled package). A dead peer must abort loudly, not idle (#75).
    for ($i = 0; $i -lt 10; $i++) {
        $p = (Invoke-Bounded -What "adb shell pidof" -TimeoutSec 10 -Exe $adb -NoThrow `
                             -CmdArgs @('-s', $AndroidSerial, 'shell', 'pidof', $App.AndroidPackage))
        if ($p) { Say "android: app up (pid $($p.Trim()))"; return }
        Start-Sleep -Milliseconds 500
    }
    throw "android: app did NOT start after launch (monkey silently failed?) - is the device unlocked and the app installed?"
}
function Kill-Android { Say 'android: force-stop'; AdbQuiet shell am force-stop $App.AndroidPackage }
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
function Pmd { Invoke-Bounded -What "pymobiledevice3 $($args -join ' ')" -TimeoutSec $script:TmoPmd `
                             -Exe 'python' -CmdArgs (@('-m','pymobiledevice3') + $args) }
# For pmd calls whose stderr is benign progress logging (provision dump etc.): PS 5.1
# turns native stderr into a fatal NativeCommandError under EAP=Stop (see AdbQuiet).
function PmdQuiet {
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'SilentlyContinue'
    try {
        Invoke-Bounded -What "pymobiledevice3 $($args -join ' ')" -TimeoutSec $script:TmoPmd `
                       -Exe 'python' -CmdArgs (@('-m','pymobiledevice3') + $args) -Quiet -NoThrow | Out-Null
    } finally { $ErrorActionPreference = $prev }
}
# A `developer dvt` call that stands up its own userspace tunnel in-process (no admin).
function PmdDev {
    Invoke-Bounded -What "dvt $($args -join ' ')" -TimeoutSec $script:TmoPmdDev -Exe 'python' `
                   -CmdArgs (@('-m','pymobiledevice3','developer','dvt') + $args + @('--userspace'))
}
# Same, with a caller-chosen bound. Deliberately a separate function rather than a -Timeout
# parameter on PmdDev: a param block would make `PmdDev screenshot out.png` try to bind
# "screenshot" positionally to an int and fail. First arg is the timeout, rest is the command.
function PmdDevT {
    $t = [int]$args[0]
    $rest = @($args[1..($args.Count - 1)])
    Invoke-Bounded -What "dvt $($rest -join ' ')" -TimeoutSec $t -Exe 'python' -NoThrow `
                   -CmdArgs (@('-m','pymobiledevice3','developer','dvt') + $rest + @('--userspace'))
}
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
# Pin/clear the dev BLE role override marker. Read at app STARTUP — apply before launch.
function Role-Ios {
    Say "ios: BLE role override = $IosRole"
    if ($IosRole -eq 'auto') {
        try { Pmd apps rm $App.IosBundleId "Documents/$($App.RoleMarker)" 2>&1 | Out-Null } catch {}
    } else {
        $f = Join-Path $logs $App.RoleMarker
        # ASCII bytes, NOT Set-Content: PS 5.1 writes UTF-16LE+BOM, which fails the
        # app's strict UTF-8 read and silently disables the override (learned on-device).
        [System.IO.File]::WriteAllText($f, $IosRole)
        Pmd apps push $App.IosBundleId $f "Documents/$($App.RoleMarker)" 2>&1 | Out-Null
    }
}
# Wipe opponent history via the one-shot clearsave marker (consumed at next launch;
# device-id kept). Kill + relaunch so the wipe actually runs.
function ClearHistory-Ios {
    Say 'ios: clear opponent history (records + meta + peer-id; device-id kept)'
    $f = Join-Path $logs $App.ClearMarker
    [System.IO.File]::WriteAllText($f, '1')
    Pmd apps push $App.IosBundleId $f "Documents/$($App.ClearMarker)" 2>&1 | Out-Null
    [void](Launch-Ios -FreshProcess)   # process restart consumes the marker and wipes
}

# --- Flight recordings: collect BOTH peers' .rec files, and pair them (#171) -------
#
# WHY THIS EXISTS. Recording a linked match on both phones is only half a capability — the
# other half is getting the two files onto one machine, and that half had no command at all.
# So "did the iPhone record?" got answered by hand, through an interactive AFC shell, against
# a path with a SPACE in it (Library/Application Support). That shell is a xonsh REPL: it
# word-splits the path into "Library/Application" and "Support" and reports `cannot access`
# for both, and on a Windows console it also dies on a UTF-8 emoji in its own banner. The
# resulting listing is partial and reads exactly like an empty one.
#
# It produced a wrong bug report (#171: "the iPhone still writes NO linked recording"). The
# iPhone had in fact written twelve of them, including the very match said to be missing —
# and because the pair was believed not to exist, #159 sat blocked for two days on a diff
# that could have been run the whole time. An observation channel that fails by UNDER-
# reporting is the dangerous kind: absence looks like evidence.
#
# Hence: one command, no interactive shell, no hand-typed paths. `apps pull` takes the
# directory whole and handles the space itself.
function Pull-Rec-Android($dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $names = @(Adb shell run-as $App.AndroidPackage ls files/ 2>$null |
               ForEach-Object { $_.Trim() } | Where-Object { $_ -like $App.RecGlob })
    foreach ($n in $names) {
        # cmd's `>` is byte-exact. PowerShell's would re-encode and rewrite the line endings,
        # and the .rec parser reads lines — a stray CR corrupts the trailing `end <r> <tick>`.
        $out = Join-Path $dest $n
        & cmd /c ('"{0}" exec-out run-as {1} cat "files/{2}" > "{3}"' -f $adb, $App.AndroidPackage, $n, $out)
    }
    Say "android: pulled $($names.Count) recording(s) -> $dest"
    return $names.Count
}
function Pull-Rec-Ios($dest) {
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $stage = Join-Path $logs 'recpull'
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    # The whole directory in one call — no per-file quoting, and no listing step to get wrong.
    PmdQuiet apps pull $App.IosBundleId 'Library/Application Support' $stage
    $files = @(Get-ChildItem -Path $stage -Recurse -File -Filter $App.RecGlob -ErrorAction SilentlyContinue)
    foreach ($f in $files) { Copy-Item $f.FullName (Join-Path $dest $f.Name) -Force }
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    Say "ios: pulled $($files.Count) recording(s) -> $dest"
    return $files.Count
}
# Pair the two sides by what the MATCH is, not by filename: each peer stamps its own local
# clock and its own per-session ordinal into the name, so the same match is `...-073714-1.rec`
# on one phone and `...-073715-2.rec` on the other. Seed + build + the footer's end line
# identify it on both.
function Rec-Seed($path) {
    foreach ($line in [System.IO.File]::ReadLines($path)) { if ($line -like 'seed *') { return $line.Substring(5).Trim() } }
    return '?'
}
function Rec-Key($path) {
    $seed = $null; $end = $null; $fp = $null
    foreach ($line in [System.IO.File]::ReadLines($path)) {
        if     ($line -like 'seed *') { $seed = $line.Substring(5).Trim() }
        elseif ($line -like 'fp *')   { $fp   = $line.Substring(3).Trim() }
        elseif ($line -like 'end *')  { $end  = $line.Substring(4).Trim() }
    }
    # An ABANDONED recording (no `end` line — the app was killed mid-match) is deliberately not
    # keyed. Seed alone cannot identify a match: every session walks the same ladder from
    # kMatchSeed, so match N of Tuesday and match N of Friday share a seed. Keying those as
    # "seed/open" is what this function did on its first run, and it duly paired a 1819-tick
    # capture with an unrelated 3-tick one from a different build. Refusing to guess is better:
    # an unpaired file is reported, and a wrong pair is a fabricated diff.
    if (-not $seed -or -not $end) { return $null }
    # `end <result> <tick>` at tick 0 is a match that was torn down before it simulated anything.
    # There is nothing in it to diff, and every such file on a device carries the same end line, so
    # keying them just manufactures collisions.
    if ($end -match '\s0$') { return $null }
    return "$seed/$end/$fp"
}
function Diff-Rec-Pairs($andDir, $iosDir) {
    $exe = Join-Path $root $App.RecDiffExe
    $unkeyed = New-Object System.Collections.ArrayList
    $index = {
        param($dir, $side)
        $map = @{}
        foreach ($f in Get-ChildItem $dir -File -Filter $App.RecPairGlob -ErrorAction SilentlyContinue) {
            $k = Rec-Key $f.FullName
            if (-not $k) {
                # Say WHICH match it was anyway. These are mostly captures the app was killed
                # mid-way (no `end` line), which includes the longest and most interesting ones —
                # so the seed is what lets a human spot the obvious counterpart on the other side
                # and diff it by hand. A bare filename list would be unusable.
                [void]$unkeyed.Add("  $side not auto-pairable (no end line, or ended at tick 0): $($f.Name)  seed=$(Rec-Seed $f.FullName)")
                continue
            }
            # Two files on ONE side with the same key means the key is not unique for this batch;
            # say so rather than silently keeping whichever enumerated last. $null marks the key
            # poisoned, so a third collision must not try to Split-Path it.
            if ($map.ContainsKey($k)) {
                $other = if ($map[$k]) { Split-Path $map[$k] -Leaf } else { 'an earlier file' }
                Warn "  $side AMBIGUOUS: $($f.Name) and $other share key [$k] - skipping both"
                $map[$k] = $null; continue
            }
            $map[$k] = $f.FullName
        }
        return $map
    }
    $a = & $index $andDir 'android'
    $b = & $index $iosDir 'ios    '
    $paired = @($a.Keys | Where-Object { $a[$_] -and $b[$_] })
    Say "pairs: $($paired.Count) match(es) captured by BOTH peers ($($a.Count) android / $($b.Count) ios linked recordings)"
    # Name what did NOT pair. A one-sided capture is a real finding (the other peer was in solo,
    # or never entered the match) and silently diffing only the pairs would hide it.
    foreach ($k in @($a.Keys | Where-Object { $a[$_] -and -not $b[$_] })) { Warn "  android-only: $(Split-Path $a[$k] -Leaf)  [$k]" }
    foreach ($k in @($b.Keys | Where-Object { $b[$_] -and -not $a[$_] })) { Warn "  ios-only:     $(Split-Path $b[$k] -Leaf)  [$k]" }
    foreach ($u in $unkeyed) { Warn $u }
    if (-not (Test-Path $exe)) {
        Warn "recdiff skipped: $exe not built (scripts\rps-desktop-build.ps1)"
        return
    }
    foreach ($k in $paired) {
        Write-Host ''
        & $exe $App.RecDiffArg $a[$k] $b[$k]
    }
}

# --- Install blocking: detect it, never hang on it ---------------------------------
# THE FAILURE THIS SOLVES. `apps install` blocks for as long as the app is RUNNING on the
# phone, and it blocks SILENTLY - no progress, no error, no timeout. The human-visible shape is
# "the install just sits there until I swipe the app away, then it finishes instantly", and the
# agent-visible shape is worse: nothing at all, for as long as you are willing to wait. Android
# has no equivalent (`adb install -r` kills and replaces a running app), so an install of BOTH
# peers stalls on the iOS half only, which is exactly where nobody is looking.
#
# It was compounded by the rig itself: the old code was `Pmd apps install $x | Out-Host` followed
# unconditionally by `Say 'ios: installed'; return $true`. The exit code was never read, so a
# failed - or externally killed - install still reported success and returned the headless flag.
# A hang and a lie, in the one step that decides which binary is on the phone.
#
# WHAT DOES NOT WORK, measured on iOS 26.5 / pymobiledevice3 on 2026-08-01: `dvt kill <pid>`
# exits 0 and `dvt pkill OnlyRps` even logs "Killing OnlyRps(5099)", and the process survives
# BOTH with an unchanged pid and start time. Only a kill-existing `dvt launch` restarts it, and
# that relaunches rather than terminates, so it is no use before an install. There is no headless
# terminate. Hence: detect, say so in one actionable line, and WAIT - resuming the moment the app
# closes - instead of blocking forever on a condition nobody can see.
#
# This is the iOS twin of Test-AndroidAlive above: the same silent-stall class, the same rule -
# a bounded probe beats a call that can never return.

# The app's installation record, used as INSTALL EVIDENCE. The CLI's exit is NOT the source of
# truth: measured 2026-08-01, an install LANDED on the phone (verified by the new build running)
# while the CLI sat unreturned for over ten minutes. iOS mints a fresh Bundle/Application UUID on
# every reinstall, so a changed record proves the new binary arrived regardless of what the CLI
# did. Returns $null when the app is absent or the query fails.
# Kill pymobiledevice3 processes started since $Since. Stop-Job kills the child PowerShell but
# NOT its python grandchild, so a timed-out job can leave python alive holding the device's
# service socket — and then the next call hangs too. That is how one stall becomes a dead rig
# until someone reboots something, and it is the reason "we tried to prevent this" kept failing.
# Scoped by start time so a concurrent syslog tail the operator started earlier is never touched.
function Kill-StrayPmd([datetime]$Since) {
    try {
        $procs = Get-CimInstance Win32_Process -Filter "Name='python.exe'" -ErrorAction SilentlyContinue |
                 Where-Object { $_.CommandLine -like '*pymobiledevice3*' -and $_.CreationDate -ge $Since }
        foreach ($q in $procs) {
            Warn "reaped orphaned pymobiledevice3 (pid $($q.ProcessId)) left behind by a timeout"
            & taskkill.exe /PID $q.ProcessId /T /F 2>&1 | Out-Null
        }
    } catch { }
}

function Invoke-BoundedPmd([string[]]$PmdArgs, [int]$TimeoutSec = 60) {
    $t0 = Get-Date
    $job = Start-Job -ScriptBlock { param($a) & python -m pymobiledevice3 @a 2>$null } -ArgumentList (, $PmdArgs)
    if (-not (Wait-Job $job -Timeout $TimeoutSec)) {
        Stop-Job   $job -ErrorAction SilentlyContinue
        Remove-Job $job -Force -ErrorAction SilentlyContinue
        Kill-StrayPmd $t0
        $script:TimedOut += "pymobiledevice3 $($PmdArgs -join ' ')"
        return $null
    }
    $out = (Receive-Job $job) -join "`n"
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    return $out
}

function Get-IosAppRecord {
    # 150s, not 60: `apps list` enumerates every installed app and measured slower than a minute here.
    $out = Invoke-BoundedPmd @('apps', 'list') 150
    if (-not $out) { return $null }
    # Deliberately NOT ConvertFrom-Json. That output is ~2.25 MB on this device and PS 5.1's
    # ConvertFrom-Json refuses anything past its ~2 MB MaxJsonLength, so it threw and the evidence
    # silently became "unavailable" — which is exactly what the first real run of this reported, on an
    # install that had in fact succeeded. A targeted read of our own record has no size limit.
    #
    # Path holds the Bundle/Application UUID and SequenceNumber increments, so both change on a
    # reinstall: that is the whole signal. ("Path" cannot collide with "ParallelPlaceholderPath" —
    # the leading quote is part of the pattern.)
    $i = $out.IndexOf('"' + $App.IosBundleId + '"')
    if ($i -lt 0) { return $null }
    $seg = $out.Substring($i, [Math]::Min(4000, $out.Length - $i))
    $parts = @()
    foreach ($f in 'Path', 'SequenceNumber', 'Container') {
        $m = [regex]::Match($seg, '"' + $f + '"\s*:\s*"?([^",\r\n]+)')
        if ($m.Success) { $parts += ($f + '=' + $m.Groups[1].Value.Trim()) }
    }
    if ($parts.Count -eq 0) { return $null }
    return ($parts -join '|')
}

# Is the app running? Returns the pid as a STRING, '' when definitively not running, or 'unknown'
# when the probe itself failed (a dead tunnel must not be read as "not running"). String-typed on
# purpose: an int-vs-'unknown' comparison throws under ErrorActionPreference=Stop.
function Get-IosAppPid {
    $out = Invoke-BoundedPmd @('developer', 'dvt', 'proclist', '--userspace') 60
    if (-not $out) { return 'unknown' }
    try { $j = $out | ConvertFrom-Json } catch { return 'unknown' }
    foreach ($p in $j) {
        if ("$($p.bundleIdentifier)" -eq $App.IosBundleId) { return "$($p.pid)" }
    }
    return ''
}

# TERMINATE the app, headlessly. Returns $true once it is genuinely gone.
#
# THE ONE THAT ACTUALLY WORKS: `dvt signal <pid> 9`. #168 concluded no headless terminate existed on
# iOS 26 — but it had only tried `dvt kill` and `dvt pkill`, which ride a DTX request the OS accepts
# and ignores (pkill even logs "Killing OnlyRps(5099)" while the pid and start time are unchanged).
# `signal` sends a REAL POSIX signal through a different request, and SIGKILL takes the process down:
# verified 2026-08-01, pid 5255 -> absent from proclist.
#
# This matters more than a convenience. `apps install` blocks HARD on a running app — measured twice
# that day, both installs sitting until a person swiped the app away — so without a headless kill the
# rig cannot reinstall without hands on the phone, which makes the whole instrument useless for the
# unattended two-phone runs it exists for. Never replace this with an instruction to a human.
function Stop-IosApp {
    $p = Get-IosAppPid
    if ($p -eq '')        { return $true }        # already gone
    if ($p -eq 'unknown') { Warn 'ios: cannot probe the app pid (dvt tunnel refused) - cannot terminate.'; return $false }
    Say "ios: terminating $($App.LogTag) (pid $p) with SIGKILL via dvt signal"
    [void](Invoke-BoundedPmd @('developer', 'dvt', 'signal', $p, '9', '--userspace') 60)
    # Trust the PROBE, not the exit code — `kill`/`pkill` taught us the CLI reports success either way.
    for ($i = 0; $i -lt 8; $i++) {
        Start-Sleep -Seconds 2
        $q = Get-IosAppPid
        if ($q -eq '') { Say 'ios: app terminated'; return $true }
        if ($q -eq 'unknown') { return $false }
    }
    Warn "ios: $($App.LogTag) survived SIGKILL - still running."
    return $false
}

# `apps install` with a HARD bound, and its exit code actually returned. -1 means it was still
# running at the deadline and we killed it.
function Invoke-BoundedInstall([string]$Path, [int]$TimeoutSec) {
    $t0 = Get-Date
    # A JOB, not `Start-Process -PassThru`. That was the first attempt and its Process object does not
    # reliably expose ExitCode: a real install printed "Installation succeed." and the rig then said
    # "install FAILED (exit code )" from a $null that matched neither 0 nor -1. Reading $LASTEXITCODE
    # INSIDE the job gets the actual code, and Wait-Job still bounds it. -1 = timed out and killed,
    # -2 = exited but the code could not be read (caller falls through to the bundle evidence rather
    # than inventing a verdict).
    $job = Start-Job -ScriptBlock {
        param($p)
        & python -m pymobiledevice3 apps install $p 2>&1 | Out-Null
        $LASTEXITCODE
    } -ArgumentList $Path
    if (-not (Wait-Job $job -Timeout $TimeoutSec)) {
        Stop-Job   $job -ErrorAction SilentlyContinue
        Remove-Job $job -Force -ErrorAction SilentlyContinue
        Kill-StrayPmd $t0
        $script:TimedOut += 'pymobiledevice3 apps install'
        return -1
    }
    $code = @(Receive-Job $job) | Where-Object { $_ -is [int] } | Select-Object -Last 1
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    if ($null -eq $code) { return -2 }
    return [int]$code
}

# The one install path: clear the blocker if we can, bound the call, and decide success on
# EVIDENCE rather than on the CLI's say-so.
function Install-IpaHeadless([string]$Path, [string]$What) {
    if ($ForceUninstall) {
        Warn 'ios: -ForceUninstall - uninstalling first (WIPES the container: device GUID, opponent history, unpulled .rec files).'
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'SilentlyContinue'
        try { Invoke-Bounded -What 'pymobiledevice3 apps uninstall' -TimeoutSec 120 -Exe 'python' -Quiet -NoThrow `
                             -CmdArgs @('-m','pymobiledevice3','apps','uninstall', $App.IosBundleId) | Out-Null } catch {}
        $ErrorActionPreference = $prev
    }
    $before = Get-IosAppRecord
    $appPid = Get-IosAppPid
    if ($appPid -eq 'unknown') {
        Warn 'ios: could not probe whether the app is running (dvt tunnel refused) - installing anyway, bounded.'
    } elseif ($appPid -ne '') {
        # The app is running, and `apps install` will BLOCK until it exits — measured, repeatedly, on
        # 2026-08-01: the two installs that day only completed at the moment a person swiped the app
        # away. It is a wall, not a delay. Waiting it out is not an option.
        #
        # And asking a human is not an option either: this rig exists to be driven headlessly, so a
        # step that needs hands on the phone is a broken step, not a documented one. Terminate it
        # ourselves — Stop-IosApp is the headless kill, and it is required to succeed.
        Warn "ios: $($App.LogTag) is RUNNING (pid $appPid) - it would block the install; terminating it."
        if (-not (Stop-IosApp)) {
            throw ("ios: could not terminate $($App.LogTag) headlessly, and a running app blocks " +
                   '`apps install` indefinitely. Re-run with -ForceUninstall for the guaranteed ' +
                   'zero-touch path (it wipes the container - pull .rec files first with -Action pullrec).')
        }
    }
    Say "ios: headless install ($What), bounded at ${InstallTimeoutSec}s"
    $code  = Invoke-BoundedInstall $Path $InstallTimeoutSec
    $after = Get-IosAppRecord
    if ($code -eq 0) { Say 'ios: installed'; return $true }
    # Not a clean exit. Before calling it a failure, look at the phone: the install may well have
    # landed anyway (see Get-IosAppRecord).
    if ($before -and $after -and ($before -ne $after)) {
        Warn "ios: install did not exit cleanly (code $code) but the bundle record CHANGED - the new build IS on the phone."
        return $true
    }
    $evidence = 'bundle record unchanged'
    if (-not $before -or -not $after) { $evidence = 'bundle record unavailable, so this is inconclusive' }
    if ($code -eq -1) {
        throw ("ios: install TIMED OUT after ${InstallTimeoutSec}s ($evidence). " +
               'The usual cause is the app still running on the phone: close it and re-run. ' +
               'Raise -InstallTimeoutSec for a genuinely slow link.')
    }
    throw "ios: install FAILED (exit code $code; $evidence)."
}

# Install a NEW build of the app. Code signing is the one Apple gate; since 2026-07-18
# it is fully automated with LOCAL material (nothing Apple-ID-credential-shaped):
#   1. -SignedIpa <path>   : already signed -> `apps install` (fully headless).
#   2. AUTO-ZSIGN (default): zsign the unsigned CI .ipa with the free dev cert/key PEMs
#      Sideloadly persists (%APPDATA%\Sideloadly, cert lasts ~1 year) + the NEWEST
#      matching provisioning profile dumped from the device itself (profiles last 7
#      days; Sideloadly's weekly renewal is picked up automatically by re-dumping).
#      zsign is a dev-only Tool (MIT), never linked into the app.
#   3. -ZsignP12 + -ZsignProfile : explicit signing material (overrides the auto path).
#   4. otherwise           : open Sideloadly at the .ipa (ASSISTED - the fallback).
# Returns $true if the install path was headless (no human needed), else $false.
function Install-Ios {
    Ensure-Ios
    # (1) Pre-signed ipa -> headless apps install.
    if ($SignedIpa) {
        if (-not (Test-Path $SignedIpa)) { throw "signed ipa not found: $SignedIpa" }
        return (Install-IpaHeadless $SignedIpa ('signed ' + (Split-Path $SignedIpa -Leaf)))
    }
    # (2)/(3) zsign + apps install. Explicit -ZsignP12/-ZsignProfile wins; else auto-locate.
    $zsign = (Get-Command zsign -ErrorAction SilentlyContinue).Source
    if (-not $zsign) { $cand = Join-Path $env:LOCALAPPDATA 'LurMotorn\tools\zsign.exe'; if (Test-Path $cand) { $zsign = $cand } }
    $key = $ZsignP12; $cert = $null; $prov = $ZsignProfile
    if (-not $key) {
        $key  = Join-Path $env:APPDATA 'Sideloadly\key.pem'
        $cert = (Get-ChildItem (Join-Path $env:APPDATA 'Sideloadly\cert-*.pem') -ErrorAction SilentlyContinue | Select-Object -First 1).FullName
        if (-not (Test-Path $key) -or -not $cert) { $key = $null }
    }
    if (-not $prov -and $zsign -and $key) {
        # Re-dump the device's profiles (picks up Sideloadly's weekly renewal) and take
        # the newest one whose app id matches this bundle.
        $profDir = Join-Path $env:LOCALAPPDATA 'LurMotorn\tools\profiles'
        New-Item -ItemType Directory -Force -Path $profDir | Out-Null
        PmdQuiet provision dump $profDir
        $prov = & python -c @"
import glob, plistlib, sys
best = None
for p in glob.glob(r'$profDir' + '/*.mobileprovision'):
    raw = open(p, 'rb').read()
    s = raw.find(b'<?xml'); e = raw.find(b'</plist>') + 8
    if s < 0: continue
    pl = plistlib.loads(raw[s:e])
    if '$($App.IosBundleId)' not in pl.get('Entitlements', {}).get('application-identifier', ''): continue
    exp = pl.get('ExpirationDate')
    if best is None or exp > best[0]: best = (exp, p)
print(best[1] if best else '')
"@
        if (-not $prov) { Warn 'ios: no matching provisioning profile on the device - renew via Sideloadly once, then retry.' }
    }
    if ($zsign -and $key -and $prov) {
        if (-not (Test-Path $Ipa)) { throw "unsigned ipa not found: $Ipa" }
        $signed = Join-Path $logs 'signed.ipa'
        Say 'ios: zsign the unsigned CI ipa (persisted cert + freshest device profile)'
        # Local, but bounded like everything else: a wedged signer hangs the install path just
        # as effectively as a wedged device, and 90s is many times a normal 0.7s sign.
        $zargs = if ($cert) { @('-k', $key, '-c', $cert, '-m', $prov, '-b', $App.IosBundleId, '-o', $signed, '-z', '5', $Ipa) }
                 else       { @('-k', $key, '-p', $ZsignPassword, '-m', $prov, '-b', $App.IosBundleId, '-o', $signed, '-z', '5', $Ipa) }
        $zout = Invoke-Bounded -What 'zsign' -TimeoutSec 90 -Exe $zsign -CmdArgs $zargs
        if ($zout) { Write-Host $zout }
        if (-not (Test-Path $signed)) { throw 'zsign produced no output ipa (profile expired? renew via Sideloadly once, then retry).' }
        return (Install-IpaHeadless $signed 'zsigned')
    }
    Warn 'ios: headless signing material incomplete (zsign / key+cert PEMs / profile) - falling back to Sideloadly.'
    # (3) Assisted Sideloadly - the honest single human touch (first run also does the
    # Apple-ID login + 2FA; thereafter the daemon auto-refreshes until the cert expires).
    if (-not (Test-Path $Sideloadly)) { throw "Sideloadly not found: $Sideloadly" }
    if (-not (Test-Path $Ipa)) { throw "ipa not found: $Ipa" }
    # DON'T pass `-i <ipa>`: when a Sideloadly instance already holds localhost:28811
    # (always, incl. the daemon) the flag is forwarded mangled and fails with
    # "Ipa file -i does not exist". Instead open the GUI + reveal the .ipa in Explorer so
    # it can be dragged in (the reliable assisted path).
    Say ('ios: assisted install of ' + (Split-Path $Ipa -Leaf) + ' - drag the revealed .ipa into Sideloadly, then Start.')
    if (-not (Get-Process -Name 'sideloadly' -ErrorAction SilentlyContinue)) { Start-Process $Sideloadly }
    Start-Process explorer.exe -ArgumentList ('/select,"' + $Ipa + '"')
    Warn 'ios: install is ASSISTED (Sideloadly GUI drag-drop). For headless installs pass -SignedIpa or -ZsignP12/-ZsignProfile.'
    return $false
}

# Headless launch via the iOS 17+ userspace tunnel (no admin). Returns $true on success.
# Two modes:
#   default        --no-kill-existing: foreground the running app without disturbing it
#                  (repeated kill-existing relaunches churn the CAMetalLayer black).
#   -FreshProcess  default kill-existing: the ONLY reliable way to restart the process
#                  (`dvt kill`/`pkill` proved unable to kill it), which is required for
#                  anything read at startup — the role override + clearsave markers.
function Launch-Ios([switch]$FreshProcess) {
    # dvt launch on iOS 26 fails every other call ("Unable to launch...") - always retry.
    function TryLaunch([string[]]$extra) {
        foreach ($attempt in 1..2) {
            try { PmdDev launch @extra $App.IosBundleId 2>&1 | Out-Null; return $true } catch {}
            Start-Sleep -Seconds 1
        }
        return $false
    }
    try {
        if ($FreshProcess) {
            if (-not (TryLaunch @())) { throw 'kill-existing launch failed twice' }
            Say 'ios: fresh-launched (killed old instance; startup markers re-read)'
            # #73 heal, proven 2026-07-19: a DVT kill-existing relaunch comes up with
            # scene hosting the window server never composites (screen black, app
            # perfect in-process - even win/key/scene/host all healthy). No in-process
            # action fixes it; a real background/foreground cycle does, 3/3 on
            # hardware. So bounce: foreground Settings, then re-foreground the app.
            Start-Sleep -Seconds 3
            PmdDev launch com.apple.Preferences 2>&1 | Out-Null
            Start-Sleep -Seconds 2
            if (-not (TryLaunch @('--no-kill-existing'))) { throw 'bounce re-foreground failed twice' }
            Say 'ios: bounce done (Settings -> app) - scene re-hosted, rendering live (#73)'
        } else {
            if (-not (TryLaunch @('--no-kill-existing'))) { throw 'foreground launch failed twice' }
            Say 'ios: launched/foregrounded (userspace tunnel, no admin)'
        }
        return $true
    } catch {
        Warn 'ios: launch failed. The app must be installed + Developer Mode on; first run needs a one-time Bluetooth allow.'
        return $false
    }
}
# Remove EVERY installed build of the app. Re-signing with a different cert yields a new
# bundle-id suffix (e.g. ...onlychess.<SIGNER>), so stale copies accumulate; this sweeps
# all bundles under $App.IosBundleBase. Android takes the fixed package id.
function Uninstall-Ios {
    Ensure-Ios
    $json = (Pmd apps list) | Out-String
    $base = [regex]::Escape($App.IosBundleBase)
    $ids  = [regex]::Matches($json, ('"(' + $base + '[^"]*)"\s*:')) | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique
    if (-not $ids) { Say "ios: no installed build under $($App.IosBundleBase)"; return }
    foreach ($id in $ids) {
        Say "ios: uninstalling $id"
        Pmd apps uninstall $id | Out-Host
    }
    Say "ios: removed $($ids.Count) build(s)"
}
function Uninstall-Android {
    Say "android: uninstalling $($App.AndroidPackage)"
    & $adb uninstall $App.AndroidPackage 2>$null | Out-Host
}
# The APK path is PER-GAME ($App.AndroidApk), like every other app-specific value.
#
# It was hardcoded to the Chess tree in both call sites, so `-Game rps -Action install` installed
# the CHESS apk — a different package id, so adb reported Success and the RPS app on the phone was
# never touched. The phone then kept whatever RPS build it already had, and the only symptom was
# the build-fingerprint gate refusing to start a match ("different build" on both screens, #112)
# while a fresh APK sat unread on disk. Cost a full test cycle on 2026-08-08.
#
# Verifying the file exists first is the other half: `adb install` on a missing path fails in a way
# that scrolls past, and a silently-not-installed build is exactly the failure above.
function Install-Android {
    $Apk = Join-Path $root $App.AndroidApk
    if (-not (Test-Path $Apk)) {
        Warn "android: no APK at $Apk - build it first (gradlew assembleDebug); NOT installing"
        return $false
    }
    Say "android: installing $($App.AndroidPackage) <- $($App.AndroidApk)"
    & $adb install -r $Apk | Out-Host
    return $true
}
function Shot-Ios($path) {
    # 20s, not the 45s dvt default (#179). This is the call that actually stalls: with the app
    # foregrounded, `dvt proclist` answers in seconds while `dvt screenshot` never returns at
    # all — no output, no error, no file. A screenshot that has not answered in 20s is not
    # coming, and the old `try/catch` could never help because a HANG throws nothing.
    Remove-Item $path -Force -ErrorAction SilentlyContinue
    PmdDevT 20 screenshot $path | Out-Null
    if (Test-Path $path) { Say "ios: screenshot -> $path" }
    else { Warn 'ios: no screenshot (timed out, or device locked / Developer Mode off) - continuing' }
}
# NOTE: there is deliberately NO Kill-Ios. `dvt kill`/`dvt pkill` proved unreliable on
# this device (they report the kill but the process survives), so the ONLY dependable
# process restart is the kill-existing `dvt launch` — Launch-Ios -FreshProcess above.

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
    # gh refuses to overwrite an existing artifact file - clear the old ipa first.
    Remove-Item (Join-Path $dist 'OnlyChess-unsigned.ipa') -ErrorAction SilentlyContinue
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
        Role-Ios                                            # role marker read at app startup
        # -Fresh: kill-existing launch restarts the PROCESS (startup markers re-read);
        # otherwise just foreground whatever is running.
        if (-not (Launch-Ios -FreshProcess:$Fresh)) {
            if ($Interactive) { Read-Host 'Press Enter once the iPhone app is foregrounded + Bluetooth allowed' }
            else { throw 'ios: launch FAILED (non-interactive) - aborting instead of idling with an empty log.' }
        }
        Arm-Ios
    }
    if ($doAndroid) {
        Ensure-Android
        if ($Fresh) {
            # Clean-measurement mode: force-stop (NOT pm clear, so identity/opponent survive)
            # and clear the log window so we capture a fresh handshake + uninterrupted play.
            Say 'android: -Fresh -> force-stop + clear log, then relaunch (identity kept)'
            Kill-Android; Role-Android; Arm-Android; Adb logcat -c | Out-Null; Launch-Android
        } elseif ($NoReset) {
            # Peers are already paired + linked: don't wipe identity (pm clear would drop
            # the link) and don't clear logcat (keep the just-happened handshake lines).
            Say 'android: -NoReset -> arming the running app, preserving the live link + log buffer'
            Arm-Android; Launch-Android
        } else {
            Reset-Android; Arm-Android; Adb logcat -c | Out-Null; Launch-Android
        }
    }
    $jobs = @()
    if ($doAndroid) { $jobs += Start-Job -Name and -ScriptBlock { param($a,$s,$t,$o) & $a -s $s logcat -s "$t`:*" | Out-File -FilePath $o -Encoding utf8 } -ArgumentList $adb,$AndroidSerial,$App.LogTag,$andLog }
    if ($doIos)     { $jobs += Start-Job -Name ios -ScriptBlock { param($t,$o) & python -m pymobiledevice3 syslog live 2>$null | Select-String -Pattern $t | ForEach-Object { $_.Line } | Out-File -FilePath $o -Encoding utf8 } -ArgumentList $App.LogTag,$iosLog }
    Say "linking... (settling ${SettleSec}s)"; Start-Sleep -Seconds $SettleSec
    $start = Get-Date
    $linked = $false
    try {
        while ($true) {
            Start-Sleep -Seconds 3
            $el = ((Get-Date) - $start).TotalSeconds
            # EARLY-OUT: the peers must actually LINK (Net: READY in either engine log)
            # within LinkTimeoutSec, or we abort LOUDLY with each side's evidence -
            # never idle out the whole DurationSec against a dead/unlinked peer.
            if (-not $linked) {
                $andUp = $doAndroid -and (Test-Path $andLog) -and (Select-String -Path $andLog -Pattern 'Net: READY' -Quiet)
                $iosUp = $doIos     -and (Test-Path $iosLog) -and (Select-String -Path $iosLog -Pattern 'Net: READY' -Quiet)
                if ($andUp -or $iosUp) { $linked = $true; Say 'link is UP (Net: READY seen)' }
                elseif (($SettleSec + $el) -ge $LinkTimeoutSec) {
                    Warn "LINK FAILED: no 'Net: READY' from any peer after $([int]($SettleSec + $el))s - aborting run."
                    foreach ($pair in @(@($andLog, 'android'), @($iosLog, 'ios'))) {
                        $f = $pair[0]; $n = $pair[1]
                        if (Test-Path $f) { Warn "--- $n last lines ---"; Get-Content $f -Tail 6 | ForEach-Object { Warn "  $_" } }
                        else { Warn "--- $n captured NOTHING (log job dead? app silent?) ---" }
                    }
                    throw 'link never established - see the evidence above.'
                }
            }
            $ended = 0
            if ($doAndroid -and (Test-Path $andLog)) { $ended = [math]::Max($ended, (Get-Content $andLog | Select-String 'MATCH END').Count) }
            if ($doIos     -and (Test-Path $iosLog)) { $ended = [math]::Max($ended, (Get-Content $iosLog | Select-String 'MATCH END').Count) }
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
    'clearhistory' {
        # Wipe both peers' opponent history (records/meta/peer-id; device-id KEPT), so the
        # next link is a clean pairing — used between role-configuration test runs.
        if ($doAndroid) { Ensure-Android; ClearHistory-Android }
        if ($doIos)     { Ensure-Ios;     ClearHistory-Ios }
    }
    'install'   { if ($doIos) { [void](Install-Ios) }; if ($doAndroid) { Ensure-Android; Install-Android } }
    'uninstall' { if ($doIos) { Uninstall-Ios }; if ($doAndroid) { Ensure-Android; Uninstall-Android } }
    'arm'     { if ($doAndroid) { Ensure-Android; Arm-Android }; if ($doIos) { Ensure-Ios; Arm-Ios } }
    'disarm'  { if ($doAndroid) { Ensure-Android; Disarm-Android }; if ($doIos) { Ensure-Ios; Disarm-Ios } }
    # -Fresh restarts the PROCESS instead of just foregrounding it. Needed after every install:
    # a plain foreground launch leaves the OLD process running, so the new binary sits on disk
    # unused and the app under test is silently the previous build (this cost a whole misleading
    # two-phone test run: the iPhone kept serving pre-fix behaviour from a stale process).
    'launch'  { if ($doAndroid) { Ensure-Android; Launch-Android }; if ($doIos) { Ensure-Ios; [void](Launch-Ios -FreshProcess:$Fresh) } }
    'shot'    { if ($doAndroid) { Ensure-Android; Shot-Android (Join-Path $logs 'android.png') }; if ($doIos) { Ensure-Ios; Shot-Ios (Join-Path $logs 'ios.png') } }
    'tail'    {
        if ($doIos -and -not $doAndroid) { Say 'ios: tailing engine log (Ctrl-C to stop)'; & python -m pymobiledevice3 syslog live 2>$null | Select-String -Pattern $App.LogTag | ForEach-Object { $_.Line } }
        elseif ($doAndroid -and -not $doIos) { Ensure-Android; Say 'android: tailing engine log (Ctrl-C to stop)'; Adb logcat -s "$($App.LogTag):*" }
        else { Warn 'tail: choose one -Peer (android or ios)' }
    }
    'status'  { Summarize (Join-Path $logs 'android.log') 'android'; Summarize (Join-Path $logs 'ios.log') 'ios' }
    'pullrec' {
        if (-not $App.RecGlob) { Warn "pullrec: -Game $Game has no flight recorder"; break }
        $recRoot = if ($RecDir) { $RecDir } else { Join-Path $root 'dist\rec' }
        $andDir = Join-Path $recRoot 'android'
        $iosDir = Join-Path $recRoot 'ios'
        if ($doAndroid) { Ensure-Android; [void](Pull-Rec-Android $andDir) }
        if ($doIos)     { Ensure-Ios;     [void](Pull-Rec-Ios     $iosDir) }
        # Diffing is the POINT of pulling both — a pair sitting undiffed on disk is the same
        # dead end as a pair sitting on two phones, so it happens without a second command.
        if ($doAndroid -and $doIos) { Diff-Rec-Pairs $andDir $iosDir }
    }
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
        if ($doAndroid -and (Test-Path (Join-Path $root $App.AndroidApk))) {
            Ensure-Android; Install-Android | Out-Null
        }
        for ($i = 1; $i -le $Iterations; $i++) {
            Say "=== cycle iteration $i / $Iterations ==="
            Invoke-Run $false
        }
        Say "=== cycle complete: $Iterations iteration(s) ==="
    }
}

# --- Post-run report: a timeout must never be mistaken for success ------------------
# Several actions deliberately continue past a bounded failure (a screenshot is not worth
# aborting a test run for), so without this the run ends printing nothing but its successes
# and the exit code says fine. That is the same "silent stall" trap one level up: the
# operator concludes the device is healthy when in fact a call was killed.
if ($script:TimedOut.Count -gt 0) {
    Write-Host ''
    Write-Host "[device-rig] $($script:TimedOut.Count) call(s) TIMED OUT and were killed:" -ForegroundColor Red
    foreach ($t in $script:TimedOut) { Write-Host "  - $t" -ForegroundColor Red }
    Write-Host '[device-rig] The device did not answer in time. Nothing is hung now (the process' -ForegroundColor Red
    Write-Host '[device-rig] tree was killed), but the result above is INCOMPLETE.' -ForegroundColor Red
    exit 2
}
