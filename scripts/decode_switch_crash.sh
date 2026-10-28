#!/bin/bash
# Turn an Atmosphere crash report into function names and source lines.
#
#   scripts/decode_switch_crash.sh /tmp/PILOT_RE_crash.log [elf]
#
#   elf defaults to build-switch-dbg/PILOT_RE.elf, which is the image the debug
#   .nro was made from; pass build-switch/PILOT_RE.elf for a release report.
#
# WHY THE OFFSETS GO STRAIGHT TO addr2line: the report lists PC, LR and a scanned
# stack of return addresses as "PILOT_RE + 0x<offset>". A .nro is a
# position-independent image and switch.ld starts it at zero (__start__ = 0x0), so
# the module-relative offset the report prints is already the virtual address in
# the ELF - there is no load slide to subtract. That is the only lucky thing about
# debugging on this console.
#
# What must NOT be decoded: the abort target ("Address:") is data, and "Program ID"
# is an identifier, not a location. The target is still the number to look at -
# compare it against the "out= ... end=" line the app logged for the same run
# (scripts/deploy_switch.sh --pull-log). A target a little past "end" means a size
# was read too big; a target far outside the image means the stream itself was
# parsed wrong. This script prints both so they sit next to each other.
#
# The binutils exist only inside the devkitA64 image, so addr2line runs through
# podman with the repo mounted - the same arrangement the build uses, and the
# reason this is a script rather than one command.

set -eu

REPO=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
IMAGE=${PILOTS_SWITCH_IMAGE:-docker.io/devkitpro/devkita64:latest}
ADDR2LINE=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-addr2line

REPORT=${1:-}
ELF=${2:-build-switch-dbg/PILOT_RE.elf}

[ -n "${REPORT}" ] || { echo "usage: decode_switch_crash.sh <crash-report> [elf]" >&2; exit 2; }
[ -f "${REPORT}" ] || { echo "no such report: ${REPORT}" >&2; exit 1; }
case "${ELF}" in
  "${REPO}"/*) ELF=${ELF#"$REPO"/} ;;
  /*) echo "the elf has to be inside the repo for the container mount: ${ELF}" >&2; exit 1 ;;
esac
[ -f "${REPO}/${ELF}" ] || {
  echo "no ELF at ${ELF} - the report is only decodable against the image it came from" >&2
  echo "(build one: sh scripts/build_switch_podman.sh debug)" >&2
  exit 1
}

echo "=== the abort ==="
grep -E "Result:|Type:|Address:|Fault Address" "${REPORT}" | head -6

echo
echo "=== frames (${ELF}) ==="
# Label + offset for every line that names our module: PC, LR, ReturnAddress[NN].
# Duplicated offsets are kept on purpose, a scanned stack repeats a frame and the
# repetition is itself a clue.
PAIRS=$(sed -nE 's/^[[:space:]]*([A-Za-z0-9_]+(\[[0-9]+\])?):[[:space:]]+[0-9a-fA-F]+[[:space:]]*\(PILOT_RE \+ (0x[0-9a-fA-F]+)\).*/\1 \3/p' \
        "${REPORT}" | head -24)
[ -n "${PAIRS}" ] || { echo "no PILOT_RE frames in this report - wrong build's report?" >&2; exit 1; }

# The elf goes FIRST so the inner loop can hold it in a named variable: with the
# frames before it, its position would depend on how many frames there are.
podman run --rm -v "${REPO}:/data" "${IMAGE}" bash -c '
  elf=$1; shift
  while [ "$#" -gt 1 ]; do
    printf "%-18s %-10s " "$1" "$2"
    '"${ADDR2LINE}"' -f -C -e "/data/${elf}" "$2" | tr "\n" " "
    echo
    shift 2
  done
' -- "${ELF}" ${PAIRS}

echo
echo "=== stack below the frames (raw) ==="
grep -cE "ReturnAddress\[" "${REPORT}" | sed 's/^/return addresses scanned: /'
