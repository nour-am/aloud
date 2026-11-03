#!/bin/sh
# Aloud launcher for jailbroken Kindles (FW 5.x).
# Modelled on KOReader's "framework kept running" mode so the stock powerd and
# Bluetooth audio daemons stay alive while the stock UI is cleanly paused and
# guaranteed to be restored on exit or crash (ARCHITECTURE.md D9, RESEARCH.md §3).

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

# 2. Single-instance lock.
if [ -f "$PID_FILE" ]; then
  old_pid=$(cat "$PID_FILE" 2>/dev/null)
  if [ -n "$old_pid" ] && kill -0 "$old_pid" 2>/dev/null; then
    exit 0
  fi
  rm -f "$PID_FILE"
fi
echo "$$" > "$PID_FILE"

STOPPED_STATUSBAR=0
STOPPED_AWESOME=0
STOPPED_VOLUMD=0
MOVED_PASSWD=0

cleanup() {
  trap - EXIT INT TERM HUP

  # Ensure screensaver inhibition is never left stuck on.
  if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 >/dev/null 2>&1 || true
  fi

  if [ "$MOVED_PASSWD" -eq 1 ] && [ -e "$PASSWD_BAK" ]; then
    mv -f "$PASSWD_BAK" "$PASSWD_FLAG" 2>/dev/null || true
  fi

  if [ "$STOPPED_VOLUMD" -eq 1 ]; then
    killall -CONT volumd 2>/dev/null || true
  fi

  if [ "$STOPPED_STATUSBAR" -eq 1 ] && command -v start >/dev/null 2>&1; then
    start statusbar >/dev/null 2>&1 || true
  fi

  if [ "$STOPPED_AWESOME" -eq 1 ]; then
    killall -CONT awesome 2>/dev/null || killall -CONT cvm 2>/dev/null || true
  fi

  if [ -f "$FB_DUMP" ] && [ -w /dev/fb0 ]; then
    cat "$FB_DUMP" > /dev/fb0 2>/dev/null || true
    rm -f "$FB_DUMP"
  fi

  if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1 || true
    lipc-set-prop com.lab126.appmgrd start app://com.lab126.booklet.home >/dev/null 2>&1 || true
  fi

  rm -f "$PID_FILE"
}

trap cleanup EXIT INT TERM HUP

# 3. Temporarily stash passcode lock flag so screensaver wake returns to Aloud.
if [ -e "$PASSWD_FLAG" ]; then
  if mv -f "$PASSWD_FLAG" "$PASSWD_BAK" 2>/dev/null; then
    MOVED_PASSWD=1
  fi
fi

# 4. Save framebuffer contents and pause the stock window manager / chrome / USB daemon.
if [ -r /dev/fb0 ]; then
  cat /dev/fb0 > "$FB_DUMP" 2>/dev/null || rm -f "$FB_DUMP"
fi

if command -v lipc-set-prop >/dev/null 2>&1; then
  lipc-set-prop com.lab126.pillow disableEnablePillow disable >/dev/null 2>&1 || \
    lipc-set-prop com.lab126.pillow interrogatePillow '{"pillowId": "default_status_bar", "function": "nativeBridge.hideMe();"}' >/dev/null 2>&1 || true
fi

if killall -STOP awesome 2>/dev/null || killall -STOP cvm 2>/dev/null; then
  STOPPED_AWESOME=1
fi

if [ -f /etc/upstart/statusbar.conf ] && command -v stop >/dev/null 2>&1; then
  if stop statusbar >/dev/null 2>&1; then
    STOPPED_STATUSBAR=1
  fi
fi

if killall -STOP volumd 2>/dev/null; then
  STOPPED_VOLUMD=1
fi

# 5. Run Aloud.
"$ALOUD_BIN" \
  --library "/mnt/us/audiobooks" \
  --state "$ALOUD_DIR/state" \
  --fonts "$ALOUD_DIR/fonts" \
  --helpers "$ALOUD_DIR/bin" \
  "$@" >>"$ALOUD_LOG" 2>&1
