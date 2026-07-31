# Regression test for the build fingerprint (#112's gate, #164's bug).
#
# The fingerprint's ONE job is "these two binaries were built from the same source". It failed at
# that job because it was computed at CMAKE CONFIGURE time and cached: Gradle/Ninja only reconfigure
# when a CMakeLists.txt changes, so the ordinary loop (commit, installDebug) shipped an APK stamped
# with the PREVIOUS commit — usually `<sha>-dirty`. Two phones then reported badbuild=1 from
# identical source (hit twice on 2026-07-31), which is the worse failure of the two: a diagnostic
# that cries wolf gets ignored, and then it can't do its real job either.
#
# So the property under test is not "a fingerprint exists" but "it tracks the working tree at BUILD
# time" — which is why the generator is a script run per build rather than a cached CMake variable.
# This drives that script directly against a throwaway git repo, which is the only way to assert the
# three transitions without doing three full builds.
$ErrorActionPreference = 'Stop'

$root  = Split-Path (Split-Path $MyInvocation.MyCommand.Path)
$gen   = Join-Path $root 'cmake\BuildFingerprint.cmake'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
$git   = (Get-Command git   -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw 'cmake not found' }
if (-not $git)   { throw 'git not found' }
if (-not (Test-Path $gen)) { throw "missing generator script: $gen" }

$failures = 0
function Check($cond, $what) {
    if (-not $cond) { Write-Host "FAIL  $what" -ForegroundColor Red; $script:failures++ }
    else            { Write-Host "ok    $what" }
}

# A throwaway repo so the test never depends on this checkout's state (and never touches it).
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("lur-fp-" + [System.Guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $tmp | Out-Null
$out = Join-Path $tmp 'Fingerprint.cpp'
try {
    Push-Location $tmp
    & $git init -q .
    & $git config user.email 'test@lurmotorn'
    & $git config user.name  'fp test'
    'one' | Out-File -Encoding utf8 (Join-Path $tmp 'Source.cpp')
    & $git add -A
    & $git -c commit.gpgsign=false commit -q -m 'first'
    Pop-Location

    # Read the fingerprint back out of the generated TU the way the compiler would.
    function RunGen($config = 'Development') {
        & $cmake "-DLUR_FP_REPO=$tmp" "-DLUR_FP_CONFIG=$config" "-DLUR_FP_OUT=$out" -P $gen 2>&1 | Out-Null
        if ($LASTEXITCODE) { throw "generator failed ($LASTEXITCODE)" }
        $text = Get-Content -Raw $out
        if ($text -match 'return\s+"([^"]+)"') { return $Matches[1] }
        throw "no fingerprint literal in generated file:`n$text"
    }

    $clean1 = RunGen
    Check ($clean1 -match '^[0-9a-f]{7,}\+Development$') "clean tree -> '<sha>+<config>' (got '$clean1')"

    # THE #164 BUG. A commit with no reconfigure used to leave the fingerprint at its old value, so
    # two phones built either side of a commit disagreed while their source matched.
    Push-Location $tmp
    'two' | Out-File -Encoding utf8 (Join-Path $tmp 'Source.cpp')
    & $git add -A
    & $git -c commit.gpgsign=false commit -q -m 'second'
    Pop-Location
    $clean2 = RunGen
    Check ($clean2 -ne $clean1) "a commit changes the fingerprint without a reconfigure ('$clean1' -> '$clean2')"
    Check ($clean2 -notmatch 'dirty') "a committed tree is not reported dirty (got '$clean2')"

    # The other half, and the one a configure-time snapshot can NEVER catch: an uncommitted edit.
    # Without this the gate has a false NEGATIVE exactly when it matters — two builds from the same
    # commit but different working trees claim to match.
    'three' | Out-File -Encoding utf8 (Join-Path $tmp 'Source.cpp')
    $dirty = RunGen
    Check ($dirty -match '-dirty\+Development$') "an uncommitted edit is reported dirty (got '$dirty')"
    Check ($dirty -ne $clean2) "dirty differs from clean at the same commit"

    # The config is part of the identity: same commit, different LUR_CONFIG => different sim.
    $shipping = RunGen 'Shipping'
    Check ($shipping -ne $dirty) "LUR_CONFIG is part of the fingerprint"

    # Running per build must not COST anything when nothing moved: the file is rewritten only when
    # the value changes, so the TU that carries it relinks on a real change and never otherwise.
    $before = (Get-Item $out).LastWriteTimeUtc
    Start-Sleep -Milliseconds 1100     # coarse FS timestamps: make a rewrite unambiguously visible
    $again = RunGen 'Shipping'
    $after = (Get-Item $out).LastWriteTimeUtc
    Check ($again -eq $shipping) "an unchanged tree yields the same fingerprint"
    Check ($after -eq $before)   "an unchanged fingerprint does not rewrite the file (no spurious relink)"
}
finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

if ($failures) { Write-Host "`nbuild-fingerprint: $failures FAILURE(S)" -ForegroundColor Red; exit 1 }
Write-Host "`nbuild-fingerprint: ALL PASS" -ForegroundColor Green
