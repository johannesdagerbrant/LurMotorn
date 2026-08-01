# Drives the REAL Get-IosAppPid / Get-IosAppRecord out of device-rig.ps1 (lifted via the AST, so
# this tests the shipped code, not a copy) against canned pymobiledevice3 output.
$ErrorActionPreference = 'Stop'
$errs = $null; $toks = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    'C:\games\lurmotorn\Tools\DeviceRig\device-rig.ps1', [ref]$toks, [ref]$errs)
$want = 'Get-IosAppPid', 'Get-IosAppRecord'
$ast.FindAll({ $args[0] -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $true) |
    Where-Object { $want -contains $_.Name } |
    ForEach-Object { Invoke-Expression $_.Extent.Text }

$App = @{ IosBundleId = 'com.lurmotorn.onlyrps.L5XBWVZ7N3'; LogTag = 'OnlyRps' }
$script:Canned = $null
function Invoke-BoundedPmd([string[]]$PmdArgs, [int]$TimeoutSec = 60) { return $script:Canned }

$fails = 0
function Check($name, $got, $expect) {
    if ("$got" -eq "$expect") { Write-Host "  PASS $name -> '$got'" -ForegroundColor Green }
    else { Write-Host "  FAIL $name -> got '$got', want '$expect'" -ForegroundColor Red; $script:fails++ }
}

# --- Get-IosAppPid -------------------------------------------------------------------
# Real proclist shape, captured from the device 2026-08-01.
$script:Canned = @'
[{"bundleIdentifier":"com.apple.Preferences","name":"Preferences","pid":401},
 {"bundleIdentifier":"com.lurmotorn.onlyrps.L5XBWVZ7N3","displayLocalizedAppName":"OnlyRps",
  "foregroundRunning":true,"isApplication":true,"name":"OnlyRps","pid":5099,
  "startDate":"2026-08-01 06:48:27.796780+00:00"}]
'@
Check 'running app returns its pid' (Get-IosAppPid) '5099'

$script:Canned = '[{"bundleIdentifier":"com.apple.Preferences","name":"Preferences","pid":401}]'
Check 'app absent returns empty' (Get-IosAppPid) ''

$script:Canned = $null
Check 'dead probe returns unknown' (Get-IosAppPid) 'unknown'

$script:Canned = 'ERROR: Device is not connected'
Check 'garbage returns unknown' (Get-IosAppPid) 'unknown'

# A process whose bundle id merely CONTAINS ours must not count as ours.
$script:Canned = '[{"bundleIdentifier":"com.lurmotorn.onlyrps.L5XBWVZ7N3.extension","pid":77}]'
Check 'near-miss bundle id is not a match' (Get-IosAppPid) ''

# --- Get-IosAppRecord ----------------------------------------------------------------
$script:Canned = '{"com.lurmotorn.onlyrps.L5XBWVZ7N3":{"CFBundleName":"OnlyRps","Path":"/var/containers/Bundle/Application/AAA/OnlyRps.app"}}'
$a = Get-IosAppRecord
$script:Canned = '{"com.lurmotorn.onlyrps.L5XBWVZ7N3":{"CFBundleName":"OnlyRps","Path":"/var/containers/Bundle/Application/BBB/OnlyRps.app"}}'
$b = Get-IosAppRecord
Check 'record is non-null' ([bool]$a) 'True'
Check 'reinstall changes the record' ($a -ne $b) 'True'

$script:Canned = '{"com.someone.else":{"CFBundleName":"Other"}}'
Check 'absent app -> null record' ([bool](Get-IosAppRecord)) 'False'

$script:Canned = $null
Check 'dead probe -> null record' ([bool](Get-IosAppRecord)) 'False'

# REGRESSION: `apps list` is ~2.25 MB on a real device and PS 5.1's ConvertFrom-Json refuses anything
# past ~2 MB. The first implementation parsed the whole document, so it threw on real hardware and the
# evidence silently became "unavailable" — on an install that had actually succeeded. Pad past the cap
# with the real record embedded at the far end, exactly where the real one sits.
$rec = '"com.lurmotorn.onlyrps.L5XBWVZ7N3": {' +
       '"CFBundleVersion": "1",' +
       '"Container": "/private/var/mobile/Containers/Data/Application/4A314254",' +
       '"ParallelPlaceholderPath": true,' +
       '"Path": "/private/var/containers/Bundle/Application/5DF6A615/OnlyRps.app",' +
       '"SequenceNumber": 2000}'
$script:Canned = '{"com.other.padding": {"Blob": "' + ('x' * 2500000) + '"},' + $rec + '}'
$big = Get-IosAppRecord
Check 'record survives a 2.5MB listing' ([bool]$big) 'True'
Check 'picks Path, not ParallelPlaceholderPath' ($big -match 'Path=/private/var/containers/Bundle/Application/5DF6A615/OnlyRps\.app') 'True'
Check 'captures SequenceNumber too' ($big -match 'SequenceNumber=2000') 'True'

# A reinstall changes Path AND SequenceNumber; that difference is the entire verdict.
$script:Canned = '{' + ($rec -replace '5DF6A615', 'F0934752' -replace '2000', '2001') + '}'
Check 'reinstall changes the big-listing record' ($big -ne (Get-IosAppRecord)) 'True'

if ($fails -gt 0) { Write-Host "$fails FAILED" -ForegroundColor Red; exit 1 }
Write-Host 'ALL PASS' -ForegroundColor Green
