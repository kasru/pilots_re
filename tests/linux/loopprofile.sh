#!/bin/sh
# The loop profile: once per two seconds the port says what its loop did -
# pumps, messages taken (and how deep the queue still was), presents with
# their cost in us, rects blitted per second SPLIT into the message path
# (drawn while the game's wndproc runs - the cursor redrawn on a motion) and
# the frame path (the scene, drawn outside it), frames/s (main.c polls
# GetCursorPos once per frame) and "frame N ms ago (delay M)" - a healthy
# loop keeps N about M. The split is the whole point: "the animation froze"
# looks identical from the sofa whether the scene stopped drawing or the
# pointer's repaints ate the frame budget, and only these numbers tell them
# apart.
#
# What is measured here: the line prints in the v0.1-verbatim format, the
# first window is a real two seconds (the init records the base, not 0),
# both pumps tick, a pointer warp lands in the queue counter, and in scene 1
# - where the engine's own arrow is visible and a motion redraws it
# synchronously inside the dispatch (the premise the Switch stick throttle
# is built on) - the warp attributes rects to the message path while the
# animated scene puts its rects into the frame path. Counts are snapshotted
# BEFORE each act: every wait asks for one MORE line than the log had, so a
# value that was already there cannot satisfy it, and a line printed between
# the snapshot and the action cannot be mistaken for the action's either.
#
# Measured trap this harness carries: a warp on the STARTUP screen reaches
# the messages counter but produces NO rects - that screen does not draw the
# engine's cursor (moving it there blits nothing), hence the rect check lives
# in the scene, not at the menu.
#
#   sh tests/linux/loopprofile.sh
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
count_re() { grep -cE -- "$1" "$LOG" 2>/dev/null; }  # count_re <egrep pattern> -> number
num() { v=$(echo "$1" | sed -E "$2"); case "$v" in ''|*[!0-9-]*) echo 0 ;; *) echo "$v" ;; esac; }

