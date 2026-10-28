#!/bin/sh
# Launch PILOT_RE inside the pilots-build podman container (the whole repo is
# mounted at /w; the game runs with CWD /w/SRC, which is where it opens its
# data file .\PILOTS.EXE). Everything stays in the container - the host only
# needs podman, the image and the game data.
#
# Usage:
#   sh scripts/run_podman.sh           headless run (SDL dummy video/audio)
#   sh scripts/run_podman.sh --xvfb    render on a virtual X display (xvfb)
#   sh scripts/run_podman.sh --display real window on the session's X server
#   sh scripts/run_podman.sh --pulse   host PulseAudio sound (with any of the above)
#
# --display forwards DISPLAY, /tmp/.X11-unix and the X cookie into the
# container: start it from a graphical session (RDP/local desktop) and the
# game window opens right there; `ssh -X` works too (loopback display, the
# container then joins the host network). If the connection is refused for
# lack of a cookie, run `xhost +local:` as the desktop user.
#
# --pulse forwards the host PulseAudio socket at its own path (PULSE_SERVER /
# XDG_RUNTIME_DIR detection - the same trick as podrunner.sh's --pulseaudio),
# the client cookie and /etc/machine-id; SDL then opens the "pulseaudio"
# driver instead of the silent dummy one. Rootless podman keeps the host uid,
# so the daemon accepts the connection as the same user. Dies with a clear
# message when the host has no Pulse server.
#
# Ctrl+C or SIGTERM asks the game to quit cleanly (SIGUSR1 test hook of the
# port); a second Ctrl+C force-stops the container. The container's
# stdout/stderr is relayed live; the game's own log is
# $BUILDDIR/PILOT_RE_debug.log (host path).
#
# Prerequisites: podman, the pilots-build image (see Containerfile), a built
# $BUILDDIR/PILOT_RE (cmake -S . -B $BUILDDIR -DPILOTS_M32=ON, built INSIDE the
# image - the host has no i386 toolchain) and the original game data
# SRC/PILOTS.EXE.

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${PILOTS_IMAGE:-pilots-build}
# Where the 32-bit binary lives, relative to the repo root. The native 64-bit
# build uses build64/, so the container one gets its own dir and the two never
# overwrite each other's SDL2/ffmpeg-linked objects.
BUILDDIR=${PILOTS_BUILDDIR:-build32}
NAME=plr-run
MODE=run
PULSE=0
QUIT_SENT=0
LOGS_PID=

usage() {
  cat <<'EOF'
Launch PILOT_RE inside the pilots-build podman container.

  sh scripts/run_podman.sh           headless run (SDL dummy video/audio)
  sh scripts/run_podman.sh --xvfb    render on a virtual X display (xvfb)
  sh scripts/run_podman.sh --display real window on the session's X server
  sh scripts/run_podman.sh --pulse   host PulseAudio sound (with any of the above)

Ctrl+C / SIGTERM = clean quit via the game's SIGUSR1 hook; a second
Ctrl+C force-stops the container. Game log: $PILOTS_BUILDDIR/PILOT_RE_debug.log
(default build32; both that env var and PILOTS_IMAGE select the build).
EOF
}

die() { echo "run_podman: $*" >&2; exit 1; }

for a in "$@"; do
  case "$a" in
    --xvfb)
      if [ "$MODE" = display ]; then die "--xvfb and --display are mutually exclusive"; fi
      MODE=xvfb ;;
    --display)
      if [ "$MODE" = xvfb ]; then die "--xvfb and --display are mutually exclusive"; fi
      MODE=display ;;
    --pulse) PULSE=1 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; die "unknown option: $a" ;;
  esac
done

# --- preconditions ---------------------------------------------------------
command -v podman >/dev/null 2>&1 || die "podman not found"
podman image exists "$IMAGE" 2>/dev/null \
  || die "image '$IMAGE' not found (podman build -t $IMAGE . on the Containerfile)"
[ -f "$REPO/$BUILDDIR/PILOT_RE" ] \
  || die "$BUILDDIR/PILOT_RE not found (cmake -S . -B $BUILDDIR -DPILOTS_M32=ON && cmake --build $BUILDDIR, run inside the image)"
[ -f "$REPO/SRC/PILOTS.EXE" ] \
  || die "SRC/PILOTS.EXE (original game data) not found - the game cannot start"

# --- helpers ---------------------------------------------------------------
# PID of PILOT_RE inside the container (slim image has no pidof).
game_pid() {
  podman exec "$NAME" sh -c \
    'for p in /proc/[0-9]*; do
       [ "$(cat "$p/comm" 2>/dev/null)" = PILOT_RE ] && { echo "${p#/proc/}"; break; }
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
if [ "$PULSE" = 1 ] && [ -z "${SDL_AUDIODRIVER:-}" ]; then
  AUDIODRV=pulseaudio               # SDL's driver name for the PulseAudio backend
fi
case "$MODE" in
  xvfb|display) VIDEODRV=${SDL_VIDEODRIVER:-x11} ;;
  *)            VIDEODRV=${SDL_VIDEODRIVER:-dummy} ;;
esac

