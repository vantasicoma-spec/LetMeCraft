#requires -Version 5.1
<#
.SYNOPSIS
    Builds the LetMeCraft UE4SS C++ mod (and optionally the UE4SS runtime) with CMake + VS2022.

.DESCRIPTION
    Locates cmake automatically (PATH -> vswhere -> default VS install path) so no machine
    specific path is ever hard-coded. Configures the build tree on first run, then builds the
    requested targets in the chosen configuration.

    Default target is just the mod (LetMeCraft). Pass -Full to also build the UE4SS loader
    (UE4SS.dll) and the proxy (dwmapi.dll) that package.ps1 needs for a full drop-in archive.

.PARAMETER Configuration
    CMake configuration. Default: Game__Shipping__Win64 (the release config).

.PARAMETER Full
    Also build the UE4SS runtime targets (UE4SS + proxy). Required by package.ps1.

.PARAMETER Install
    Path to the GAME ROOT folder (the one that contains the "G1R" folder). When set, the freshly
    built main.dll is copied to <Install>\G1R\Binaries\Win64\Mods\LetMeCraft\dlls\main.dll.
    NOTE: the game must be CLOSED, otherwise the loaded DLL is locked and the copy fails.

.PARAMETER CMake
    Explicit path to cmake.exe (overrides auto-detection).

.EXAMPLE
    .\scripts\build.ps1
    Build only the mod into the build tree.

.EXAMPLE
    .\scripts\build.ps1 -Install "D:\Games\Steam\steamapps\common\Gothic 1 Remake"
    Build the mod and copy main.dll straight into the installed game.
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Game__Shipping__Win64',
    [switch]$Full,
    [string]$Install,
    [string]$CMake
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# --- repo layout (resolved relative to this script, never hard-coded) ---
$RepoRoot = Split-Path -Parent $PSScriptRoot
$CppDir   = Join-Path $RepoRoot 'cpp'
$BuildDir = Join-Path $CppDir 'build-vs2022'

function Find-CMake {
    param([string]$Override)

    if ($Override) {
        if (Test-Path $Override) { return $Override }
        throw "cmake not found at the path given via -CMake: $Override"
    }

    $onPath = Get-Command cmake -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoot = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($vsRoot) {
            $candidate = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }

    throw "cmake not found. Install CMake (or Visual Studio 2022 with the CMake component), or pass -CMake <path>."
}

$cmake = Find-CMake -Override $CMake
Write-Host "[build] cmake : $cmake"
Write-Host "[build] config: $Configuration"

# UE4SS links a Rust component via corrosion; make sure the standard cargo bin is on PATH if present.
$cargoBin = Join-Path $env:USERPROFILE '.cargo\bin'
if (Test-Path $cargoBin) { $env:Path = "$cargoBin;$env:Path" }

# --- configure the build tree only if it has not been configured yet ---
if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
    Write-Host "[build] configuring build tree (first run; UE4SS needs Rust + initialised submodules)..."
    & $cmake -S $CppDir -B $BuildDir -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)." }
}

# --- targets ---
$targets = @('LetMeCraft')
if ($Full) { $targets += @('UE4SS', 'proxy') }

foreach ($t in $targets) {
    Write-Host "[build] building target '$t'..."
    & $cmake --build $BuildDir --config $Configuration --target $t
    if ($LASTEXITCODE -ne 0) { throw "Build of target '$t' failed ($LASTEXITCODE)." }
}

# --- verify artifacts ---
$ModDll = Join-Path $BuildDir "LetMeCraft\$Configuration\LetMeCraft.dll"
if (-not (Test-Path $ModDll)) { throw "Built mod DLL not found: $ModDll" }
Write-Host "[build] OK mod   : $ModDll"

if ($Full) {
    $ue4ss = Join-Path $BuildDir "$Configuration\bin\UE4SS.dll"
    $proxy = Join-Path $BuildDir "$Configuration\bin\dwmapi.dll"
    foreach ($f in @($ue4ss, $proxy)) {
        if (-not (Test-Path $f)) { throw "UE4SS runtime artifact not found: $f" }
    }
    Write-Host "[build] OK ue4ss : $ue4ss"
    Write-Host "[build] OK proxy : $proxy"
}

# --- optional install into the running game's mod folder ---
if ($Install) {
    $dest = Join-Path $Install 'G1R\Binaries\Win64\Mods\LetMeCraft\dlls'
    if (-not (Test-Path $dest)) { New-Item -ItemType Directory -Force -Path $dest | Out-Null }
    try {
        Copy-Item $ModDll (Join-Path $dest 'main.dll') -Force
    }
    catch {
        throw "Failed to copy main.dll into the game. Is the game still running? (the DLL gets locked). $_"
    }
    Write-Host "[build] installed: $(Join-Path $dest 'main.dll')"
}

Write-Host "[build] done."
