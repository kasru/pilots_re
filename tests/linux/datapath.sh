#!/bin/sh
# -datapath <dir> - where the game looks for its resources. Linux counterpart of
# tests/windows/datapath.ps1.
#
# The switch makes WinMain change the working directory BEFORE the game opens
# anything, so every CWD-relative lookup follows at once: with it the engine
# builds its data path literally like the original did (".\PILOTS.EXE"), and
# saves/movies follow g_ExecutablePath into the same directory. Without the
# switch port-main's own default applies: the rebuilt exe carries no game data
# of its own, so the data file is the PILOTS.EXE next to it - a copy in the CWD
# is deliberately ignored. A directory that cannot be taken falls back to that
# default instead of dying.
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
# port-main markers: every real file open goes through ShimOpenWin (win32_shim.c)
# which logs the resolved fullPath and the fd, and the main loop announces itself
# as "ENTER: GameMainLoop" (DBG_ENTER in host.c).
#
#   sh tests/linux/datapath.sh
# Needs: a built $BUILDDIR/PILOT_RE and SRC/ with the original game data.
case "$0" in
  */*) cd "$(dirname "$0")/../.." || exit 1 ;;
esac
BUILDDIR=${PILOTS_BUILDDIR:-build64}
DISP=${PILOTS_DPDISPLAY:-:96}
SW=1024; SH=768
LOG=$(pwd)/$BUILDDIR/PILOT_RE_debug.log
EXE=$(pwd)/$BUILDDIR/PILOT_RE

[ -x "$EXE" ] || { echo "no $EXE - build it first"; exit 1; }
[ -f SRC/PILOTS.EXE ] || { echo "no SRC/PILOTS.EXE - game data missing"; exit 1; }

pgrep -f "Xvfb $DISP" >/dev/null 2>&1 || {
  Xvfb "$DISP" -screen 0 "${SW}x${SH}x24" >/dev/null 2>&1 &
  sleep 2
}
export DISPLAY=$DISP
echo "== display $DISP ${SW}x${SH} ($BUILDDIR)"

pkill -9 PILOT_RE 2>/dev/null
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

# Both helpers look at the DATA FILE's own open (PILOTS.EXE), not at the last
# open in the log: the intro movie opens its INTRO.BMP next to the exe, and
# that one line would otherwise answer both questions in every case.
resolved() { sed -n "s/.*fullPath='\([^']*\)'.*/\1/p" "$LOG" | grep -i 'PILOTS\.EXE' | tail -n1; }
openfd()   { grep -i 'PILOTS\.EXE' "$LOG" | sed -n 's/.*fd=\(-\{0,1\}[0-9][0-9]*\).*/\1/p' | tail -n1; }
endswith() {
  # The engine always builds Win-style paths and the shim formats fullPath as
  # one too, so /tmp/x comes back as \tmp\x. Normalise the separators before
  # comparing or every suffix check below fails on the separator alone.
  case "$(printf '%s' "$1" | tr '\\' '/')" in *"$2") return 0 ;; *) return 1 ;; esac
}

# run <workdir> [args...] - leaves $REACHED = 1 when the main loop was reached.
# REACH_N bounds the wait: the intro plays ~34 s first, a case with no data
# never gets there at all, so those cases can cap it low (REACH_N=60).
run() {
  wd=$1; shift
  : > "$LOG"
  ( cd "$wd" && exec "$EXE" "$@" ) >/tmp/pilots_dp.out 2>&1 &
  P=$!
  REACHED=0
  n=${REACH_N:-400}
  while [ "$n" -gt 0 ]; do
    kill -0 "$P" 2>/dev/null || break
    have "ENTER: GameMainLoop" && { REACHED=1; break; }
    sleep 0.2; n=$((n - 1))
  done
  sleep 0.5
  kill -9 "$P" 2>/dev/null
  wait "$P" 2>/dev/null
}

echo "== 1. default, no switch: the data file comes from NEXT TO THE EXE"
# port-main keeps the original engine's lookup for saves/movies (they follow
# g_ExecutablePath), but the rebuilt exe itself carries no game data, so
# without the switch the data file is the PILOTS.EXE beside it - a copy of the
# data in the CWD is deliberately NOT what gets opened.
run "$RUN"
[ -n "$(resolved)" ]; check $? "data file path was resolved"
endswith "$(resolved)" "/$BUILDDIR/PILOTS.EXE"
check $? "resolved next to the exe, not against the CWD ($(resolved))"
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

echo "== 3. no switch, empty dir: the exe-relative default still works"
run "$EMPTY"
endswith "$(resolved)" "/$BUILDDIR/PILOTS.EXE"
check $? "resolved next to the exe anyway ($(resolved))"
[ "$(openfd)" -gt 0 ] 2>/dev/null; check $? "data file opened (fd=$(openfd))"
[ "$REACHED" = 1 ]; check $? "reached the main loop"

echo "== 4. -datapath pointing nowhere falls back to the default instead of dying"
run "$EMPTY" -datapath "$TMP/pilots no such dir"
have "cannot use it, keeping the current directory"
check $? "reports that it could not take the directory"
endswith "$(resolved)" "/$BUILDDIR/PILOTS.EXE"
check $? "fell back to the exe-relative default ($(resolved))"
[ "$REACHED" = 1 ]; check $? "reached the main loop"

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
