# Instruction tests for our XenonRecomp patches:
#   recomp\tests\isa_tests.py -> XenonRecomp test mode -> build XenonTests -> run.
#   powershell -ExecutionPolicy Bypass -File scripts\test_recomp.ps1
#
# Passes when every check holds and the deliberately wrong "canary" test fails.
param([string]$Work = "$env:LOCALAPPDATA\Skate3Native")
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$src = Join-Path $Work 'tools\XenonRecomp'
$build = Join-Path $Work 'tools\XenonRecomp-build'
$recomp = Join-Path $build 'XenonRecomp\Release\XenonRecomp.exe'
$tests = Join-Path $Work 'isa-tests'
$outDir = Join-Path $src 'XenonTests'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
function Check($what) { if ($LASTEXITCODE) { throw "$what failed (exit $LASTEXITCODE)" } }
if (-not (Test-Path $recomp)) { throw "XenonRecomp isn't built yet. Run scripts\setup_tools.ps1 first." }

python -I (Join-Path $repo 'recomp\tests\isa_tests.py') $tests; Check 'test generation'

Get-ChildItem $outDir -Filter *.cpp | Remove-Item
$log = & $recomp (Join-Path $tests 'bin') $outDir; Check 'XenonRecomp test mode'
$gaps = $log | Where-Object { $_ -match 'unimplemented|Unrecognized|Unable to decode' }
if ($gaps) { $gaps | Write-Host; throw 'XenonRecomp could not translate every test instruction.' }

& $cmake -S $src -B $build | Out-Null; Check 'CMake configure'
& $cmake --build $build --config Release --target XenonTests -- /m /v:minimal | Select-String -Pattern 'error' | Write-Host
Check 'XenonTests build'

$output = & (Join-Path $build 'XenonTests\Release\XenonTests.exe')
$failures = @($output | Where-Object { $_ -match 'EXPECTED' })
$canary = @($failures | Where-Object { $_ -match '^test_canary ' })
$real = @($failures | Where-Object { $_ -notmatch '^test_canary ' })
if (-not $canary) { throw 'The canary test passed, so the harness is not reporting failures.' }
if ($real) {
    $real | Write-Host
    throw "$($real.Count) check(s) failed."
}
Write-Host 'All instruction checks passed; the canary failed as intended.'
