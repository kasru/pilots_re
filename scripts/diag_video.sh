#!/bin/sh
# Phase-6 smoke test: play intro+start movies through the ffmpeg MCI, then a
# clean quit (SIGUSR1 test hook). Expects SRC/movies/{intro,start}.avi.
# Timeline: boot ~2s -> intro ~4s -> scene -> start ~4s -> menu (~12s+).
cd /home/ntadmin/work/pilots_re || exit 1
LOG=build/PILOT_RV_debug.log
podman rm -f plr >/dev/null 2>&1
: > "$LOG"
podman run -d --name plr -v "$PWD:/w" -w /w/SRC \
  -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy \
  pilots-build /w/build/PILOT_RV || exit 1
sleep 15
pid=$(podman exec plr sh -c 'for p in /proc/[0-9]*; do c=$(cat "$p/comm" 2>/dev/null); [ "$c" = PILOT_RV ] && echo "${p#/proc/}"; done')
echo "t=15s pid=$pid - SIGUSR1 for a clean quit"
podman exec plr sh -c "kill -USR1 $pid"
sleep 4
alive=$(podman container inspect -f '{{.State.Running}}' plr)
echo "running=$alive exitcode=$(podman container inspect -f '{{.State.ExitCode}}' plr)"
echo "---- STDERR (crash backtrace would be here) ----"
podman logs plr 2>&1 | grep -E 'FATAL|stack trace' || echo "(no crash)"
echo "---- VIDEO MARKERS ----"
grep -E 'PlayIntroVideo|MciFfmpeg|OpenMCIVideoDevice' "$LOG"
echo "---- TAIL ----"
tail -6 "$LOG"
echo "---- VERDICT ----"
ok=1
[ "$(grep -c 'playback finished (60 frames shown)' $LOG)" = 2 ] || { echo "FAIL: expected 2 finished movies"; ok=0; }
[ "$(grep -c 'posting MM_MCINOTIFY' $LOG)" = 2 ] || { echo "FAIL: expected 2 notifies"; ok=0; }
grep -q FATAL "$LOG" && { echo "FAIL: crash"; ok=0; }
[ "$alive" = "false" ] && [ "$(podman container inspect -f '{{.State.ExitCode}}' plr)" = 0 ] \
  || { echo "FAIL: no clean exit"; ok=0; }
[ $ok = 1 ] && echo "PASS: 2 movies played, notified, clean exit"
podman rm -f plr >/dev/null 2>&1
exit $((1 - ok))
