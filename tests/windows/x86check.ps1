# x86 regression check: build the 32-bit debug exe, run it for a few seconds and
# report the debug-log markers that form the 32-bit baseline: the game starts,
# initializes, reaches its main loop, presents its first frame and dies cleanly
# on the harness' Stop-Process without a single exception.
#
# x86 lives in its own output dir (out\x86) with its own SDL2.dll, so it no longer
# depends on which arch was built last.
#
# Usage:
#   pwsh -NoProfile -File tests\windows\x86check.ps1 [-Seconds 12] [-Build bat|cmake]
#
# -Build bat (default) builds through SRC\build-vs2022.bat into SRC\out\x86, the
# proven Windows baseline. -Build cmake builds the same x86 debug through CMake
# into build-win32 and runs THAT exe instead - the whole point being that the two
# paths must produce the same markers, so the CMake Windows port can be checked
# against the .bat rather than trusted. The run/measure part is identical either
# way; only where the exe comes from differs.
param([int]$Seconds = 12, [ValidateSet('bat','cmake')][string]$Build = 'bat')

$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path

Get-Process PILOT_RE,PILOT_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

. "$PSScriptRoot\rundir.ps1"

try {
  $exeDir = Build-GameExe -ExeName 'PILOT_RE.EXE' -Config Debug -Build $Build
} catch {
  "$($_.Exception.Message)"; exit 1
}
# The baseline markers below are debug-log lines and a Release build writes no log
# at all (DBG_* are no-ops), so say so now instead of failing later with a bare
# "no log". The Visual Studio generator spells the config out as a subdir
# (build-win32\Debug); a single-config generator like Ninja writes the exe at the
# build root and keeps the config in the cache instead. Only the CMake path has
# this ambiguity - the bat's out\x86 always means the config it was just built with.
if ($Build -eq 'cmake') {
  $bdir = Join-Path (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path 'build-win32'
  $cfg = Split-Path $exeDir -Leaf
  if ($cfg -eq (Split-Path $bdir -Leaf)) {
    $cfg = (Select-String -Path (Join-Path $bdir 'CMakeCache.txt') -Pattern '^CMAKE_BUILD_TYPE:').Line -replace '.*=', ''
  }
  if ($cfg -notmatch '^(Debug|)$') {
    "WARNING: the exe about to run is a '$cfg' build - the baseline markers come"
    "         from the debug log, which a Release build does not write"
  }
}
"exe                 : $exeDir\PILOT_RE.EXE"

# Run from the scratch dir: it holds the data but not MOVIES. A 32-bit build DOES
# find the intro AVI (WoW64 still ships the 32-bit MCI driver) and then spends the
# whole measurement window on the roll, which shifts the baseline markers purely
# because the videos happen to be installed. See rundir.ps1.
$RUN = New-GameRunDir -ExeName 'PILOT_RE.EXE' -FromDir $exeDir
$LOG = Join-Path $RUN 'PILOT_RE_debug.log'

Remove-Item $LOG -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$RUN\PILOT_RE.EXE" -WorkingDirectory $RUN -PassThru
# a loader failure (wrong SDL2.dll arch) puts up a modal box before main() runs;
# catch it while the process is still alive instead of reporting a silent NO LOG
$dlg = $null
for ($i = 0; $i -lt 15 -and -not $dlg; $i++) { Start-Sleep -Milliseconds 200; $dlg = Get-StartupDialog $p }
if ($dlg) { Report-StartupFailure $p $dlg; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }; exit 1 }
Start-Sleep -Seconds $Seconds
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500

if (-not (Test-Path $LOG)) { Report-StartupFailure $p; exit 1 }
$l = Get-Content $LOG

""
"survived run      : $alive   (window stayed up $Seconds s)"
"log lines         : $($l.Count)"
"exceptions        : $(($l | Select-String -SimpleMatch 'EXCEPTION: 0xC000').Count)"
"access violations : $(($l | Select-String -SimpleMatch 'Access violation').Count)"
""
"--- ENTER markers ---"
$l | Select-String -SimpleMatch 'ENTER: ' |
  ForEach-Object { ($_.Line -replace '^\[PILOTS\] ','') } |
  Group-Object | Sort-Object Count -Descending | Select-Object -First 8 |
  ForEach-Object { "  {0,-30} x{1}" -f $_.Name, $_.Count }

$ex = $l | Select-String -Pattern 'GmGame_Initialize: ok|First present|ENTER: (WinMain|GameMainLoop)' | Select-Object -First 8
""
"--- baseline markers (init=1, First present=1, GameMainLoop>=1) ---"
if ($ex) { $ex | ForEach-Object { ($_.Line -replace '^\[PILOTS\] ','') } } else { "(none)" }
$init = ($l | Select-String -SimpleMatch 'GmGame_Initialize: ok').Count
$pres = ($l | Select-String -SimpleMatch 'First present').Count
$gml  = ($l | Select-String -SimpleMatch 'ENTER: GameMainLoop').Count

$bad = ($l | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation').Count
""
if ($bad -eq 0 -and $alive -and $init -eq 1 -and $pres -eq 1 -and $gml -ge 1) {
  "RESULT: PASS  (exceptions=$bad, survived=$alive, init=$init, first present=$pres, GameMainLoop=$gml)"
} else {
  "RESULT: FAIL  (exceptions=$bad, survived=$alive, init=$init, first present=$pres, GameMainLoop=$gml; baseline is 0/1/1/1/>=1)"
}
