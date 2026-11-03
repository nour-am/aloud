#!/bin/sh
# Build a minimal, audio-only static FFmpeg for one target.
#   scripts/build-ffmpeg.sh kindle   -> build/kindle/ffmpeg   (ARMv7 musl, static)
#   scripts/build-ffmpeg.sh host     -> build/host/ffmpeg     (this machine, for tests)
# Only what an audiobook player needs is enabled: no video, no network, no filters.
set -eu

TARGET=${1:?usage: build-ffmpeg.sh kindle|host}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/third_party/ffmpeg"
OUT="$ROOT/build/$TARGET/ffmpeg"
WORK="$ROOT/build/$TARGET/ffmpeg-build"

[ -d "$SRC" ] || { echo "missing $SRC (run scripts/fetch-deps.sh)" >&2; exit 1; }
rm -rf "$WORK" && mkdir -p "$WORK" "$OUT"

COMMON="
  --prefix=$OUT
  --enable-static --disable-shared
  --disable-programs --disable-doc --disable-avdevice --disable-avfilter --disable-swscale
  --disable-network --disable-autodetect --disable-everything
  --disable-debug --disable-runtime-cpudetect
  --enable-protocol=file
  --enable-demuxer=mov,mp3,ogg,flac,wav,aac,matroska
  --enable-parser=aac,mpegaudio,flac,opus,vorbis
  --enable-decoder=aac,mp3float,mp3,opus,vorbis,flac,alac,pcm_s16le,pcm_s16be,pcm_s24le,pcm_f32le,pcm_u8
  --enable-swresample
"

cd "$WORK"
case "$TARGET" in
kindle)
  # shellcheck disable=SC2086
  "$SRC/configure" $COMMON \
    --enable-cross-compile --target-os=linux --arch=arm --cpu=cortex-a8 \
    --cc="$ROOT/tools/arm-cc" --ar="$ROOT/tools/arm-ar" --ranlib="$ROOT/tools/arm-ranlib" \
    --nm="$(xcrun -f llvm-nm 2>/dev/null || echo nm)" \
    --enable-neon --enable-vfp --enable-armv6 --enable-armv6t2 --enable-thumb \
    --extra-cflags="-O2 -ffunction-sections -fdata-sections" \
    --pkg-config=false
  ;;
host)
  # shellcheck disable=SC2086
  "$SRC/configure" $COMMON --extra-cflags="-O2" --pkg-config=false
  ;;
*) echo "unknown target $TARGET" >&2; exit 1 ;;
esac

make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
make install
echo "FFmpeg ($TARGET) installed to $OUT"
