#!/bin/sh
# Render the README showcase: a demo library of popular audiobooks in five
# languages, drawn by the real UI code (build/host/aloud-sim), then framed in a
# Kindle-style bezel and assembled into docs/media/*.png + an animated GIF.
#
# Needs: host ffmpeg, ImageMagick 7 (`magick`), a built simulator (`make sim`).
# The audio is silence at 8 kHz/8 kbps, so a 10-hour book costs ~2 MB and seconds.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT="$ROOT/build/showcase"
LIB="$OUT/library"
MEDIA="$ROOT/docs/media"
FONTS="$ROOT/third_party/fonts"
SIM="$ROOT/build/host/aloud-sim"
FF="ffmpeg -hide_banner -loglevel error -y"
SERIF="$FONTS/Literata-SemiBold.ttf"
SANS="$FONTS/Inter-Regular.ttf"

[ -x "$SIM" ] || { echo "build the simulator first: make sim" >&2; exit 1; }
command -v magick >/dev/null || { echo "ImageMagick 7 (magick) is required" >&2; exit 1; }

rm -rf "$OUT"
mkdir -p "$LIB" "$OUT/tmp" "$OUT/screens" "$MEDIA"

. "$ROOT/tools/showcase-cover.sh"

# ---------------------------------------------------------------------------
# Books. Chapter list file: one "minutes<TAB>title" per line.
SILENCE="$OUT/tmp/silence.m4a"
$FF -f lavfi -i anullsrc=r=8000:cl=mono -t 54000 -c:a aac -b:a 8k "$SILENCE"

# m4b <file> <title> <author> <cover.png or -> <chapters-file>
m4b() {
  file=$1 title=$2 author=$3 art=$4 chapters=$5
  meta="$OUT/tmp/meta.txt"
  printf ';FFMETADATA1\ntitle=%s\nalbum=%s\nartist=%s\nalbum_artist=%s\n' \
    "$title" "$title" "$author" "$author" > "$meta"
  start=0
  while IFS='	' read -r min name; do
    end=$((start + min * 60000))
    printf '\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=%s\n' "$start" "$end" "$name" >> "$meta"
    start=$end
  done < "$chapters"
  secs=$((start / 1000))
  if [ "$art" = - ]; then
    $FF -t "$secs" -i "$SILENCE" -i "$meta" -map 0:a -map_metadata 1 -map_chapters 1 \
      -c copy "$LIB/$file"
  else
    $FF -t "$secs" -i "$SILENCE" -i "$art" -i "$meta" -map 0:a -map 1:v \
      -map_metadata 2 -map_chapters 2 -c:a copy -c:v png -disposition:v:0 attached_pic \
      "$LIB/$file"
  fi
}

# mp3dir <folder> <album> <author> <cover.png> <chapters-file>
mp3dir() {
  dir="$LIB/$1" album=$2 author=$3 art=$4 chapters=$5
  mkdir -p "$dir"
  magick "$art" "$dir/cover.jpg"
  n=0 total=$(wc -l < "$chapters" | tr -d ' ')
  while IFS='	' read -r min name; do
    n=$((n + 1))
    $FF -f lavfi -i anullsrc=r=8000:cl=mono -t $((min * 60)) -c:a libmp3lame -b:a 8k \
      -metadata title="$name" -metadata album="$album" -metadata artist="$author" \
      -metadata album_artist="$author" -metadata track="$n/$total" \
      "$dir/$(printf '%02d' $n) $name.mp3" < /dev/null
  done < "$chapters"
}

ch() { printf '%s\n' "$@" > "$OUT/tmp/ch.txt"; echo "$OUT/tmp/ch.txt"; }
T='	'

CJK="$FONTS/NotoSansCJK-Bold.otf"
CJKR="$FONTS/NotoSansCJK-Regular.otf"

