#!/bin/sh
# Render docs/media/demo.mp4: a 40-second tour of Aloud with sound.
#
# The picture is the real UI drawn by the simulator; the sound is the real playback
# path (decoder, pitch-preserving speed, volume) fed with a public-domain LibriVox
# recording of Pride and Prejudice read by Annie Coleman. Taps are scripted in
# src/demo.c. Run `make showcase` first: the other books on the shelf come from it.
#
# Needs: host ffmpeg, ImageMagick 7, python3, a built simulator.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/demo"
LIB="$OUT/library"
FONTS="$ROOT/third_party/fonts"
SIM="$ROOT/build/host/aloud-sim"
SHOWCASE="$ROOT/build/showcase/library"
FF="ffmpeg -hide_banner -loglevel error -y"

# LibriVox "Pride and Prejudice (version 2)", chapters 1-4, public domain.
LV_URL="https://archive.org/download/solo_pride_librivox/prideandprejudice_01-04_austen_apc_64kb.mp3"
LV_SHA256=74f6dc0c7d5a353c33829dbb4bb39fcab0d0e61e2aa0a7ca9cc039949c4bc3ee
LV_MP3="$ROOT/build/librivox-pride-and-prejudice-01-04.mp3"

[ -x "$SIM" ] || { echo "build the simulator first: make sim" >&2; exit 1; }
[ -d "$SHOWCASE" ] || { echo "run tools/make-showcase.sh first" >&2; exit 1; }

if [ ! -f "$LV_MP3" ]; then
  curl -fL --retry 5 --retry-all-errors --connect-timeout 30 -o "$LV_MP3" "$LV_URL"
fi
if [ -n "$LV_SHA256" ]; then echo "$LV_SHA256  $LV_MP3" | shasum -a 256 -c -; fi

rm -rf "$OUT"
mkdir -p "$LIB" "$OUT/state" "$OUT/tmp"

# The recording as an M4B with chapters at the pauses between them (seconds).
. "$ROOT/tools/showcase-cover.sh"
cover "$OUT/tmp/pp.png" light "Pride and Prejudice" "Jane Austen"
cat > "$OUT/tmp/meta.txt" <<'EOF'
;FFMETADATA1
title=Pride and Prejudice
album=Pride and Prejudice
artist=Jane Austen
album_artist=Jane Austen
comment=LibriVox recording read by Annie Coleman (public domain)

[CHAPTER]
TIMEBASE=1/1000
START=0
END=328200
title=Chapter 1

[CHAPTER]
TIMEBASE=1/1000
START=328200
END=674500
title=Chapter 2

[CHAPTER]
TIMEBASE=1/1000
START=674500
END=1231100
title=Chapter 3

[CHAPTER]
TIMEBASE=1/1000
START=1231100
END=1628000
title=Chapter 4
EOF
$FF -i "$LV_MP3" -i "$OUT/tmp/pp.png" -i "$OUT/tmp/meta.txt" \
  -map 0:a -map 1:v -map_metadata 2 -map_chapters 2 \
  -c:a aac -b:a 64k -c:v png -disposition:v:0 attached_pic "$LIB/Jane Austen - Pride and Prejudice.m4b"

# The rest of the shelf, in the README's language order.
for b in "$SHOWCASE"/*; do ln -s "$b" "$LIB/$(basename "$b")"; done

# Pride and Prejudice on top, resuming just after the LibriVox introduction (the
# player rewinds 2 s on resume, landing on "Chapter 1").
cat > "$OUT/state/positions.tsv" <<'EOF'
# aloud-positions-v1
Jane Austen - Pride and Prejudice.m4b	0	39500	100	1793699999	0
Andy Weir - Project Hail Mary.m4b	0	21300000	100	1793690500	0
朝井リョウ - イン・ザ・メガチャーチ.m4b	0	9100000	100	1793690400	0
Carmen Mola - Los huérfanos	6	540000	100	1793690300	0
刘慈欣 - 三体.m4b	0	25800000	100	1793690200	0
أوراق شمعون المصري.m4b	0	14700000	100	1793690100	0
# end
EOF

"$SIM" --library "$LIB" --state "$OUT/state" --fonts "$FONTS" --demo "$OUT/run" >/dev/null

set -- $(python3 -I "$ROOT/tools/demo-compose.py" "$OUT/run" "$ROOT/tools/demo-frame.sh" "$FONTS/Inter-SemiBold.ttf")
audio_start=$1 end_ms=$2
end_s=$(awk "BEGIN{print $end_ms/1000}")

$FF -f concat -safe 0 -i "$OUT/run/segments.txt" -i "$OUT/run/audio.wav" \
  -filter_complex "[1:a]adelay=${audio_start}|${audio_start},apad,atrim=0:${end_s},afade=t=out:st=$(awk "BEGIN{print $end_s-0.6}"):d=0.6[a]" \
  -map 0:v -map "[a]" -t "$end_s" \
  -c:v libx264 -preset slow -crf 24 -tune stillimage -pix_fmt yuv420p -r 30 \
  -c:a aac -b:a 96k -ac 1 -movflags +faststart "$ROOT/docs/media/demo.mp4"

ls -lh "$ROOT/docs/media/demo.mp4"
echo "Upload docs/media/demo.mp4 through github.com (drag into an issue) and put the user-attachments link in README.md."
