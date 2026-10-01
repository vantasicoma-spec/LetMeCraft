#requires -Version 5.1

<#
.SYNOPSIS
    Builds the redistributable LetMeCraft release ZIPs: a full drop-in (mod + UE4SS runtime) and a
    mod-only archive for players who already have UE4SS installed.

.DESCRIPTION
    1) Builds the mod from the repository (build.ps1) and, when the full archive is requested, the
       UE4SS proxy as well (build.ps1 -Full), so the archives are fully reproducible and never
       depend on any installed game. Building the mod always compiles UE4SS.dll too, because the
       mod links against it; only the proxy is skipped for the mod-only archive.
    2) Checks every input of the requested archives up front, so a missing file never leaves a
       partial release behind. Previous ZIPs of the requested archives must not be in use by
       another program; they are removed before packaging starts.
    3) Stages each requested archive separately and compresses it:
         dist\LetMeCraft-v<version>.zip           FULL, flat game layout (G1R\Binaries\Win64\...):
                                                  mod + UE4SS loader, proxy, settings, license and
                                                  the stock UE4SS Lua mods (mods.txt lists the mod).
         dist\LetMeCraft-v<version>-no-ue4ss.zip  MOD-ONLY: a single LetMeCraft\ folder
                                                  (dlls\main.dll, enabled.txt, README.txt). Contains
                                                  no UE4SS file and no mods.txt, so the player's own
                                                  UE4SS setup is never overwritten.
       An archive that was not requested is left untouched in the output directory. On failure no
       staging folder and no partially written ZIP is left behind.

    The full archive is extracted into the GAME ROOT folder (the one that contains the "G1R"
    folder). The mod-only archive is extracted into the Mods folder of the player's UE4SS:
    G1R\Binaries\Win64\ue4ss\Mods when it exists, otherwise G1R\Binaries\Win64\Mods.

    Bundled UE4SS files (loader + built-in Lua mods + license) come from the in-repo
    cpp\RE-UE4SS sources, so no third party's machine paths or unrelated mods leak into the zip.

    The mod-only archive works only with the UE4SS build the mod was compiled against (see
    scripts\release-readme-no-ue4ss.txt); players with any other UE4SS should use the full archive.

.PARAMETER Version
    Version string for the archive names. Default: parsed from ModVersion in dllmain.cpp.

.PARAMETER OutDir
    Output directory for the archives. Default: <repo>\dist.

.PARAMETER SkipBuild
    Do not rebuild; package the artifacts already present in the build tree.

.PARAMETER Configuration
    CMake configuration. Default: Game__Shipping__Win64.

.PARAMETER CMake
    Explicit path to cmake.exe (forwarded to build.ps1).

.PARAMETER Which
    Which archives to produce:
      All     (default) both the full and the mod-only archive.
      Full    only LetMeCraft-v<version>.zip.
      ModOnly only LetMeCraft-v<version>-no-ue4ss.zip; skips the UE4SS proxy and needs no
              UE4SS.dll / dwmapi.dll in the build tree (UE4SS.dll is still compiled when
              building, as a link dependency of the mod).

.EXAMPLE
    .\scripts\package.ps1
    Build the mod + UE4SS proxy and produce both archives.

.EXAMPLE
    .\scripts\package.ps1 -Which ModOnly -SkipBuild
    Produce only the mod-only archive from the mod DLL already present in the build tree.
#>
[CmdletBinding()]
param(
    [string]$Version,
    [string]$OutDir,
    [switch]$SkipBuild,
    [string]$Configuration = 'Game__Shipping__Win64',
    [string]$CMake,
    [ValidateSet('All', 'Full', 'ModOnly')]
    [string]$Which = 'All'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$WantFull    = $Which -in @('All', 'Full')
$WantModOnly = $Which -in @('All', 'ModOnly')

# --- repo layout (resolved relative to this script) ---
$RepoRoot = Split-Path -Parent $PSScriptRoot
$CppDir   = Join-Path $RepoRoot 'cpp'
$BuildDir = Join-Path $CppDir 'build-vs2022'
$Assets   = Join-Path $CppDir 'RE-UE4SS\assets'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'dist' }
$OutDir = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir)

# --- version from dllmain.cpp (single source of truth) ---
if (-not $Version) {
    $dllmain = Join-Path $CppDir 'LetMeCraft\dllmain.cpp'
    $hit = Select-String -LiteralPath $dllmain -Pattern 'ModVersion\s*=\s*STR\("([^"]+)"\)' | Select-Object -First 1
    if ($hit) { $Version = $hit.Matches[0].Groups[1].Value } else { $Version = '0.0.0' }
}
Write-Host "[pkg] version : $Version"
Write-Host "[pkg] archives: $Which"

if (-not $SkipBuild) {
    $buildArgs = @{ Configuration = $Configuration; CMake = $CMake }
    if ($WantFull) { $buildArgs.Full = $true }
    & (Join-Path $PSScriptRoot 'build.ps1') @buildArgs
}

# --- build artifacts + bundled UE4SS sources ---
$ModDll        = Join-Path $BuildDir "LetMeCraft\$Configuration\LetMeCraft.dll"
$Ue4ss         = Join-Path $BuildDir "$Configuration\bin\UE4SS.dll"
$Proxy         = Join-Path $BuildDir "$Configuration\bin\dwmapi.dll"
$Ue4ssIni      = Join-Path $Assets 'UE4SS-settings.ini'
$ModsSrc       = Join-Path $Assets 'Mods'
$License       = Join-Path $CppDir 'RE-UE4SS\LICENSE'
$Readme        = Join-Path $PSScriptRoot 'release-readme.txt'
$ReadmeNoUe4ss = Join-Path $PSScriptRoot 'release-readme-no-ue4ss.txt'