# numbered <count> <minutes> <label-command>: chapter list via a label function.
numbered() {
  n=$1 min=$2 fn=$3 i=0
  : > "$OUT/tmp/ch.txt"
  while [ $i -lt "$n" ]; do
    i=$((i + 1))
    printf '%s\t%s\n' $((min + (i * 7) % 11 - 5)) "$($fn $i)" >> "$OUT/tmp/ch.txt"
  done
  echo "$OUT/tmp/ch.txt"
}
en_ch() { echo "Chapter $1"; }
es_ch() { echo "Capítulo $1"; }
cjk_num() { # 1..40 in kanji numerals
  set -- "$1" 一 二 三 四 五 六 七 八 九 十
  n=$1; shift
  t=$((n / 10)) o=$((n % 10)) s=""
  if [ $t -gt 1 ]; then eval "s=\${$t}"; fi
  [ $t -ge 1 ] && s="$s十"
  if [ $o -gt 0 ]; then eval "s=\$s\${$o}"; fi
  echo "$s"
}
ja_ch() { echo "第$(cjk_num "$1")章"; }
zh_ch() { echo "第$(cjk_num "$1")章"; }
ar_ch() {
  set -- "$1" الأول الثاني الثالث الرابع الخامس السادس السابع الثامن التاسع العاشر \
    "الحادي عشر" "الثاني عشر" "الثالث عشر" "الرابع عشر" "الخامس عشر" "السادس عشر" \
    "السابع عشر" "الثامن عشر" "التاسع عشر" العشرون
  n=$1; shift
  eval "echo \"الفصل \${$n}\""
}

# Popular titles across the five languages (generated covers; the real artwork is
# the publishers'). Chapter names are generic.
cover "$OUT/tmp/en.png" dark "Project Hail Mary" "Andy Weir"
m4b "Andy Weir - Project Hail Mary.m4b" "Project Hail Mary" "Andy Weir" "$OUT/tmp/en.png" \
  "$(numbered 30 32 en_ch)"

cover "$OUT/tmp/ja.png" light "イン・ザ・メガチャーチ" "朝井リョウ" "$CJK" "$CJKR"
m4b "朝井リョウ - イン・ザ・メガチャーチ.m4b" "イン・ザ・メガチャーチ" "朝井リョウ" "$OUT/tmp/ja.png" \
  "$(numbered 14 48 ja_ch)"

cover "$OUT/tmp/es.png" sun "Los huérfanos" "Carmen Mola"
mp3dir "Carmen Mola - Los huérfanos" "Los huérfanos" "Carmen Mola" "$OUT/tmp/es.png" \
  "$(numbered 18 27 es_ch)"

cover "$OUT/tmp/zh.png" rules "三体" "刘慈欣" "$CJK" "$CJKR"
m4b "刘慈欣 - 三体.m4b" "三体" "刘慈欣" "$OUT/tmp/zh.png" "$(numbered 36 22 zh_ch)"

cover "$OUT/tmp/ar.png" star "" ""
m4b "أوراق شمعون المصري.m4b" "أوراق شمعون المصري" "أسامة عبد الرءوف الشاذلي" "$OUT/tmp/ar.png" \
  "$(numbered 20 34 ar_ch)"

# ---------------------------------------------------------------------------
# Screens. The simulator shows the most recently played book on its Player and
# Chapters screens, so render once per language with that book on top.
# The library lists books most recent first, so timestamps keep EN, JA, ES, ZH, AR.
LANGS="en ja es zh ar"
BOOKS="en|Andy Weir - Project Hail Mary.m4b|0|21300000|1793690500
ja|朝井リョウ - イン・ザ・メガチャーチ.m4b|0|9100000|1793690400
es|Carmen Mola - Los huérfanos|6|540000|1793690300
zh|刘慈欣 - 三体.m4b|0|25800000|1793690200
ar|أوراق شمعون المصري.m4b|0|14700000|1793690100"

positions() { # lang-on-top
  echo "# aloud-positions-v1"
  echo "$BOOKS" | while IFS='|' read -r lang key idx pos up; do
    [ "$lang" = "$1" ] && up=1793699999
    printf '%s\t%s\t%s\t100\t%s\t0\n' "$key" "$idx" "$pos" "$up"
  done
  echo "# end"
}

for lang in library $LANGS; do
  st="$OUT/state-$lang"
  mkdir -p "$st"
  positions "$lang" > "$st/positions.tsv"
  "$SIM" --library "$LIB" --state "$st" --fonts "$FONTS" --export-screens "$OUT/screens/$lang" >/dev/null
done
echo "showcase screens: $OUT/screens"

