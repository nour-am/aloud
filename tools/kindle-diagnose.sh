#!/bin/sh
# Name: Aloud diagnostics
# Author: Aloud
#
# Read-only fact-finding for Aloud development. Changes nothing on the device.
# Use: copy to the Kindle's documents/ folder and open it from the library
# (scriptlet), or run it from kterm/SSH. Writes aloud-diagnose.txt to the USB root.
# Serial numbers and Bluetooth MAC addresses are masked before writing.

OUT=/mnt/us/aloud-diagnose.txt
[ -d /mnt/us ] || OUT=./aloud-diagnose.txt

if command -v timeout >/dev/null 2>&1; then T="timeout 15"; else T=""; fi

section() { printf '\n===== %s =====\n' "$1"; }
run() { printf '$ %s\n' "$*"; $T "$@" 2>&1; printf '[exit %s]\n' "$?"; }
show() { for f in "$@"; do [ -e "$f" ] && { printf -- '--- %s\n' "$f"; cat "$f" 2>&1; }; done; }
mask_macs() { sed -E 's/([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}/XX:XX:XX:XX:XX:XX/g'; }

{
  section "Device & firmware"
  run date
  run uname -a
  show /etc/prettyversion.txt /etc/version.txt
  if [ -r /proc/usid ]; then
    usid=$(cat /proc/usid)
    printf 'usid prefix (model code only): %s\n' "$(printf '%s' "$usid" | cut -c1-6)"
  fi
  run ls -la /lib/ld-linux.so.3 /lib/ld-linux-armhf.so.3
  run ls /lib/libc-*.so /lib/libc.so.6
  run cat /proc/cpuinfo
  run free

  section "GStreamer"
  run ls -la /usr/lib/libgstreamer-0.10.so.0 /usr/lib/libgstreamer-1.0.so.0
  run ls /usr/lib/gstreamer-0.10 /usr/lib/gstreamer-1.0
  for b in gst-launch gst-launch-0.10 gst-launch-1.0 gst-inspect gst-inspect-0.10 gst-inspect-1.0; do
    p=$(command -v "$b" 2>/dev/null) && printf 'found %s\n' "$p"
  done
  for b in gst-inspect-1.0 gst-inspect-0.10 gst-inspect; do
    command -v "$b" >/dev/null 2>&1 && { run "$b"; run "$b" mixersink; run "$b" fdsrc; break; }
  done

  section "ALSA"
  show /proc/asound/cards /proc/asound/pcm /etc/asound.conf /etc/audio/volume
  run ls -la /dev/snd
  command -v aplay >/dev/null 2>&1 && { run aplay -l; run aplay -L; }
  command -v amixer >/dev/null 2>&1 && run amixer

  section "Audio / Bluetooth daemons"
  ps 2>/dev/null | grep -E 'btd|bsa_server|audiomgr|playermgr|btfd|acsbtfd|btmanagerd|mixer|ivona|awesome|cvm|pillow|statusbar|lipc' | grep -v grep
  run ls /etc/upstart
  run ls -la /dev/shm /var/run/mixer /var/local/zbluetooth

  section "lipc"
  for b in lipc-get-prop lipc-set-prop lipc-wait-event lipc-probe lipc-send-event powerd_test sox mplay aplayer; do
    p=$(command -v "$b" 2>/dev/null) && printf 'found %s\n' "$p"
  done
  for pub in com.lab126.btfd com.lab126.audiomgrd com.lab126.powerd com.lab126.pillow com.lab126.winmgr; do
    run lipc-probe -v "$pub"
  done
  for prop in BTstate BTconnectedDevName ListConnected; do run lipc-get-prop com.lab126.btfd "$prop"; done
  for prop in state status preventScreenSaver; do run lipc-get-prop com.lab126.powerd "$prop"; done
  run lipc-get-prop com.lab126.audiomgrd audioOutputConnected
  command -v powerd_test >/dev/null 2>&1 && run powerd_test -s

  section "Display & input"
  for f in virtual_size bits_per_pixel rotate stride name; do show "/sys/class/graphics/fb0/$f"; done
  show /proc/bus/input/devices
  run ls -la /dev/input /dev/input/by-path

  section "Fonts"
  run ls /usr/java/lib/fonts /mnt/us/fonts
} 2>&1 | mask_macs > "$OUT"

echo "Aloud diagnostics saved to $(basename "$OUT") on the Kindle's USB drive."
