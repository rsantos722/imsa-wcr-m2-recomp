#!/usr/bin/env bash
# Build the FFmpeg that release packages bundle.
#
#   packaging/ffmpeg/build-ffmpeg.sh <prefix>
#
# Distribution FFmpeg builds (Homebrew, MSYS2, Debian/Ubuntu) enable GPL
# components, and bundling one would make the whole package GPL. This builds
# the pinned upstream release as shared LGPL libraries with only what
# host/audio/mpeg_decoder.cpp uses: the MPEG-1 video and MPEG audio layer II
# decoders and their parsers. Nothing is auto-detected, so no third-party codec
# library can be linked in. The configure step is checked afterwards and the
# script fails if the result is not plain LGPL.
set -euo pipefail

version=9.0.2
sha256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e
url="https://ffmpeg.org/releases/ffmpeg-$version.tar.xz"

prefix=${1:?usage: build-ffmpeg.sh <prefix>}
mkdir -p "$prefix"
prefix=$(cd "$prefix" && pwd)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"

curl -fsSL -o ffmpeg.tar.xz "$url"
if command -v sha256sum >/dev/null; then
    actual=$(sha256sum ffmpeg.tar.xz | cut -d' ' -f1)
else
    actual=$(shasum -a 256 ffmpeg.tar.xz | cut -d' ' -f1)
fi
if [ "$actual" != "$sha256" ]; then
    echo "FFmpeg $version tarball checksum mismatch: $actual" >&2
    exit 1
fi
tar -xf ffmpeg.tar.xz
cd "ffmpeg-$version"

jobs=${FFMPEG_BUILD_JOBS:-4}

./configure \
    --prefix="$prefix" \
    --enable-shared --disable-static --enable-pic \
    --disable-debug --disable-programs --disable-doc --disable-network \
    --disable-autodetect --disable-everything \
    --disable-avformat --disable-avfilter --disable-avdevice --disable-swscale \
    --enable-decoder=mpeg1video,mp2,mp2float \
    --enable-parser=mpegvideo,mpegaudio

grep -q '^#define CONFIG_GPL 0$' config.h
grep -q '^#define CONFIG_NONFREE 0$' config.h

make -j"$jobs"
make install

# Source for the LGPL offer: keep the exact tarball reference beside the libs.
printf '%s\n%s\n' "$url" "sha256 $sha256" > "$prefix/FFMPEG_SOURCE.txt"
echo "FFmpeg $version (LGPL) installed in $prefix"
