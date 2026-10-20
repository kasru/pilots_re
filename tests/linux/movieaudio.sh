#!/bin/sh
# A movie's own sound.
#
# The AVIs the game ships carry a second stream that nothing else in this port plays: its header
# is hdrl/LIST strl with strh.fccType 'auds' and a 16-byte strf WAVEFORMATEX (PCM, mono, 8 bit),
# its samples sit in movi as '01wb' chunks. ffmpeg's avi demuxer reports only the video for these
# files ("non-interleaved AVI", one stream), the game's own AVI reader has no sound at all, and
# there is no mciavi32 here to be asked. So mci_ffmpeg.c walks the RIFF by hand and paces the
# samples against the frames it has already shown, and the mixer in sdl_backend.c adds them to
# the music and the effects - see SdlMix_MovieOpen.
#
# What is asserted below is the mixer's own numbers, not a listening test. SDL_AUDIODRIVER=dummy
# still runs the callback at the rate it was given, so the frame counts are real; the peak says
# whether the samples that reached it were silence or sound. That pair is what caught the wave
# path's first version, which queued 73728 frames per 5 s of a silent conversion: counted
# activity, inaudible result. A nonzero peak is the thing that was missing there.
#
# The two shipped movies are the two integer ratios into the mixer's 44100 Hz: INTRO at 22050 and
# START at 11025. Their expected sizes below are the shipped files' own - 776062 and 554989 are
# what the audio strh's dwLength promises, and the port's chunk walk adds up to exactly that.
#
#   sh tests/linux/movieaudio.sh
# Needs: a built $BUILDDIR/PILOT_RV (build64 by default: dummy drivers need no X, so this runs
# the host binary directly rather than in the 32-bit container) and SRC/ with the game data and
# SRC/MOVIES/{INTRO,START}.AVI.
case "$0" in
  */*) cd "$(dirname "$0")/../.." || exit 1 ;;
esac
BUILDDIR=${PILOTS_BUILDDIR:-build64}
LOG=$(pwd)/$BUILDDIR/PILOT_RV_debug.log
EXE=$(pwd)/$BUILDDIR/PILOT_RV

[ -x "$EXE" ] || { echo "no $EXE - build it first"; exit 1; }
[ -f SRC/PILOTS.EXE ] || { echo "no SRC/PILOTS.EXE - game data missing"; exit 1; }
[ -f SRC/MOVIES/INTRO.AVI ] || { echo "no SRC/MOVIES/INTRO.AVI - movies missing"; exit 1; }

dur() {  # whole seconds of an AVI, default if ffprobe or the file is missing
  d=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "SRC/MOVIES/$1" 2>/dev/null | cut -d. -f1)
  echo "${d:-$2}"
}
INTRO_DUR=$(dur INTRO.AVI 34)
START_DUR=$(dur START.AVI 50)
WAIT=$((INTRO_DUR + START_DUR + 20))             # both movies, plus the boot and the slack
echo "== $BUILDDIR: INTRO ${INTRO_DUR}s + START ${START_DUR}s, running ${WAIT}s (dummy video and audio)"

pkill -9 PILOT_RV 2>/dev/null
sleep 1
: > "$LOG"

# The engine asks for ".\PILOTS.EXE" against the process CWD, so it has to be started from SRC -
# the same way the other Linux harnesses start it. $EXE and $LOG above are absolute, so the log
# still lands next to the image and the checks below still read it.
cd SRC || exit 1
SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy "$EXE" > /tmp/pilots_movieaudio.out 2>&1 &PID=$!
sleep "$WAIT"
kill -USR1 "$PID" 2>/dev/null                    # the clean-quit hook
sleep 4
if kill -0 "$PID" 2>/dev/null; then
  RUNNING=yes
  kill -9 "$PID" 2>/dev/null
else
  RUNNING=no
fi

FAILED=""
check() {  # check <shell status: 0 = ok> <name> [what]
  if [ "$1" = 0 ]; then printf '%-58s PASS  %s\n' "$2" "$3"
  else printf '%-58s FAIL  %s\n' "$2" "$3"; FAILED="$FAILED|$2"; fi
}

echo "---- what the mixer saw ----"
grep -E "SdlShareAudioWanted|SdlMix_Open|movie sound|SdlAudioMix: .*movie [1-9]" "$LOG" | head -12

# The probe: the stream is found by walking hdrl, and the size it counts is the size the header
# promises - a walk that stopped at the first chunk or counted video bytes would differ here.
grep -q "movie sound: stream 1 PCM 22050 Hz mono 8 bit, 776062 bytes in 24 chunks" "$LOG"
check $? "INTRO's sound stream probed (22050 Hz mono 8 bit, 776062 bytes in 24 chunks)"

grep -q "movie sound: stream 1 PCM 11025 Hz mono 8 bit, 554989 bytes in 5 chunks" "$LOG"
check $? "START's sound stream probed (11025 Hz mono 8 bit, 554989 bytes in 5 chunks)"

# A movie is the first thing that wants audio, and the mixer must open once for everything after
# it: the intro plays before any music, so "movie asked first" is also the proof that a source
# arriving before the others is not a special case.
grep -q "SdlMix_Open: one device for both sources (movie asked first)" "$LOG"
check $? "the mixer opened once, on the movie's asking"

# Sound, not counted silence: at least one window where movie frames arrived with a nonzero peak.
grep -qE "movie [1-9][0-9]* frames \(peak [1-9]" "$LOG"
check $? "movie frames reached the mixer non-silent (peak above 0)"

# Pacing is the video's clock: the mixer consumes 44100 frames a second when a movie runs, so a
# 5 s window cannot carry more than that by much. A window far above it would mean the samples
# were poured in faster than realtime (a race-ahead), which sounds like a fast-forward.
MAX=$(grep -oE "movie [0-9]+ frames" "$LOG" | awk '{ if ($2 + 0 > m) m = $2 + 0 } END { print m + 0 }')
[ "$MAX" -gt 44100 ] && [ "$MAX" -le 230000 ]
check $? "the busiest 5 s window carried $MAX frames (one movie at realtime is ~220500)" \
  "not racing ahead of the picture"

# The sync guard (drop the gap rather than hurry through it) is for a starved pump. On an idle
# host it must never fire: if it does, the pacing has lost the picture's clock somewhere.
BEHIND=$(grep -c "behind the picture" "$LOG")
[ "$BEHIND" = 0 ]
check $? "the pump kept up: the sync guard dropped nothing" "($BEHIND drops)"

# The intro's audible part must have been handed over. Its track is 35.2 s while the picture is
# 34.0 s, and the last four seconds of it are silence (measured: no sample differs from 128 in
# them), so a fed count near the end but short of the total is exactly right.
FED=$(grep -oE "movie sound stopped: [0-9]+ of 776062" "$LOG" | head -1 | awk '{ print $4 }')
[ -n "$FED" ] && [ "$FED" -gt 700000 ]
check $? "the mixer was given $FED of INTRO's 776062 samples" "(the rest is the movie's silent tail)"

EXC=$(grep -c "EXCEPTION: 0xC000" "$LOG")
[ "$EXC" = 0 ]
check $? "no exceptions" "($EXC in the log)"

[ "$RUNNING" = no ]
check $? "clean exit on SIGUSR1"

echo
if [ -n "$FAILED" ]; then
  echo "RESULT: FAILED -$(echo "$FAILED" | tr '|' ' ')"
  exit 1
fi
echo "RESULT: PASS - both movie sound streams found, mixed non-silent at realtime, nothing dropped"
