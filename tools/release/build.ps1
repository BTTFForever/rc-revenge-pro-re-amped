# Builds the Windows release (the counterpart of tools/release/build.sh) and packs it:
#   dist\rc-revenge-pro-windows\  rc-launcher.exe, rc_revenge_pro_usa.exe, rc_revenge_pro_eu.exe (each version set up),
#                                 the DLLs they need (SDL3, zlib, winpthread), README.txt, LICENSE.txt
#   dist\RC_Revenge_Pro_Re-Amped-<git describe>-windows-x86_64.zip and its .sha256
# Needs MSYS2 as for tools\setup.ps1 (plus make and diffutils: pacman -S --needed make diffutils), and tools\setup.ps1
# run for each version to include (the recompiled code and generated headers come from your own disc; the release
# contains the compiled game code, never the disc's files).
#   tools\release\build.ps1 [-Versions usa,eu] [-NoZip]
# FFmpeg: a static build with only the movies' decoder (tools/release/ffmpeg-windows.sh, signature-checked) in
# C:\msys64\opt\rc-ffmpeg-7.1.1 ($env:RC_FFMPEG_PREFIX: another folder, with no spaces), made on the first run.
# libstdc++ and libgcc are linked statically. Build folders: build-release-win-<usa|eu|launcher> (ignored by git).
param(
    [string[]]$Versions = @(),
    [switch]$NoZip
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\version.ps1')
Use-RcToolchainPath
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
Set-Location $root
$msys = if ($env:RC_MSYS2) { $env:RC_MSYS2 } else { 'C:\msys64' }

function Run {
    param([string]$exe, [string[]]$arguments)
    $ErrorActionPreference = 'Continue'
    & $exe @arguments 2>&1 | ForEach-Object { "$_" } | Write-Host
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($code -ne 0) { throw "$exe $($arguments -join ' ') failed (exit $code)" }
}

if ($Versions.Count -eq 0) {
    if ((Test-Path output) -and (Test-Path decomp\generated\usa\rc_addrs.h)) { $Versions += 'usa' }
    if ((Test-Path output_eu) -and (Test-Path decomp\generated\eu\rc_addrs.h)) { $Versions += 'eu' }
}
if ($Versions.Count -eq 0) { throw 'no version set up: run tools\setup.ps1 with your disc first' }

# FFmpeg (once).
$ffmpegUnix = if ($env:RC_FFMPEG_PREFIX) { $env:RC_FFMPEG_PREFIX } else { '/c/msys64/opt/rc-ffmpeg-7.1.1' }
$ffmpegWin = (& "$msys\usr\bin\cygpath.exe" -w $ffmpegUnix).Trim()
if (-not (Test-Path (Join-Path $ffmpegWin 'lib\pkgconfig\libavcodec.pc'))) {
    Write-Host '== building FFmpeg (once)'
    $rootUnix = (& "$msys\usr\bin\cygpath.exe" -u $root).Trim()
    $env:MSYSTEM = 'MINGW64'
    Run "$msys\usr\bin\bash.exe" @('-lc', "cd '$rootUnix' && bash tools/release/ffmpeg-windows.sh '$ffmpegUnix'")
}
$env:PKG_CONFIG_PATH = "$ffmpegWin\lib\pkgconfig"
$static = '-static-libgcc -static-libstdc++'

Write-Host '== building the launcher'
Run cmake @('-S', 'launcher', '-B', 'build-release-win-launcher', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_EXE_LINKER_FLAGS=$static")
Run ninja @('-C', 'build-release-win-launcher')
foreach ($v in $Versions) {
    if ($v -notin 'usa', 'eu') { throw "usa or eu, not $v" }
    Write-Host "== building $v"
    # The same options as the development builds (no build type, RC_GAME_OPT=-O1), FFmpeg static.
    Run cmake @('-S', '.', '-B', "build-release-win-$v", '-G', 'Ninja', "-DRC_VERSION=$v", '-DPS2X_FFMPEG_STATIC=ON', "-DCMAKE_EXE_LINKER_FLAGS=$static")
    Run ninja @('-C', "build-release-win-$v", 'rc_revenge_pro')
}

$dist = Join-Path $root 'dist\rc-revenge-pro-windows'
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
New-Item -ItemType Directory -Force $dist | Out-Null
Copy-Item build-release-win-launcher\rc-launcher.exe $dist
foreach ($v in $Versions) { Copy-Item "build-release-win-$v\rc_revenge_pro.exe" (Join-Path $dist "rc_revenge_pro_$v.exe") }
Get-ChildItem $dist -Filter *.exe | ForEach-Object { Run strip @($_.FullName) }

# The DLLs from MSYS2 the programs load (repeated until none is missing: a DLL can need others).
$mingwBin = Join-Path $msys 'mingw64\bin'
do {
    $added = 0
    foreach ($f in Get-ChildItem $dist -Include *.exe, *.dll -Recurse) {
        $deps = & objdump -p $f.FullName | Select-String 'DLL Name: (.+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
        foreach ($d in $deps) {
            $src = Join-Path $mingwBin $d
            if ((Test-Path $src) -and -not (Test-Path (Join-Path $dist $d))) { Copy-Item $src $dist; $added++ }
        }
    }
} while ($added -gt 0)
foreach ($d in Get-ChildItem $dist -Filter *.dll) {
    if ((& objdump -p $d.FullName | Select-String 'DLL Name: av|DLL Name: sw')) { throw "$($d.Name) needs a shared FFmpeg" }
}
foreach ($f in Get-ChildItem $dist -Filter rc_revenge_pro_*.exe) {
    if ((& objdump -p $f.FullName | Select-String 'DLL Name: (av|sw)')) { throw "$($f.Name) links a shared FFmpeg, not the static one" }
}
Copy-Item LICENSE (Join-Path $dist 'LICENSE.txt')
Copy-Item (Join-Path $PSScriptRoot 'windows-readme.txt') (Join-Path $dist 'README.txt')

Write-Host "== $dist"
Get-ChildItem $dist | Format-Table Name, Length -AutoSize | Out-String | Write-Host
Write-Host '== DLLs the programs need from Windows'
$all = foreach ($f in Get-ChildItem $dist -Include *.exe, *.dll -Recurse) {
    & objdump -p $f.FullName | Select-String 'DLL Name: (.+)$' | ForEach-Object { $_.Matches[0].Groups[1].Value.Trim() }
}
$bundled = (Get-ChildItem $dist -Filter *.dll).Name
($all | Sort-Object -Unique | Where-Object { $_ -notin $bundled }) -join ' ' | Write-Host

if (-not $NoZip) {
    $describe = (& git describe --tags --always --dirty).Trim()
    $zip = Join-Path $root "dist\RC_Revenge_Pro_Re-Amped-$describe-windows-x86_64.zip"
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path $dist -DestinationPath $zip
    $hash = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
    [System.IO.File]::WriteAllText("$zip.sha256", "$hash  $(Split-Path $zip -Leaf)`n")
    Write-Host "== $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
    Write-Host "sha256 $hash"
}
