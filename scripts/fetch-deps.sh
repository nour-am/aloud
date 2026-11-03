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
