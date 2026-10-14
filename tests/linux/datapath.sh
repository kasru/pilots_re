#!/bin/sh
# -datapath <dir> - where the game looks for its resources. Linux counterpart of
# tests/windows/datapath.ps1.
#
# Without the switch nothing changes: the engine asks for .\PILOTS.EXE and
# .\movies\... as it always did, resolved against the process CWD. With it,
# WinMain changes the working directory BEFORE the game opens anything, so every
# CWD-relative lookup follows at once while main.c still strcpy's the literal
# ".\PILOTS.EXE" - no game-code change, one place in the port layer.
#
# The Linux-specific half is the shim: SetCurrentDirectoryA lives in
# win32_shim.c next to GetFullPathNameA, and resolves its argument the same way
# every file open does (case-insensitive walk), so -datapath ./PILOTS lands on
# ./pilots even on a case-sensitive filesystem.
#
# The debug log follows the image (/proc/self/exe via GetModuleFileNameA), not
# the CWD, so every case reads the same file - which is what makes the checks
# below possible at all.
#
#   sh tests/linux/datapath.sh
# Needs: a built $BUILDDIR/PILOT_RV and SRC/ with the original game data.
case "$0" in
  */*) cd "$(dirname "$0")/../.." || exit 1 ;;
esac
BUILDDIR=${PILOTS_BUILDDIR:-build64}
DISP=${PILOTS_DPDISPLAY:-:96}
SW=1024; SH=768
LOG=$(pwd)/$BUILDDIR/PILOT_RV_debug.log
EXE=$(pwd)/$BUILDDIR/PILOT_RV

[ -x "$EXE" ] || { echo "no $EXE - build it first"; exit 1; }
[ -f SRC/PILOTS.EXE ] || { echo "no SRC/PILOTS.EXE - game data missing"; exit 1; }

pgrep -f "Xvfb $DISP" >/dev/null 2>&1 || {
  Xvfb "$DISP" -screen 0 "${SW}x${SH}x24" >/dev/null 2>&1 &
  sleep 2
}
export DISPLAY=$DISP
echo "== display $DISP ${SW}x${SH} ($BUILDDIR)"

pkill -9 PILOT_RV 2>/dev/null
sleep 1

FAILED=""
check() {  # check <shell status: 0 = ok> <name>
  if [ "$1" = 0 ]; then printf '%-56s PASS\n' "$2"
  else printf '%-56s FAIL\n' "$2"; FAILED="$FAILED|$2"; fi
}
have() { grep -q -F -- "$1" "$LOG" 2>/dev/null; }

TMP=${TMPDIR:-/tmp}
# A directory whose name carries a space: the switch has to survive quoting, which
# is the part a naive tokenizer breaks on. Symlinks instead of copies - 48 MB each.
RUN="$TMP/pilots rundir"
DATA="$TMP/pilots data dir"
EMPTY="$TMP/pilots empty dir"
rm -rf "$RUN" "$DATA" "$EMPTY" "$TMP/pilots no such dir"
mkdir -p "$RUN" "$DATA" "$EMPTY"     # $EMPTY stays empty on purpose
ln -sfn "$PWD/SRC/PILOTS.EXE" "$RUN/PILOTS.EXE"
ln -sfn "$PWD/SRC/PILOTS.EXE" "$DATA/PILOTS.EXE"

resolved() { sed -n "s/.*Game_OpenFile: fullPath='\(.*\)'.*/\1/p" "$LOG" | tail -n1; }
openfd()   { sed -n 's/.*Game_OpenFile: fd=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' "$LOG" | tail -n1; }
endswith() {
  # The engine always builds Win-style paths and the shim formats fullPath as
  # one too, so /tmp/x comes back as \tmp\x. Normalise the separators before
  # comparing or every suffix check below fails on the separator alone.
  case "$(printf '%s' "$1" | tr '\\' '/')" in *"$2") return 0 ;; *) return 1 ;; esac
}

# run <workdir> [args...] - leaves $REACHED = 1 when the main loop was reached.
run() {
  wd=$1; shift
  : > "$LOG"
  ( cd "$wd" && exec "$EXE" "$@" ) >/tmp/pilots_dp.out 2>&1 &
  P=$!
  REACHED=0
  n=150                                   # ~30 s: the menu is a few seconds in
  while [ "$n" -gt 0 ]; do
    kill -0 "$P" 2>/dev/null || break
    have "auxMainLoop: startResourceId" && { REACHED=1; break; }
    sleep 0.2; n=$((n - 1))
  done
  sleep 0.5
  kill -9 "$P" 2>/dev/null
  wait "$P" 2>/dev/null
}

echo "== 1. default: run from the data dir, no switch"
run "$RUN"
[ -n "$(resolved)" ]; check $? "data file path was resolved"
endswith "$(resolved)" "/pilots rundir/PILOTS.EXE"
check $? "resolved against the CWD ($(resolved))"
[ "$(openfd)" -gt 0 ] 2>/dev/null; check $? "data file opened (fd=$(openfd))"
[ "$REACHED" = 1 ]; check $? "reached the main loop"

echo "== 2. -datapath <dir with a space>, started from an empty dir"
run "$EMPTY" -datapath "$DATA"
have "SdlBackendParseCmdLine: -datapath '"; check $? "switch seen"
have "datapath '$TMP/pilots data dir'"
check $? "switch reports the directory it took"
endswith "$(resolved)" "/pilots data dir/PILOTS.EXE"
check $? "resolved into -datapath ($(resolved))"
[ "$(openfd)" -gt 0 ] 2>/dev/null; check $? "data file opened (fd=$(openfd))"
[ "$REACHED" = 1 ]; check $? "reached the main loop"

echo "== 3. no switch, started from an empty dir (the case -datapath exists for)"
run "$EMPTY"
endswith "$(resolved)" "/pilots empty dir/PILOTS.EXE"
check $? "resolved against the empty CWD ($(resolved))"
[ "$(openfd)" -le 0 ] 2>/dev/null; check $? "data file refused (fd=$(openfd))"
[ "$REACHED" = 0 ]; check $? "did NOT reach the main loop"

echo "== 4. -datapath pointing nowhere falls back instead of dying"
run "$EMPTY" -datapath "$TMP/pilots no such dir"
have "cannot use it, keeping the current directory"
check $? "reports that it could not take the directory"
endswith "$(resolved)" "/pilots empty dir/PILOTS.EXE"
check $? "fell back to the CWD ($(resolved))"

echo "== 5. -fullscreen still parses after the tokenizer rewrite"
run "$RUN" -fullscreen
have "SdlBackendParseCmdLine: -fullscreen"; check $? "-fullscreen recognised"

[ -s /tmp/pilots_dp.out ] && {
  echo "== last lines of the game's own output:"
  tail -n 6 /tmp/pilots_dp.out | sed 's/^/    /'
}

if [ -n "$FAILED" ]; then echo "FAILED:${FAILED}"; exit 1; fi
echo "PASSED"
exit 0
