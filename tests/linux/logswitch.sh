#!/bin/sh
# -log [file] - when the Linux build writes its log. Linux counterpart of
# tests/windows/logswitch.ps1, and the only place the ABSOLUTE path case can be
# checked: on Windows an absolute path starts with a drive letter, while here it
# starts with '/', which the switch parser used to reject as "another switch" -
# '-log /tmp/journal.log' silently became plain '-log' and the file appeared
# next to the image instead of where it was asked for.
#
# The flavour is detected, not assumed: run once without the key and see whether
# a log appears. Release -> it must NOT (that is the whole point of the key);
# debug -> it must, exactly as before -log existed. Every following case then
# asserts what BOTH flavours share: -log turns writing on and -log <file> picks
# the file, absolute or relative.
#
# Log locations differ by design and both are asserted:
#   default file  -> next to the IMAGE (/proc/self/exe), i.e. $BUILDDIR, NOT CWD
#   -log <file>   -> relative to the LAUNCH directory, because DbgLogInit reads
#                    the key before -datapath can chdir
# so every run happens from a scratch dir and the two places are checked apart.
#
#   sh tests/linux/logswitch.sh
# Needs: a built $BUILDDIR/PILOTS_RE[_x64] and SRC/ with the original game data.
case "$0" in
  */*) cd "$(dirname "$0")/../.." || exit 1 ;;
esac
BUILDDIR=${PILOTS_BUILDDIR:-build64}
BIN=PILOTS_RE
[ -x "$BUILDDIR/PILOTS_RE_x64" ] && BIN=PILOTS_RE_x64
DISP=${PILOTS_DPDISPLAY:-:96}
SW=1024; SH=768
EXE=$(pwd)/$BUILDDIR/$BIN
DEFLOG=$(pwd)/$BUILDDIR/${BIN}_debug.log

[ -x "$EXE" ] || { echo "no $EXE - build it first"; exit 1; }
[ -f SRC/PILOTS.EXE ] || { echo "no SRC/PILOTS.EXE - game data missing"; exit 1; }
DATA=$(pwd)/SRC

pgrep -f "Xvfb $DISP" >/dev/null 2>&1 || {
  Xvfb "$DISP" -screen 0 "${SW}x${SH}x24" >/dev/null 2>&1 &
  sleep 2
}
export DISPLAY=$DISP
echo "== display $DISP ${SW}x${SH} ($BUILDDIR)"

FAILED=""
check() {  # check <shell status: 0 = ok> <name>
  if [ "$1" = 0 ]; then printf '%-56s PASS\n' "$2"
  else printf '%-56s FAIL\n' "$2"; FAILED="$FAILED|$2"; fi
}
have() { grep -q -F -- "$1" "$2" 2>/dev/null; }

TMP=${TMPDIR:-/tmp}
RUN="$TMP/pilots logswitch"
ABS="$TMP/pilots-log-absolute.log"
REL=reljournal.log
rm -rf "$RUN"
rm -f "$ABS" "$DEFLOG" "$RUN/$REL"
mkdir -p "$RUN"
# Данные симлинком, 48 МБ копировать незачем; ролики не нужны - игра доходит до
# главного цикла и без них (как в datapath.sh).
ln -sfn "$DATA/PILOTS.EXE" "$RUN/PILOTS.EXE"
cd "$RUN" || exit 1

pkill -9 "$BIN" 2>/dev/null
sleep 1

# Запуск из $RUN (каталог запуска), 8 с. ALIVE = 0, если процесс дожил.
run() {
  rm -f "$DEFLOG"
  "$EXE" -datapath "$DATA" "$@" >/dev/null 2>&1 &
  pid=$!
  sleep 8
  if kill -0 "$pid" 2>/dev/null; then ALIVE=0; else ALIVE=1; fi
  kill "$pid" 2>/dev/null
  wait "$pid" 2>/dev/null
  sleep 1
}

echo "== определение сборки (запуск без ключа)"
run
check $ALIVE "процесс дожил до конца прогона"
if [ -e "$DEFLOG" ]; then
  FLAVOUR=debug
  check 0 "debug: без ключа журнал пишется"
  have 'ENTER: WinMain' "$DEFLOG"; check $? "debug: маркер ENTER: WinMain"
else
  FLAVOUR=release
  check 0 "release: без ключа журнала нет"
fi
echo "== сборка: $FLAVOUR"

echo "== -log без имени: файл рядом с образом"
run -log
check $ALIVE "-log: процесс дожил"
if [ -e "$DEFLOG" ]; then st=0; else st=1; fi
check $st "-log создаёт образный лог"
have 'ENTER: WinMain' "$DEFLOG"; check $? "-log: маркер ENTER: WinMain"
have 'First present: 640x480' "$DEFLOG"; check $? "-log: маркер First present"

echo "== -log <относительное имя>: файл в каталоге запуска"
run -log "$REL"
check $ALIVE "-log $REL: процесс дожил"
if [ -e "$RUN/$REL" ]; then st=0; else st=1; fi
check $st "-log $REL -> \$CWD/$REL"
if [ -e "$DEFLOG" ]; then st=1; else st=0; fi
check $st "-log $REL: образный лог не создан"
have 'ENTER: WinMain' "$RUN/$REL"; check $? "-log $REL: маркер в журнале"

echo "== -log <абсолютный путь>: файл по указанному пути"
run -log "$ABS"
check $ALIVE "-log <абс. путь>: процесс дожил"
if [ -e "$ABS" ]; then st=0; else st=1; fi
check $st "-log $ABS -> файл по пути"
if [ -e "$DEFLOG" ]; then st=1; else st=0; fi
check $st "-log <абс. путь>: образный лог не создан"
have 'ENTER: WinMain' "$ABS"; check $? "-log <абс. путь>: маркер в журнале"

pkill -9 "$BIN" 2>/dev/null
rm -rf "$RUN"
rm -f "$ABS" "$DEFLOG"

if [ -n "$FAILED" ]; then
  echo "FAILED:$FAILED"
  exit 1
fi
echo "ALL PASS ($FLAVOUR)"
