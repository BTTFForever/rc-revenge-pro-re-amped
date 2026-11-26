# From your own RC Revenge Pro disc image to a working Windows build (the counterpart of tools/setup.sh).
#
#   tools\setup.ps1 [-Oracle] [-NoBuild] [-Jobs N] <disc image>
#
# <disc image>: a .iso (2048-byte sectors), or a raw .bin (2352-byte sectors) or its .cue. The version is read from the
# disc: SLUS-20153 (USA, NTSC) or SLES-50077 (Europe, PAL). Run it once per version you own; both can sit side by side.
#   -Oracle     also build the offline oracle (build-oracle or build-oracle-eu)
#   -NoBuild    stop after generating the code (recompiled code, headers, VU1 code) and configuring
#   -Jobs N     parallel build jobs (default: ninja's)
#
# Needs MSYS2 (https://www.msys2.org, in C:\msys64 or $env:RC_MSYS2) with, from its MINGW64 shell:
#   pacman -S --needed mingw-w64-x86_64-{gcc,cmake,ninja,pkgconf,sdl3,ffmpeg,glslang,xxhash,zlib}
# and Python 3 for Windows (python.org or the Microsoft Store), plus network access on the first configure.
#
# What it does (all output is ignored by git: game\, output*\, vu1*\, decomp\generated\, build*\):
#   1. game\<version>\: the disc's files in disc\; the image itself is read where it is (its path is kept in
#      disc-image.txt; no links on Windows), a .bin is converted once to game\<version>\<name>.iso.
#   2. Checks the executable is the one this project was made from.
#   3. Builds the recompiler (third_party\ps2recomp\ps2xRecomp) in build-tools\ps2recomp.
#   4. Recompiles the executable into output\ or output_eu\.
#   5. Generates the headers the decompiled code compiles against.
#   6. Converts the VU1 microprograms into vu1\.
#   7. Configures and builds the game in build-decomp (USA) or build-eu (Europe).
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Image,
    [switch]$Oracle,
    [switch]$NoBuild,
    [int]$Jobs = 0
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
Use-RcToolchainPath
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
$python = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $python) { throw 'python (Python 3 for Windows) is required' }
foreach ($tool in 'cmake', 'ninja', 'g++') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is required (see the MSYS2 packages above)" }
}
$ninjaJobs = if ($Jobs -gt 0) { @('-j', "$Jobs") } else { @() }
function Step([string]$text) { Write-Host "`n== $text" }
# Runs a native program; stops the set-up when it fails.
function Run {
    param([string]$exe, [string[]]$arguments, [string]$log = '')
    if ($log) {
        & $exe @arguments *> $log
    } else {
        & $exe @arguments
    }
    if ($LASTEXITCODE -ne 0) {
        if ($log) { Get-Content $log -Tail 20 | Write-Host }
        throw "$exe failed (exit $LASTEXITCODE)$(if ($log) { ": see $log" })"
    }
}
function Py([string[]]$arguments) {
    $out = & python -I @arguments
    if ($LASTEXITCODE -ne 0) { throw "python $($arguments -join ' ') failed" }
    $out
}

