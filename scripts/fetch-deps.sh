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

mkdir -p "$TP/fonts" "$TP/fonts-full"

# fonttools (pinned) trims fonts to what Aloud draws; it runs in its own venv.
FONTTOOLS_VERSION=4.60.1
pyftsubset() {
  venv="$TP/fonttools-venv"
  if [ ! -x "$venv/bin/pyftsubset" ]; then
    python3 -m venv "$venv"
    "$venv/bin/pip" install -q "fonttools==$FONTTOOLS_VERSION"
  fi
  "$venv/bin/pyftsubset" "$@"
}

# --- Base fonts (shipped in the main zip): Literata (serif) + Inter (sans). -------------
if [ ! -f "$TP/fonts-full/Literata-Regular.ttf" ] || [ ! -f "$TP/fonts-full/Literata-SemiBold.ttf" ]; then
  lzip="$TP/literata-$LITERATA_VERSION.zip"
  [ -f "$lzip" ] || curl -fL --retry 5 --retry-all-errors --connect-timeout 30 \
    -o "$lzip" "https://github.com/googlefonts/literata/releases/download/$LITERATA_VERSION/$LITERATA_VERSION.zip"
  echo "$LITERATA_SHA256  $lzip" | shasum -a 256 -c -
  unzip -q -j -o "$lzip" "fonts/ttf/Literata-Regular.ttf" "fonts/ttf/Literata-SemiBold.ttf" \
    "OFL.txt" -d "$TP/fonts-full"
  mv "$TP/fonts-full/OFL.txt" "$TP/fonts-full/Literata-OFL.txt"
fi
echo "literata @ $LITERATA_VERSION"

if [ ! -f "$TP/fonts-full/Inter-Regular.ttf" ] || [ ! -f "$TP/fonts-full/Inter-SemiBold.ttf" ]; then
  izip="$TP/Inter-$INTER_VERSION.zip"
  [ -f "$izip" ] || curl -fL --retry 5 --retry-all-errors --connect-timeout 30 \
    -o "$izip" "https://github.com/rsms/inter/releases/download/v$INTER_VERSION/Inter-$INTER_VERSION.zip"
  echo "$INTER_SHA256  $izip" | shasum -a 256 -c -
  unzip -q -j -o "$izip" "extras/ttf/Inter-Regular.ttf" "extras/ttf/Inter-SemiBold.ttf" \
    "LICENSE.txt" -d "$TP/fonts-full"
  mv "$TP/fonts-full/LICENSE.txt" "$TP/fonts-full/Inter-OFL.txt"
fi
echo "inter @ $INTER_VERSION"

# Trim to Latin (incl. Vietnamese), Greek, Cyrillic and the punctuation/symbols the UI
# uses, without hinting (stb_truetype ignores it) but keeping kerning: 1.48 MB -> 0.58 MB,
# which is also heap the app no longer holds. Outlines are untouched.
LATIN_UNICODES="U+0000-036F,U+0370-03FF,U+0400-052F,U+1E00-1EFF,U+1F00-1FFF,U+2000-206F,U+20A0-20CF,U+2100-214F,U+2190-21FF,U+2200-22FF,U+25A0-25FF,U+FFFD"
for f in Literata-Regular Literata-SemiBold Inter-Regular Inter-SemiBold; do
  if [ ! -f "$TP/fonts/$f.ttf" ] || [ "$TP/fonts-full/$f.ttf" -nt "$TP/fonts/$f.ttf" ] \
     || [ "$(wc -c < "$TP/fonts/$f.ttf")" -gt 250000 ]; then
    pyftsubset "$TP/fonts-full/$f.ttf" --unicodes="$LATIN_UNICODES" --layout-features=kern \
      --no-hinting --name-IDs='*' --notdef-outline --output-file="$TP/fonts/$f.ttf"
  fi
done
cp "$TP/fonts-full/Literata-OFL.txt" "$TP/fonts-full/Inter-OFL.txt" "$TP/fonts/"
echo "base fonts trimmed (fonttools $FONTTOOLS_VERSION)"

noto_get() { # file url sha256
  if [ ! -f "$TP/fonts/$1" ] || ! echo "$3  $TP/fonts/$1" | shasum -a 256 -c - >/dev/null 2>&1; then
    curl -fL --retry 5 --retry-all-errors --connect-timeout 30 -o "$TP/fonts/$1" "$2"
    echo "$3  $TP/fonts/$1" | shasum -a 256 -c -
  fi
}

# --- Optional font packs (separate downloads; see `make fontpacks`). -------------------
# Arabic: Noto Naskh Arabic (serif) + Noto Sans Arabic UI. src/text.c shapes and
# reorders Arabic; these only supply glyphs. Unhinted TTFs, pinned and hash-checked.
NOTO_REV=cfa91e36c7240b622fa7fcd3a90eeff15e28ae1c
NOTO_ARABIC_REV=f1ba474be91d6a5d2cfcc4374bb03cc5f1011088
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

# Chinese + Japanese: Noto Sans CJK SC (Regular + Bold), 16 MB each upstream, trimmed to
# everyday characters (scripts/cjk-charset.py): 2.5 MB each. Aloud memory-maps them.
NOTO_CJK_REV=f8d157532fbfaeda587e826d4cd5b21a49186f7c
cjk_out() { # weight src_sha256 subset_sha256
  out="$TP/fonts/NotoSansCJK-$1.otf"
  if [ -f "$out" ] && echo "$3  $out" | shasum -a 256 -c - >/dev/null 2>&1; then return; fi
  src="$TP/NotoSansCJKsc-$1.otf"
  if [ ! -f "$src" ] || ! echo "$2  $src" | shasum -a 256 -c - >/dev/null 2>&1; then
    curl -fL --retry 5 --retry-all-errors --connect-timeout 30 -o "$src" \
      "https://raw.githubusercontent.com/notofonts/noto-cjk/$NOTO_CJK_REV/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-$1.otf"
    echo "$2  $src" | shasum -a 256 -c -
  fi
  python3 -I "$ROOT/scripts/cjk-charset.py" > "$TP/cjk-unicodes.txt" 2>/dev/null
  pyftsubset "$src" --unicodes-file="$TP/cjk-unicodes.txt" \
    --layout-features='' --no-hinting --desubroutinize --name-IDs='*' --notdef-outline \
    --output-file="$out"
  echo "$3  $out" | shasum -a 256 -c - || echo "warning: $out differs from the reference build" >&2
}
cjk_out Regular 2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b \
  65b0b8005681cb5595a5b1c4da0ff95426e0ed352ac15078d282ace7dd1e922b
cjk_out Bold b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a \
  73f617b1e2dac4c3d6d61414e6ff5624d8a44278408812fb46a19c999a43e098
noto_get NotoSansCJK-OFL.txt "https://raw.githubusercontent.com/notofonts/noto-cjk/$NOTO_CJK_REV/Sans/LICENSE" \
  6a73f9541c2de74158c0e7cf6b0a58ef774f5a780bf191f2d7ec9cc53efe2bf2
echo "noto cjk @ ${NOTO_CJK_REV%"${NOTO_CJK_REV#???????}"}"
