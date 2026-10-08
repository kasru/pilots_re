# Build/run environment for the Linux port of the Pilots reconstruction.
# The host stays untouched (no i386 packages): everything - toolchain, SDL2 i386,
# ffmpeg i386 - lives in this image; source and data are bind-mounted at run time.
#
#   podman build -t pilots-build .
#   podman run --rm -v "$PWD:/w" -w /w pilots-build cmake -S . -B build && cmake --build build
FROM debian:bookworm-slim

# i386 foreign arch BEFORE apt-get update so libsdl2-dev:i386 & co resolve in one pass.
RUN dpkg --add-architecture i386 \
 && apt-get update \
 && apt-get install -y --no-install-recommends \
      gcc g++ gcc-multilib g++-multilib \
      make cmake pkg-config \
      libsdl2-dev:i386 \
      libavcodec-dev:i386 libavformat-dev:i386 libavutil-dev:i386 libswscale-dev:i386 \
      xvfb xauth \
 && rm -rf /var/lib/apt/lists/*