$required = @($ModDll)
if ($WantFull)    { $required += @($Ue4ss, $Proxy, $Ue4ssIni, $ModsSrc, $License, $Readme) }
if ($WantModOnly) { $required += @($ReadmeNoUe4ss) }
$buildHint = if ($WantFull) { 'scripts\build.ps1 -Full' } else { 'scripts\build.ps1' }

foreach ($f in $required) {
    if (-not (Test-Path -LiteralPath $f)) {
        throw "Missing input for packaging: $f  (build first: $buildHint)"
    }
}

function New-Staging {
    param([string]$Name)

    $path = Join-Path $OutDir $Name
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $path | Out-Null
    return $path
}

function Add-ModFiles {
    param([string]$ModsDest)

    # our mod: main.dll + enabled.txt (so it loads without the user editing mods.txt)
    $lmcDest = Join-Path $ModsDest 'LetMeCraft'
    New-Item -ItemType Directory -Force -Path (Join-Path $lmcDest 'dlls') | Out-Null
    Copy-Item -LiteralPath $ModDll -Destination (Join-Path $lmcDest 'dlls\main.dll') -Force
    Set-Content -LiteralPath (Join-Path $lmcDest 'enabled.txt') -Value '' -NoNewline -Encoding Ascii
}

function Compress-Staging {
    param([string]$Staging, [string]$Zip)

    # --- 3) zip ---
    $items  = @(Get-ChildItem -LiteralPath $Staging -Force | ForEach-Object { $_.FullName })
    $tmpZip = Join-Path ([System.IO.Path]::GetTempPath()) ('LetMeCraft-' + [guid]::NewGuid().ToString('N') + '.zip')
    try {
        Compress-Archive -LiteralPath $items -DestinationPath $tmpZip -CompressionLevel Optimal
        [System.IO.File]::Move($tmpZip, $Zip)
    }
    catch {
        if (Test-Path -LiteralPath $Zip) { Remove-Item -LiteralPath $Zip -Force }
        throw
    }
    finally {
        if (Test-Path -LiteralPath $tmpZip) { Remove-Item -LiteralPath $tmpZip -Force }
    }

    $size = [math]::Round((Get-Item -LiteralPath $Zip).Length / 1MB, 2)
    Write-Host "[pkg] done: $Zip ($size MB)"
}

$FullZip    = Join-Path $OutDir "LetMeCraft-v$Version.zip"
$ModOnlyZip = Join-Path $OutDir "LetMeCraft-v$Version-no-ue4ss.zip"
$targets = @()
if ($WantFull)    { $targets += $FullZip }
if ($WantModOnly) { $targets += $ModOnlyZip }

foreach ($t in $targets) {
    if (Test-Path -LiteralPath $t) {
        try { [System.IO.File]::Open($t, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::None).Dispose() }
        catch { throw "Cannot replace $t (is it open in another program?): $($_.Exception.GetBaseException().Message)" }
    }
}
foreach ($t in $targets) {
    if (Test-Path -LiteralPath $t) { Remove-Item -LiteralPath $t -Force }
}

$StagingNames = @('_staging-full', '_staging-no-ue4ss')
try {
    # --- 2) stage in the game layout ---
    if ($WantFull) {
        $Staging  = New-Staging $StagingNames[0]
        $Win64    = Join-Path $Staging 'G1R\Binaries\Win64'
        $ModsDest = Join-Path $Win64 'Mods'
        New-Item -ItemType Directory -Force -Path $ModsDest | Out-Null

        # UE4SS loader + proxy + settings + license
        Copy-Item -LiteralPath $Proxy    -Destination (Join-Path $Win64 'dwmapi.dll')         -Force
        Copy-Item -LiteralPath $Ue4ss    -Destination (Join-Path $Win64 'UE4SS.dll')          -Force
        Copy-Item -LiteralPath $Ue4ssIni -Destination (Join-Path $Win64 'UE4SS-settings.ini') -Force
        Copy-Item -LiteralPath $License  -Destination (Join-Path $Win64 'UE4SS-LICENSE.txt')  -Force

        # UE4SS built-in Lua mods (mods.txt / mods.json + the stock mod folders)
        Get-ChildItem -LiteralPath $ModsSrc -Force | Copy-Item -Destination $ModsDest -Recurse -Force

        Add-ModFiles $ModsDest

        # belt-and-suspenders: also list it in the bundled mods.txt
        $modsTxt = Join-Path $ModsDest 'mods.txt'
        if (Test-Path -LiteralPath $modsTxt) {
            $lines = Get-Content -LiteralPath $modsTxt
            if (-not ($lines -match '^\s*LetMeCraft\s*:')) {
                Add-Content -LiteralPath $modsTxt -Value 'LetMeCraft : 1'
            }
        }

        # install instructions at the archive root (Russian, kept in a separate UTF-8 file)
        Copy-Item -LiteralPath $Readme -Destination (Join-Path $Staging 'README.txt') -Force

        Compress-Staging $Staging $FullZip
    }

    if ($WantModOnly) {
        $Staging = New-Staging $StagingNames[1]

        Add-ModFiles $Staging

        Copy-Item -LiteralPath $ReadmeNoUe4ss -Destination (Join-Path $Staging 'LetMeCraft\README.txt') -Force

        Compress-Staging $Staging $ModOnlyZip
    }
}
finally {
    # cleanup staging
    foreach ($name in $StagingNames) {
        $path = Join-Path $OutDir $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
    }
}
