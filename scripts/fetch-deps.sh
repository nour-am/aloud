#!/bin/sh
# Fetch third-party sources at pinned, verified revisions into third_party/.
# Re-running is safe: existing checkouts are left alone.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TP="$ROOT/third_party"
mkdir -p "$TP"

FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

FBINK_REPO=https://github.com/NiLuJe/FBInk.git
FBINK_REV=886f25f13368859ad8a899b88d04c26e19cda32e
SONIC_REPO=https://github.com/waywardgeek/sonic.git
SONIC_REV=b93885dcb70aae50c6f76b0fe4e0868f029a077e
STB_REPO=https://github.com/nothings/stb.git
STB_REV=d14d46f6b44456eb88beac58cba833a4135d4c89

git_pinned() { # name repo rev [submodules]
  dir="$TP/$1"
  if [ ! -d "$dir/.git" ]; then
    git init -q "$dir"
    git -C "$dir" remote add origin "$2"
  fi
  if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$3" ]; then
    git -C "$dir" fetch -q --depth 1 origin "$3"
    git -C "$dir" checkout -q FETCH_HEAD
  fi
  if [ "${4:-}" = submodules ]; then
    git -C "$dir" submodule update -q --init --recursive --depth 1
  fi
  echo "$1 @ $(git -C "$dir" rev-parse --short HEAD)"
}

git_pinned FBInk "$FBINK_REPO" "$FBINK_REV" submodules
git_pinned sonic "$SONIC_REPO" "$SONIC_REV"
git_pinned stb "$STB_REPO" "$STB_REV"

if [ ! -f "$TP/ffmpeg/VERSION" ] || [ "$(cat "$TP/ffmpeg/VERSION")" != "$FFMPEG_VERSION" ]; then
  tarball="$TP/ffmpeg-$FFMPEG_VERSION.tar.xz"
  # ffmpeg.org often times out on the first connection attempt; retry.
  [ -f "$tarball" ] || curl -fL --retry 5 --retry-all-errors --connect-timeout 30 \
    -o "$tarball" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
  echo "$FFMPEG_SHA256  $tarball" | shasum -a 256 -c -
  rm -rf "$TP/ffmpeg" "$TP/ffmpeg-$FFMPEG_VERSION"
  tar -C "$TP" -xf "$tarball"
  mv "$TP/ffmpeg-$FFMPEG_VERSION" "$TP/ffmpeg"
fi
echo "ffmpeg @ $(cat "$TP/ffmpeg/VERSION")"

LITERATA_VERSION=3.103
LITERATA_SHA256=f7fb973cafb26cf785cbebaeaf51c18f87c15a3bcf4d82a7d4857564db5b056d
INTER_VERSION=4.1
INTER_SHA256=9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e

mkdir -p "$TP/fonts"
if [ ! -f "$TP/fonts/Literata-Regular.ttf" ] || [ ! -f "$TP/fonts/Literata-SemiBold.ttf" ]; then
  lzip="$TP/literata-$LITERATA_VERSION.zip"
  [ -f "$lzip" ] || curl -fL --retry 5 --retry-all-errors --connect-timeout 30 \
    -o "$lzip" "https://github.com/googlefonts/literata/releases/download/$LITERATA_VERSION/$LITERATA_VERSION.zip"
  echo "$LITERATA_SHA256  $lzip" | shasum -a 256 -c -
  unzip -q -j -o "$lzip" \
    "fonts/ttf/Literata-Regular.ttf" \
    "fonts/ttf/Literata-SemiBold.ttf" \
    "OFL.txt" -d "$TP/fonts"
  mv "$TP/fonts/OFL.txt" "$TP/fonts/Literata-OFL.txt"
fi
echo "literata @ $LITERATA_VERSION"

if [ ! -f "$TP/fonts/Inter-Regular.ttf" ] || [ ! -f "$TP/fonts/Inter-SemiBold.ttf" ]; then
  izip="$TP/Inter-$INTER_VERSION.zip"
  [ -f "$izip" ] || curl -fL --retry 5 --retry-all-errors --connect-timeout 30 \
    -o "$izip" "https://github.com/rsms/inter/releases/download/v$INTER_VERSION/Inter-$INTER_VERSION.zip"
  echo "$INTER_SHA256  $izip" | shasum -a 256 -c -
  unzip -q -j -o "$izip" \
    "extras/ttf/Inter-Regular.ttf" \
    "extras/ttf/Inter-SemiBold.ttf" \
    "LICENSE.txt" -d "$TP/fonts"
  mv "$TP/fonts/LICENSE.txt" "$TP/fonts/Inter-OFL.txt"
fi
echo "inter @ $INTER_VERSION"


# Noto Naskh Arabic (serif) + Noto Sans Arabic UI: glyph fallback so Arabic titles
# and chapter names render (src/text.c shapes and reorders them). Unhinted TTFs from
# the Noto project's published font tree, pinned by commit and checked by SHA-256.
NOTO_REV=cfa91e36c7240b622fa7fcd3a90eeff15e28ae1c
NOTO_ARABIC_REV=f1ba474be91d6a5d2cfcc4374bb03cc5f1011088
noto_get() { # file url sha256
  if [ ! -f "$TP/fonts/$1" ] || ! echo "$3  $TP/fonts/$1" | shasum -a 256 -c - >/dev/null 2>&1; then
    curl -fL --retry 5 --retry-all-errors --connect-timeout 30 -o "$TP/fonts/$1" "$2"
    echo "$3  $TP/fonts/$1" | shasum -a 256 -c -
  fi
}
NOTO_BASE="https://raw.githubusercontent.com/notofonts/notofonts.github.io/$NOTO_REV/fonts"
noto_get NotoNaskhArabic-Regular.ttf "$NOTO_BASE/NotoNaskhArabic/unhinted/ttf/NotoNaskhArabic-Regular.ttf" \
  c34fdbd98af4dbc45ca192a23d2eeb77032add83086f5fe32957d23b3f36b221
noto_get NotoNaskhArabic-Bold.ttf "$NOTO_BASE/NotoNaskhArabic/unhinted/ttf/NotoNaskhArabic-Bold.ttf" \
  380ac34b249d336e1a43c21053df440594e1e3db8f75fabd4d85b649ca1ea4c6
noto_get NotoSansArabicUI-Regular.ttf "$NOTO_BASE/NotoSansArabicUI/unhinted/ttf/NotoSansArabicUI-Regular.ttf" \
  f4fe1866a64cba1e8b8d08a67cd29070f919edf778ca5de983c1f2a690fc953b
noto_get NotoSansArabicUI-Bold.ttf "$NOTO_BASE/NotoSansArabicUI/unhinted/ttf/NotoSansArabicUI-Bold.ttf" \
  efb903b545a00a557c9d2dd739725157c04bf44215ba31b8ca3a805f099fdc33
noto_get Noto-OFL.txt "https://raw.githubusercontent.com/notofonts/arabic/$NOTO_ARABIC_REV/OFL.txt" \
  a7a5a25eb188bf1cd96982030d53e23c33485c69b1044a562254226857ee13af
echo "noto arabic @ ${NOTO_REV%"${NOTO_REV#???????}"}"
