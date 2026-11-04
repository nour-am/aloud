#!/bin/sh
# Aloud KUAL extension launcher for jailbroken Kindles (FW 5.x).
# Touch input is grabbed exclusively via EVIOCGRAB inside the aloud binary and
# automatically released by the kernel when the process exits, so we never need
# to SIGSTOP awesome or volumd (which could leave the Kindle UI frozen if killed).
#
# Background playback (ARCHITECTURE.md D13): this script stays alive next to aloud
# and owns every change to the Kindle UI.
#   aloud  --SIGUSR1--> launcher  "I let go of screen and input": give the Kindle UI back.
#   launcher --SIGUSR2--> aloud   "the screen is yours again" (after another launch).
#
# Usage: aloud.sh [--choose-folder]          open Aloud (or bring it back to the front)
#        aloud.sh --ctl toggle|back|fwd      control background playback from KUAL

# The overrides exist only for tests/test_launcher.sh, which runs this on a host.
ALOUD_DIR="${ALOUD_TEST_DIR:-/mnt/us/extensions/aloud}"
RUN_DIR="${ALOUD_TEST_RUN:-/var/tmp}"
ALOUD_BIN="$ALOUD_DIR/bin/aloud"
ALOUD_LOG="$ALOUD_DIR/state/aloud.log"
PID_FILE="$RUN_DIR/aloud.pid"      # pid of the launcher that owns the running aloud
BG_FLAG="$RUN_DIR/aloud.bg"        # exists while aloud plays in the background
CTL_FIFO="$RUN_DIR/aloud.ctl"      # created by aloud (--control)
FB_DUMP="$RUN_DIR/aloud-fb.dump"
PASSWD_FLAG="${ALOUD_TEST_PASSWD:-/var/local/system/userpasswdenabled}"
PASSWD_BAK="$RUN_DIR/aloud-userpasswdenabled"

mkdir -p "$ALOUD_DIR/state" 2>/dev/null

# Alive and not a zombie (kill -0 succeeds on an exited child nobody has reaped yet).
alive() {
  kill -0 "$1" 2>/dev/null || return 1
  ! grep -q '^[0-9]* ([^)]*) Z' "/proc/$1/stat" 2>/dev/null
}

