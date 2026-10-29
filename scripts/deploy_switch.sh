#!/bin/bash
# Deploy PILOTS_RE.nro to the Nintendo Switch over FTP and reset the debug log.
#
# Usage (from any directory, on the machine that builds the nro):
#   scripts/deploy_switch.sh              upload the nro, archive + delete the log
#   scripts/deploy_switch.sh --pull-log   download the console log to $LOG_LOCAL
#   scripts/deploy_switch.sh --pull-crash download the newest Atmosphère crash report
#   scripts/deploy_switch.sh --pull-shot  download the newest screenshot the console took
#   scripts/deploy_switch.sh --ls         list what is actually in the remote dir
#   scripts/deploy_switch.sh --push-data  upload PILOTS.EXE and MOVIES/ (one-time)
#
# Environment overrides:
#   SWITCH_HOST  console address, default 192.168.1.112
#   SWITCH_PORT  sys-ftpd port,     default 5000
#   SWITCH_DIR   remote app dir,    default /switch/pilots - the same directory
#                                     the port itself falls back to as
#                                     sdmc:/switch/pilots when it finds
#                                     PILOTS.EXE in it
#   BUILDDIR     nro directory,     default build-switch; for the debug build
#                                     use BUILDDIR=build-switch-dbg
#   NRO          full path to the .nro, wins over BUILDDIR
#
# Notes:
#   - Close the app first: hbmenu's FTP server and a running app do not coexist.
#   - The retries are there because sys-ftpd is occasionally slow to answer.
#   - hbmenu scans /switch only at start: after the very first upload into a new
#     directory, restart hbmenu or the icon will not show up.
#   - The console log is PILOTS_RE_debug.log *in this same directory*. There is no
#     /config/... path here: GetModuleFileNameA has no /proc/self/exe on newlib,
#     so it returns the relative "PILOTS_RE.EXE" (win32_shim.c), debug_log.h turns
#     that into "PILOTS_RE_debug.log" in the CWD, and libnx's __libnx_init_cwd()
#     has already chdir'd to the nro's own directory (fs_dev.c). Saves land there
#     for the same reason.
#   - Only a build configured with -DPILOTS_DEBUG_LOG=ON writes that log, i.e.
#     `sh scripts/build_switch_podman.sh debug`. The plain build is Release and
#     the DBG_* macros are no-ops in it, so it runs silent - checked below,
#     because otherwise "the log never appeared" is indistinguishable from "the
#     path is wrong".
#   - On a crash, take BOTH: the log says how far the run got (it is flushed line
#     by line), the crash report says where the CPU stopped. The log cannot say
#     the second part - a CPU exception on newlib is not a POSIX signal, so the
#     SIGSEGV handler of debug_log.h never runs there, however tidy it looks on
#     Linux. --pull-crash is the crash channel on this platform.
#
# Modelled on switch-filmix's .code/deploy.sh, with the app in its own directory
# (the game data has to come there anyway) and the log next to the nro instead of
# under /config, which is where an NSO would keep it.

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"

HOST="${SWITCH_HOST:-192.168.1.112}"
PORT="${SWITCH_PORT:-5000}"
DIR="${SWITCH_DIR:-/switch/pilots}"
NRO="${NRO:-$REPO/${BUILDDIR:-build-switch}/PILOTS_RE.nro}"

FTP="ftp://$HOST:$PORT"
REMOTE_NRO="$DIR/PILOTS_RE.nro"
LOG_REMOTE="$DIR/PILOTS_RE_debug.log"
LOG_LOCAL="${TMPDIR:-/tmp}/PILOTS_RE_debug.log"
ARCHIVE_DIR="${TMPDIR:-/tmp}/pilots_deploy_logs"
CRASH_REMOTE="/sdmc:/atmosphere/crash_reports/"     # sys-ftpd exposes the devices
CRASH_LOCAL="${TMPDIR:-/tmp}/PILOTS_RE_crash.log"    #   as top-level dirs
DUMP_LOCAL="${TMPDIR:-/tmp}/pilots_mix.wav"         # the mixer's output recorder, see --pull-dump
SHOT_LOCAL="${TMPDIR:-/tmp}/PILOTS_RE_shot.jpg"

