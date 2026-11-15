#!/bin/sh
# Render the README showcase: a demo library of public-domain audiobooks in eight
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

# ---------------------------------------------------------------------------
# Covers (600x600 grayscale; the app dithers them to 16 levels like e-ink).
# cover <out> <style> <title> <author>
cover() {
  out=$1 style=$2 title=$3 author=$4 fg=white
  base="$OUT/tmp/base.png"
  case $style in
    light) fg=black
           magick -size 600x600 xc:white -fill none -stroke black -strokewidth 6 \
             -draw 'rectangle 24,24 575,575' -strokewidth 2 -draw 'rectangle 40,40 559,559' "$base" ;;
    dark)  magick -size 600x600 xc:black -fill none -stroke white -strokewidth 2 \
             -draw 'rectangle 36,36 563,563' -draw 'line 200,420 400,420' "$base" ;;
    sun)   magick -size 600x600 gradient:'#d8d8d8-#202020' -fill white -stroke none \
             -draw 'circle 300,170 300,95' "$base" ;;
    waves) magick -size 600x600 gradient:'#505050-#000000' \
             \( -size 600x600 gradient: -rotate 90 -function sinusoid 9,0,0.25,0.6 \) \
             -compose multiply -composite "$base" ;;
    rules) magick -size 600x600 gradient:'#3a3a3a-#000000' -fill none -stroke white \
             -strokewidth 3 -draw 'line 60,120 540,120' -draw 'line 60,480 540,480' \
             -strokewidth 1 -draw 'line 60,132 540,132' -draw 'line 60,468 540,468' "$base" ;;
    star)  magick -size 600x600 xc:'#101010' -fill none -stroke '#e8e8e8' -strokewidth 5 \
             -draw 'rectangle 150,150 450,450' \
             -draw 'translate 300,300 rotate 45 rectangle -150,-150 150,150' \
             -strokewidth 2 -draw 'circle 300,300 300,110' -draw 'circle 300,300 300,95' \
             -fill '#e8e8e8' -stroke none -draw 'circle 300,300 300,262' "$base" ;;
  esac
  if [ -n "$title" ]; then
    magick "$base" \
      \( -background none -fill "$fg" -font "$SERIF" -pointsize 64 -size 470x \
         -gravity center caption:"$title" \) -gravity center -geometry +0-10 -composite \
      \( +size -background none -fill "$fg" -font "$SANS" -pointsize 30 label:"$author" \) \
      -gravity south -geometry +0+64 -composite -colorspace gray "$out"
  else
    magick "$base" -colorspace gray "$out"
  fi
}

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

cover "$OUT/tmp/en.png" light "Pride and Prejudice" "Jane Austen"
m4b "Pride and Prejudice.m4b" "Pride and Prejudice" "Jane Austen" "$OUT/tmp/en.png" "$(ch \
  "28${T}Chapter 1" "31${T}Chapter 2" "26${T}Chapter 3" "34${T}Chapter 4" "29${T}Chapter 5" \
  "41${T}Chapter 6" "37${T}Chapter 7" "33${T}Chapter 8" "38${T}Chapter 9" "35${T}Chapter 10" \
  "32${T}Chapter 11" "36${T}Chapter 12")"

cover "$OUT/tmp/fr.png" dark "Les Misérables" "Victor Hugo"
m4b "Les Misérables.m4b" "Les Misérables" "Victor Hugo" "$OUT/tmp/fr.png" "$(ch \
  "42${T}Monsieur Myriel" "38${T}Monsieur Myriel devient monseigneur Bienvenu" \
  "35${T}À bon évêque dur évêché" "47${T}Les œuvres semblables aux paroles" \
  "40${T}Comment monseigneur faisait durer ses soutanes" "44${T}Par qui il faisait garder sa maison" \
  "39${T}Cravatte" "51${T}Philosophie après boire" "36${T}Le frère raconté par la sœur" \
  "48${T}L’évêque en présence d’une lumière inconnue")"

cover "$OUT/tmp/es.png" sun "Don Quijote" "Miguel de Cervantes"
mp3dir "Miguel de Cervantes - Don Quijote" "Don Quijote de la Mancha" "Miguel de Cervantes" "$OUT/tmp/es.png" "$(ch \
  "24${T}Capítulo I — Que trata de la condición del famoso hidalgo" \
  "31${T}Capítulo II — De la primera salida del ingenioso don Quijote" \
  "27${T}Capítulo III — De la graciosa manera de armarse caballero" \
  "29${T}Capítulo IV — De lo que le sucedió al salir de la venta" \
  "22${T}Capítulo V — Donde se prosigue la narración de la desgracia" \
  "33${T}Capítulo VI — Del donoso y grande escrutinio")"

m4b "Franz Kafka - Der Prozeß.m4b" "Der Prozeß" "Franz Kafka" - "$(ch \
  "46${T}Verhaftung" "38${T}Gespräch mit Frau Grubach · Dann Fräulein Bürstner" \
  "52${T}Erste Untersuchung" "41${T}Im leeren Sitzungssaal · Der Student · Die Kanzleien" \
  "18${T}Der Prügler" "49${T}Der Onkel · Leni" "77${T}Advokat · Fabrikant · Maler" \
  "58${T}Kaufmann Block · Kündigung des Advokaten" "44${T}Im Dom" "12${T}Ende")"