running_launcher() {
  [ -f "$PID_FILE" ] || return 1
  pid=$(cat "$PID_FILE" 2>/dev/null)
  [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && echo "$pid"
}

# 1. KUAL transport buttons. Only meaningful while aloud runs in the background.
if [ "$1" = "--ctl" ]; then
  cmd="$2"
  if running_launcher >/dev/null && [ -f "$BG_FLAG" ] && [ -p "$CTL_FIFO" ]; then
    # aloud keeps the FIFO open, so this never blocks; the watchdog makes sure a
    # KUAL tap can't hang even if aloud died a moment ago.
    ( echo "$cmd" > "$CTL_FIFO" ) &
    writer=$!
    ( sleep 2; kill "$writer" 2>/dev/null ) &
    wait "$writer" 2>/dev/null
    exit 0
  fi
  [ "$cmd" = "toggle" ] || exit 0  # nothing to skip in when Aloud isn't open
  shift 2                         # Play/Pause with Aloud closed: just open it
fi

# 2. Aloud already playing in the background: bring it to the front, start nothing.
if [ -f "$BG_FLAG" ]; then
  owner=$(running_launcher)
  if [ -n "$owner" ]; then
    kill -USR2 "$owner" 2>/dev/null
    exit 0
  fi
  rm -f "$BG_FLAG"  # stale: its launcher is gone
fi

# 3. Re-exec from a per-launch copy in /var/tmp so no shell fd is held open on the
#    /mnt/us vfat mount (and a second launch never rewrites a script that is running).
if [ -z "$ALOUD_REEXEC" ] && [ -d "$RUN_DIR" ]; then
  tmp_script="$RUN_DIR/aloud-launcher-$$.sh"
  if cp -f "$0" "$tmp_script" 2>/dev/null && chmod +x "$tmp_script" 2>/dev/null; then
    export ALOUD_REEXEC=1
    exec "$tmp_script" "$@"
  fi
fi

# 4. Self-heal any previously stopped stock daemons and wake Bluetooth radio.
killall -CONT awesome cvm volumd 2>/dev/null || true
if command -v lipc-set-prop >/dev/null 2>&1; then
  lipc-set-prop com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1 || true
  lipc-set-prop -s com.lab126.btfd BTenable 0:1 >/dev/null 2>&1 || true
fi

# 5. Single-instance check: an aloud stuck in the foreground is terminated cleanly.
if [ -n "$(running_launcher)" ] || pidof aloud >/dev/null 2>&1; then
  killall -TERM aloud aloud-sink aloud-sink-armhf aloud-sink-armel 2>/dev/null || true
  sleep 1
  killall -KILL aloud aloud-sink aloud-sink-armhf aloud-sink-armel 2>/dev/null || true
fi
echo "$$" > "$PID_FILE"
rm -f "$BG_FLAG"

MODE=fg                 # fg: aloud owns the screen; bg: the Kindle UI does
MOVED_PASSWD=0
STOPPED_STATUSBAR=0
ALOUD_PID=""

# Take the screen from the Kindle UI (launch, and every return from the background).
take_screen() {
  # Stash the passcode lock flag so waking from the screensaver returns to Aloud.
  if [ -e "$PASSWD_FLAG" ] && mv -f "$PASSWD_FLAG" "$PASSWD_BAK" 2>/dev/null; then
    MOVED_PASSWD=1
  fi

  # Save the framebuffer so the Kindle's screen can be put back as it was.
  if [ -r /dev/fb0 ]; then
    cat /dev/fb0 > "$FB_DUMP" 2>/dev/null || rm -f "$FB_DUMP"
  fi

  # Hide the stock status bar ("pillow") while Aloud owns the screen, otherwise its
  # periodic redraws land on top of our headers. Same mechanism as KOReader.
  # FW >= 5.17 draws the bar from a separate upstart job (`statusbar`), which must be
  # stopped as well (RESEARCH.md §3, verified by KOReader on PW4/PW6).
  BAR_HIDDEN=0
  if lipc-set-prop com.lab126.pillow disableEnablePillow disable >/dev/null 2>&1 ||
     lipc-set-prop com.lab126.pillow interrogatePillow \
       '{"pillowId": "default_status_bar", "function": "nativeBridge.hideMe();"}' >/dev/null 2>&1; then
    BAR_HIDDEN=1
  fi
  if [ -f /etc/upstart/statusbar.conf ]; then
    stop statusbar >/dev/null 2>&1 && STOPPED_STATUSBAR=1
    if status statusbar 2>/dev/null | grep -q 'stop/'; then BAR_HIDDEN=1; else BAR_HIDDEN=0; fi
  fi
}

# Give the screen back to the Kindle UI exactly as it was when we took it.
give_back_screen() {
  if [ "$MOVED_PASSWD" -eq 1 ] && [ -e "$PASSWD_BAK" ]; then
    mv -f "$PASSWD_BAK" "$PASSWD_FLAG" 2>/dev/null && MOVED_PASSWD=0
  fi
  if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop com.lab126.pillow disableEnablePillow enable >/dev/null 2>&1 || true
  fi
  if [ "$STOPPED_STATUSBAR" -eq 1 ]; then
    start statusbar >/dev/null 2>&1 || true
    STOPPED_STATUSBAR=0
  fi
  if [ -f "$FB_DUMP" ] && [ -w /dev/fb0 ]; then
    cat "$FB_DUMP" > /dev/fb0 2>/dev/null || true
    rm -f "$FB_DUMP"
  fi
  if command -v eips >/dev/null 2>&1; then
    eips -f -g /dev/null >/dev/null 2>&1 || true
  fi
}

# aloud released screen and input: hand them to the Kindle UI, keep playing.
on_background() {
  [ "$MODE" = "fg" ] || return
  MODE=bg
  give_back_screen
  touch "$BG_FLAG"
  echo "aloud: background $(date)" >>"$ALOUD_LOG"
}

# Another launch asked for Aloud: take the screen, then tell aloud it may draw.
on_foreground() {
  [ "$MODE" = "bg" ] || return
  take_screen
  MODE=fg
  rm -f "$BG_FLAG"
  [ -n "$ALOUD_PID" ] && kill -USR2 "$ALOUD_PID" 2>/dev/null
}

cleanup() {
  # Run to completion: a second signal must not abort us half-way (passcode, screen).
  trap - EXIT
  trap '' INT TERM HUP USR1 USR2

  # Ask aloud to save and quit (it does on SIGTERM), then make sure it's gone.
  if [ -n "$ALOUD_PID" ] && alive "$ALOUD_PID"; then
    kill -TERM "$ALOUD_PID" 2>/dev/null
    i=0
    while alive "$ALOUD_PID" && [ $i -lt 30 ]; do
      usleep 100000 2>/dev/null || sleep 1
      i=$((i + 1))
    done
    kill -KILL "$ALOUD_PID" 2>/dev/null
  fi
  killall -TERM aloud-sink aloud-sink-armhf aloud-sink-armel 2>/dev/null || true
  rm -f /tmp/aloud_audio.fifo "$CTL_FIFO" 2>/dev/null || true

  # Ensure screensaver inhibition and Bluetooth connection hold are never left stuck on.
  if command -v lipc-set-prop >/dev/null 2>&1; then
    lipc-set-prop -i com.lab126.powerd preventScreenSaver 0 >/dev/null 2>&1 || true
    lipc-set-prop -i com.lab126.btfd ensureBTconnection 0 >/dev/null 2>&1 || true
  fi

  # Only repaint if Aloud is on screen: in the background the user is reading, and the
  # saved screen would be stale.
  if [ "$MODE" = "fg" ]; then
    give_back_screen
  fi
  # Belt and braces: whatever happened, the passcode lock must come back.
  if [ -e "$PASSWD_BAK" ] && [ ! -e "$PASSWD_FLAG" ]; then
    mv -f "$PASSWD_BAK" "$PASSWD_FLAG" 2>/dev/null || true
  fi

  killall -CONT awesome cvm volumd 2>/dev/null || true
  rm -f "$PID_FILE" "$BG_FLAG" "$FB_DUMP"
  case "$0" in "$RUN_DIR"/aloud-launcher-*) rm -f "$0" ;; esac
}

