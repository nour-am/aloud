#!/bin/sh
# Aloud on-device performance snapshot (read-only; writes /mnt/us/aloud-perf.txt).
#
# Best run from KUAL while Aloud is playing in the background, ideally with a book open
# in the stock reader: it measures over 30 s how much CPU Aloud, its audio helper and
# the Kindle's own processes use, Aloud's memory, and then benchmarks decoding your most
# recently played audiobook (ARCHITECTURE.md D13/D14, docs/DEVICE-TESTING.md).

OUT=/mnt/us/aloud-perf.txt
ALOUD_DIR=/mnt/us/extensions/aloud
STATE="$ALOUD_DIR/state"
SAMPLE_S=30

exec >"$OUT" 2>&1
echo "== aloud-perf $(date)"
cat /etc/prettyversion.txt 2>/dev/null
grep -E "^(Hardware|model name|processor)" /proc/cpuinfo 2>/dev/null | sort -u
grep -E "^(MemTotal|MemFree|MemAvailable|Buffers|Cached|SwapTotal):" /proc/meminfo

ticks() { # utime+stime of a pid, in clock ticks
  [ -r "/proc/$1/stat" ] && awk '{print $14 + $15}' "/proc/$1/stat" 2>/dev/null || echo 0
}
total_ticks() { awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8; exit}' /proc/stat; }
idle_ticks() { awk '/^cpu /{print $5+$6; exit}' /proc/stat; }

PIDS=""
for name in aloud aloud-sink-armhf aloud-sink-armel gst-launch-1.0 gst-launch-0.10 \
            cvm awesome mixer audiomgrd btd bsa_server; do
  for p in $(pidof "$name" 2>/dev/null); do PIDS="$PIDS $name:$p"; done
done

echo
echo "== Aloud process"
ALOUD_PID=$(pidof aloud 2>/dev/null | awk '{print $1}')
if [ -n "$ALOUD_PID" ]; then
  [ -f /var/tmp/aloud.bg ] && echo "mode: background" || echo "mode: foreground"
  grep -E "^(VmRSS|VmHWM|VmSize|Threads):" "/proc/$ALOUD_PID/status"
  for t in /proc/$ALOUD_PID/task/*; do
    echo "thread $(basename "$t") nice $(awk '{print $19}' "$t/stat")"
  done
else
  echo "aloud is not running (start a book and tap 'Play in background' for the full picture)"
fi

echo
echo "== CPU over ${SAMPLE_S} s (100% = the whole core)"
T0=$(total_ticks); I0=$(idle_ticks)
for e in $PIDS; do eval "B_${e##*:}=$(ticks "${e##*:}")"; done
sleep "$SAMPLE_S"
T1=$(total_ticks); I1=$(idle_ticks)
DT=$((T1 - T0)); [ "$DT" -gt 0 ] || DT=1
echo "system busy: $(( (DT - (I1 - I0)) * 1000 / DT / 10 )).$(( (DT - (I1 - I0)) * 1000 / DT % 10 ))%"
for e in $PIDS; do
  name=${e%%:*}; pid=${e##*:}
  eval "b=\$B_$pid"
  d=$(( $(ticks "$pid") - b ))
  echo "$name ($pid): $(( d * 1000 / DT / 10 )).$(( d * 1000 / DT % 10 ))%"
done

echo
echo "== Decode benchmark (most recently played book)"
LIB=$(cat "$STATE/library_dir.txt" 2>/dev/null)
KEY=$(awk -F'\t' '!/^#/ && NF >= 5 { if ($5 > best) { best = $5; key = $1 } } END { print key }' \
      "$STATE/positions.tsv" 2>/dev/null)
BOOK="$LIB/$KEY"
if [ -n "$KEY" ] && [ -d "$BOOK" ]; then
  BOOK=$(find "$BOOK" -type f \( -iname '*.mp3' -o -iname '*.m4b' -o -iname '*.m4a' \
         -o -iname '*.ogg' -o -iname '*.opus' -o -iname '*.flac' \) | sort | head -n 1)
fi
if [ -n "$KEY" ] && [ -f "$BOOK" ]; then
  # Deliberately no libavformat comparison here: on a long M4B that path needs ~100 MB,
  # which is exactly the memory pressure we must not put on a reading Kindle.
  "$ALOUD_DIR/bin/aloud" --bench "$BOOK" 60
else
  echo "no played book found in $STATE/positions.tsv"
fi
echo
echo "== done $(date)"
