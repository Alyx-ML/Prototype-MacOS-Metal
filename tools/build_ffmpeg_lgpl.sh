#!/bin/sh
# Build the LGPL FFmpeg libraries for the release (libavcodec and libavutil
# with only the Bink decoders). The result is the FFMPEG_PREFIX for
# tools/package_release.sh.
#
# Usage: sh tools/build_ffmpeg_lgpl.sh <ffmpeg source tarball>
# Output: build/ffmpeg-lgpl (install prefix), build/ffmpeg-src (sources).
set -eu

PORT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
TARBALL="${1:?usage: build_ffmpeg_lgpl.sh <ffmpeg-x.y.z.tar.xz>}"
SRC="$PORT_DIR/build/ffmpeg-src"
PREFIX="$PORT_DIR/build/ffmpeg-lgpl"
STAGE="$PORT_DIR/build/ffmpeg-stage"
# The libraries hold the configure options (avcodec_configuration). A
# fixed prefix keeps the path of the build computer out of the release
# libraries; make install writes to STAGE/ffmpeg-lgpl, and the script then
# moves that directory to PREFIX.
NEUTRAL_PREFIX=/ffmpeg-lgpl

rm -rf "$SRC" "$PREFIX" "$STAGE"
mkdir -p "$SRC"
tar -xf "$TARBALL" -C "$SRC" --strip-components 1

cd "$SRC"
# No --enable-gpl and no --enable-nonfree: the libraries stay LGPL 2.1+.
./configure \
    --prefix="$NEUTRAL_PREFIX" \
    --arch=arm64 --target-os=darwin --cc=clang \
    --extra-cflags="-mmacosx-version-min=14.0" --extra-ldflags="-mmacosx-version-min=14.0" \
    --install-name-dir=@rpath \
    --enable-shared --disable-static --enable-pic \
    --disable-autodetect --disable-programs --disable-doc --disable-network \
    --disable-everything \
    --disable-avdevice --disable-avformat --disable-avfilter --disable-swscale --disable-swresample \
    --enable-decoder=bink,binkaudio_rdft,binkaudio_dct
make -j"$(sysctl -n hw.ncpu)"
make install DESTDIR="$STAGE"
mv "$STAGE$NEUTRAL_PREFIX" "$PREFIX"
rm -rf "$STAGE"
mkdir -p "$PREFIX/share/doc/ffmpeg"
cp COPYING.LGPLv2.1 "$PREFIX/share/doc/ffmpeg/COPYING.LGPLv2.1"
echo "FFMPEG_PREFIX=$PREFIX"
