<#
.SYNOPSIS
    Build ps3hddtool

.DESCRIPTION
    Locates and imports the Visual Studio x64 developer environment.

.PARAMETER Config
    Release (default) -> build/rel, or Debug -> build/debug.

.PARAMETER Target
    Build a single CMake target (e.g. ps3hdd_ui, ps3hdd_pkg_info) instead of everything.

.PARAMETER Clean
    Delete the build directory first (forces a full reconfigure + rebuild).

.PARAMETER Test
    Run the test suite (ctest) after a successful build.

.PARAMETER Run
    Launch build/<cfg>/bin/ps3hdd_ui.exe after a successful build.

.EXAMPLE
    .\build.ps1                 # incremental Release build
    .\build.ps1 -Test           # build + run all tests
    .\build.ps1 -Run            # build + launch the GUI
    .\build.ps1 -Clean -Test    # from-scratch build + tests
    .\build.ps1 -Target ps3hdd_pkg_info
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [string]$Target,
    [switch]$Clean,
    [switch]$Test,
    [switch]$Run
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
if ($Config -eq 'Release') { $sub = 'rel' } else { $sub = 'debug' }
$BuildDir = Join-Path $Root "build\$sub"

$OrigVcpkgRoot = $env:VCPKG_ROOT

function Say($msg, $color = 'Cyan') { Write-Host ">> $msg" -ForegroundColor $color }

Get-Process UFS2Xplorer, ps3hdd_helper -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

if ($Clean -and (Test-Path $BuildDir)) { Say "Removing $BuildDir"; Remove-Item -Recurse -Force $BuildDir }

function Get-VcVarsFromCache($dir) {
    $cache = Join-Path $dir 'CMakeCache.txt'
    if (-not (Test-Path $cache)) { return $null }
    $m = Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:FILEPATH=(.+)$' | Select-Object -First 1
    if (-not $m) { return $null }
    $cl = $m.Matches[0].Groups[1].Value
    $i = $cl.IndexOf('\VC\Tools\MSVC'); if ($i -lt 0) { $i = $cl.IndexOf('/VC/Tools/MSVC') }
    if ($i -lt 0) { return $null }
    $vc = Join-Path $cl.Substring(0, $i) 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path $vc) { return $vc } else { return $null }
}

function Find-VcVars {
    $fromCache = Get-VcVarsFromCache $BuildDir
    if ($fromCache) { return $fromCache }
    foreach ($ver in '2022', '2019') {
        foreach ($ed in 'Community', 'Professional', 'Enterprise', 'BuildTools') {
            foreach ($pf in 'C:\Program Files', 'C:\Program Files (x86)') {
                $p = "$pf\Microsoft Visual Studio\$ver\$ed\VC\Auxiliary\Build\vcvars64.bat"
                if (Test-Path $p) { return $p }
            }
        }
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($vsRoot) {
            $p = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

function Find-Ninja($vsRoot) {
    $cmd = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($vsRoot) {
        $p = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
        if (Test-Path $p) { return $p }
    }
    foreach ($r in @($OrigVcpkgRoot, (Join-Path $env:USERPROFILE 'vcpkg'), 'C:\vcpkg')) {
        if ($r) {
            $d = Join-Path $r 'downloads\tools\ninja'
            if (Test-Path $d) {
                $n = Get-ChildItem -Path $d -Recurse -Filter ninja.exe -ErrorAction SilentlyContinue | Select-Object -First 1
                if ($n) { return $n.FullName }
            }
        }
    }
    return $null
}

$VcVars = Find-VcVars
if (-not $VcVars) { throw "vcvars64.bat not found - install VS 2022 with the 'Desktop development with C++' workload." }
Say "Dev env: $VcVars"

cmd /c "`"$VcVars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}

$VsRoot = $VcVars -replace '\\VC\\Auxiliary\\Build\\vcvars64\.bat$', ''
$Ninja = Find-Ninja $VsRoot
if ($Ninja) { $env:PATH = (Split-Path $Ninja) + ';' + $env:PATH; Say "Ninja: $Ninja" }
else { Write-Warning "ninja.exe not found on PATH or in VS/vcpkg; configure may fail." }

if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    $toolchain = $null
    foreach ($r in @($OrigVcpkgRoot, (Join-Path $env:USERPROFILE 'vcpkg'), 'C:\vcpkg')) {
        if ($r) {
            $tc = Join-Path $r 'scripts\buildsystems\vcpkg.cmake'
            if (Test-Path $tc) { $toolchain = $tc; break }
        }
    }
    $cfg = @('-S', $Root, '-B', $BuildDir, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Config", '-DPS3HDD_BUILD_UI=ON', '-DPS3HDD_BUILD_TESTS=ON',
        '-DVCPKG_MANIFEST_MODE=OFF', '-DVCPKG_TARGET_TRIPLET=x64-windows')
    if ($Ninja) { $cfg += "-DCMAKE_MAKE_PROGRAM=$Ninja" }
    if ($toolchain) { $cfg += "-DCMAKE_TOOLCHAIN_FILE=$toolchain" }
    else { Write-Warning "vcpkg toolchain not found (set VCPKG_ROOT); configuring without it." }
    Say "Configuring $BuildDir"
    & cmake @cfg
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }
}

Say "Building ($Config)$(if ($Target) { " target=$Target" })"
$buildArgs = @('--build', $BuildDir)
if ($Target) { $buildArgs += @('--target', $Target) }
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed." }
Say "build ok -> $BuildDir\bin" 'Green'

if ($Test) {
    Say "running tests"
    Push-Location $BuildDir
    try { & ctest --output-on-failure; $code = $LASTEXITCODE } finally { Pop-Location }
    if ($code -ne 0) { throw "Tests failed." }
    Say "all tests good" 'Green'
}

if ($Run) {
    $exe = Join-Path $BuildDir 'bin\UFS2Xplorer.exe'
    if (Test-Path $exe) { Say "launching $exe"; & $exe }
    else { Write-Warning "$exe not found (ui built?)." }
}