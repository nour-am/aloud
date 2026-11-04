#!/bin/sh
# Generate deterministic audiobook test fixtures using the host ffmpeg.
# No binary media blobs are committed to git (ARCHITECTURE.md D10).
set -eu

OUT=${1:?usage: gen_fixtures.sh <output-dir>}
# Tests mutate state/ (positions, library cache, thumbnails): reset it on every run
# so each `make test` starts from the same state and results never depend on how
# many times the suite ran before. Media fixtures below are generated only once.
rm -rf "$OUT/state"
mkdir -p "$OUT/library" "$OUT/state" "$OUT/tmp"

# Pre-populate a realistic listening position in state/positions.tsv for screenshots.
cat > "$OUT/state/positions.tsv" << 'EOF'
# aloud-positions-v1
Moby-Dick.m4b	0	4140	125	1793690000	0
Octavia E. Butler - Kindred	1	1200	100	1793680000	0
# end
EOF

# Stamp file so repeated `make test` runs reuse generated fixtures instantaneously.
if [ -f "$OUT/.fixtures_v2" ]; then
  exit 0
fi

FFMPEG="ffmpeg -hide_banner -loglevel error -y"

# 1. Generate two distinct procedural cover images (rich geometric gradient + frame
#    so Floyd-Steinberg 16-level dithering is clearly exercised).
$FFMPEG -f lavfi -i "gradients=s=480x480:c0=0x181818:c1=0xe8e8e8:x0=0:y0=0:x1=480:y1=480:n=2,drawbox=x=28:y=28:w=424:h=424:color=white:t=4,drawbox=x=56:y=56:w=368:h=368:color=black:t=2,drawbox=x=140:y=140:w=200:h=200:color=0x303030:t=fill,drawbox=x=180:y=180:w=120:h=120:color=0xd8d8d8:t=fill" \
  -frames:v 1 "$OUT/tmp/cover_moby.png"

$FFMPEG -f lavfi -i "gradients=s=480x480:c0=0xd0d0d0:c1=0x202020:x0=240:y0=0:x1=240:y1=480:n=2,drawbox=x=32:y=32:w=416:h=416:color=black:t=4,drawbox=x=110:y=110:w=260:h=260:color=white:t=6" \
  -frames:v 1 "$OUT/tmp/cover_kindred.png"

# 2. Single-file M4B with 4 embedded chapters, metadata tags, and attached cover art.
cat > "$OUT/tmp/moby_meta.txt" << 'EOF'
;FFMETADATA1
title=Moby-Dick; or, The Whale
album=Moby-Dick
artist=Herman Melville
album_artist=Herman Melville

[CHAPTER]
TIMEBASE=1/1000
START=0
END=3000
title=Chapter 1 — Loomings

[CHAPTER]
TIMEBASE=1/1000
START=3000
END=6000
title=Chapter 2 — The Carpet-Bag

[CHAPTER]
TIMEBASE=1/1000
START=6000
END=9000
title=Chapter 3 — The Spouter-Inn

[CHAPTER]
TIMEBASE=1/1000
START=9000
END=12000
title=Chapter 4 — The Counterpane
EOF

$FFMPEG \
  -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=12" \
  -i "$OUT/tmp/cover_moby.png" \
  -i "$OUT/tmp/moby_meta.txt" \
  -map 0:a -map 1:v -map_metadata 2 -map_chapters 2 \
  -c:a aac -b:a 64k -c:v png -disposition:v:0 attached_pic \
  "$OUT/library/Moby-Dick.m4b"

# 3. Multi-file MP3 chapter folder with natural-order numbers (1, 2, 10) and cover.png.
BOOK_DIR="$OUT/library/Octavia E. Butler - Kindred"
mkdir -p "$BOOK_DIR"
cp "$OUT/tmp/cover_kindred.png" "$BOOK_DIR/cover.png"

$FFMPEG -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=3" \
  -metadata title="Prologue" -metadata album="Kindred" \
  -metadata artist="Octavia E. Butler" -metadata album_artist="Octavia E. Butler" \
  -metadata track="1/3" -c:a libmp3lame -q:a 6 "$BOOK_DIR/01 Prologue.mp3"

$FFMPEG -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=4" \
  -metadata title="The River" -metadata album="Kindred" \
  -metadata artist="Octavia E. Butler" -metadata album_artist="Octavia E. Butler" \
  -metadata track="2/3" -c:a libmp3lame -q:a 6 "$BOOK_DIR/2 The River.mp3"

$FFMPEG -f lavfi -i "sine=frequency=550:sample_rate=44100:duration=3" \
  -metadata title="The Fire" -metadata album="Kindred" \
  -metadata artist="Octavia E. Butler" -metadata album_artist="Octavia E. Butler" \
  -metadata track="10/10" -c:a libmp3lame -q:a 6 "$BOOK_DIR/10 The Fire.mp3"

# 4. Book without cover art (exercises the typographic serif bookplate placeholder).
$FFMPEG -f lavfi -i "sine=frequency=523:sample_rate=44100:duration=6" \
  -metadata title="Ficciones" -metadata album="Ficciones" \
  -metadata artist="Jorge Luis Borges" -metadata album_artist="Jorge Luis Borges" \
  -c:a flac "$OUT/library/Jorge Luis Borges - Ficciones.flac"

# 5. Deliberately truncated/corrupt MP3 file outside library root for fault-tolerance tests.
cp "$BOOK_DIR/01 Prologue.mp3" "$OUT/tmp/corrupt_truncated.mp3"
dd if="$BOOK_DIR/01 Prologue.mp3" of="$OUT/tmp/corrupt_truncated.mp3" bs=1024 count=6 2>/dev/null
printf '\xff\xff\x00\xba\xad\xf0\x0d' >> "$OUT/tmp/corrupt_truncated.mp3"

# 6. MP4 variants for the low-memory reader (src/mp4lite.c): a fragmented file it must
#    hand to libavformat, and a truncated M4B it must play up to the cut.
$FFMPEG -i "$OUT/library/Moby-Dick.m4b" -map 0:a -c copy \
  -movflags frag_keyframe+empty_moov "$OUT/tmp/fragmented.m4a"
SIZE=$(wc -c < "$OUT/library/Moby-Dick.m4b")
dd if="$OUT/library/Moby-Dick.m4b" of="$OUT/tmp/truncated.m4b" bs=1 count=$((SIZE * 2 / 3)) 2>/dev/null

touch "$OUT/.fixtures_v2"
