#!/bin/sh
# Hold Ctrl -> the port marks every object the engine's click hit test would accept;
# release -> nothing is drawn. Windows counterpart: tests/windows/hotspots.ps1.
#
# Ctrl reaches the overlay through GetAsyncKeyState, which on Linux is the
# SDL_GetModState shim (win32_shim.c) - so what has to be delivered is a real X key
# event, i.e. XTEST. Focus matters here: there is no window manager under Xvfb, so
# focus follows the pointer and the pointer has to sit inside the game window before
# any key is sent.
#
#   sh tests/linux/hotspots.sh
# Needs: a built $BUILDDIR/PILOT_RV, SRC/ with the original game data and
# tests/linux/x11ctl.py as the X driver.
case "$0" in
  */*) cd "$(dirname "$0")/../.." || exit 1 ;;
esac
# Same reasoning as fullscreen.sh: the host has no i386 runtime, so the default is
# the native 64-bit build; PILOTS_BUILDDIR overrides it for a binary that runs here.
BUILDDIR=${PILOTS_BUILDDIR:-build64}
DISP=${PILOTS_HSDISPLAY:-:97}
SW=1024; SH=768
LOG=$(pwd)/$BUILDDIR/PILOT_RV_debug.log
X="python3 tests/linux/x11ctl.py"

[ -x "$BUILDDIR/PILOT_RV" ] || { echo "no $BUILDDIR/PILOT_RV - build it first"; exit 1; }

pgrep -f "Xvfb $DISP" >/dev/null 2>&1 || {
  Xvfb "$DISP" -screen 0 "${SW}x${SH}x24" >/dev/null 2>&1 &
  sleep 2
}
export DISPLAY=$DISP
echo "== display $DISP ${SW}x${SH} ($BUILDDIR)"

pkill -9 PILOT_RV 2>/dev/null
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
key() { $X xtest-key "$1" "$2" >/dev/null; }        # key <Control_L> <down|up>
# Marker count of the most recent edge, or "" when that edge is not the last one logged.
# Reading the log and not the pixels: the scene animates by itself, so two idle frames
# always differ a little even with the overlay off.
last_n() {                                          # last_n down|up -> number or ""
  grep -F "Ctrl $1, " "$LOG" 2>/dev/null | tail -n1 |
    sed -n "s/.*Ctrl $1, \([0-9][0-9]*\) interactive.*/\1/p"
}
overlay() {                                         # overlay <label>
  key Control_L down
  sleep 1
  wait_log "Ctrl down, " 5
  N=$(last_n down)
  [ -n "$N" ] && [ "$N" -gt 0 ]; check $? "$1: Ctrl down marks objects (count=${N:-none})"
  key Control_L up
  sleep 1
  wait_log "Ctrl up, 0 interactive" 5; check $? "$1: Ctrl up logged"
  N=$(last_n up)
  [ -n "$N" ] && [ "$N" -eq 0 ]; check $? "$1: Ctrl up clears them (count=${N:-none})"
}

cd SRC || exit 1
../$BUILDDIR/PILOT_RV >/tmp/pilots_hs.out 2>&1 &
P=$!
cd ..

ID=""
if ! acquire 45; then
  echo "FAIL: no live game window within 45s"
  [ -s /tmp/pilots_hs.out ] && {
    echo "  the game said:"
    tail -n 5 /tmp/pilots_hs.out | sed 's/^/    /'
  }
  kill -9 "$P" 2>/dev/null
  exit 1
fi
echo "== window $ID"

# Keyboard focus follows the pointer and there is no WM to move it, so put the
# pointer inside the window before sending anything.
$X warp "$ID" 5 5 >/dev/null

# Same dwell as the Windows harness: the window exists long before the menu's
# objects do, and reading the overlay during that window just reads an empty stack.
echo "== 1. waiting for the menu"
wait_log "ENTER: GameMainLoop" 40; check $? "game reached its main loop"
sleep 2

echo "== 2. overlay on the startup screen"
overlay startup

echo "== 3. entering scene 1 (first toolbar square)"
acquire 10
$X fake-click "$ID" 42 448 >/dev/null     # windowed is the identity: window px == game px
sleep 6
if ! kill -0 "$P" 2>/dev/null; then
  echo "FAIL: the game died entering scene 1"
  exit 1
fi

echo "== 4. overlay in scene 1"
acquire 10
$X warp "$ID" 5 5 >/dev/null              # the click moved the pointer; refocus
overlay scene1

echo "== 5. shutdown"
kill -USR1 "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; wait "$P" 2>/dev/null
n=$(grep -cE 'EXCEPTION|SIGSEGV' "$LOG" 2>/dev/null)
[ "${n:-0}" -eq 0 ]; check $? "no exception in the log"
echo "   counts: startup=$(grep -F 'Ctrl down, ' "$LOG" | sed -n 's/.*Ctrl down, \([0-9]*\) interactive.*/\1/p' | tr '\n' ' ')"

if [ -n "$FAILED" ]; then echo "FAILED:${FAILED}"; exit 1; fi
echo "PASSED"
exit 0
