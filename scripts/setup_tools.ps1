# Clones and builds the generic recompilers into the work folder, pinned to the
# commits this project was tested with, then applies our patches: XenonRecomp +
# XenonAnalyse (recomp\xenonrecomp-patches: missing instructions and fixes) and the
# XenosRecomp shader translator (recomp\xenosrecomp-patches).
#   powershell -ExecutionPolicy Bypass -File scripts\setup_tools.ps1
#
# The checkouts are reset on every run: change the tools by editing the patches.
param([string]$Work = "$env:LOCALAPPDATA\Skate3Native")
$ErrorActionPreference = 'Stop'

$commit = 'ddd128bcca99fe8bfbb99bea583c972351fa6ace'  # hedge-dev/XenonRecomp, 2025-08-04
$repo = Split-Path $PSScriptRoot -Parent
$src = Join-Path $Work 'tools\XenonRecomp'
$build = Join-Path $Work 'tools\XenonRecomp-build'

function Check($what) { if ($LASTEXITCODE) { throw "$what failed (exit $LASTEXITCODE)" } }

if (-not (Test-Path $src)) {
    git clone https://github.com/hedge-dev/XenonRecomp.git $src; Check 'git clone'
}
git -C $src fetch --quiet origin; Check 'git fetch'
git -C $src reset --hard --quiet $commit; Check 'git reset'
git -C $src submodule update --init --recursive; Check 'git submodule update'
foreach ($patch in Get-ChildItem (Join-Path $repo 'recomp\xenonrecomp-patches\*.patch') | Sort-Object Name) {
    git -C $src apply --whitespace=nowarn $patch.FullName; Check "git apply $($patch.Name)"
    Write-Host "Applied $($patch.Name)"
}

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $cmake = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
& $cmake -S $src -B $build -G 'Visual Studio 17 2022' -A x64 -T ClangCL; Check 'CMake configure'
& $cmake --build $build --config Release --target XenonAnalyse XenonRecomp; Check 'build'
Write-Host "XenonRecomp ready in $build"

# XenosRecomp, the shader translator, the same way: pinned, reset, patched, built.
$shaderCommit = '990d03b'  # hedge-dev/XenosRecomp
$shaderSrc = Join-Path $Work 'tools\XenosRecomp'
$shaderBuild = Join-Path $Work 'tools\XenosRecomp-build'
if (-not (Test-Path $shaderSrc)) {
    git -c core.longpaths=true clone https://github.com/hedge-dev/XenosRecomp.git $shaderSrc; Check 'git clone'
}
git -C $shaderSrc fetch --quiet origin; Check 'git fetch'
git -C $shaderSrc reset --hard --quiet $shaderCommit; Check 'git reset'
git -C $shaderSrc -c core.longpaths=true submodule update --init --recursive --depth 1; Check 'git submodule update'
foreach ($patch in Get-ChildItem (Join-Path $repo 'recomp\xenosrecomp-patches\*.patch') | Sort-Object Name) {
    git -C $shaderSrc apply --whitespace=nowarn $patch.FullName; Check "git apply $($patch.Name)"
    Write-Host "Applied $($patch.Name)"
}
& $cmake -S $shaderSrc -B $shaderBuild -G 'Visual Studio 17 2022' -A x64 -T ClangCL -DXENOS_RECOMP_DXIL=OFF; Check 'CMake configure'
& $cmake --build $shaderBuild --config Release --target XenosRecomp; Check 'build'
Write-Host "XenosRecomp ready in $shaderBuild"
