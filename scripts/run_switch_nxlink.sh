#!/bin/sh
# Upload PILOT_RV.nro to the Switch and RUN it from the PC, over the network.
#
# Usage:
#   sh scripts/run_switch_nxlink.sh                  build-switch/PILOT_RV.nro
#   sh scripts/run_switch_nxlink.sh debug            build-switch-dbg/PILOT_RV.nro
#   sh scripts/run_switch_nxlink.sh <path.nro>       any .nro by path
#   SWITCH_HOST=192.168.1.5 sh scripts/run_switch_nxlink.sh debug
#
# WHAT HAS TO BE HAPPENING ON THE CONSOLE FIRST
# hbmenu has to be open with its netloader switched on: press **Y** in hbmenu (B
# switches it off again; on a touch screen, swipe down). That is the one thing
# that cannot be done from the PC. After that this script uploads and starts the
# app as often as you like without touching the console again - which is the whole
# point: the FTP route means copy, find the icon, press A, wait, come back.
#
# THE BONUS THAT MATTERS MORE THAN THE LAUNCH
# An app started this way is told who launched it, and
# SdlSwitchAttachDebugChannel (switch_libnx.c) redirects stderr to that host.
# Every DBG_* line goes to stderr too (OutputDebugStringA in win32_shim.c), so the
# debug log arrives HERE, live, while the app runs - no FTP round trip, no
# waiting for the app to die before its own numbers can be read.
#
# The log file on the card and the Atmosphere crash report are still written, and
# still the right thing to pull after a crash: the socket dies with the process, so
# a crash shows up here as the log simply stopping mid-line.
#
# WHY --network host
# nxlink announces itself to hbmenu, then hbmenu connects BACK to reach it. With
# podman's default NAT the console would be dialling the host machine and never
# the container, so the upload would hang. One-way traffic (which is all FTP needs,
# hence scripts/deploy_switch.sh working fine without this) is not enough here.
#
# nxlink ships with the devkitA64 image (tools/bin/nxlink), the same image the
# build uses, so nothing is installed for this.

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${PILOTS_SWITCH_IMAGE:-docker.io/devkitpro/devkita64:latest}
HOST=${SWITCH_HOST:-192.168.1.112}
NXLINK=/opt/devkitpro/tools/bin/nxlink

case "${1:-}" in
  debug)  NRO="$REPO/build-switch-dbg/PILOT_RV.nro" ;;
  -h|--help) sed -n '2,44p' "$0"; exit 0 ;;
  "")   NRO="$REPO/build-switch/PILOT_RV.nro" ;;
  *.nro) NRO="$1" ;;
  *) echo "run_switch_nxlink: what is '$1'? (no argument, 'debug', or a .nro path)" >&2; exit 2 ;;
esac

[ -f "$NRO" ] || {
  echo "run_switch_nxlink: no nro at $NRO" >&2
  echo "                   build it with: sh scripts/build_switch_podman.sh [debug]" >&2
  exit 1
}
command -v podman >/dev/null 2>&1 || { echo "run_switch_nxlink: podman not found" >&2; exit 1; }

echo "nxlink: $NRO -> $HOST  (hbmenu must be sitting in netloader mode: Y)"
# ${NRO#"$REPO/"}: the repo is what gets mounted, so nxlink has to see the path
# through the mount, not through this checkout.
exec podman run --rm -it --network host -v "$REPO:/data" "$IMAGE" \
  "$NXLINK" -a "$HOST" -r 5 "/data/${NRO#"$REPO/"}"
