#!/bin/sh
# Build the self-contained Linux x86_64 binary the release ships.
#
# Usage:
#   sh scripts/build_linux_static_podman.sh              release -> build-static/PILOTS_RE_x64
#   sh scripts/build_linux_static_podman.sh debug        + debug log -> build-static-dbg/
#   sh scripts/build_linux_static_podman.sh image        rebuild the toolchain image only
#   sh scripts/build_linux_static_podman.sh clean        remove both build dirs
#
# WHY A CONTAINER: the build needs a Linux x86_64 toolchain plus the static
# SDL2 archive, and this project's other Linux builds already go through podman
# (see Containerfile and scripts/run_podman.sh). The image also carries the
# minimal static ffmpeg the release links; Containerfile.static explains why the
# distro's libavcodec.a cannot be used and what the built ffmpeg contains.
#
# WHAT THE RESULT DEPENDS ON: only glibc and the desktop libraries SDL2 itself
# uses (X11/Wayland, ALSA/PulseAudio) - no libSDL2-2.0.so.0 and no libav*.so.
# glibc is dynamic on purpose: SDL2 dlopen()s its video/audio drivers at
# runtime, and a statically linked glibc cannot do that. The script checks the
# result with ldd and fails if either library family leaked back in.
#
# The build dirs are separate per flavour for the same reason build32 and
# build64 are: both produce a file called PILOTS_RE_x64 and a debug binary must
# never be mistaken for the release one. The release file is stripped (8.4 MB
# -> 3.9 MB, measured); the debug one is not.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

IMAGE=${PILOTS_STATIC_IMAGE:-pilots-static}

build_image() {
    podman build -f Containerfile.static -t "$IMAGE" "$ROOT"
}

require_podman() {
    command -v podman >/dev/null 2>&1 || {
        echo "error: podman not found - the static Linux build runs in the image" >&2
        echo "       from Containerfile.static (podman build -f Containerfile.static -t $IMAGE .)" >&2
        exit 1
    }
    podman image exists "$IMAGE" 2>/dev/null || build_image
}

build() {
    builddir=$1
    debuglog=$2
    podman run --rm -v "$ROOT:/w" -w /w "$IMAGE" sh -c "
        set -e
        cmake -S . -B '$builddir' \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_FLAGS_RELEASE='-O2 -DNDEBUG' \
            -DPILOTS_STATIC_SDL2=ON \
            -DPILOTS_STATIC_AV=ON \
            -DPILOTS_STATIC_RUNTIME=ON \
            -DPILOTS_DEBUG_LOG=$debuglog \
        && cmake --build '$builddir' -j\$(nproc)
        echo '--- ldd:'
        ldd '$builddir/PILOTS_RE_x64'
        if ldd '$builddir/PILOTS_RE_x64' | grep -Eq 'lib(SDL2|avcodec|avformat|avutil|swscale|swresample)\.so'; then
            echo 'error: the binary still links SDL2/ffmpeg dynamically' >&2
            exit 1
        fi
        # The release asset is stripped: a static link of SDL2 and libav*
        # carries their symbol tables, which double the download for nothing.
        # The release build has no crash handler either (PILOTS_DEBUG_LOG=OFF),
        # so no backtrace could name those symbols anyway; the debug flavour
        # below keeps every symbol for exactly that.
        if [ '$debuglog' = OFF ]; then
            strip --strip-all '$builddir/PILOTS_RE_x64'
        fi
        ls -l '$builddir/PILOTS_RE_x64'
    "
    echo "built: $ROOT/$builddir/PILOTS_RE_x64"
}

require_podman || exit 1

case ${1:-build} in
    build)
        build build-static OFF
        ;;
    debug)
        build build-static-dbg ON
        ;;
    image)
        build_image
        ;;
    clean)
        rm -rf "$ROOT/build-static" "$ROOT/build-static-dbg"
        echo "removed build-static/ and build-static-dbg/"
        ;;
    *)
        echo "usage: $0 [build|debug|image|clean]" >&2
        exit 1
        ;;
esac
