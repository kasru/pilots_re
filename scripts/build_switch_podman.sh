#!/bin/sh
# Cross-compile PILOT_RV.nro for the Nintendo Switch, inside the
# devkitpro/devkita64 podman container.
#
# Usage:
#   sh scripts/build_switch_podman.sh             configure + build the .nro
#   sh scripts/build_switch_podman.sh clean       make clean
#   sh scripts/build_switch_podman.sh distclean   drop the build dir entirely
#
# Why a container and not a local toolchain: devkitA64, libnx and
# portlibs/switch must all come out of one dkp-pacman tree, and the host carries
# none of them. The docker.io/devkitpro/devkita64 image already ships
# everything this project needs:
#
#   switch-sdl2 2.28.5   portlibs/switch/lib/pkgconfig/sdl2.pc
#   switch-ffmpeg 7.1    libavcodec.pc / libavformat.pc / libavutil.pc /
#                        libswscale.pc  (static .a, which is what we link anyway)
#   switch-tools         elf2nro, nacptool
#   libnx 4.12           plus cmake/Platform/NintendoSwitch.cmake, which defines
#                        nx_create_nacp/nx_create_nro - the .nro step itself
#
# So unlike the wiliwili-based builds this needs NO custom packages downloaded
# into the repo root: the reconstruction uses only SDL2 and the plain libav*
# libraries, no libmpv, no libass, no forwarder/NSP tooling.
#
# No `dkp-pacman -Syu` either: the image is a snapshot that already works, and
# a full system update on a fresh container is the usual way to break it.
#
# Clean has to run INSIDE the container. CMake bakes the mount path /data into
# the generated Makefiles, so `make clean` on the host fails on `cd /data/...`.
# distclean is a plain host rm and needs no container.

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${PILOTS_SWITCH_BUILDDIR:-build-switch}
IMAGE=${PILOTS_SWITCH_IMAGE:-docker.io/devkitpro/devkita64:latest}
TOOLCHAIN=/opt/devkitpro/cmake/Switch.cmake
MODE=${1:-build}

case "$MODE" in
  clean)
    [ -f "$REPO/$BUILD_DIR/Makefile" ] \
      || { echo "build_switch_podman: nothing to clean ($BUILD_DIR has no Makefile)"; exit 0; }
    exec podman run --rm -v "$REPO:/data" "$IMAGE" \
      bash -c "make -C /data/${BUILD_DIR} clean"
    ;;
  distclean)
    rm -rf "$REPO/$BUILD_DIR"
    echo "build_switch_podman: removed $BUILD_DIR"
    exit 0
    ;;
  build) ;;
  -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
  *) echo "build_switch_podman: unknown mode '$MODE' (use: build | clean | distclean)" >&2; exit 2 ;;
esac

command -v podman >/dev/null 2>&1 \
  || { echo "build_switch_podman: podman not found" >&2; exit 1; }
podman image exists "$IMAGE" 2>/dev/null \
  || { echo "build_switch_podman: image '$IMAGE' not found (podman pull $IMAGE)" >&2; exit 1; }

# Both steps in one --rm container: the tree is configured with the mount path
# /data, so configure and build have to agree on it. PILOTS_DEBUG_LOG defaults
# OFF for the Switch (app0: is read-only), -DPLATFORM_SWITCH=ON is what makes
# CMakeLists add the nx_create_nro step on top of the regular executable.
podman run --rm -v "$REPO:/data" "$IMAGE" bash -c "
    set -e
    cd /data
    cmake -S . -B ${BUILD_DIR} \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN} \
      -DPLATFORM_SWITCH=ON
    make -C ${BUILD_DIR} PILOT_RV_nro -j\$(nproc)
"

NRO="$REPO/$BUILD_DIR/PILOT_RV.nro"
if [ -f "$NRO" ]; then
  echo ""
  echo "build_switch_podman: built $NRO"
  ls -lh "$NRO"
else
  echo "build_switch_podman: $NRO was not produced" >&2
  exit 1
fi