# argv is built with set --: podman flags, then the image, then the game
# command - optional flags stay free of word-splitting pitfalls.
set -- run -d --name "$NAME" -v "$REPO:/w" -w /w/SRC

if [ "$MODE" = display ]; then
  [ -n "${DISPLAY:-}" ] || die "--display needs DISPLAY (graphical session or ssh -X)"
  case "$DISPLAY" in
    :*)
      # unix socket display (:0, :77, ...) - forward the socket directory
      XSOCK=$(readlink -f /tmp/.X11-unix 2>/dev/null || true)
      [ -n "$XSOCK" ] && [ -d "$XSOCK" ] \
        || die "no /tmp/.X11-unix on the host - is an X server running?"
      set -- "$@" -v "$XSOCK:/tmp/.X11-unix:ro" ;;
    localhost:*|127.0.0.1:*)
      # ssh -X listens on the host loopback - the container needs host net
      set -- "$@" --network host ;;
  esac
  # X cookie (XAUTHORITY, default ~/.Xauthority); without one the X server
  # must allow local clients: xhost +local: as the desktop user.
  XAUTHF=$(readlink -f "${XAUTHORITY:-$HOME/.Xauthority}" 2>/dev/null || true)
  if [ -n "$XAUTHF" ] && [ -f "$XAUTHF" ]; then
    set -- "$@" -e XAUTHORITY="$XAUTHF" -v "$XAUTHF:$XAUTHF:ro"
  fi
  set -- "$@" -e DISPLAY="$DISPLAY"
  echo "run_podman: forwarding DISPLAY=$DISPLAY (the window opens on that X server)"
fi

if [ "$PULSE" = 1 ]; then
  # Host PulseAudio socket mounted at the same path in the container (the
  # podrunner.sh --pulseaudio approach). Rootless podman keeps the host uid, so
  # the daemon accepts the connection as the same user; the cookie and
  # /etc/machine-id are mounted belt-and-braces. Socket location: PULSE_SERVER
  # if set (unix: or a plain path), else the session's XDG_RUNTIME_DIR.
  PSOCK=
  case "${PULSE_SERVER:-}" in
    unix:*) PSOCK=${PULSE_SERVER#unix:} ;;
    /*)     PSOCK=$PULSE_SERVER ;;
  esac
  if [ -z "$PSOCK" ]; then
    CAND=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native
    [ -S "$CAND" ] && PSOCK=$CAND
  fi
  [ -n "$PSOCK" ] && [ -S "$PSOCK" ] \
    || die "no PulseAudio socket - is pulseaudio (or pipewire-pulse) running on the host?"
  PD=$(dirname "$PSOCK")
  set -- "$@" -v "$PD:$PD" \
           -e PULSE_SERVER="unix:$PSOCK" \
           -e XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  COOKIE=${XDG_CONFIG_HOME:-$HOME/.config}/pulse/cookie
  if [ -f "$COOKIE" ]; then
    set -- "$@" -v "$COOKIE:/root/.config/pulse/cookie:ro"
  fi
  if [ -f /etc/machine-id ]; then
    set -- "$@" -v /etc/machine-id:/etc/machine-id:ro
  fi
  echo "run_podman: forwarding PulseAudio $PSOCK (SDL driver: $AUDIODRV)"
fi

set -- "$@" -e SDL_VIDEODRIVER="$VIDEODRV" -e SDL_AUDIODRIVER="$AUDIODRV" "$IMAGE"

if [ "$MODE" = xvfb ]; then
  # Direct Xvfb + exec instead of the stock xvfb-run: that wrapper hangs in
  # this image (it gets stuck even with `echo hello` - its display-ready wait
  # never completes) while Xvfb itself starts fine. Wait for the socket, give
  # up loudly, then exec the game as PID1 so the SIGUSR1 quit keeps working.
  set -- "$@" sh -c 'Xvfb :99 -screen 0 800x600x24 &
    n=0
    while [ ! -S /tmp/.X11-unix/X99 ]; do
      n=$((n + 1))
      if [ "$n" -ge 50 ]; then
        echo "run_podman: Xvfb failed to become ready" >&2
        exit 1
      fi
      sleep 0.1
    done
    DISPLAY=:99 exec "/w/$1/PILOT_RE"' plr-xvfb "$BUILDDIR"
else
  set -- "$@" "/w/$BUILDDIR/PILOT_RE"
fi

podman rm -f "$NAME" >/dev/null 2>&1 || true
podman "$@" >/dev/null || die "podman run failed"

trap 'on_quit' INT TERM
trap 'cleanup' EXIT

podman logs -f "$NAME" 2>&1 &      # live relay (background jobs ignore Ctrl+C)
LOGS_PID=$!

echo "run_podman: '$NAME' started in image '$IMAGE' (video=$VIDEODRV audio=$AUDIODRV)"
echo "run_podman: Ctrl+C for a clean quit, game log: $BUILDDIR/PILOT_RE_debug.log"

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
  echo "run_podman: check $BUILDDIR/PILOT_RE_debug.log (backtrace on stderr is in podman logs)" >&2
fi
exit "$EXIT_CODE"
