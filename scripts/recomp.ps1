# Phase 2 pipeline, all inside the work folder (nothing game-derived touches the repo):
#   your ISO -> default.xex -> xexscan (jump tables, leaf functions) -> XenonRecomp (C++)
#
#   powershell -ExecutionPolicy Bypass -File scripts\recomp.ps1 [-Iso <Skate 3 ISO or folder>]
#
# Without -Iso it uses the game you picked in the launcher. Needs the xexscan target
# built (cmake --build <build dir> --config Release --target xexscan).
param([string]$Iso, [string]$Work = "$env:LOCALAPPDATA\Skate3Native", [string]$Xexscan)
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$recomp = Join-Path $Work 'tools\XenonRecomp-build\XenonRecomp\Release\XenonRecomp.exe'
$context = Join-Path $Work 'tools\XenonRecomp\XenonUtils\ppc_context.h'
if (-not (Test-Path $recomp)) { throw "XenonRecomp isn't built yet. Run scripts\setup_tools.ps1 first." }
if (-not $Xexscan) {
    $Xexscan = @("$Work\build\bin\xexscan.exe", "$repo\build\bin\Release\xexscan.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Xexscan) { throw 'xexscan.exe not found. Build the xexscan target, or pass -Xexscan <path>.' }

# Runs a tool with stdout and stderr captured to one log; returns the exit code.
function Invoke-Logged([string]$exe, [string[]]$arguments, [string]$log) {
    $quoted = $arguments | ForEach-Object { '"' + $_ + '"' }
    $p = Start-Process -FilePath $exe -ArgumentList $quoted -NoNewWindow -Wait -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError "$log.stderr"
    Get-Content "$log.stderr" | Add-Content $log
    Remove-Item "$log.stderr"
    return $p.ExitCode
}

# 1. default.xex from your copy of the game
$game = Join-Path $Work 'game'
$xex = Join-Path $game 'default.xex'
if (-not (Test-Path $xex)) {
    if (-not $Iso) {
        $config = Join-Path $env:APPDATA 'Skate3Native\config.ini'
        $line = if (Test-Path $config) { Select-String -Path $config -Pattern '^source\s*=\s*(.+)$' | Select-Object -First 1 }
        if ($line) { $Iso = $line.Matches[0].Groups[1].Value.Trim() }
    }
    if (-not $Iso) { throw 'Pass -Iso <your Skate 3 ISO>, or choose the game in the launcher first.' }
    New-Item -ItemType Directory -Force $game | Out-Null
    if (Test-Path (Join-Path $Iso 'default.xex')) {
        Copy-Item (Join-Path $Iso 'default.xex') $xex
    } else {
        python -I (Join-Path $repo 'tools\s3inspect.py') extract $Iso $game default.xex
        if ($LASTEXITCODE) { throw "Couldn't extract default.xex from $Iso" }
    }
}

# The addresses in recomp\skate3.toml belong to exactly one build of the game.
$expected = '2f1e63fbdfc90a30381328c428437747ee91295b'
$actual = (Get-FileHash $xex -Algorithm SHA1).Hash.ToLower()
if ($actual -ne $expected) {
    throw "default.xex (sha1 $actual) is not the build recomp\skate3.toml was made for ($expected)."
}

# 2. Jump tables and leaf-function boundaries, then 3. C++
$out = Join-Path $Work 'recomp'
New-Item -ItemType Directory -Force (Join-Path $out 'out') | Out-Null

Write-Host 'xexscan: finding jump tables, leaf functions and non-code blocks...'
$generated = Join-Path $out 'skate3_generated.toml'
$code = Invoke-Logged $Xexscan @($xex, '--generate', (Join-Path $out 'skate3_switch_tables.toml'), $generated) `
    (Join-Path $out 'xexscan.log')
if ($code) { throw "xexscan failed (exit $code); see $out\xexscan.log" }
Get-Content (Join-Path $out 'xexscan.log') | Write-Host

# skate3.toml has only a [main] table, so appended keys (functions, invalid_instructions)
# belong to it.
$config = Join-Path $out 'skate3.toml'
Copy-Item (Join-Path $repo 'recomp\skate3.toml') $config -Force
Add-Content $config ("`n" + (Get-Content $generated -Raw))

Write-Host 'XenonRecomp: translating PowerPC to C++ (takes a few minutes)...'
$code = Invoke-Logged $recomp @((Join-Path $out 'skate3.toml'), $context) (Join-Path $out 'recomp.log')
if ($code) { throw "XenonRecomp failed (exit $code); see $out\recomp.log" }

$files = Get-ChildItem (Join-Path $out 'out') -File
$size = ($files | Measure-Object Length -Sum).Sum / 1MB
Write-Host ("Done: {0} files, {1:N0} MB of C++ in {2}" -f $files.Count, $size, (Join-Path $out 'out'))
Write-Host "Logs: $out\xexscan.log, $out\recomp.log"
