# Plays the game on Windows (the counterpart of tools/play.sh), optionally recording or replaying the controller.
#   tools\play.ps1                          plays USA (no recording)
#   tools\play.ps1 -Version eu              plays Europe ($env:RC_VERSION=eu does the same)
#   tools\play.ps1 record <name>            writes decomp\recordings\<name>.pad
#   tools\play.ps1 replay <name>            plays it back (<name> with a \ or / in it: that .pad file)
# The memory cards are kept between runs in %LOCALAPPDATA%\rc-revenge-pro\<version>\game\mc0, mc1 (the launcher's;
# $env:RC_CARDS names another folder holding mc0 and mc1). Recording, replaying and $env:RC_FRESH_CARD=1 start from
# empty cards instead (a replay needs the same start).
# The game's output goes to <build folder>\play\session.log. Every variable the game reads can be set beforehand
# ($env:RC_BOOT_LEVEL=5, $env:PS2X_PAD_LIVE=<file>, ...); the defaults below are used only where none is set.
# -Wait 0 starts the game and returns at once (for tests that drive it through PS2X_PAD_LIVE); the process id is printed.
param(
    [ValidateSet('play', 'record', 'replay')][string]$Mode = 'play',
    [string]$Name = '',
    [ValidateSet('', 'usa', 'eu')][string]$Version = '',
    [int]$Wait = 1
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'version.ps1')
$v = Get-RcVersion $Version
if ($Mode -ne 'play' -and -not $Name) { throw "$Mode needs a recording name" }

$exe = Join-Path $v.GameBuild 'rc_revenge_pro.exe'
if (-not (Test-Path $exe)) { throw "no game build at $($v.GameBuild): run tools\setup.ps1 first" }
$image = Get-RcDiscImage $v

# The sandbox: the executable copied from the disc, and the memory cards beside it (a junction to the kept cards).
$sandbox = Join-Path $v.GameBuild 'play'
$disc = Join-Path $sandbox 'disc'
if (Test-Path $sandbox) { Remove-RcSandbox $sandbox }
New-Item -ItemType Directory -Force $disc | Out-Null
Copy-Item (Join-Path $v.Disc $v.Elf) $disc
$fresh = $Mode -ne 'play' -or $env:RC_FRESH_CARD -eq '1'
foreach ($card in 'mc0', 'mc1') {
    if ($fresh) {
        New-Item -ItemType Directory -Force (Join-Path $disc $card) | Out-Null
    } else {
        $kept = Join-Path $v.Cards $card
        New-Item -ItemType Directory -Force $kept | Out-Null
        New-Item -ItemType Junction -Path (Join-Path $disc $card) -Target $kept | Out-Null
    }
}

$pad = ''
if ($Mode -ne 'play') {
    $pad = if ($Name -match '[\\/]') { $Name } else { Join-Path $v.Root "decomp\recordings\$Name.pad" }
    if ($Mode -eq 'replay' -and -not (Test-Path $pad)) { throw "no recording $pad" }
}

# What tools/play.sh sets; anything already in the environment wins.
$defaults = [ordered]@{
    PS2X_DEBUG_PANEL = '0'; PS2X_GS_GPU = '1'; PS2X_GS_HW = '1'; PS2X_GS_THREAD = '1'; PS2X_MTVU = '1'
    PS2X_VU1_TIMING = '0'; PS2X_EE_HOST_PACING = '1'; PS2X_GS_SHADER_PRELOAD = '0'; PS2X_GS_SHADER_CACHE = '1'
    PS2X_GS_SHADER_CACHE_DIR = (Join-Path $v.GameBuild 'shader-cache'); RC_TRACK_SHADER = '1'; RC_VU1_REF = '0'
}
foreach ($k in $defaults.Keys) {
    if (-not [Environment]::GetEnvironmentVariable($k)) { [Environment]::SetEnvironmentVariable($k, $defaults[$k]) }
}
$env:PS2X_PAD_REPLAY = ''
$env:PS2X_PAD_RECORD = ''
if ($Mode -eq 'record') { $env:PS2X_PAD_RECORD = $pad }
if ($Mode -eq 'replay') { $env:PS2X_PAD_REPLAY = $pad }
$env:RC_DISC_IMAGE = $image
Use-RcToolchainPath # the MinGW runtime DLLs (SDL3, FFmpeg, libstdc++) for a development build

$log = Join-Path $sandbox 'session.log'
$elf = Join-Path $disc $v.Elf
$p = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', "`"`"$exe`" `"$elf`" > `"$log`" 2>&1`"" `
    -WorkingDirectory $v.Root -WindowStyle Hidden -PassThru
Write-Host "started $($v.Id) (process $($p.Id)), log: $log"
if ($Wait) {
    $p.WaitForExit()
    exit $p.ExitCode
}
