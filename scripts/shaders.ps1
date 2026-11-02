# Shader pipeline, all inside the work folder (nothing game-derived touches the repo):
#   your ISO -> data\big\shaders_final.big -> XenosRecomp (HLSL) -> DXC (SPIR-V) -> shader cache
#
#   powershell -ExecutionPolicy Bypass -File scripts\shaders.ps1 [-Iso <Skate 3 ISO or folder>]
#
# Without -Iso it uses the game you picked in the launcher. Needs XenosRecomp built
# (scripts\setup_tools.ps1). The runtime loads <work>\shaders\shader_cache.bin;
# shader_cache.cpp beside it is XenosRecomp's compiled-in form, kept for reference.
param([string]$Iso, [string]$Work = "$env:LOCALAPPDATA\Skate3Native")
$ErrorActionPreference = 'Stop'

$repo = Split-Path $PSScriptRoot -Parent
$recomp = Join-Path $Work 'tools\XenosRecomp-build\XenosRecomp\Release\XenosRecomp.exe'
$common = Join-Path $Work 'tools\XenosRecomp\XenosRecomp\shader_common.h'
if (-not (Test-Path $recomp)) { throw "XenosRecomp isn't built yet. Run scripts\setup_tools.ps1 first." }

if (-not $Iso) {
    $config = Join-Path $env:APPDATA 'Skate3Native\config.ini'
    $line = if (Test-Path $config) { Select-String -Path $config -Pattern '^source\s*=\s*(.+)$' | Select-Object -First 1 }
    if ($line) { $Iso = $line.Matches[0].Groups[1].Value.Trim() }
}
if (-not $Iso) { throw 'Pass -Iso <your Skate 3 ISO>, or choose the game in the launcher first.' }

# 1. The shader archive from your copy of the game: 392 Xbox 360 shader containers.
$shaders = Join-Path $Work 'shaders'
$source = Join-Path $shaders 'source'
New-Item -ItemType Directory -Force $source | Out-Null
$archive = Join-Path $source 'data\big\shaders_final.big'
if (-not (Test-Path $archive)) {
    if (Test-Path (Join-Path $Iso 'data\big\shaders_final.big')) {
        New-Item -ItemType Directory -Force (Split-Path $archive) | Out-Null
        Copy-Item (Join-Path $Iso 'data\big\shaders_final.big') $archive
    } else {
        python -I (Join-Path $repo 'tools\s3inspect.py') extract $Iso $source data/big/shaders_final.big
        if ($LASTEXITCODE) { throw "Couldn't extract the shader archive from $Iso" }
    }
}

# 2. Every container to HLSL, then to SPIR-V; one cache file with all of them.
$cache = Join-Path $shaders 'shader_cache.cpp'
Write-Host 'XenosRecomp: translating shaders (a minute or two)...'
& $recomp $source $cache $common | Where-Object { $_ -notmatch 'Recompiling shaders' }
if ($LASTEXITCODE) { throw "XenosRecomp failed (exit $LASTEXITCODE)" }
$runtimeCache = Join-Path $shaders 'shader_cache.bin'
Move-Item -Force "$cache.bin" $runtimeCache
Write-Host "Done: $runtimeCache"
