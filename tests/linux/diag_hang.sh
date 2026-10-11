#!/bin/sh
# Shutdown-path test: SIGUSR1 -> graceful quit via WinMain LABEL_19; if the
# process is still alive 10s later it hangs in cleanup - catch with SIGSEGV
# so the game's own handler prints a backtrace of the stuck point.
cd "$(dirname "$0")/../.." || exit 1
BUILDDIR=${PILOTS_BUILDDIR:-build32}
LOG=$BUILDDIR/PILOT_RV_debug.log
podman rm -f plr >/dev/null 2>&1
: > "$LOG"
podman run -d --name plr -v "$PWD:/w" -w /w/SRC \
  -e SDL_VIDEODRIVER=dummy -e SDL_AUDIODRIVER=dummy \
  pilots-build "/w/$BUILDDIR/PILOT_RV" || exit 1
sleep 15
pid=$(podman exec plr sh -c 'for p in /proc/[0-9]*; do c=$(cat "$p/comm" 2>/dev/null); [ "$c" = PILOT_RV ] && echo "${p#/proc/}"; done')
echo "t=15s pid=$pid - sending SIGUSR1"
podman exec plr sh -c "kill -USR1 $pid"
sleep 10
alive=$(podman container inspect -f '{{.State.Running}}' plr)
echo "after SIGUSR1+10s running=$alive"
if [ "$alive" = "true" ]; then
  echo "HANG IN SHUTDOWN - SIGSEGV backtrace:"
  podman exec plr sh -c "kill -SEGV $pid"
  sleep 2
  tail -35 "$LOG"
else
  echo "clean exit code=$(podman container inspect -f '{{.State.ExitCode}}' plr)"
  echo ---- LOG TAIL ----
  tail -25 "$LOG"
fi
podman rm -f plr >/dev/null 2>&1
