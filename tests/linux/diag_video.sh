#!/bin/sh
# Phase-6 smoke test: play intro+start movies through the ffmpeg MCI, then a
# clean quit (SIGUSR1 test hook). Expects SRC/MOVIES/{INTRO,START}.AVI - the
# names the original CD ships; the engine asks for ".\movies\intro.AVI" (its own
# data section says lowercase) and the shim resolves that case like Win9x did, so
# a verbatim copy of the original resources works on a case-sensitive FS.
#
# The waits are measured, not guessed: with the original 15fps 320x240 movies
# INTRO runs ~34s and START ~50s, so a fixed 15s nap (enough for the old 60-frame
# synthetic clips) only ever proved the first frames. ffprobe reads the real
# durations off the mounted tree and the script waits that long plus slack.
cd "$(dirname "$0")/../.." || exit 1
BUILDDIR=${PILOTS_BUILDDIR:-build32}
LOG=$BUILDDIR/PILOT_RE_debug.log

dur() {  # whole seconds of an AVI, default if ffprobe/file is missing
  d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "SRC/MOVIES/$1" 2>/dev/null | cut -d. -f1)
  echo "${d:-$2}"
}
INTRO_DUR=$(dur INTRO.AVI 34)
START_DUR=$(dur START.AVI 50)
WAIT1=$((INTRO_DUR + 12))             # boot + intro + slack
WAIT2=$((INTRO_DUR + START_DUR + 30)) # ... + scene switch + start + slack
echo "INTRO=${INTRO_DUR}s START=${START_DUR}s -> wait1=${WAIT1}s wait2=${WAIT2}s"

podman rm -f plr >/dev/null 2>&1
: > "$LOG"
podman run -d --name plr -v "$PWD:/w" -w /w/SRC \
  -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy \
  pilots-build "/w/$BUILDDIR/PILOT_RE" || exit 1

sleep "$WAIT1"
echo "---- after ${WAIT1}s (expect 1 finished movie, 1 notify) ----"
echo "finished=$(grep -c 'playback finished' "$LOG") notifies=$(grep -c 'posting MM_MCINOTIFY' "$LOG")"

sleep $((WAIT2 - WAIT1))
pid=$(podman exec plr sh -c 'for p in /proc/[0-9]*; do c=$(cat "$p/comm" 2>/dev/null); [ "$c" = PILOT_RE ] && echo "${p#/proc/}"; done')
echo "t=${WAIT2}s pid=$pid - SIGUSR1 for a clean quit"
podman exec plr sh -c "kill -USR1 $pid" 2>/dev/null
sleep 5
alive=$(podman container inspect -f '{{.State.Running}}' plr)
echo "running=$alive exitcode=$(podman container inspect -f '{{.State.ExitCode}}' plr)"

echo "---- CASE RESOLUTION (original resources, engine asks lowercase) ----"
grep -E "MCI_OPEN" "$LOG"
echo "---- VIDEO MARKERS ----"
grep -E 'PlayIntroVideo: g_VideoFilePath|playback finished|posting MM_MCINOTIFY|LoadVideoBitmap: FileName' "$LOG"
echo "---- STDERR (crash backtrace would be here) ----"
podman logs plr 2>&1 | grep -E 'FATAL|stack trace' || echo "(no crash)"

echo "---- VERDICT ----"
ok=1
[ "$(grep -c 'playback finished' "$LOG")" = 2 ] || { echo "FAIL: expected 2 finished movies"; ok=0; }
[ "$(grep -c 'posting MM_MCINOTIFY' "$LOG")" = 2 ] || { echo "FAIL: expected 2 notifies"; ok=0; }
grep -q 'MCI_OPEN' "$LOG" || { echo "FAIL: no MCI_OPEN"; ok=0; }
grep -q 'MCI_OPEN .* -> .*/MOVIES/INTRO.AVI' "$LOG" || { echo "FAIL: original MOVIES/INTRO.AVI not resolved"; ok=0; }
grep -q FATAL "$LOG" && { echo "FAIL: crash"; ok=0; }
[ "$alive" = "false" ] && [ "$(podman container inspect -f '{{.State.ExitCode}}' plr)" = 0 ] \
  || { echo "FAIL: no clean exit"; ok=0; }
[ $ok = 1 ] && echo "PASS: original resources, both movies played, notified, clean exit"
podman rm -f plr >/dev/null 2>&1
exit $((1 - ok))
