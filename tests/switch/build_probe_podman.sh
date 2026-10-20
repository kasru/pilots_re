#!/bin/sh
# Build the memory-map probe (tests/switch/memmap_probe.c) into a .nro, and
# optionally put it on the console.
#
#   sh tests/switch/build_probe_podman.sh             build only
#   sh tests/switch/build_probe_podman.sh --upload    build, then upload as PILOT_MAP.nro
#   sh tests/switch/build_probe_podman.sh --grow      upload, and arm the heap-growth test
#   sh tests/switch/build_probe_podman.sh --nogrow    disarm it again
#
# WHY IT EXISTS: the engine keeps a pool pointer in a real 32-bit field and reads it
# back as (INT_PTR), so that pool has to sit below 2 GB - and the first hardware run
# measured that the .nro (and so its .bss) is loaded at 0x78c3fa888-ish, far above it.
# Whether the console can produce low memory at all has never been measured. The
# probe measures, and reads like an explanation: tests/switch/memmap_probe.c.
#
# READING THE ANSWER: the probe writes PILOT_MAP.log next to its own .nro, so
#   sh scripts/deploy_switch.sh --ls        # see that it is there
#   curl -O ftp://$SWITCH_HOST:$SWITCH_PORT$SWITCH_DIR/PILOT_MAP.log
# After a run, with the app closed (FTP and a running app do not coexist).
#
# It is a SEPARATE icon in hbmenu ("Pilots memmap probe") and never touches the game;
# hbmenu only scans /switch when it starts, so restart hbmenu after the first upload.
#
# --grow writes the marker file that arms the one destructive thing the probe does:
# svcSetHeapSize with a real size, which remaps the process heap and does not preserve
# its contents - and hbmenu's heap is in that process. It answers "can we get low
# memory at all" and can make the return to hbmenu ugly; --nogrow takes it away.

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
IMAGE=${PILOTS_SWITCH_IMAGE:-docker.io/devkitpro/devkita64:latest}
BUILDDIR=${PROBE_BUILDDIR:-build-memmap}
TOOLCHAIN=/opt/devkitpro/cmake/Switch.cmake

HOST=${SWITCH_HOST:-192.168.1.112}
PORT=${SWITCH_PORT:-5000}
DIR=${SWITCH_DIR:-/switch/pilots}
FTP="ftp://${HOST}:${PORT}"

MODE=build
case "${1:-}" in
  "")        MODE=build ;;
  --upload)  MODE=upload ;;
  --grow)    MODE=grow ;;
  --nogrow)  MODE=nogrow ;;
  -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
  *) echo "build_probe_podman: what is '$1'? (no argument, --upload, --grow, --nogrow)" >&2; exit 2 ;;
esac

command -v podman >/dev/null 2>&1 || { echo "build_probe_podman: podman not found" >&2; exit 1; }

if [ "$MODE" != "nogrow" ]; then
  echo "probe: configuring ${BUILDDIR} (devkitA64, tests/switch)"
  podman run --rm -v "${REPO}:/data" "${IMAGE}" bash -c \
    "cmake -S /data/tests/switch -B /data/${BUILDDIR} -DPLATFORM_SWITCH=ON -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}" \
    || { echo "build_probe_podman: configure failed" >&2; exit 1; }

  echo "probe: building"
  podman run --rm -v "${REPO}:/data" "${IMAGE}" bash -c \
    "cmake --build /data/${BUILDDIR} -j \$(nproc)" \
    || { echo "build_probe_podman: build failed" >&2; exit 1; }

  PROBE_NRO="${REPO}/${BUILDDIR}/PILOT_MAP.nro"
  [ -f "${PROBE_NRO}" ] || { echo "build_probe_podman: no ${PROBE_NRO} after the build" >&2; exit 1; }
  ls -l "${PROBE_NRO}"
fi

if [ "$MODE" = "build" ]; then
  echo "built ${PROBE_NRO} (add --upload to put it on the console)"
  exit 0
fi

echo "probe: uploading ${DIR}/PILOT_MAP.nro to ${HOST}:${PORT}"
curl --fail -T "${PROBE_NRO}" "${FTP}${DIR}/PILOT_MAP.nro" \
  || { echo "build_probe_podman: upload failed (app running? FTP is off while an app runs)" >&2; exit 1; }

case "$MODE" in
  grow)
    printf 'armed\n' > /tmp/PILOT_PROBE_GROWHEAP
    curl --fail -T /tmp/PILOT_PROBE_GROWHEAP "${FTP}${DIR}/PILOT_PROBE_GROWHEAP" \
      || { echo "build_probe_podman: could not write the marker" >&2; exit 1; }
    echo "uploaded, heap-growth test ARMED (hbmenu may not survive the exit - that is the cost of the answer)"
    ;;
  nogrow)
    curl -s -Q "DELE ${DIR}/PILOT_PROBE_GROWHEAP" "${FTP}" >/dev/null 2>&1 || true
    echo "marker removed; the probe will only read"
    ;;
  *)
    echo "uploaded, read-only mode (no heap growth)"
    ;;
esac

echo "restart hbmenu to pick up the new icon, run it, then:"
echo "  curl -o /tmp/PILOT_MAP.log \"${FTP}${DIR}/PILOT_MAP.log\""
