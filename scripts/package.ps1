#requires -Version 5.1
<#
.SYNOPSIS
    Builds the redistributable LetMeCraft release ZIP (full drop-in: mod + UE4SS runtime).

.DESCRIPTION
    1) Builds the mod and the UE4SS runtime from the repository (build.ps1 -Full) so the archive
       is fully reproducible and never depends on any installed game.
    2) Stages the files in the game's folder layout (G1R\Binaries\Win64\...).
    3) Compresses everything into dist\LetMeCraft-v<version>.zip.

    The end user just extracts the archive into the GAME ROOT folder (the one that contains the
    "G1R" folder) and launches the game. UE4SS + LetMeCraft load automatically.

    Bundled UE4SS files (loader + built-in Lua mods + license) come from the in-repo
    cpp\RE-UE4SS sources, so no third party's machine paths or unrelated mods leak into the zip.

.PARAMETER Version
    Version string for the archive name. Default: parsed from ModVersion in dllmain.cpp.

.PARAMETER OutDir
    Output directory for the archive. Default: <repo>\dist.

.PARAMETER SkipBuild
    Do not rebuild; package the artifacts already present in the build tree.

.PARAMETER Configuration
    CMake configuration. Default: Game__Shipping__Win64.

.PARAMETER CMake
    Explicit path to cmake.exe (forwarded to build.ps1).
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$OutDir,
    [switch]$SkipBuild,
    [string]$Configuration = 'Game__Shipping__Win64',
    [string]$CMake
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- repo layout (resolved relative to this script) ---
$RepoRoot = Split-Path -Parent $PSScriptRoot
$CppDir   = Join-Path $RepoRoot 'cpp'
$BuildDir = Join-Path $CppDir 'build-vs2022'
$Assets   = Join-Path $CppDir 'RE-UE4SS\assets'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }

# --- version from dllmain.cpp (single source of truth) ---
if (-not $Version) {
    $dllmain = Join-Path $CppDir 'LetMeCraft\dllmain.cpp'
    $hit = Select-String -Path $dllmain -Pattern 'ModVersion\s*=\s*STR\("([^"]+)"\)' | Select-Object -First 1
    if ($hit) { $Version = $hit.Matches[0].Groups[1].Value } else { $Version = '0.0.0' }
}
Write-Host "[pkg] version: $Version"

# --- 1) build (mod + UE4SS runtime) ---
if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -Full -Configuration $Configuration -CMake $CMake
}

# --- build artifacts + bundled UE4SS sources ---
$ModDll   = Join-Path $BuildDir "LetMeCraft\$Configuration\LetMeCraft.dll"
$Ue4ss    = Join-Path $BuildDir "$Configuration\bin\UE4SS.dll"
$Proxy    = Join-Path $BuildDir "$Configuration\bin\dwmapi.dll"
$Ue4ssIni = Join-Path $Assets 'UE4SS-settings.ini'
$ModsSrc  = Join-Path $Assets 'Mods'
$License  = Join-Path $CppDir 'RE-UE4SS\LICENSE'
$Readme   = Join-Path $PSScriptRoot 'release-readme.txt'

foreach ($f in @($ModDll, $Ue4ss, $Proxy, $Ue4ssIni, $ModsSrc, $License, $Readme)) {
    if (-not (Test-Path $f)) {
        throw "Missing input for packaging: $f  (build first: scripts\build.ps1 -Full)"
    }
}

# --- 2) stage in the game layout ---
$Staging = Join-Path $OutDir '_staging'
if (Test-Path $Staging) { Remove-Item -Recurse -Force $Staging }
$Win64    = Join-Path $Staging 'G1R\Binaries\Win64'
$ModsDest = Join-Path $Win64 'Mods'
$LmcDest  = Join-Path $ModsDest 'LetMeCraft'
New-Item -ItemType Directory -Force -Path $Win64 | Out-Null
New-Item -ItemType Directory -Force -Path $ModsDest | Out-Null

# UE4SS loader + proxy + settings + license
Copy-Item $Proxy    (Join-Path $Win64 'dwmapi.dll')         -Force
Copy-Item $Ue4ss    (Join-Path $Win64 'UE4SS.dll')          -Force
Copy-Item $Ue4ssIni (Join-Path $Win64 'UE4SS-settings.ini') -Force
Copy-Item $License  (Join-Path $Win64 'UE4SS-LICENSE.txt')  -Force

# UE4SS built-in Lua mods (mods.txt / mods.json + the stock mod folders)
Copy-Item (Join-Path $ModsSrc '*') $ModsDest -Recurse -Force

# our mod: main.dll + enabled.txt (so it loads without the user editing mods.txt)
New-Item -ItemType Directory -Force -Path (Join-Path $LmcDest 'dlls') | Out-Null
Copy-Item $ModDll (Join-Path $LmcDest 'dlls\main.dll') -Force
Set-Content -Path (Join-Path $LmcDest 'enabled.txt') -Value '' -NoNewline -Encoding Ascii

# belt-and-suspenders: also list it in the bundled mods.txt
$modsTxt = Join-Path $ModsDest 'mods.txt'
if (Test-Path $modsTxt) {
    $lines = Get-Content $modsTxt
    if (-not ($lines -match '^\s*LetMeCraft\s*:')) {
        Add-Content -Path $modsTxt -Value 'LetMeCraft : 1'
    }
}

# install instructions at the archive root (Russian, kept in a separate UTF-8 file)
Copy-Item $Readme (Join-Path $Staging 'README.txt') -Force

# --- 3) zip ---
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Force -Path $OutDir | Out-Null }
$Zip = Join-Path $OutDir "LetMeCraft-v$Version.zip"
if (Test-Path $Zip) { Remove-Item -Force $Zip }
Compress-Archive -Path (Join-Path $Staging '*') -DestinationPath $Zip -CompressionLevel Optimal

# cleanup staging
Remove-Item -Recurse -Force $Staging

$size = [math]::Round((Get-Item $Zip).Length / 1MB, 2)
Write-Host "[pkg] done: $Zip ($size MB)"
