#!/bin/bash
# Builds the Windows release's FFmpeg (tools/release/build.ps1 runs it in MSYS2's bash, MINGW64 tools first on PATH):
# FFmpeg 7.1.1 from ffmpeg.org, signature checked, static, only what the game uses (the MPEG-2 decoder and parser for
# the movies, swscale; avformat and swresample are linked), the same set as the Linux release's (Containerfile).
#   ffmpeg-windows.sh <prefix>     <prefix> must have no spaces (FFmpeg's configure and pkg-config files cannot).
set -euo pipefail
export TMPDIR=/tmp # not Windows TEMP: configure cannot use a C: path
prefix="$1"
version=7.1.1
key=FCF986EA15E6E293A5644F10B4322F04D67658D8 # FFmpeg release signing key <ffmpeg-devel@ffmpeg.org>
case "$prefix" in *' '*) echo "the prefix must have no spaces: $prefix" >&2; exit 2 ;; esac
[ -f "$prefix/lib/pkgconfig/libavcodec.pc" ] && { echo "FFmpeg already in $prefix"; exit 0; }
work="$(mktemp -d /tmp/rc-ffmpeg.XXXXXX)"
trap 'rm -rf "$work"' EXIT
cd "$work"
curl -fsSL -o "ffmpeg-$version.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"
curl -fsSL -o "ffmpeg-$version.tar.xz.asc" "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz.asc"
curl -fsSL -o ffmpeg-devel.asc https://ffmpeg.org/ffmpeg-devel.asc
export GNUPGHOME="$work/gnupg"
mkdir -p "$GNUPGHOME"; chmod 700 "$GNUPGHOME" 2>/dev/null || true # (MSYS2: no Unix modes on some file systems)
gpg -q --batch --import ffmpeg-devel.asc
gpg --batch --status-fd 1 --verify "ffmpeg-$version.tar.xz.asc" "ffmpeg-$version.tar.xz" 2>/dev/null | grep -q "VALIDSIG $key" || {
    echo "ffmpeg-$version.tar.xz: no valid signature by $key" >&2; exit 1; }
gpgconf --kill gpg-agent 2>/dev/null || true
tar -xJf "ffmpeg-$version.tar.xz"
cd "ffmpeg-$version"
./configure --prefix="$prefix" --target-os=mingw32 --arch=x86_64 --enable-static --disable-shared --disable-programs \
    --disable-doc --disable-autodetect --disable-network --disable-everything --disable-x86asm \
    --enable-decoder=mpeg2video --enable-parser=mpegvideo --enable-swscale --enable-swresample > ../configure.log
make -j"$(nproc)" > ../make.log
make install > ../install.log
echo "FFmpeg $version installed in $prefix"
