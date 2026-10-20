#!/bin/sh
# WM regression battery for the maximize/restore freeze fix (commit 7112ea8):
# the game must keep client=640x480 (its logical size) across any unmap/resize
# cycle - a stuck client=0x0 after restore freezes rendering forever (clip bound
# 0 in RenderDirtyRegions, cursor gone, no repaint).
#
# Needs: the game already running on DISPLAY=:0 with PILOTS_DEBUG_HB=1
# (1s heartbeat in $BUILDDIR/PILOT_RV_debug.log) and tests/linux/x11ctl.py as the WM
# driver (no xdotool/wmctrl on the host). Run from anywhere:
#   sh tests/linux/test_wm_freeze.sh
set -e
cd "$(dirname "$0")/../.."
BUILDDIR=${PILOTS_BUILDDIR:-build32}
export DISPLAY=:0
X="python3 tests/linux/x11ctl.py"

ID=$($X list | awk '/[Pp]ilot/{print $1}')
[ -n "$ID" ] || { echo "game window not found"; exit 1; }
echo "== window $ID"

hb() { grep -a HB: "$BUILDDIR/PILOT_RV_debug.log" | tail -n "$1"; }
origin() { G=$($X geom "$ID"); OX=${G%%,*}; OY=${G#*,}; OY=${OY%% *}; }

echo "== phase A: maximize -> hardware mouse beyond logical size -> restore"
$X state "$ID" max
sleep 2
origin
echo "max geom $G (origin $OX,$OY)"
$X xtest-motion $((OX + 1200)) $((OY + 700))
sleep 1
$X xtest-motion $((OX + 1000)) $((OY + 550))
$X xtest-click
sleep 1
$X state "$ID" restore
sleep 2
origin
echo "restore geom $G (origin $OX,$OY)"
$X xtest-motion $((OX + 400)) $((OY + 300))
$X xtest-click
sleep 3
echo "-- HB after A (expect client=640x480, win=640x480, growing presents):"
hb 4

echo "== phase B: fullscreen -> hardware mouse -> unfullscreen"
$X state "$ID" fs
sleep 2
origin
echo "fs geom $G (origin $OX,$OY)"
$X xtest-motion $((OX + 1100)) $((OY + 650))
$X xtest-click
sleep 1
$X state "$ID" unfs
sleep 2
origin
echo "unfs geom $G (origin $OX,$OY)"
$X xtest-motion $((OX + 300)) $((OY + 200))
sleep 3
echo "-- HB after B (expect client=640x480, win=640x480):"
hb 4

echo "== phase C: iconify -> restore (the original freeze trigger)"
$X iconify "$ID"
sleep 3
echo "-- HB while minimized (expect client=0x0, flags 0x40):"
hb 2
$X active "$ID"
sleep 3
echo "-- HB after restore (expect client=640x480, growing presents):"
hb 4

echo "== window events of the run:"
grep -a -n -e SDL_WINDOWEVENT -e MINIMIZED: -e RESTORED: "$BUILDDIR/PILOT_RV_debug.log" | tail -n 30
echo "== done"
