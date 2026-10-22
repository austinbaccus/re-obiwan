# Build the recompiled code with MSVC (Visual Studio 2022) and CMake/Ninja.
#   .\recomp\build.ps1            # x86: the differential test harness
#   .\recomp\build.ps1 -Arch x64  # x64: runtime + generated code only
param([ValidateSet('x86', 'x64')] [string]$Arch = 'x86', [string]$Config = 'Release')
$ErrorActionPreference = 'Stop'

$installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
$vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio with the C++ tools was not found.' }
$cmakeDir = "$vs\Common7\IDE\CommonExtensions\Microsoft\CMake"
$repo = Split-Path $PSScriptRoot -Parent
$buildDir = Join-Path $repo "build\$Arch"

$env:PATH = "$installer;$cmakeDir\CMake\bin;$cmakeDir\Ninja;$env:PATH"
cmd /c "`"$vs\VC\Auxiliary\Build\vcvarsall.bat`" $(if ($Arch -eq "x86") { "amd64_x86" } else { "amd64" }) >nul && cmake -S `"$PSScriptRoot`" -B `"$buildDir`" -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$buildDir`""
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
