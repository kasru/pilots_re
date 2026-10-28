#!/bin/sh
# The music path end to end: enter scene 1 (its script starts a track) and
# require the libADLMIDI synth to have opened, taken its first midiOutShortMsg,
# rendered its first audible PCM and fed the shared mixer real frames.
#
# Windows counterpart: tests/windows/audio.ps1 - it drives the panel buttons
# and there midiOut is the real winmm (this file is an empty TU on _WIN32),
# so the synth itself is measured here and on the Switch's log only.
#
#   sh tests/linux/midisynth.sh
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
have_re() { grep -qE -- "$1" "$LOG" 2>/dev/null; }
wait_log_re() {                                    # wait_log_re <egrep pattern> <seconds>
  n=$(( $2 * 5 ))
  while [ "$n" -gt 0 ]; do
    have_re "$1" && return 0
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
RUN=/tmp/pilots_midisynth_rundir
rm -rf "$RUN"; mkdir -p "$RUN"
ln -sf "$REPO/SRC/PILOTS.EXE" "$RUN/PILOTS.EXE"
cd "$RUN" || exit 1
"$REPO/$BUILDDIR/PILOT_RE" >/tmp/pilots_ms.out 2>&1 &
P=$!
cd "$REPO"
ID=""
if ! acquire 45; then
  echo "FAIL: no live game window within 45s"
  [ -s /tmp/pilots_ms.out ] && {
    echo "  the game said:"
    tail -n 5 /tmp/pilots_ms.out | sed 's/^/    /'
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

echo "== 2. the synth at rest"
# Two opens by then: GmAudio at startup, then GmAudio_Restart after the movies.
# The bank/rate are the arch-invariant part of the line (emu string and the
# first-message/first-PCM counts are timing-dependent - see sdl_backend.c).
wait_log "SdlMidi_Open: libADLMIDI ready" 10; check $? "synth opened (SdlMidi_Open ready)"
have "bank=68 rate=44100"; check $? "the WOPL bank 68 and 44100 Hz are in effect"
n=$(grep -c -F "SdlMidi_Open: libADLMIDI ready" "$LOG" 2>/dev/null)
echo "   synth opens: ${n:-0}"

echo "== 3. entering scene 1 (first toolbar square)"
acquire 10
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

echo "== 4. the music path"
# The track is the game's own (opcode 16 -> GmMidi_Play, started by the
# startup screen's script and re-played by GmAudio_Restart after the
# movies): the 10 ms timer runs, GmMidi_Pump parses events into
# midiOutShortMsg words and the synth both receives them and renders them.
# The first message's status byte and the nonzero count of the first chunk
# are timing-dependent (a 10 ms timer tick decides how many events got
# there) - only their PRESENCE is asserted.
wait_log "SdlMidi_ShortMsg: first message" 30; check $? "the synth took its first midiOutShortMsg"
wait_log "SdlMidi_Pump: first audible PCM" 10; check $? "the synth rendered its first audible PCM"
# The mixer's own 5 s report: frames actually handed over AND a peak, so the
# PCM is not just counted silence (the wave path was caught exactly this way).
wait_log_re 'midi [1-9][0-9]* frames \(peak [1-9]' 15; check $? "the mixer got real midi frames (count and peak)"
grep -E 'midi [0-9]+ frames' "$LOG" | tail -n 1 | sed 's/^.*midi /   last report: midi /;s/, movie.*//'

echo "== 5. shutdown"
kill -USR1 "$P" 2>/dev/null; sleep 3; kill -9 "$P" 2>/dev/null; wait "$P" 2>/dev/null
n=$(grep -cE 'EXCEPTION|SIGSEGV' "$LOG" 2>/dev/null)
[ "${n:-0}" -eq 0 ]; check $? "no exception in the log"
echo "   reset logs: $(grep -c -F 'SdlMidi_Reset' "$LOG" 2>/dev/null) (a Halt/Stop in the run)"

if [ -n "$FAILED" ]; then echo "FAILED:${FAILED}"; exit 1; fi
echo "PASSED"
exit 0