trap cleanup EXIT
trap 'cleanup; exit 0' INT TERM HUP
trap on_background USR1
trap on_foreground USR2

take_screen
TOP_INSET_MM=0
[ "$BAR_HIDDEN" -eq 1 ] || TOP_INSET_MM=6
if [ -r "$ALOUD_DIR/state/top_inset_mm.txt" ]; then
  user_inset=$(tr -cd '0-9.' < "$ALOUD_DIR/state/top_inset_mm.txt" 2>/dev/null)
  [ -n "$user_inset" ] && TOP_INSET_MM="$user_inset"
fi

# Helpers run from /mnt/us: a /var/tmp copy failed to start on device (2026-11-03).
HELPER_DIR="$ALOUD_DIR/bin"
echo "aloud: launch $(date) helpers=$HELPER_DIR top_inset_mm=$TOP_INSET_MM" >>"$ALOUD_LOG"

# Run Aloud (loads user's chosen folder from state/library_dir.txt, or prompts on first
# run). In the background so this shell can handle USR1/USR2: a trapped signal makes
# `wait` return early, after which we simply wait again.
"$ALOUD_BIN" \
  --top-inset-mm "$TOP_INSET_MM" \
  --state "$ALOUD_DIR/state" \
  --fonts "$ALOUD_DIR/fonts" \
  --helpers "$HELPER_DIR" \
  --launcher-pid "$$" \
  --control "$CTL_FIFO" \
  "$@" >>"$ALOUD_LOG" 2>&1 &
ALOUD_PID=$!

while kill -0 "$ALOUD_PID" 2>/dev/null; do
  wait "$ALOUD_PID"
done
