# Configures and builds the project with Ninja and clang-cl, in parallel. This is the
# practical way to compile the recompiled game (~280 large files); the Visual Studio
# generator works too but compiles them one at a time.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Target s3native] [-Config Release]
#
# The build folder defaults to %LOCALAPPDATA%\Skate3Native\build: short paths keep the
# toolchain happy, and it sits beside the generated code it compiles.
param(
    [string]$Target = 'all',
    [string]$Config = 'Release',
    [string]$BuildDir = "$env:LOCALAPPDATA\Skate3Native\build"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
if (-not $vs) { throw 'Visual Studio (Build Tools) with "C++ Clang tools for Windows" is required.' }

# Import the x64 developer environment (compiler, linker, Windows SDK, CMake, Ninja).
# vcvars looks for vswhere on PATH.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}
$llvm = "$vs\VC\Tools\Llvm\x64\bin"

# The runtime renders with Vulkan. A shell opened before the SDK was installed doesn't
# have its variable yet, so take it from the machine settings.
if (-not $env:VULKAN_SDK) { $env:VULKAN_SDK = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine') }
if (-not $env:VULKAN_SDK) { throw 'The Vulkan SDK is required (https://vulkan.lunarg.com).' }

if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    cmake -S $repo -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config" `
        "-DCMAKE_C_COMPILER=$llvm\clang-cl.exe" "-DCMAKE_CXX_COMPILER=$llvm\clang-cl.exe" "-DCMAKE_LINKER=$llvm\lld-link.exe"
    if ($LASTEXITCODE) { throw "CMake configure failed (exit $LASTEXITCODE)" }
}
cmake --build $BuildDir --target $Target
if ($LASTEXITCODE) { throw "Build failed (exit $LASTEXITCODE)" }
Write-Host "Built $Target in $BuildDir\bin"
