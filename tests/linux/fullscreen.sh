#!/bin/sh
# -fullscreen / Alt+Enter smoke test on X11: borderless desktop fullscreen, the
# aspect-kept letterbox, the movie click mapping and the Alt+Enter toggle back
# and forth. Counterpart of tests/windows/fullscreen.ps1 (there the same battery
# is driven by PostMessage/real cursor).
#
# Starts its OWN Xvfb on :98 with a 16:9 screen: the shared :99 is 800x600 which
# is the game's own 4:3, where "scaled to fit" and "identity" produce the exact
# same numbers - a test that cannot fail is not a test. On 1280x720 the 640x480
# picture must become 960x720 at (160,0).
#
#   sh tests/linux/fullscreen.sh
# Needs: a built $BUILDDIR/PILOT_RV, SRC/ with the original game data and
# SRC/MOVIES/INTRO.AVI, tests/linux/x11ctl.py as the X driver.
# $0 has no slash when the script is piped into `sh -s` (run_remote.ps1 prepends the
# tar extract and leaves us in the repo root) - then just stay where we are.
case "$0" in
  */*) cd "$(dirname "$0")/../.." || exit 1 ;;
esac
# Unlike diag_*.sh this script STARTS the game itself, on the host's X display, so it
# defaults to build64: the host carries no i386 runtime at all - build32 is built inside
# the pilots-build container and only runs there (a direct exec dies with ENOENT on
# /lib/ld-linux.so.2, which looks like "the game never opened a window"). Override with
# PILOTS_BUILDDIR when driving a binary that does run here.
BUILDDIR=${PILOTS_BUILDDIR:-build64}
DISP=${PILOTS_FSDISPLAY:-:98}
SW=1280; SH=720
LOG=$(pwd)/$BUILDDIR/PILOT_RV_debug.log
X="python3 tests/linux/x11ctl.py"

[ -x "$BUILDDIR/PILOT_RV" ] || { echo "no $BUILDDIR/PILOT_RV - build it first"; exit 1; }

# expected letterbox for a 4:3 picture on this screen (fit the height, centre)
EPH=$SH
EPW=$((SH * 4 / 3))
EPX=$(( (SW - EPW) / 2 ))
EPY=0

pgrep -f "Xvfb $DISP" >/dev/null 2>&1 || {
  Xvfb "$DISP" -screen 0 "${SW}x${SH}x24" >/dev/null 2>&1 &
  sleep 2
}
export DISPLAY=$DISP
echo "== display $DISP ${SW}x${SH}"

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
geom() { $X geom "$ID" 2>/dev/null; }
# The id must be one X still accepts. XQueryTree can hand back a "Pilots Bros." child
# that dies a moment later (a previous run's window racing its destroy), and a stale id
# turns every geom/warp into "X Error ... BadWindow": one run then asserted on empty
# geometry and failed four checks at once. So acquire = find AND validate, and it is
# re-run before each step that talks to X. Keeps the old id when nothing live is found.
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
# The window must STABILIZE at WxH: right after -fullscreen is parsed the window
# is still the windowed 640x480 one (it is mapped later, from PlayIntroVideo), and
# a single early reading is exactly how this test lied the first time - it clicked
# the centre of the WRONG rectangle and got game (256,192).
wait_geom() {                                      # wait_geom <WxH> <seconds>
  n=$(( $2 * 5 )); last=""
  while [ "$n" -gt 0 ]; do
    G=$(geom); S=${G#* }
    [ "$S" = "$1" ] && [ "$S" = "$last" ] && return 0
    last=$S
    sleep 0.2; n=$((n - 1))
  done
  return 1
}
key() {                                            # key <Alt_L|Return> <down|up>
  $X xtest-key "$1" "$2" >/dev/null
}
alt_enter() {
  key Alt_L down; key Return down; key Return up; key Alt_L up
}

cd SRC || exit 1
../$BUILDDIR/PILOT_RV -fullscreen >/tmp/pilots_fs.out 2>&1 &
P=$!
cd ..

ID=""
if ! acquire 45; then
  echo "FAIL: no live game window within 45s"
  [ -s /tmp/pilots_fs.out ] && {  # exec failure (no i386 runtime: ENOENT on
    echo "  the game said:"        # /lib/ld-linux.so.2) or an SDL init error
    tail -n 5 /tmp/pilots_fs.out | sed 's/^/    /'
  }
  kill -9 "$P" 2>/dev/null
  exit 1
fi
echo "== window $ID"

echo "== 1. option + fullscreen applied"
wait_log "-fullscreen ->" 30; check $? "-fullscreen parsed"
wait_log "SdlApplyFullscreen: on ->" 10; check $? "SdlApplyFullscreen: on logged"
wait_geom "${SW}x${SH}" 10; check $? "window covers the screen"
echo "   geom: $(geom)"

echo "== 2. picture letterboxed to the screen"
have "picture ${EPW}x${EPH} at (${EPX},${EPY})"
check $? "picture ${EPW}x${EPH} at (${EPX},${EPY}) (aspect kept, centred)"
grep -F "SdlUpdateLayout: " "$LOG" | tail -n 3 | sed 's/^/   /'

echo "== 3. click the centre of the movie"
wait_log "PlayIntroVideo: g_MCIDeviceID" 60
check $? "movie opened"
sleep 3                                     # let a frame with content be on screen
command -v ffmpeg >/dev/null 2>&1 && \
  ffmpeg -y -f x11grab -video_size "${SW}x${SH}" -i "$DISP" -frames:v 1 \
    "$BUILDDIR/fs_movie.png" >/dev/null 2>&1

acquire 10                                       # the id may have gone stale since section 1
G=$(geom)
if [ -z "$G" ]; then
  echo "FAIL: no geometry for the click (BadWindow on $ID)"
  kill -9 "$P" 2>/dev/null
  exit 1
fi
echo "   geom before the click: $G"
CW=$(echo "$G" | sed 's/.* \([0-9]*\)x\([0-9]*\)$/\1/')
CH=$(echo "$G" | sed 's/.* \([0-9]*\)x\([0-9]*\)$/\2/')
CX=$((CW / 2)); CY=$((CH / 2))
echo "   clicking $CX,$CY of ${CW}x${CH} (the picture is centred, so this is game 320,240)"
$X fake-click "$ID" "$CX" "$CY"
wait_log "PlayIntroVideo: wait loop ended" 15
check $? "movie stopped by the click"
have "-> game (320,240)"; check $? "click mapped to game (320,240)"

echo "== 4. Alt+Enter -> windowed"
acquire 10
$X warp "$ID" 5 5 >/dev/null                 # keyboard focus follows the pointer
alt_enter
wait_log "SdlApplyFullscreen: off ->" 10; check $? "Alt+Enter leaves fullscreen"
wait_geom "640x480" 10; check $? "window back to 640x480"
echo "   geom: $(geom)"
command -v ffmpeg >/dev/null 2>&1 && \
  ffmpeg -y -f x11grab -video_size "${SW}x${SH}" -i "$DISP" -frames:v 1 \
    "$BUILDDIR/fs_windowed.png" >/dev/null 2>&1

echo "== 5. Alt+Enter -> fullscreen again"
acquire 10
$X warp "$ID" 5 5 >/dev/null
alt_enter
n=$(grep -c -F "SdlApplyFullscreen: on ->" "$LOG" 2>/dev/null)
[ "${n:-0}" -ge 2 ]; check $? "Alt+Enter returns to fullscreen"
wait_geom "${SW}x${SH}" 10; check $? "window covers the screen again"
echo "   geom: $(geom)"

echo "== 6. shutdown"
kill -USR1 "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; wait "$P" 2>/dev/null
n=$(grep -cE 'EXCEPTION|SIGSEGV' "$LOG" 2>/dev/null)
[ "${n:-0}" -eq 0 ]; check $? "no exception in the log"
echo "   fullscreens on=$(grep -c -F 'SdlApplyFullscreen: on ->' "$LOG") \
off=$(grep -c -F 'SdlApplyFullscreen: off ->' "$LOG") \
mapped=$(grep -c -F -e '-> game (320,240)' "$LOG")"

if [ -n "$FAILED" ]; then echo "FAILED:${FAILED}"; exit 1; fi
echo "PASSED"
exit 0
