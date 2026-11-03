#!/bin/sh
# Name: Aloud Audio Test
# Author: Aloud
#
# On-device hardware verification scriptlet (AGENTS.md Roadmap Step 1).
# Plays a 440 Hz tone for 15 seconds through the Kindle audio backend (ALSA on
# Kindle Touch, GStreamer mixersink on Bluetooth Kindles) while listening for
# powerd events and answering readyToSuspend with deferSuspend.
# Writes a detailed log to /mnt/us/aloud-audio-test.txt.

OUT=/mnt/us/aloud-audio-test.txt
[ -d /mnt/us ] || OUT=./aloud-audio-test.txt
BIN_DIR=/mnt/us/extensions/aloud/bin

{
  echo "=== Aloud Audio & Power Test ==="
  date
  uname -a
  if [ -x "$BIN_DIR/aloud" ]; then
    "$BIN_DIR/aloud" --tone-test 15 --helpers "$BIN_DIR"
    echo "[aloud --tone-test exit $?]"
  elif [ -e /lib/ld-linux-armhf.so.3 ] && [ -x "$BIN_DIR/aloud-sink-armhf" ]; then
    "$BIN_DIR/aloud-sink-armhf" --tone 15
    echo "[aloud-sink-armhf exit $?]"
  elif [ -x "$BIN_DIR/aloud-sink-armel" ]; then
    "$BIN_DIR/aloud-sink-armel" --tone 15
    echo "[aloud-sink-armel exit $?]"
  else
    echo "ERROR: Aloud binaries not found in $BIN_DIR"
  fi
} >"$OUT" 2>&1

echo "Aloud audio test complete. Log saved to $(basename "$OUT")."
