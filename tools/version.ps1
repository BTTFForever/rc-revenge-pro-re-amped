# Dot-sourced by the Windows scripts (play.ps1, setup.ps1, release\build.ps1): the counterpart of tools/version.sh.
# The paths of the game version chosen by -Version or $env:RC_VERSION: usa (SLUS-20153, the default) or eu (SLES-50077).

function Get-RcVersion([string]$Version = '') {
    if (-not $Version) { $Version = if ($env:RC_VERSION) { $env:RC_VERSION } else { 'usa' } }
    $root = Split-Path $PSScriptRoot -Parent
    $cardsBase = if ($env:RC_CARDS) { $env:RC_CARDS } else { $null }
    switch ($Version) {
        'usa' { $elf = 'SLUS_201.53'; $iso = 'RC Revenge Pro (USA).iso'; $build = 'build-decomp'; $oracle = 'build-oracle'; $out = 'output' }
        'eu' { $elf = 'SLES_500.77'; $iso = 'RC Revenge Pro (Europe).iso'; $build = 'build-eu'; $oracle = 'build-oracle-eu'; $out = 'output_eu' }
        default { throw "RC_VERSION must be usa or eu, not $Version" }
    }
    if (-not $cardsBase) { $cardsBase = Join-Path $env:LOCALAPPDATA "rc-revenge-pro\$Version\game" }
    [pscustomobject]@{
        Id          = $Version
        Root        = $root
        Game        = Join-Path $root "game\$Version"
        Disc        = Join-Path $root "game\$Version\disc"
        Elf         = $elf
        IsoName     = $iso
        GameBuild   = Join-Path $root $build
        OracleBuild = Join-Path $root $oracle
        Output      = Join-Path $root $out
        Cards       = $cardsBase
    }
}

# The disc image the game reads: game\<version>\<image name> if it is there (a converted .bin), else the path
# tools\setup.ps1 recorded in game\<version>\disc-image.txt (an .iso is read where it is: no links on Windows).
function Get-RcDiscImage($v) {
    $local = Join-Path $v.Game $v.IsoName
    if (Test-Path $local) { return $local }
    $record = Join-Path $v.Game 'disc-image.txt'
    if (Test-Path $record) {
        $path = (Get-Content -LiteralPath $record -Encoding UTF8 -TotalCount 1).Trim()
        if (Test-Path -LiteralPath $path) { return $path }
        throw "the disc image recorded in $record is missing: $path (run tools\setup.ps1 again)"
    }
    throw "no disc image for $($v.Id): run tools\setup.ps1 with your disc image"
}

# MSYS2's MinGW-w64 tools and DLLs first on PATH ($env:RC_MSYS2 names another MSYS2 folder than C:\msys64).
function Use-RcToolchainPath {
    $msys = if ($env:RC_MSYS2) { $env:RC_MSYS2 } else { 'C:\msys64' }
    $bin = Join-Path $msys 'mingw64\bin'
    if (-not (Test-Path (Join-Path $bin 'g++.exe'))) { throw "no MinGW-w64 GCC in ${bin}: install MSYS2 and the packages in README.md" }
    if (-not ($env:PATH -split ';' | Where-Object { $_ -eq $bin })) {
        $env:PATH = "$bin;$(Join-Path $msys 'usr\bin');$env:PATH"
    }
}

# Removes a sandbox without following its junctions into the kept memory cards.
function Remove-RcSandbox([string]$path) {
    Get-ChildItem -LiteralPath $path -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue |
        ForEach-Object { [System.IO.Directory]::Delete($_.FullName) }
    Remove-Item -LiteralPath $path -Recurse -Force
}
