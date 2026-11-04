#!/bin/sh
# Host test for package/launcher.sh's background-playback handoff (ARCHITECTURE.md D13).
# Runs the real launcher against a fake `aloud` that records what it is told, then walks
# through: launch -> background -> KUAL control -> second launch (foreground) ->
# background -> exit while in background. Kindle-only commands (lipc-set-prop, eips,
# start/stop) don't exist on the host, which the launcher must tolerate anyway.
set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/aloud-launcher-test.XXXXXX")
trap 'kill $LPID 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/ext/bin" "$T/ext/state" "$T/run"
cp "$ROOT/package/launcher.sh" "$T/ext/aloud.sh"
chmod +x "$T/ext/aloud.sh"
EV="$T/run/events"
: > "$EV"
echo 1 > "$T/passwd"

cat > "$T/ext/bin/aloud" <<'EOF'
#!/bin/bash
LP=; CTL=
while [ $# -gt 0 ]; do
  case "$1" in --launcher-pid) LP=$2; shift ;; --control) CTL=$2; shift ;; esac
  shift
done
EV="$ALOUD_TEST_RUN/events"
trap 'echo fg >>"$EV"' USR2
trap 'echo term >>"$EV"; rm -f "$CTL"; exit 0' TERM
mkfifo "$CTL" && exec 3<>"$CTL"
echo started >>"$EV"
while :; do
  if [ -f "$ALOUD_TEST_RUN/go_bg" ]; then
    rm -f "$ALOUD_TEST_RUN/go_bg"; echo bg >>"$EV"; kill -USR1 "$LP"
  fi
  if [ -f "$ALOUD_TEST_RUN/quit" ]; then echo quit >>"$EV"; rm -f "$CTL"; exit 0; fi
  if read -t 1 line <&3; then echo "cmd $line" >>"$EV"; fi
done
EOF
chmod +x "$T/ext/bin/aloud"

export ALOUD_TEST_DIR="$T/ext" ALOUD_TEST_RUN="$T/run" ALOUD_TEST_PASSWD="$T/passwd"
FAILS=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }
wait_for() { i=0; while ! eval "$1" && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done; eval "$1"; }

sh "$T/ext/aloud.sh" &
LPID=$!
wait_for 'grep -q started "$EV"'
check "aloud started by launcher" 'grep -q started "$EV"'
check "launcher recorded its pid" '[ "$(cat "$T/run/aloud.pid")" = "$LPID" ]'
check "passcode flag stashed while Aloud owns the screen" '[ ! -e "$T/passwd" ]'

touch "$T/run/go_bg"
wait_for '[ -f "$T/run/aloud.bg" ]'
check "background: launcher marks bg" '[ -f "$T/run/aloud.bg" ]'
check "background: passcode lock restored for the reader" '[ -e "$T/passwd" ]'
check "background: launcher still running" 'kill -0 $LPID 2>/dev/null'

sh "$T/ext/aloud.sh" --ctl toggle
wait_for 'grep -q "cmd toggle" "$EV"'
check "KUAL Play/Pause reaches aloud" 'grep -q "cmd toggle" "$EV"'

sh "$T/ext/aloud.sh"
wait_for 'grep -q "^fg" "$EV"'
check "second launch brings aloud to the front" 'grep -q "^fg" "$EV"'
check "second launch did not start another aloud" '[ "$(grep -c started "$EV")" = 1 ]'
check "foreground: bg flag cleared" '[ ! -f "$T/run/aloud.bg" ]'
check "foreground: passcode flag stashed again" '[ ! -e "$T/passwd" ]'

sh "$T/ext/aloud.sh" --ctl back
check "KUAL Back 30 s is ignored in the foreground" '! grep -q "cmd back" "$EV"'

touch "$T/run/go_bg"
wait_for '[ -f "$T/run/aloud.bg" ]'
touch "$T/run/quit"
wait_for '! kill -0 $LPID 2>/dev/null'
check "launcher exits when aloud exits in background" '! kill -0 $LPID 2>/dev/null'
check "exit: pid and bg flag removed" '[ ! -e "$T/run/aloud.pid" ] && [ ! -e "$T/run/aloud.bg" ]'
check "exit: passcode lock present" '[ -e "$T/passwd" ]'
check "exit: per-launch script copy removed" '! ls "$T/run"/aloud-launcher-* >/dev/null 2>&1'

sh "$T/ext/aloud.sh" --ctl fwd
check "KUAL Forward with Aloud closed starts nothing" '[ "$(grep -c started "$EV")" = 1 ]'

# Killing the launcher (e.g. the framework restarting) must still save & stop aloud.
: > "$EV"
rm -f "$T/run/quit" "$T/run/go_bg"
sh "$T/ext/aloud.sh" &
LPID=$!
wait_for 'grep -q started "$EV"'
kill -TERM $LPID
wait_for 'grep -q term "$EV"'
check "SIGTERM to launcher terminates aloud cleanly" 'grep -q term "$EV"'
wait_for '[ -e "$T/passwd" ]'
check "SIGTERM: passcode lock restored" '[ -e "$T/passwd" ]'

if [ $FAILS -ne 0 ]; then
  echo "launcher test: $FAILS failure(s)"
  exit 1
fi
echo "launcher test: all checks passed"
