#!/bin/sh
# Aloud KUAL extension launcher for jailbroken Kindles (FW 5.x).
# Touch input is grabbed exclusively via EVIOCGRAB inside the aloud binary and
# automatically released by the kernel when the process exits, so we never need
# to SIGSTOP awesome or volumd (which could leave the Kindle UI frozen if killed).

ALOUD_DIR="/mnt/us/extensions/aloud"
ALOUD_BIN="$ALOUD_DIR/bin/aloud"
ALOUD_LOG="/mnt/us/extensions/aloud/state/aloud.log"
TMP_SCRIPT="/var/tmp/aloud-launcher.sh"
PID_FILE="/var/tmp/aloud.pid"
FB_DUMP="/var/tmp/aloud-fb.dump"
PASSWD_FLAG="/var/local/system/userpasswdenabled"
PASSWD_BAK="/var/tmp/aloud-userpasswdenabled"

mkdir -p "$ALOUD_DIR/state" 2>/dev/null

# 1. Re-exec from /var/tmp so no shell fd is held open on the /mnt/us vfat mount.
if [ "$0" != "$TMP_SCRIPT" ] && [ -d /var/tmp ]; then
  cp -f "$0" "$TMP_SCRIPT" 2>/dev/null && chmod +x "$TMP_SCRIPT" 2>/dev/null && exec "$TMP_SCRIPT" "$@"
fi

# 2. Self-heal any previously stopped stock daemons and wake Bluetooth radio.
killall -CONT awesome cvm volumd 2>/dev/null || true
if command -v lipc-set-prop >/dev/null 2>&1; then
  lipc-set-prop com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1 || true
  lipc-set-prop -s com.lab126.btfd BTenable 0:1 >/dev/null 2>&1 || true
fi

# 3. Single-instance check: if an older aloud is stuck, terminate it cleanly.
if [ -f "$PID_FILE" ]; then
  old_pid=$(cat "$PID_FILE" 2>/dev/null)
  if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
    killall -TERM aloud aloud-sink aloud-sink-armhf aloud-sink-armel 2>/dev/null || true
    sleep 1
    killall -KILL aloud aloud-sink aloud-sink-armhf aloud-sink-armel 2>/dev/null || true
  fi
  rm -f "$PID_FILE"
fi
echo "$$" > "$PID_FILE"

MOVED_PASSWD=0

cleanup() {
  trap - EXIT INT TERM HUP

  killall -TERM aloud-sink aloud-sink-armhf aloud-sink-armel 2>/dev/null || true
  rm -f /tmp/aloud_audio.fifo 2>/dev/null || true

  # Ensure screensaver inhibition and Bluetooth connection hold are never left stuck on.
  if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 >/dev/null 2>&1 || true
    lipc-set-prop -i com.lab126.btfd ensureBTconnection 0 >/dev/null 2>&1 || true
    lipc-set-prop com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1 || true
  fi

  if [ "$MOVED_PASSWD" -eq 1 ] && [ -e "$PASSWD_BAK" ]; then
    mv -f "$PASSWD_BAK" "$PASSWD_FLAG" 2>/dev/null || true
  fi

  killall -CONT awesome cvm volumd 2>/dev/null || true

  if [ -f "$FB_DUMP" ] && [ -w /dev/fb0 ]; then
    cat "$FB_DUMP" > /dev/fb0 2>/dev/null || true
    rm -f "$FB_DUMP"
  fi

  if command -v eips >/dev/null 2>&1; then
    eips -f -g /dev/null >/dev/null 2>&1 || true
  fi

  rm -f "$PID_FILE"
}

trap cleanup EXIT INT TERM HUP

# 4. Temporarily stash passcode lock flag so screensaver wake returns to Aloud.
if [ -e "$PASSWD_FLAG" ]; then
  if mv -f "$PASSWD_FLAG" "$PASSWD_BAK" 2>/dev/null; then
    MOVED_PASSWD=1
  fi
fi

# 5. Save framebuffer contents so we can restore the KUAL / Home view on exit.
if [ -r /dev/fb0 ]; then
  cat /dev/fb0 > "$FB_DUMP" 2>/dev/null || rm -f "$FB_DUMP"
fi

# 6. Run Aloud (loads user's chosen folder from state/library_dir.txt, or prompts on first run).
"$ALOUD_BIN" \
  --state "$ALOUD_DIR/state" \
  --fonts "$ALOUD_DIR/fonts" \
  --helpers "$ALOUD_DIR/bin" \
  "$@" >>"$ALOUD_LOG" 2>&1

