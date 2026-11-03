# Regenerates third_party\ffmpeg for an FFmpeg release: runs FFmpeg's configure for the one
# decoder the runtime uses (WMA Pro, which decodes XMA frames) with clang-cl, keeps what it
# generates (config\, include\) and lists the files its makefiles would compile
# (sources.cmake). Only needed to move to another release; the build uses what's committed.
#
#   powershell -ExecutionPolicy Bypass -File scripts\ffmpeg_config.ps1 [-Version 8.1.3]
#
# Needs MSYS2 (for configure's shell and tools) and Visual Studio with clang-cl. Afterwards,
# put the printed hash and the version in third_party\ffmpeg\CMakeLists.txt.
param(
    [string]$Version = '8.1.3',
    [string]$Msys = 'C:\msys64',
    [string]$Work = "$env:TEMP\s3n-ffmpeg"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path $repo 'third_party\ffmpeg'
$bash = Join-Path $Msys 'usr\bin\bash.exe'
if (-not (Test-Path $bash)) { throw "MSYS2 not found at $Msys" }

New-Item -ItemType Directory -Force $Work | Out-Null
$tarball = Join-Path $Work "ffmpeg-$Version.tar.xz"
if (-not (Test-Path $tarball)) {
    Invoke-WebRequest "https://ffmpeg.org/releases/ffmpeg-$Version.tar.xz" -OutFile $tarball -UseBasicParsing
}
$hash = (Get-FileHash $tarball -Algorithm SHA256).Hash.ToLower()

# configure and the tools it runs come from MSYS2; the compiler from Visual Studio.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Llvm.Clang -property installationPath
if (-not $vs) { throw 'Visual Studio (Build Tools) with "C++ Clang tools for Windows" is required.' }
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item "env:$($Matches[1])" $Matches[2] }
}
$env:PATH = "$vs\VC\Tools\Llvm\x64\bin;$env:PATH"
$env:MSYS2_PATH_TYPE = 'inherit'
$env:CHERE_INVOKING = '1'
$env:MSYSTEM = 'MSYS'

$flags = @(
    '--toolchain=msvc', '--cc=clang-cl', '--cxx=clang-cl', '--target-os=win64', '--arch=x86_64',
    '--disable-everything', '--enable-decoder=wmapro',
    '--disable-programs', '--disable-doc', '--disable-network', '--disable-autodetect',
    '--disable-avdevice', '--disable-avformat', '--disable-swscale', '--disable-swresample', '--disable-avfilter',
    '--disable-x86asm', '--disable-inline-asm', '--disable-pthreads', '--disable-w32threads',
    '--disable-debug', '--enable-static', '--disable-shared'
) -join ' '
$unix = ($Work -replace '\\', '/') -replace '^([A-Za-z]):', { "/$($_.Groups[1].Value.ToLower())" }
& $bash -lc "cd '$unix' && rm -rf ffmpeg-$Version configured && tar -xJf ffmpeg-$Version.tar.xz && mkdir configured && cd configured && ../ffmpeg-$Version/configure $flags > configure.log 2>&1"
if ($LASTEXITCODE) { throw "configure failed (exit $LASTEXITCODE): see $Work\configured\configure.log" }

$configured = Join-Path $Work 'configured'
Remove-Item -Recurse -Force (Join-Path $out 'config'), (Join-Path $out 'include') -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $out 'config\libavcodec'), (Join-Path $out 'config\libavutil'),
    (Join-Path $out 'include\libavutil') | Out-Null
Copy-Item (Join-Path $configured 'config.h'), (Join-Path $configured 'config_components.h') (Join-Path $out 'config')
Copy-Item (Join-Path $configured 'libavcodec\*_list.c') (Join-Path $out 'config\libavcodec')
Copy-Item (Join-Path $configured 'libavutil\avconfig.h') (Join-Path $out 'include\libavutil')
$ffversion = "/* Written by scripts/ffmpeg_config.ps1, as FFmpeg's version.sh would. */`n" +
    "#ifndef AVUTIL_FFVERSION_H`n#define AVUTIL_FFVERSION_H`n#define FFMPEG_VERSION `"$Version`"`n" +
    "#endif /* AVUTIL_FFVERSION_H */`n"
[IO.File]::WriteAllText((Join-Path $out 'config\libavutil\ffversion.h'), $ffversion)

python -I (Join-Path $PSScriptRoot 'ffmpeg_sources.py') (Join-Path $Work "ffmpeg-$Version") $configured (Join-Path $out 'sources.cmake')
if ($LASTEXITCODE) { throw "ffmpeg_sources.py failed (exit $LASTEXITCODE)" }
Write-Host "FFmpeg $Version configured. In third_party\ffmpeg\CMakeLists.txt: URL_HASH SHA256=$hash"
