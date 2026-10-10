# Scratch run directory for the game, shared by the harnesses.
#
# Dot-source it, then call New-GameRunDir:
#
#   . "$PSScriptRoot\rundir.ps1"
#   $RUN = New-GameRunDir -ExeName 'PILOT_RV_x64.EXE'
#   $LOG = Join-Path $RUN 'PILOT_RV_x64_debug.log'
#
# Why a scratch dir at all. main.c resolves .\PILOTS.EXE (the game's own data file)
# and the .\movies\... video lookup against the process CWD, so the exe has to be
# started from a directory that holds the game data. That directory deliberately
# does NOT hold MOVIES, because:
#
#  - on current Windows the AVI MCI driver (avivideo) exists only in the 32-bit
#    flavour. The 64-bit build therefore crashes inside mciSendCommandA(MCI_OPEN)
#    as soon as movies\intro.AVI is present - it faults even with a zeroed
#    parameter block, which an isolated P/Invoke probe confirmed outside the game.
#    A test about the toolbar toggles must not depend on that.
#
#  - a 32-bit build DOES play the intro, and then spends its whole measurement
#    window on the roll, so x86check's timing markers (auxMainLoop, GameMainLoop)
#    shift purely because the videos happen to be installed. Skipping the intro
#    keeps the baseline independent of optional game data.
#
# Files are hardlinked, not copied: the data file is ~15 MB and the directory is
# rebuilt on every run. mklink /H needs no elevation (only symlinks do).
$__TestsDir = $PSScriptRoot

function New-GameRunDir {
  param([string]$ExeName, [string]$FromDir = $null)

  $SRC = (Resolve-Path (Join-Path $__TestsDir '..\..\SRC')).Path
  if (-not $FromDir) { $FromDir = $SRC }
  $RUN = Join-Path $env:TEMP 'pilots_rundir'
  New-Item -ItemType Directory -Force -Path $RUN | Out-Null

  # SDL2.dll is arch-shared, so whatever the last build left in SRC is the right one.
  foreach ($f in @($ExeName, 'SDL2.dll', 'PILOTS.EXE')) {
    $s = Join-Path $FromDir $f
    if (-not (Test-Path $s)) { throw "missing game file: $s" }
    $d = Join-Path $RUN $f
    if (Test-Path $d) { Remove-Item $d -Force -ErrorAction SilentlyContinue }
    cmd /c "mklink /H `"$d`" `"$s`"" | Out-Null
    if (-not (Test-Path $d)) { Copy-Item $s $d -Force }   # fallback, e.g. other volume
  }
  return $RUN
}
