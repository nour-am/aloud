#!/bin/sh
# Sourced by the showcase scripts: cover <out.png> <style> <title> <author> [title-font] [author-font]
# draws a 600x600 grayscale audiobook cover (the app dithers it like e-ink).
# Expects ROOT and OUT; uses the fonts fetched into third_party/fonts.
SERIF=${SERIF:-$ROOT/third_party/fonts/Literata-SemiBold.ttf}
SANS=${SANS:-$ROOT/third_party/fonts/Inter-Regular.ttf}

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
  tfont=${5:-$SERIF} afont=${6:-$SANS}
  if [ -n "$title" ]; then
    magick "$base" \
      \( -background none -fill "$fg" -font "$tfont" -pointsize 64 -size 470x \
         -gravity center caption:"$title" \) -gravity center -geometry +0-10 -composite \
      \( +size -background none -fill "$fg" -font "$afont" -pointsize 30 label:"$author" \) \
      -gravity south -geometry +0+64 -composite -colorspace gray "$out"
  else
    magick "$base" -colorspace gray "$out"
  fi
}