cover "$OUT/tmp/ru.png" rules "Анна Каренина" "Лев Толстой"
m4b "Анна Каренина.m4b" "Анна Каренина" "Лев Толстой" "$OUT/tmp/ru.png" "$(ch \
  "54${T}Часть первая" "61${T}Часть вторая" "58${T}Часть третья" "47${T}Часть четвёртая" \
  "63${T}Часть пятая" "56${T}Часть шестая" "49${T}Часть седьмая" "38${T}Часть восьмая")"

cover "$OUT/tmp/el.png" waves "Οδύσσεια" "Όμηρος"
m4b "Οδύσσεια.m4b" "Οδύσσεια" "Όμηρος" "$OUT/tmp/el.png" "$(ch \
  "33${T}Ραψωδία α — Θεών αγορά" "29${T}Ραψωδία β — Ιθακησίων αγορά" \
  "36${T}Ραψωδία γ — Τα εν Πύλω" "48${T}Ραψωδία δ — Τα εν Λακεδαίμονι" \
  "31${T}Ραψωδία ε — Οδυσσέως σχεδία" "22${T}Ραψωδία ζ — Οδυσσέως άφιξις εις Φαίακας")"

cover "$OUT/tmp/ar.png" star "" ""
m4b "كليلة ودمنة.m4b" "كليلة ودمنة" "ابن المقفع" "$OUT/tmp/ar.png" "$(ch \
  "34${T}باب الأسد والثور" "41${T}باب الفحص عن أمر دمنة" "28${T}باب الحمامة المطوقة" \
  "37${T}باب البوم والغربان" "23${T}باب القرد والغيلم" "19${T}باب الناسك وابن عرس")"

cover "$OUT/tmp/pl.png" light "Pan Tadeusz" "Adam Mickiewicz"
m4b "Pan Tadeusz.m4b" "Pan Tadeusz" "Adam Mickiewicz" "$OUT/tmp/pl.png" "$(ch \
  "52${T}Księga pierwsza — Gospodarstwo" "47${T}Księga druga — Zamek" \
  "44${T}Księga trzecia — Umizgi" "58${T}Księga czwarta — Dyplomatyka i łowy" \
  "49${T}Księga piąta — Kłótnia" "38${T}Księga szósta — Zaścianek")"

# ---------------------------------------------------------------------------
# Screens. The simulator shows the most recently played book on its Player and
# Chapters screens, so render once per language with that book on top.
# key<TAB>file_index<TAB>position_ms<TAB>speed<TAB>updated<TAB>finished
BOOKS="en|Pride and Prejudice.m4b|0|8040000
fr|Les Misérables.m4b|0|15300000
es|Miguel de Cervantes - Don Quijote|2|420000
de|Franz Kafka - Der Prozeß.m4b|0|0
ru|Анна Каренина.m4b|0|9500000
el|Οδύσσεια.m4b|0|4200000
ar|كليلة ودمنة.m4b|0|5100000
pl|Pan Tadeusz.m4b|0|2400000"

positions() { # lang-on-top
  echo "# aloud-positions-v1"
  t=1793600000
  echo "$BOOKS" | while IFS='|' read -r lang key idx pos; do
    t=$((t + 100))
    [ "$pos" = 0 ] && [ "$lang" != "$1" ] && continue
    up=$t; [ "$lang" = "$1" ] && up=1793690000
    printf '%s\t%s\t%s\t100\t%s\t0\n' "$key" "$idx" "$pos" "$up"
  done
  echo "# end"
}

for lang in en fr es de ru el ar pl; do
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

# Hero: Library, Player (French), Chapters (Greek), side by side.
device "$S/en/pw5_library.png"  "$F/hero1.png" 560
device "$S/fr/pw5_player.png"   "$F/hero2.png" 560
device "$S/el/pw5_chapters.png" "$F/hero3.png" 560
magick -background none "$F/hero1.png" "$F/hero2.png" "$F/hero3.png" +smush 24 \
  -strip "$MEDIA/hero.png"

# Feature screens used in the README table.
device "$S/ar/pw5_player.png"      "$MEDIA/player-arabic.png" 420
device "$S/ru/pw5_chapters.png"    "$MEDIA/chapters-russian.png" 420
device "$S/es/pw5_sheet_speed.png" "$MEDIA/speed.png" 420
device "$S/de/pw5_sheet_sleep.png" "$MEDIA/sleep-timer.png" 420
device "$S/pl/pw5_folders.png"     "$MEDIA/folders.png" 420
device "$S/en/kt4_player.png"      "$MEDIA/kindle-touch.png" 420
device_landscape "$S/fr/pw5_landscape_player.png" "$MEDIA/landscape.png" 860

# Language tour GIF: the Player in eight languages, with the black-white flash an
# e-ink panel does on a full refresh between books.
G="$OUT/gif"
mkdir -p "$G"
set --
for lang in en fr es de pl ru el ar; do
  device "$S/$lang/pw5_player.png" "$G/$lang.png" 380 flat
  magick "$S/$lang/pw5_player.png" -negate "$G/$lang-neg.png"
  device "$G/$lang-neg.png" "$G/$lang-flash.png" 380 flat
  set -- "$@" -delay 12 "$G/$lang-flash.png" -delay 190 "$G/$lang.png"
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