# ---------------------------------------------------------------------------
# Device frames. device <screen.png> <out.png> <width>: a Paperwhite-like bezel
# (thicker chin), rounded corners and a soft shadow, scaled to <width> px.
device() { # <screen.png> <out.png> <width> [flat]
  shadow='( +clone -background black -shadow 40x28+0+18 ) +swap'
  [ "${4:-}" = flat ] && shadow=''
  # shellcheck disable=SC2086  # $shadow is a deliberate word list
  magick "$1" -resize 1236x1648\! \
    -bordercolor '#1c1c1e' -border 80 -gravity south -background '#1c1c1e' -splice 0x70 \
    \( +clone -alpha extract -fill black -colorize 100 -fill white \
       -draw "roundrectangle 0,0 %[fx:w-1],%[fx:h-1] 100,100" \) \
    -alpha off -compose copyopacity -composite \
    $shadow -background none \
    -compose over -layers merge +repage -resize "$3"x "$2"
}
device_landscape() {
  magick "$1" -bordercolor '#1c1c1e' -border 80 -gravity east -background '#1c1c1e' -splice 70x0 \
    \( +clone -alpha extract -fill black -colorize 100 -fill white \
       -draw "roundrectangle 0,0 %[fx:w-1],%[fx:h-1] 100,100" \) \
    -alpha off -compose copyopacity -composite \
    \( +clone -background black -shadow 40x28+0+18 \) +swap -background none \
    -compose over -layers merge +repage -resize "$3"x "$2"
}

S="$OUT/screens"
F="$OUT/frames"
mkdir -p "$F"

# Hero: Library (EN, JA, ES, ZH in order), the Player in English, Chapters in Japanese.
rm -f "$MEDIA"/*.png "$MEDIA"/*.gif
device "$S/library/pw5_library.png" "$F/hero1.png" 560
device "$S/en/pw5_player.png"       "$F/hero2.png" 560
device "$S/ja/pw5_chapters.png"     "$F/hero3.png" 560
magick -background none "$F/hero1.png" "$F/hero2.png" "$F/hero3.png" +smush 24 \
  -strip "$MEDIA/hero.png"

# README screens, one language each, in order.
device "$S/ja/pw5_player.png"      "$MEDIA/player-japanese.png" 420
device "$S/es/pw5_chapters.png"    "$MEDIA/chapters-spanish.png" 420
device "$S/zh/pw5_sheet_speed.png" "$MEDIA/speed-chinese.png" 420
device "$S/ar/pw5_sheet_sleep.png" "$MEDIA/sleep-arabic.png" 420
device_landscape "$S/en/pw5_landscape_player.png" "$MEDIA/landscape.png" 860
device "$S/zh/kt4_player.png"      "$MEDIA/kindle-touch.png" 420

# Language tour GIF: the Player in each language, with the black-white flash an
# e-ink panel does on a full refresh between books.
G="$OUT/gif"
mkdir -p "$G"
set --
for lang in $LANGS; do
  device "$S/$lang/pw5_player.png" "$G/$lang.png" 380 flat
  magick "$S/$lang/pw5_player.png" -negate "$G/$lang-neg.png"
  device "$G/$lang-neg.png" "$G/$lang-flash.png" 380 flat
  set -- "$@" -delay 12 "$G/$lang-flash.png" -delay 200 "$G/$lang.png"
done
magick -dispose background "$@" -channel A -threshold 50% +channel -loop 0 -layers optimize -colors 64 "$MEDIA/languages.gif"

ls -lh "$MEDIA"

# Logo: an e-ink-black tile with a serif "A" and three sound arcs.
magick -size 512x512 xc:none -fill '#111111' -draw 'roundrectangle 0,0 511,511 112,112' \
  -fill white -font "$SERIF" -pointsize 300 -gravity center -annotate -64+4 'A' \
  -fill none -stroke white -strokewidth 22 \
  -draw 'stroke-linecap round arc 292,196 372,316 -55,55' \
  -draw 'stroke-linecap round arc 272,156 412,356 -55,55' \
  -draw 'stroke-linecap round arc 252,116 452,396 -55,55' \
  -resize 256x256 -strip "$MEDIA/logo.png"