# Read-only modes first: none of them needs a local nro.
case "${1:-}" in
  --pull-crash)
    # Atmosphere drops a report for every crash into
    # sdmc:/atmosphere/crash_reports/<tick>_<program-id>.log. It holds what our own
    # log cannot - the abort address, every register and the stack scan as
    # "PILOTS_RE + 0x<offset>" - because a CPU exception on newlib is not a POSIX
    # signal, so the SIGSEGV handler in debug_log.h is decoration there. The tick
    # in the name is monotonic, so the newest report is the largest prefix.
    NEWEST=$(curl -s --connect-timeout 8 --max-time 30 "${FTP}${CRASH_REMOTE}" \
             | awk '{print $NF}' | grep -E '^[0-9]+_.*\.log$' | sort -t_ -k1,1n | tail -1)
    if [ -z "${NEWEST}" ]; then
      echo "no crash reports in ${CRASH_REMOTE} (nothing crashed? FTP off?)" >&2
      exit 1
    fi
    if ! curl -s --connect-timeout 8 --max-time 40 "${FTP}${CRASH_REMOTE}${NEWEST}" \
         -o "${CRASH_LOCAL}"; then
      echo "ERROR: could not download ${NEWEST}" >&2
      exit 1
    fi
    ls -l "${CRASH_LOCAL}"
    echo "decode it: scripts/decode_switch_crash.sh ${CRASH_LOCAL}"
    exit 0
    ;;

  --pull-shot)
    # The newest screenshot. Atmosphere's capture button (the small one under the right
    # stick) writes a JPEG to sdmc:/switch/.captures/, and this is the only way to LOOK
    # at the picture from here: the log can say how far a run got, only the frame can
    # say what is wrong with it. Names are "Screenshot <n>.jpg", so the newest is the
    # largest number, and the space has to go back as %20 for the URL.
    SHOTS="/sdmc:/switch/.captures/"
    NEWEST=$(curl -s --connect-timeout 8 --max-time 30 "${FTP}${SHOTS}" \
             | grep -i '\.jpg' \
             | awk '{ n = $NF; sub(/\.jpg/, "", n); if (n + 0 > best) { best = n + 0; keep = $0 } } END { print keep }')
    if [ -z "${NEWEST}" ]; then
      echo "no screenshots in ${SHOTS} (capture button not pressed? FTP off?)" >&2
      exit 1
    fi
    ENC=$(printf '%s' "${NEWEST}" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' -e 's/ /%20/g')
    if ! curl -s --connect-timeout 8 --max-time 60 "${FTP}${SHOTS}${ENC}" -o "${SHOT_LOCAL}"; then
      echo "ERROR: could not download ${NEWEST}" >&2
      exit 1
    fi
    ls -l "${SHOT_LOCAL}"
    exit 0
    ;;

  --pull-log)
    PULLED=0
    for i in 1 2 3 4 5; do
      echo "try $i"
      rm -f "${LOG_LOCAL}"
      if curl -s --connect-timeout 8 --max-time 30 "${FTP}${LOG_REMOTE}" \
           -o "${LOG_LOCAL}" && [ -s "${LOG_LOCAL}" ]; then
        PULLED=1
        break
      fi
      sleep 6
    done
    if [ "${PULLED}" != "1" ]; then
      echo "ERROR: no log at ${LOG_REMOTE}" >&2
      echo "       - release nro? then there is nothing to fetch" >&2
      echo "       - app still running? FTP and the app do not coexist" >&2
      exit 1
    fi
    ls -l "${LOG_LOCAL}"
    exit 0
    ;;

  --pull-dump)
    # The mixer's output recorder writes next to the nro (the same directory the debug log is in),
    # and unlike the log it is written once and never appended, so one pull is enough - but only
    # after a run that reached an effect, because that is what arms the recorder.
    PULLED=0
    for i in 1 2 3; do
      echo "try $i"
      rm -f "${DUMP_LOCAL}"
      if curl -s --connect-timeout 8 --max-time 120 "${FTP}${DIR}/pilots_mix.wav" \
           -o "${DUMP_LOCAL}" && [ -s "${DUMP_LOCAL}" ]; then
        PULLED=1
        break
      fi
      sleep 6
    done
    if [ "${PULLED}" != "1" ]; then
      echo "ERROR: no dump at ${DIR}/pilots_mix.wav" >&2
      echo "       - the run never reached a sound effect (the recorder waits for one)" >&2
      echo "       - or it is a release nro, which has no recorder" >&2
      exit 1
    fi
    ls -l "${DUMP_LOCAL}"
    exit 0
    ;;

  --ls)
    curl -s --connect-timeout 8 --max-time 30 "${FTP}${DIR}/"
    exit 0
    ;;

  --push-data)
    [ "${SWITCH_PUSH_DATA:-}" = 1 ] \
      || { echo "refusing: this uploads ~58 MB. run it as SWITCH_PUSH_DATA=1 scripts/deploy_switch.sh --push-data" >&2; exit 2; }
    [ -f "${REPO}/SRC/PILOTS.EXE" ] || { echo "missing ${REPO}/SRC/PILOTS.EXE" >&2; exit 1; }
    curl -s -Q "MKD ${DIR}/MOVIES" "${FTP}${DIR}/" >/dev/null 2>&1 || true
    for f in "${REPO}/SRC/PILOTS.EXE" "${REPO}"/SRC/MOVIES/*; do
      [ -f "${f}" ] || continue
      target="${DIR}/${f#"$REPO"/SRC/}"
      echo "upload ${f} -> ${target}"
      curl -s --connect-timeout 8 --max-time 600 -T "${f}" "${FTP}${target}"
    done
    echo "DATA_OK"
    exit 0
    ;;
esac

[ -f "${NRO}" ] || {
  echo "deploy_switch: no nro at ${NRO}" >&2
  echo "               build it with: sh scripts/build_switch_podman.sh [debug]" >&2
  exit 1
}

# The worst thing this script can upload is an OLD nro. The console then runs a build that does not
# contain the thing being tested, and the log it writes reads like evidence about that thing. This
# happened: a Switch build failed on a compile error, the previous nro was still sitting in the build
# dir, and the deploy reported UPLOAD_OK for it. A stale binary is not a slower path to the same
# answer, it is a different experiment, so refuse and say which sources are newer.
NEWER_SRC=$(find "${REPO}/SRC" -maxdepth 1 \( -name '*.c' -o -name '*.h' \) -newer "${NRO}" -print 2>/dev/null | head -3)
if [ -n "${NEWER_SRC}" ] && [ "${PILOTS_DEPLOY_FORCE}" != "1" ]; then
  echo "STALE_NRO: ${NRO} is older than these sources - build first (sh scripts/build_switch_podman.sh debug), or PILOTS_DEPLOY_FORCE=1:" >&2
  echo "${NEWER_SRC}" | sed 's/^/  /' >&2
  exit 1
fi

# A Release nro carries no debug-log code at all, so there is nothing to pull
# afterwards. Say so before the upload, not after a failed --pull-log.
if ! grep -qa 'Debug logging started' "${NRO}"; then
  echo "WARNING: this nro was built without -DPILOTS_DEBUG_LOG=ON:" >&2
  echo "         it runs silent on the console, --pull-log will find nothing." >&2
fi

UPLOADED=0
for i in 1 2 3 4 5; do
  echo "try $i"
  if curl -s --connect-timeout 8 --max-time 120 -T "${NRO}" "${FTP}${REMOTE_NRO}"; then
    echo "UPLOAD_OK ${REMOTE_NRO}"
    UPLOADED=1
    break
  fi
  sleep 5
done

if [ "${UPLOADED}" != "1" ]; then
  echo "ERROR: failed to upload ${NRO} after 5 attempts (FTP unavailable?)" >&2
  exit 1
fi

# Never lose the previous run's log: archive it locally before deleting.
mkdir -p "${ARCHIVE_DIR}"
ARCHIVE_FILE="${ARCHIVE_DIR}/PILOTS_RE_debug_$(date +%Y%m%d_%H%M%S).log"
if curl -s --connect-timeout 8 --max-time 60 "${FTP}${LOG_REMOTE}" \
     -o "${ARCHIVE_FILE}" && [ -s "${ARCHIVE_FILE}" ]; then
  echo "LOG_ARCHIVED (${ARCHIVE_FILE})"
  if curl -s -Q "DELE ${LOG_REMOTE}" "${FTP}/" >/dev/null 2>&1; then
    echo "LOG_DELETED"
  else
    # We just read this file, so it is there and the delete really did fail: the only way a
    # file on the SD refuses to go away is a process holding it open, i.e. the game is
    # running. Worth a loud word, and only this case is.
    echo "LOG_DELETE_FAILED (the log is there and was archived; the game is running and holds it open -" >&2
    echo "                  close it before starting the new nro, the old image is still in memory)" >&2
  fi
else
  # curl happily fetches an empty body for a path that does not exist, which is why the test
  # is -s and not just the exit code. No file is the ordinary state after a second deploy:
  # the first one took the log away and no run has written a new one since. Saying "delete
  # failed" here reads like a fault and is not one.
  rm -f "${ARCHIVE_FILE}"
  echo "LOG_ABSENT (nothing to archive - no run since the last deploy, or it was already taken)"
fi
exit 0
