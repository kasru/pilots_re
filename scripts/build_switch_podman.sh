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
# none of them.
#
# WHERE SDL2 AND FFMPEG COME FROM: they are already in the image. devkitPro's
# devkita64 image ships the whole Switch SDK, not just a compiler - 77
# switch-* packages baked in at image build time, pulled from the signed
# dkp-libs repo with devkitPro's own keyring. The ones this project uses:
#
#   switch-sdl2  2.28.5-4   sdl2.pc -> libSDL2.a. Its Cflags also carry
#                           -D__SWITCH__, one of the two places that define
#                           it (devkitPro's Platform/NintendoSwitch.cmake
#                           passes the other), which is what every
#                           #ifdef __SWITCH__ in the port layer keys off.
#   switch-ffmpeg 7.1-5     libavcodec/libavformat/libavutil/libswscale .pc
#                           and .a, linked transitively against dav1d, bz2,
#                           zlib and swresample - also all in the image.
#                           Crucially it contains ff_msrle_decoder: MS RLE is
#                           the compression the game's own AVI files use, and
#                           mci_ffmpeg.c decodes them through it.
#   switch-tools 1.13.1-1   elf2nro + nacptool, i.e. the .nro step itself
#   libnx 4.12.0-1          plus cmake/Platform/NintendoSwitch.cmake, which
#                           defines nx_generate_nacp / nx_create_nro
#   switch-pkg-config       aarch64-none-elf-pkg-config, what FindPkgConfig
#                           runs once the toolchain file has set it
#
# So unlike the wiliwili-style Switch builds this downloads NOTHING into the
# repo root and never runs dkp-pacman. Those scripts curl .pkg.tar.zst files
# (switch-libmpv, switch-libass, hacBrewPack, switch-nspmini, and their own
# switch-ffmpeg) and `dkp-pacman -U` them, because libmpv/libass and the NSP
# forwarder tooling are simply not part of the SDK and they want a differently
# configured ffmpeg on top. This project needs only SDL2 and the plain libav*,
# which the SDK already provides - so there is nothing to fetch and nothing to
# keep in the tree.
#
# No `dkp-pacman -Syu` either: the image is a snapshot that already works, and
# a full system update on a fresh container is the usual way to break it.
#
# The one weak point this script defends against: the image is pinned only as
# :latest, so a newer image on another machine could ship a different package
# set. The build runs a pre-flight check first and fails with the exact package
# names instead of a confusing CMake "could not find" error. If that ever
# fires, the packages have to go INTO THE IMAGE - a --rm container forgets
# anything dkp-pacman installs, which is exactly why the filmix scripts
# reinstall theirs on every single build. Make a derived image:
#
#   podman run --name dkp-deps docker.io/devkitpro/devkita64 \
#       dkp-pacman -S --noconfirm switch-sdl2 switch-ffmpeg
#   podman commit dkp-deps docker.io/devkitpro/devkita64:pilots
#   podman rm dkp-deps
#   PILOTS_SWITCH_IMAGE=docker.io/devkitpro/devkita64:pilots \
#       sh scripts/build_switch_podman.sh
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
  -h|--help) sed -n '2,70p' "$0"; exit 0 ;;
  *) echo "build_switch_podman: unknown mode '$MODE' (use: build | clean | distclean)" >&2; exit 2 ;;
esac

command -v podman >/dev/null 2>&1 \
  || { echo "build_switch_podman: podman not found" >&2; exit 1; }
podman image exists "$IMAGE" 2>/dev/null \
  || { echo "build_switch_podman: image '$IMAGE' not found (podman pull $IMAGE)" >&2; exit 1; }

# One --rm container for the pre-flight check, the configure and the build: the
# tree is configured with the mount path /data, so all three have to agree on
# it. PILOTS_DEBUG_LOG defaults OFF for the Switch (app0: is read-only).
# -DPLATFORM_SWITCH=ON is what makes CMakeLists add the nx_create_nro step on
# top of the regular executable.
podman run --rm -v "$REPO:/data" "$IMAGE" bash -c "
    set -e

    # Pre-flight: every pkg-config module CMake is about to ask for must
    # resolve here. They come with the image (see the header), so in practice
    # this never fires - it exists so that a differently-stocked image fails
    # loudly and with package names, before cmake has produced ten screens of
    # output about it.
    PC=/opt/devkitpro/portlibs/switch/bin/aarch64-none-elf-pkg-config
    missing=
    for m in sdl2 libavcodec libavformat libavutil libswscale; do
      \"\$PC\" --exists \"\$m\" 2>/dev/null || missing=\"\$missing \$m\"
    done
    if [ -n \"\$missing\" ]; then
      echo \"build_switch_podman: this image lacks:\$missing\" >&2
      echo '  they ship with the image; a --rm container forgets installs, so put' >&2
      echo '  them into the image itself - see WHERE SDL2 AND FFMPEG COME FROM' >&2
      echo '  at the top of scripts/build_switch_podman.sh' >&2
      exit 1
    fi
    echo \"deps: sdl2 + libav{codec,format,util} + libswscale resolve in the image\"

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
