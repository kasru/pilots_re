# x86 regression check: build the 32-bit debug exe, run it for a few seconds and
# report the debug-log markers that must stay identical to the pre-LP64 baseline.
#
# The x86 build has to be the LAST one: both arches import the same SDL2.dll, so
# whichever bat ran last decides which exe can actually start.
#
# Usage: pwsh -NoProfile -File tests\windows\x86check.ps1 [-Seconds 12]
param([int]$Seconds = 12)

$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path

Get-Process PILOT_RV,PILOT_RV_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

"=== building x86 DEBUG (last, so SDL2.dll is the x86 one) ==="
$out = cmd /c "`"$SRC\build_sdl_debug.bat`"" 2>&1
$out | Select-String -Pattern 'SUCCESS|BUILD FAILED|error [A-Z]|LNK[0-9]' | ForEach-Object { $_.Line }

if (-not (Test-Path (Join-Path $SRC 'PILOT_RV.EXE'))) { "PILOT_RV.EXE missing"; exit 1 }

# Run from the scratch dir: it holds the data but not MOVIES. A 32-bit build DOES
# find the intro AVI (WoW64 still ships the 32-bit MCI driver) and then spends the
# whole measurement window on the roll, which shifts the baseline markers purely
# because the videos happen to be installed. See rundir.ps1.
. "$PSScriptRoot\rundir.ps1"
$RUN = New-GameRunDir -ExeName 'PILOT_RV.EXE'
$LOG = Join-Path $RUN 'PILOT_RV_debug.log'

Remove-Item $LOG -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$RUN\PILOT_RV.EXE" -WorkingDirectory $RUN -PassThru
Start-Sleep -Seconds $Seconds
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500

if (-not (Test-Path $LOG)) { "NO LOG - exe did not start (wrong SDL2.dll arch?)"; exit 1 }
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

$ex = $l | Select-String -Pattern 'auxMainLoop: startResourceId|ENTER: GameMainLoop' | Select-Object -First 6
""
"--- baseline markers (auxMainLoop=1, GameMainLoop=2) ---"
if ($ex) { $ex | ForEach-Object { ($_.Line -replace '^\[PILOTS\] ','') } } else { "(none)" }
$aux = ($l | Select-String -SimpleMatch 'auxMainLoop: startResourceId').Count
$gml = ($l | Select-String -SimpleMatch 'ENTER: GameMainLoop').Count

$bad = ($l | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation').Count
""
if ($bad -eq 0 -and $alive -and $aux -eq 1 -and $gml -ge 1) {
  "RESULT: PASS  (exceptions=$bad, survived=$alive, auxMainLoop=$aux, GameMainLoop=$gml)"
} else {
  "RESULT: FAIL  (exceptions=$bad, survived=$alive, auxMainLoop=$aux, GameMainLoop=$gml; baseline is 0/1/>=1)"
}