# The scratch run dir with the data file. AVI/BMP are looked up next to the EXE
# (g_ExecutablePath), so the rundir's lack of MOVIES changes nothing and both
# movies play (INTRO before the loop, START inside it).
REPO=$(pwd)
RUN=/tmp/pilots_loopprofile_rundir
rm -rf "$RUN"; mkdir -p "$RUN"
ln -sf "$REPO/SRC/PILOTS.EXE" "$RUN/PILOTS.EXE"
cd "$RUN" || exit 1
# The default 1000 ms quiet threshold would MISS the scene-1 transition
# (measured: a still screen under a second under a 2036 KB burst) - the same
# reason loadbeat.sh sets it. The big-KB pattern filters the tiny warp-close
# episodes this makes possible (measured: 68 ms, 8 bytes).
PILOTS_LOAD_QUIET_MS=50 "$REPO/$BUILDDIR/PILOT_RV" >/tmp/pilots_lp.out 2>&1 &
P=$!
cd "$REPO"
ID=""
if ! acquire 45; then
  echo "FAIL: no live game window within 45s"
  [ -s /tmp/pilots_lp.out ] && {
    echo "  the game said:"
    tail -n 5 /tmp/pilots_lp.out | sed 's/^/    /'
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
if ! kill -0 "$P" 2>/dev/null; then
  echo "FAIL: the game died before the profile was read"
  exit 1
fi

echo "== 2. the profile speaks"
# The line starts printing at the first pump - during the intro movie already,
# so by the menu it exists; the wait only bounds the pathological case. The
# sed is unanchored on purpose: DBG_LOG prefixes every line with [PILOTS].
wait_re_n 'SdlLoopProfile: [0-9]+ ms - pumps' 1 30; check $? "SdlLoopProfile line appeared"
FIRST=$(grep -m1 -E 'SdlLoopProfile:' "$LOG" 2>/dev/null)
# The init remembers the base time: the first window spans ~2000 ms, not
# "now - 0" and not an epoch - a window in seconds, plausibly 1.5..15 s.
SPAN=$(num "$FIRST" 's/.*SdlLoopProfile: ([0-9]+) ms.*/\1/')
[ "$SPAN" -ge 1500 ] && [ "$SPAN" -le 15000 ]
check $? "first window is ~2 s ($SPAN ms)"
PUMP=$(num "$FIRST" 's/.*pumps ([0-9]+) \(.*/\1/')
[ "$PUMP" -ge 1 ]; check $? "the pump tick counted pumps ($PUMP)"
# Format smoke: this exact shape is what the docs and the v0.1 logs promise.
echo "$FIRST" | grep -qE 'us in present \(avg [0-9]+ us\).*queue now -?[0-9]+'
check $? "the v0.1 field set is verbatim (avg, queue now)"

echo "== 3. the frame counter at the menu"
# Movie windows carry frames 0/s - during the intro the modal loop does not
# poll the cursor. One line with frames >= 1 can only come from the running
# main loop; one more than the log has cannot be a leftover of the movies.
FRM0=$(count_re '\), frames [1-9][0-9]*/s,'); FRM0=${FRM0:-0}
wait_re_n '\), frames [1-9][0-9]*/s,' $((FRM0 + 1)) 15; check $? "a line with frames >= 1 (main loop polls)"
NEW=$(grep -E 'SdlLoopProfile:' "$LOG" | tail -n 1)
FRM=$(num "$NEW" 's/.*\), frames ([0-9]+)\/s.*/\1/')
[ "$FRM" -ge 1 ]; check $? "frames/s on the newest line ($FRM)"

echo "== 4. a pointer warp: the queue counter"
# The warp is the only message source at a resting menu, so every line with
# messages >= 1 after this point contains it. No rect expectation here: the
# startup screen does not draw the engine's cursor, and moving it blits
# nothing - measured, and the reason the rect check lives in the scene.
MSG0=$(count_re 'messages taken [1-9][0-9]* \('); MSG0=${MSG0:-0}
$X warp "$ID" 300 200 >/dev/null
wait_re_n 'messages taken [1-9][0-9]* \(' $((MSG0 + 1)) 15
check $? "the warp reached the queue counter (messages taken)"

echo "== 5. entering scene 1"
# Snapshot BEFORE the click: the transition can load in under two seconds
# (measured: one burst of 2036 KB), and a snapshot taken after the dwell
# would already contain the very summary the wait looks for.
BIG0=$(grep -cE 'the screen stood still [0-9]+ s with [1-9][0-9][0-9]+ KB read' "$LOG" 2>/dev/null); BIG0=${BIG0:-0}
# The pointer must sit on the target BEFORE the click: a synthetic click
# (XSendEvent) does not move the pointer, and SDL derives the button event's
# position from its own cursor state - updated by motion events only; the
# engine picks the click's target by the cursor. Without the warp the click
# lands at (5,5) and the game never leaves the startup screen.
$X warp "$ID" 42 448 >/dev/null
$X fake-click "$ID" 42 448 >/dev/null     # windowed is the identity: window px == game px
sleep 2
if ! kill -0 "$P" 2>/dev/null; then
  echo "FAIL: the game died entering scene 1"
  exit 1
fi
wait_re_n 'the screen stood still [0-9]+ s with [1-9][0-9][0-9]+ KB read' $((BIG0 + 1)) 60 \
  ; check $? "scene 1 entered (its load was summarised)"

echo "== 6. in the scene: the message path draws, the frame path draws"
# A motion in the scene redraws the engine's arrow synchronously inside the
# dispatch (UpdateCursorPosition -> RenderDirtyRegions - the premise the
# Switch stick throttle is built on), so this warp must grow the msg-rect
# count; the menu could not. The one-second pause lets the CLICK's own
# window line settle first: without it that line could land after the
# snapshot and satisfy the wait on its own.
sleep 1
MSGR0=$(count_re 'rects [0-9]+/s \(msg [1-9][0-9]*/s'); MSGR0=${MSGR0:-0}
MSG1=$(count_re 'messages taken [1-9][0-9]* \('); MSG1=${MSG1:-0}
$X warp "$ID" 320 240 >/dev/null
wait_re_n 'messages taken [1-9][0-9]* \(' $((MSG1 + 1)) 15
check $? "the in-scene warp reached the queue counter"
wait_re_n 'rects [0-9]+/s \(msg [1-9][0-9]*/s' $((MSGR0 + 1)) 15
check $? "the cursor rect was attributed to the message path"
# The menu is gone now, so the only source of frame-path rects and presents
# is the scene itself, animating outside any dispatch (hotspots measured two
# resting frames of this scene differing in ~34 px). The newest line is the
# warp's own window - one that must carry both.
NEW=$(grep -E 'SdlLoopProfile:' "$LOG" | tail -n 1)
RECTF=$(num "$NEW" 's/.*\(msg [0-9]+\/s frame ([0-9]+)\/s\).*/\1/')
[ "$RECTF" -ge 1 ]; check $? "the scene put rects into the frame path ($RECTF/s)"
PRES=$(num "$NEW" 's/.*presents ([0-9]+) \(([0-9]+)\/s\).*/\2/')
[ "$PRES" -ge 1 ]; check $? "presents kept counting ($PRES/s)"
grep -E 'SdlLoopProfile:.*messages taken [1-9]' "$LOG" | tail -n 4 | sed 's/^/    /'

echo "== 7. shutdown"
kill -USR1 "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; wait "$P" 2>/dev/null
n=$(grep -cE 'EXCEPTION|SIGSEGV' "$LOG" 2>/dev/null)
[ "${n:-0}" -eq 0 ]; check $? "no exception in the log"

if [ -n "$FAILED" ]; then echo "FAILED:${FAILED}"; exit 1; fi
echo "PASSED"
exit 0
