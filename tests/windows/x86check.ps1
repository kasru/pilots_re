# x86 regression check: build the 32-bit debug exe, run it for a few seconds and
# report the debug-log markers that form the 32-bit baseline: the game starts,
# initializes, reaches its main loop, presents its first frame and dies cleanly
# on the harness' Stop-Process without a single exception.
#
# The x86 build has to be the LAST one: both arches import the same SDL2.dll, so
# whichever bat ran last decides which exe can actually start.
#
# Usage: pwsh -NoProfile -File tests\windows\x86check.ps1 [-Seconds 12]
param([int]$Seconds = 12)

$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path

Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

"=== building x86 DEBUG (last, so SDL2.dll is the x86 one) ==="
$out = cmd /c "`"$SRC\build-vs2022.bat debug x86`"" 2>&1
$out | Select-String -Pattern 'SUCCESS|BUILD FAILED|error [A-Z]|LNK[0-9]' | ForEach-Object { $_.Line }

if (-not (Test-Path (Join-Path $SRC 'out\PILOTS_RE.EXE'))) { "out\PILOTS_RE.EXE missing"; exit 1 }

# Run from the scratch dir: it holds the data but not MOVIES. A 32-bit build DOES
# find the intro AVI (WoW64 still ships the 32-bit MCI driver) and then spends the
# whole measurement window on the roll, which shifts the baseline markers purely
# because the videos happen to be installed. See rundir.ps1.
. "$PSScriptRoot\rundir.ps1"
$RUN = New-GameRunDir -ExeName 'PILOTS_RE.EXE'
$LOG = Join-Path $RUN 'PILOTS_RE_debug.log'

Remove-Item $LOG -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$RUN\PILOTS_RE.EXE" -WorkingDirectory $RUN -PassThru
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
