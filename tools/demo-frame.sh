#!/bin/sh
# demo-frame.sh <screen.png> <out.png> <plain|flash|tap:X:Y> <caption> <font.ttf>
# One video frame: the Kindle screen in a Paperwhite-style bezel, centred on a
# light background at 960x1380 with a caption underneath. "flash" inverts the panel (e-ink full refresh);
# "tap:X:Y" draws a touch marker at screen coordinates X,Y.
set -eu
src=$1 out=$2 variant=$3 caption=$4 font=$5
tmp="$out.screen.png"
case $variant in
  flash) magick "$src" -negate "$tmp" ;;
  tap:*)
    xy=${variant#tap:}; x=${xy%%:*}; y=${xy#*:}
    magick "$src" -fill 'rgba(0,0,0,0.28)' -stroke 'rgba(0,0,0,0.75)' -strokewidth 6 \
      -draw "circle $x,$y $((x + 58)),$y" "$tmp" ;;
  *) cp "$src" "$tmp" ;;
esac
magick "$tmp" -resize 1236x1648\! \
  -bordercolor '#1c1c1e' -border 80 -gravity south -background '#1c1c1e' -splice 0x70 \
  \( +clone -alpha extract -fill black -colorize 100 -fill white \
     -draw "roundrectangle 0,0 %[fx:w-1],%[fx:h-1] 100,100" \) \
  -alpha off -compose copyopacity -composite -resize x1210 -background '#ececec' -gravity north -splice 0x36 \
  -background '#ececec' -gravity north -compose over -extent 960x1380 \
  -alpha remove -alpha off \
  -font "$font" -pointsize 40 -fill '#1c1c1e' -gravity south -annotate +0+56 "$caption" "$out"
rm -f "$tmp"
