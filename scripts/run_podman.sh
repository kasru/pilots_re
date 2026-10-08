#!/bin/sh
# Launch PILOT_RV inside the pilots-build podman container (the whole repo is
# mounted at /w; the game runs with CWD /w/SRC, which is where it opens its
# data file .\PILOTS.EXE). Everything stays in the container - the host only
# needs podman, the image and the game data.
#
# Usage:
#   sh scripts/run_podman.sh           headless run (SDL dummy video/audio)
#   sh scripts/run_podman.sh --xvfb    render on a virtual X display (xvfb)
#
# Ctrl+C or SIGTERM asks the game to quit cleanly (SIGUSR1 test hook of the
# port); a second Ctrl+C force-stops the container. The container's
# stdout/stderr is relayed live; the game's own log is
# build/PILOT_RV_debug.log (host path).
#
# Prerequisites: podman, the pilots-build image (see Containerfile), a built
# build/PILOT_RV and the original game data SRC/PILOTS.EXE.

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${PILOTS_IMAGE:-pilots-build}
NAME=plr-run
MODE=run
QUIT_SENT=0
LOGS_PID=

usage() {
  cat <<'EOF'
Launch PILOT_RV inside the pilots-build podman container.

  sh scripts/run_podman.sh           headless run (SDL dummy video/audio)
  sh scripts/run_podman.sh --xvfb    render on a virtual X display (xvfb)

Ctrl+C / SIGTERM = clean quit via the game's SIGUSR1 hook; a second
Ctrl+C force-stops the container. Game log: build/PILOT_RV_debug.log.
EOF
}

die() { echo "run_podman: $*" >&2; exit 1; }

for a in "$@"; do
  case "$a" in
    --xvfb) MODE=xvfb ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $a" ;;
  esac
done

# --- preconditions ---------------------------------------------------------
command -v podman >/dev/null 2>&1 || die "podman not found"
podman image exists "$IMAGE" 2>/dev/null \
  || die "image '$IMAGE' not found (podman build -t $IMAGE . on the Containerfile)"
[ -f "$REPO/build/PILOT_RV" ] \
  || die "build/PILOT_RV not found (cmake --build build inside the image)"
[ -f "$REPO/SRC/PILOTS.EXE" ] \
  || die "SRC/PILOTS.EXE (original game data) not found - the game cannot start"

# --- helpers ---------------------------------------------------------------
# PID of PILOT_RV inside the container (slim image has no pidof).
game_pid() {
  podman exec "$NAME" sh -c \
    'for p in /proc/[0-9]*; do
       [ "$(cat "$p/comm" 2>/dev/null)" = PILOT_RV ] && { echo "${p#/proc/}"; break; }
     done' 2>/dev/null || true
}

# First Ctrl+C: SIGUSR1 = the port's clean-quit hook (flushes and exits 0).
# Second Ctrl+C: hard stop, in case the game ignores it.
on_quit() {
  if [ "$QUIT_SENT" -ne 0 ]; then
    echo ""
    echo "run_podman: still running - forcing container stop"
    podman stop -t 3 "$NAME" >/dev/null 2>&1 || true
    return 0
  fi
  QUIT_SENT=1
  pid=$(game_pid) || pid=           # the outer guard matters: command
  if [ -n "$pid" ]; then            # substitution subshells also die on
    echo ""                         # signal-group TERM (143) - errexit safe
    echo "run_podman: clean quit (SIGUSR1 to pid $pid)"
    podman exec "$NAME" sh -c "kill -USR1 $pid" 2>/dev/null || true
  else
    podman stop -t 3 "$NAME" >/dev/null 2>&1 || true
  fi
}

cleanup() {
  [ -n "$LOGS_PID" ] && kill "$LOGS_PID" 2>/dev/null || true
  podman rm -f "$NAME" >/dev/null 2>&1 || true
}

# --- start -----------------------------------------------------------------
AUDIODRV=${SDL_AUDIODRIVER:-dummy}
if [ "$MODE" = xvfb ]; then
  VIDEODRV=${SDL_VIDEODRIVER:-x11}
  # Direct Xvfb + exec instead of the stock xvfb-run: that wrapper hangs in
  # this image (it gets stuck even with `echo hello` - its display-ready wait
  # never completes) while Xvfb itself starts fine. Wait for the socket, give
  # up loudly, then exec the game as PID1 so the SIGUSR1 quit keeps working.
  set -- sh -c 'Xvfb :99 -screen 0 800x600x24 &
    n=0
    while [ ! -S /tmp/.X11-unix/X99 ]; do
      n=$((n + 1))
      if [ "$n" -ge 50 ]; then
        echo "run_podman: Xvfb failed to become ready" >&2
        exit 1
      fi
      sleep 0.1
    done
    DISPLAY=:99 exec /w/build/PILOT_RV'
else
  VIDEODRV=${SDL_VIDEODRIVER:-dummy}
  set -- /w/build/PILOT_RV
fi

podman rm -f "$NAME" >/dev/null 2>&1 || true
podman run -d --name "$NAME" \
  -v "$REPO:/w" -w /w/SRC \
  -e SDL_VIDEODRIVER="$VIDEODRV" \
  -e SDL_AUDIODRIVER="$AUDIODRV" \
  "$IMAGE" "$@" >/dev/null || die "podman run failed"

trap 'on_quit' INT TERM
trap 'cleanup' EXIT

podman logs -f "$NAME" 2>&1 &      # live relay (background jobs ignore Ctrl+C)
LOGS_PID=$!

echo "run_podman: '$NAME' started in image '$IMAGE' (video=$VIDEODRV audio=$AUDIODRV)"
echo "run_podman: Ctrl+C for a clean quit, game log: build/PILOT_RV_debug.log"

# Poll instead of a blocking `podman wait`: POSIX shells defer traps until the
# foreground command returns, and a lone TERM would never interrupt podman
# wait. Every poll command is `|| true`-guarded on purpose: timeout/Ctrl+C hit
# the whole process group, so the poll children die with 143/130 too - the
# loop must survive them (errexit would otherwise abort the script right after
# the quit handler and its EXIT trap would kill a still-shutting-down game).
EXIT_CODE=
EMPTY=0
while [ -z "$EXIT_CODE" ]; do
  state=$(podman inspect -f '{{.State.Status}}' "$NAME" 2>/dev/null || true) || state=
  case "$state" in
    running|created)
      sleep 1 || true ;;
    "")
      # inspect itself may have been signal-killed; only give up when the
      # container is really gone (3 consecutive misses)
      EMPTY=$((EMPTY + 1))
      if [ "$EMPTY" -ge 3 ]; then EXIT_CODE=1; break; fi
      sleep 1 || true ;;
    *)
      EXIT_CODE=$(podman inspect -f '{{.State.ExitCode}}' "$NAME" 2>/dev/null || true) || EXIT_CODE=
      [ -n "$EXIT_CODE" ] || EXIT_CODE=1
      break ;;
  esac
done

kill "$LOGS_PID" 2>/dev/null || true   # let the tail of the relay flush
wait "$LOGS_PID" 2>/dev/null || true

echo "run_podman: game exited with code $EXIT_CODE"
if [ "$EXIT_CODE" != 0 ]; then
  echo "run_podman: check build/PILOT_RV_debug.log (backtrace on stderr is in podman logs)" >&2
fi
exit "$EXIT_CODE"
