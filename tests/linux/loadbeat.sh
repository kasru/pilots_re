#!/bin/sh
# The load indicator: a scene loads inside the game's own loop - seconds of
# reading with nothing drawn, which reads as a hang (and was reported as
# one). The port cannot wrap that loop, but every game read goes through the
# shim (_read -> SdlBackendLoadBeat), so a still screen WITH bytes moving is
# the only evidence there is: the spinner is drawn over the last frame the
# game blitted until it blits again, and what the wait cost gets said once.
#
# PILOTS_LOAD_QUIET_MS=50 makes the trigger deterministic for the harness.
# The default 1000 ms is what measures a real load on screen (a resting
# screen is still too and simply does not read); here the mechanism itself
# is measured: after the click into scene 1 the count of spinner starts must
# grow - that transition is a load by observation - and a summary must be
# said once the scene draws again. Movie reads are not in the count on
# purpose: avformat reads through glibc, not through the shim's _read, and
# during a movie the game reads nothing at all, so the still screen alone
# never starts the spinner.
#
#   sh tests/linux/loadbeat.sh
# Needs: a built $BUILDDIR/PILOT_RE, SRC/ with the original game data and
# tests/linux/x11ctl.py as the X driver.
case "$0" in
  */*) cd "$(dirname "$0")/../.." || exit 1 ;;
esac
# Same reasoning as fullscreen.sh: the host has no i386 runtime, so the default is
# the native 64-bit build; PILOTS_BUILDDIR overrides it for a binary that runs here.
BUILDDIR=${PILOTS_BUILDDIR:-build64}
DISP=${PILOTS_HSDISPLAY:-:97}
SW=1024; SH=768
LOG=$(pwd)/$BUILDDIR/PILOT_RE_debug.log
X="python3 tests/linux/x11ctl.py"

[ -x "$BUILDDIR/PILOT_RE" ] || { echo "no $BUILDDIR/PILOT_RE - build it first"; exit 1; }

# A dying Xvfb left behind by an earlier harness makes pgrep lie and the next
# game silently fall back to SDL's offscreen driver - no window, no clicks.
# So the display is checked for liveness, not just for a matching process.
pgrep -f "Xvfb $DISP" >/dev/null 2>&1 && DISPLAY=$DISP xdpyinfo >/dev/null 2>&1 || {
  pkill -f "Xvfb $DISP" 2>/dev/null
  sleep 1
  Xvfb "$DISP" -screen 0 "${SW}x${SH}x24" >/dev/null 2>&1 &
  sleep 2
}
export DISPLAY=$DISP
echo "== display $DISP ${SW}x${SH} ($BUILDDIR)"

pkill -9 PILOT_RE 2>/dev/null
sleep 1
: > "$LOG"

FAILED=""
check() {  # check <shell status: 0 = ok> <name>
  if [ "$1" = 0 ]; then printf '%-52s PASS\n' "$2"
  else printf '%-52s FAIL\n' "$2"; FAILED="$FAILED|$2"; fi
}
have() { grep -q -F -- "$1" "$LOG" 2>/dev/null; }   # -F: patterns start with '-'
wait_log() {                                       # wait_log <pattern> <seconds>
  n=$(( $2 * 5 ))
  while [ "$n" -gt 0 ]; do
    have "$1" && return 0
    sleep 0.2; n=$((n - 1))
  done
  return 1
}
wait_log_n() {                                     # wait_log_n <pattern> <count> <seconds>
  n=$(( $3 * 5 ))
  while [ "$n" -gt 0 ]; do
    [ "$(grep -c -F -- "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0
    sleep 0.2; n=$((n - 1))
  done
  return 1
}
wait_re_n() {                                      # wait_re_n <egrep pattern> <count> <seconds>
  n=$(( $3 * 5 ))
  while [ "$n" -gt 0 ]; do
    [ "$(grep -cE -- "$1" "$LOG" 2>/dev/null)" -ge "$2" ] && return 0
    sleep 0.2; n=$((n - 1))
  done
  return 1
}
# Stale-id guard, same as fullscreen.sh: XQueryTree can hand back a window that dies
# a moment later, and a dead id turns every call into "BadWindow".
acquire() {                                         # acquire <seconds> -> 0 if ID is live
  n=$(( $1 * 5 )); old=$ID
  while [ "$n" -gt 0 ]; do
    id=$($X tree 2>/dev/null | awk 'tolower($0) ~ /pilots/ {print $1; exit}')
    if [ -n "$id" ] && [ -n "$($X geom "$id" 2>/dev/null)" ]; then
      ID=$id
      return 0
    fi
    sleep 0.2; n=$((n - 1))
  done
  ID=$old
  return 1
}

# The scratch run dir with the data file. AVI/BMP are looked up next to the EXE
# (g_ExecutablePath), so the rundir's lack of MOVIES changes nothing and both
# movies play (INTRO before the loop, START inside it).
REPO=$(pwd)
RUN=/tmp/pilots_loadbeat_rundir
rm -rf "$RUN"; mkdir -p "$RUN"
ln -sf "$REPO/SRC/PILOTS.EXE" "$RUN/PILOTS.EXE"
cd "$RUN" || exit 1
PILOTS_LOAD_QUIET_MS=50 "$REPO/$BUILDDIR/PILOT_RE" >/tmp/pilots_lb.out 2>&1 &
P=$!
cd "$REPO"
ID=""
if ! acquire 45; then
  echo "FAIL: no live game window within 45s"
  [ -s /tmp/pilots_lb.out ] && {
    echo "  the game said:"
    tail -n 5 /tmp/pilots_lb.out | sed 's/^/    /'
  }
  kill -9 "$P" 2>/dev/null
  exit 1
fi
echo "== window $ID"
$X warp "$ID" 5 5 >/dev/null

echo "== 1. waiting for the menu"
wait_log "ENTER: GameMainLoop" 40; check $? "game reached its main loop"
wait_log_n "PlayAvi: end" 2 90; check $? "the startup movie ended (both movies played)"
sleep 5

echo "== 2. the indicator armed"
# Logged on the game's first read, long before the menu - the override has
# been read and the threshold in effect is the harness's 50 ms.
wait_log "SdlLoad: a screen still for 50 ms" 15; check $? "PILOTS_LOAD_QUIET_MS override reached the beat"
n=$(grep -c -F "showing a spinner" "$LOG" 2>/dev/null)
echo "   spinner starts by the menu: ${n:-0} (may be > 0 - the override is hypersensitive)"
# Snapshot BEFORE the click: the transition can load in under two seconds
# (measured: one burst of 2036 KB), and a snapshot taken after the dwell
# would already contain the very summary the wait looks for.
BIG0=$(grep -cE 'the screen stood still [0-9]+ s with [1-9][0-9][0-9]+ KB read' "$LOG" 2>/dev/null); BIG0=${BIG0:-0}

echo "== 3. entering scene 1 (first toolbar square)"
acquire 10
# The pointer must sit on the target BEFORE the click: a synthetic click
# (XSendEvent) does not move the pointer, and SDL derives the button event's
# position from its own cursor state - updated by motion events only; the
# engine picks the click's target by the cursor. Without the warp the click
# lands at (5,5) and the game never leaves the startup screen (both harnesses
# once measured that screen twice and never noticed - the counts matched).
$X warp "$ID" 42 448 >/dev/null
$X fake-click "$ID" 42 448 >/dev/null     # windowed is the identity: window px == game px
sleep 2
if ! kill -0 "$P" 2>/dev/null; then
  echo "FAIL: the game died entering scene 1"
  exit 1
fi

echo "== 4. the transition is a load"
# The scene-1 transition reads its data for seconds with the screen standing
# still - observation. What in the log separates that from the startup
# screen's own reading is the SIZE of the episode: the scene reads hundreds
# of KB, while the resting menu with its music reads nothing at all (the
# midi file is parsed from memory and avformat does not go through the
# shim's _read - so a playing track or a movie never starts the spinner).
# Count-based, so the pre-click episodes (the startup reload really is one)
# cannot satisfy the wait: one more big summary than the log had BEFORE the
# click can only come from the click -> drawn-scene span. The pointer-warp
# before the click may close a small episode of its own (measured: 68 ms, 8
# bytes); that one does not match the big pattern.
wait_re_n 'the screen stood still [0-9]+ s with [1-9][0-9][0-9]+ KB read' $((BIG0 + 1)) 60 \
  ; check $? "a real load was summarised when the picture moved again"
KB=$(grep -E 'the screen stood still [0-9]+ s with [0-9]+ KB read' "$LOG" | tail -n 1 | sed -E 's/.*with ([0-9]+) KB.*/\1/')
[ -n "$KB" ] && [ "$KB" -ge 100 ]; check $? "the last episode read scene data (${KB:-none} KB >= 100)"
n=$(grep -c -F "showing a spinner" "$LOG" 2>/dev/null)
[ "${n:-0}" -ge 1 ]; check $? "the spinner was started at least once ($n)"
grep -F "SdlLoad:" "$LOG" | sed 's/^.*SdlLoad:/   SdlLoad:/' | sort | uniq -c

echo "== 5. shutdown"
kill -USR1 "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; wait "$P" 2>/dev/null
n=$(grep -cE 'EXCEPTION|SIGSEGV' "$LOG" 2>/dev/null)
[ "${n:-0}" -eq 0 ]; check $? "no exception in the log"

if [ -n "$FAILED" ]; then echo "FAILED:${FAILED}"; exit 1; fi
echo "PASSED"
exit 0