$Image = (Resolve-Path -LiteralPath $Image).Path
Step 'Reading the disc'
$elf = (Py @('tools\iso_extract.py', 'boot', $Image)).Trim()
switch ($elf) {
    'SLUS_201.53' { $id = 'usa'; $sha1 = 'a3f472d489e688525ea5e5513ce4c9dc6e6acd93'; $config = 'config.toml' }
    'SLES_500.77' { $id = 'eu'; $sha1 = '6957407aeb69c89f614a8a675bfcaae92dca83b3'; $config = 'config_eu.toml' }
    default { throw "this disc boots ${elf}: not RC Revenge Pro USA (SLUS_201.53) or Europe (SLES_500.77)" }
}
$v = Get-RcVersion $id
Write-Host "${elf}: version $id"
New-Item -ItemType Directory -Force $v.Game | Out-Null
$kind, $source = (Py @('tools\iso_extract.py', 'kind', $Image)).Trim() -split ' ', 2
$localIso = Join-Path $v.Game $v.IsoName
if (Test-Path -LiteralPath $localIso) { Remove-Item -LiteralPath $localIso }
$record = Join-Path $v.Game 'disc-image.txt'
if ($kind -eq 'raw') {
    Write-Host "converting $source to $localIso"
    Py @('tools\iso_extract.py', 'to-iso', $source, $localIso) | Out-Null
    if (Test-Path $record) { Remove-Item $record }
    $gameImage = $localIso
} else {
    [System.IO.File]::WriteAllText($record, "$source`n") # the game reads its files through the image at run time
    Write-Host "the game will read $source (kept in $record)"
    $gameImage = $source
}
if (Test-Path $v.Disc) { Remove-Item -Recurse -Force $v.Disc }
Py @('tools\iso_extract.py', 'extract', $gameImage, $v.Disc) | Out-Null
New-Item -ItemType Directory -Force (Join-Path $v.Disc 'mc0'), (Join-Path $v.Disc 'mc1') | Out-Null
$actual = (Py @('tools\iso_extract.py', 'sha1', (Join-Path $v.Disc $elf))).Trim()
if ($actual -ne $sha1) { throw "$elf has SHA-1 $actual, expected ${sha1}: a different release of the game, which this project does not support" }
Write-Host "$elf matches the expected executable"
$elfPath = "game\$id\disc\$elf"
$out = Split-Path $v.Output -Leaf
$gameBuild = Split-Path $v.GameBuild -Leaf
$oracleBuild = Split-Path $v.OracleBuild -Leaf
New-Item -ItemType Directory -Force build-tools | Out-Null

Step 'Building the recompiler'
Run cmake @('-S', 'third_party\ps2recomp\ps2xRecomp', '-B', 'build-tools\ps2recomp', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
Run ninja (@('-C', 'build-tools\ps2recomp') + $ninjaJobs + @('ps2_recomp'))

Step "Recompiling $elf into $out\"
if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory $out | Out-Null
Run 'build-tools\ps2recomp\ps2_recomp.exe' @($config) "build-tools\recompile-$id.log"
Write-Host "$((Get-ChildItem $out).Count) files (log: build-tools\recompile-$id.log)"

Step 'Generating the headers for the decompiled code'
Py @('tools\stabs2c.py', $elfPath, "decomp\generated\$id") | Out-Null
if ($id -eq 'usa') {
    Py @('tools\stabs2c.py', $elfPath, 'decomp\generated') | Out-Null # the reference listings the decompilation tools read
    Py @('tools\gen_addrs.py', $elfPath, $out, 'decomp\generated\usa')
} else {
    Py @('tools\gen_addrs.py', $elfPath, $out, 'decomp\generated\eu', 'decomp\generated\eu\lit4_map.txt', 'decomp\generated\eu\changed_functions.txt')
}

Step "Configuring $gameBuild"
Run cmake @('-S', '.', '-B', $gameBuild, '-G', 'Ninja', "-DRC_VERSION=$id")

Step 'Converting the VU1 microprograms'
Run ninja (@('-C', $gameBuild) + $ninjaJobs + @('vu1_recomp'))
Py @('tools\make_vu1_images.py', $elfPath, 'vu1_images') | Out-Null
if (Test-Path vu1) { Remove-Item -Recurse -Force vu1 }
New-Item -ItemType Directory vu1 | Out-Null
Run "$gameBuild\vu1_recomp.exe" @('vu1_images', 'vu1') "build-tools\vu1-$id.log"
Run cmake @('-S', '.', '-B', $gameBuild) "build-tools\configure-$id.log" # picks up vu1\
if ($Oracle) { Run cmake @('-S', '.', '-B', $oracleBuild, '-G', 'Ninja', "-DRC_VERSION=$id") "build-tools\configure-oracle-$id.log" }

if ($NoBuild) {
    Write-Host "`nGenerated and configured. Build with: ninja -C $gameBuild rc_revenge_pro"
    exit 0
}
Step "Building the game in $gameBuild (the recompiled code takes a while)"
Run ninja (@('-C', $gameBuild) + $ninjaJobs + @('rc_revenge_pro'))
if ($Oracle) {
    Step "Building the offline oracle in $oracleBuild"
    Run ninja (@('-C', $oracleBuild) + $ninjaJobs + @('rc_oracle'))
}
Write-Host "`nDone. Play with: tools\play.ps1$(if ($id -eq 'eu') { ' -Version eu' })"
