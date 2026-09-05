<#
.SYNOPSIS
    stage and zip a portable UFS2Xplorer release (rel x64).

.DESCRIPTION
    Builds (unless -SkipBuild), then copies the built bin/ tree - which windeployqt has already populated with the Qt runtime - into dist/, keeps only the shippable exes (the app and the elevated disk helper), drops the test and CLI-tool exes and debug symbols, verifies the result, adds the user docs, and produces a .zip + .sha256.

.PARAMETER Version
    release version for the file names. defaults to the UFS2XPLORER_VERSION in the source.

.PARAMETER Config
    release (default) or debug - which build/<cfg>/bin to package.

.PARAMETER SkipBuild
    package the existing build without rebuilding first.

.EXAMPLE
    .\package.ps1                    # build, then package build/rel as the source version
    .\package.ps1 -Version 0.9.0     # override the version string
    .\package.ps1 -SkipBuild         # zip what is already built
#>
[CmdletBinding()]
param(
    [string]$Version,
    [ValidateSet('Release', 'Debug')] [string]$Config = 'Release',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
if ($Config -eq 'Release') { $sub = 'rel' } else { $sub = 'debug' }
$BinDir = Join-Path $Root "build\$sub\bin"
function Say($m) { Write-Host ">> $m" -ForegroundColor Cyan }

if (-not $SkipBuild) { & (Join-Path $Root 'build.ps1') -Config $Config }
if (-not (Test-Path (Join-Path $BinDir 'UFS2Xplorer.exe'))) {
    throw "UFS2Xplorer.exe not found in $BinDir. Build first (or drop -SkipBuild)."
}

if (-not $Version) {
    $m = Select-String -Path (Join-Path $Root 'src\ps3hdd_ui\main_window.cpp') `
        -Pattern 'UFS2XPLORER_VERSION\s+"([^"]+)"' | Select-Object -First 1
    if ($m) { $Version = $m.Matches[0].Groups[1].Value } else { throw "could not read the version; pass -Version" }
}
Say "version $Version"

$stageName = "UFS2Xplorer-$Version-windows-x64"
$dist = Join-Path $Root 'dist'
$stage = Join-Path $dist $stageName
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Copy-Item -Recurse -Force (Join-Path $BinDir '*') $stage

$keep = @('UFS2Xplorer.exe', 'ps3hdd_helper.exe')
Get-ChildItem $stage -Filter *.exe | Where-Object { $keep -notcontains $_.Name } | Remove-Item -Force
Get-ChildItem $stage -Recurse -Include *.pdb, *.ilk, *.exp, *.log -ErrorAction SilentlyContinue | Remove-Item -Force

$allowLoose = $keep + @('qt.conf')
Get-ChildItem $stage -File | Where-Object {
    $_.Extension -ne '.dll' -and $allowLoose -notcontains $_.Name
} | ForEach-Object {
    Say "pruning stray file: $($_.Name) ($([math]::Round($_.Length/1MB,1)) MB)"
    Remove-Item -Force $_.FullName
}

foreach ($x in 'UFS2Xplorer.exe', 'ps3hdd_helper.exe', 'Qt6Core.dll') {
    if (-not (Test-Path (Join-Path $stage $x))) { throw "package is missing $x" }
}

foreach ($f in 'README.md', 'SETUP.md', 'LICENSE', 'LICENSE.txt', 'LICENSE.md') {
    $p = Join-Path $Root $f
    if (Test-Path $p) { Copy-Item -Force $p $stage }
}
if (Test-Path (Join-Path $Root 'assets')) { Copy-Item -Recurse -Force (Join-Path $Root 'assets') $stage }

$zip = Join-Path $dist "$stageName.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path $stage -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash
Set-Content -Encoding ascii -Path "$zip.sha256" -Value "$hash  $stageName.zip"

$size = "{0:N1} MB" -f ((Get-Item $zip).Length / 1MB)
Say "packaged -> $zip  ($size)"
Say "sha256   $hash" 'Green'