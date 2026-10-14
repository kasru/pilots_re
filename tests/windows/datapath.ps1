# -datapath <dir> - where the game looks for its resources.
#
# Without the switch nothing changes: the engine asks for .\PILOTS.EXE and
# .\movies\... exactly as it always did, resolved against the process CWD, so the
# default is "find them next to wherever the process was started".
#
# With it, WinMain changes the working directory BEFORE the game opens anything,
# and every CWD-relative lookup follows at once - data file, movies, saves - while
# main.c still strcpy's the literal ".\PILOTS.EXE". That is the whole point: no
# game-code change, one place in the port layer. The debug log and the .hlp are
# derived from GetModuleFileNameA, so they stay next to the exe - which is why
# every case below reads the SAME log file.
#
# Signals read from the log:
#   SdlBackendParseCmdLine: -datapath '...'   the switch was seen and applied
#   Game_OpenFile: fullPath=...               where the data file was resolved
#   Game_OpenFile: fd=...                     >0 -> the file really opened
#   auxMainLoop: startResourceId=             the game reached its main loop
#
# The data directory's name carries a space on purpose, so the switch has to make
# it through quoting - that is the part a naive tokenizer breaks on.
#
# Path checks are SUFFIX checks. The log is written through WriteFile in the
# process's ANSI code page and read back as UTF-8, so the non-ASCII part of
# %TEMP% (the user name) comes out mangled while every ASCII byte after it stays
# exact. Matching the tail is both correct and enough - the directory names are
# what actually has to match.
#
#   & "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File tests\windows\datapath.ps1
#   ... -Arch x86
param([ValidateSet('x86','x64')][string]$Arch = 'x64')

$ExeName = if ($Arch -eq 'x64') { 'PILOT_RV_x64.EXE' } else { 'PILOT_RV.EXE' }

$fail = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
  if ($ok) { "  ok   $what $detail" } else { "  FAIL $what $detail"; $script:fail++ }
}

. "$PSScriptRoot\rundir.ps1"
Get-Process PILOT_RV,PILOT_RV_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$RUN = New-GameRunDir -ExeName $ExeName
$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
# DbgLogInit strips the exe extension: it follows the image, not the CWD, so it is
# the same path for every case below.
$LOG = Join-Path $RUN (($ExeName -replace '\.EXE$','') + '_debug.log')

$DATA  = Join-Path $env:TEMP 'pilots data dir'    # has a space: exercises quoting
$EMPTY = Join-Path $env:TEMP 'pilots empty dir'   # holds no game data at all
New-Item -ItemType Directory -Force -Path $DATA,$EMPTY | Out-Null
Get-ChildItem $EMPTY -Force -ErrorAction SilentlyContinue |
  Remove-Item -Force -Recurse -ErrorAction SilentlyContinue

$dd = Join-Path $DATA 'PILOTS.EXE'
if (Test-Path $dd) { Remove-Item $dd -Force }
cmd /c "mklink /H `"$dd`" `"$SRC\PILOTS.EXE`"" | Out-Null
if (-not (Test-Path $dd)) { Copy-Item (Join-Path $SRC 'PILOTS.EXE') $dd -Force }

function Invoke-Game {
  # Starts the exe from $WorkDir with $GameArgs, waits for the main-loop marker
  # (or for the process to give up), then kills it and returns the log text.
  param([string]$WorkDir, [string]$GameArgs = '')

  Remove-Item $LOG -Force -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath (Join-Path $RUN $ExeName) -WorkingDirectory $WorkDir `
         -ArgumentList $GameArgs -PassThru

  # A data-file failure shows a modal box, which would otherwise park the run here
  # until the harness timed out. Dismiss() also catches the loader's box, which is
  # not owned by our pid - same helper as every other harness.
  $dialog = ''
  for ($i = 0; $i -lt 15 -and -not $dialog; $i++) {
    Start-Sleep -Milliseconds 200
    $p.Refresh()
    if ($p.HasExited) { break }
    $dialog = Get-StartupDialog $p
  }

  $reached = $false
  for ($i = 0; $i -lt 100 -and -not $reached; $i++) {
    $p.Refresh()
    if ($p.HasExited) { break }
    if (Select-String -Path $LOG -Pattern 'auxMainLoop: startResourceId' -Quiet -ErrorAction SilentlyContinue) {
      $reached = $true
      break
    }
    Start-Sleep -Milliseconds 200
  }
  Start-Sleep -Milliseconds 600
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  Start-Sleep -Milliseconds 400

  [pscustomobject]@{
    Log     = Get-Content $LOG -Raw -ErrorAction SilentlyContinue
    Dialog  = $dialog
    Reached = $reached
  }
}

function Has([string]$Log, [string]$Pattern) {
  return [bool]($Log -and ($Log -match $Pattern))
}
function Resolved([string]$Log) {
  if ($Log -and ($Log -match "Game_OpenFile: fullPath='([^']+)'")) { return $Matches[1] }
  return ''
}
function OpenFd([string]$Log) {
  if ($Log -and ($Log -match 'Game_OpenFile: fd=(-?\d+)')) { return [int]$Matches[1] }
  return -1
}

"### 1. default: run from the data dir, no switch ###"
$r = Invoke-Game -WorkDir $RUN
Check 'data file resolves against the CWD' ((Resolved $r.Log) -like '*\PILOTS.EXE') "(fullPath=$(Resolved $r.Log))"
Check 'data file opened'  ((OpenFd $r.Log) -gt 0) "(fd=$(OpenFd $r.Log))"
Check 'reached the main loop' $r.Reached ''

"### 2. -datapath <dir with a space>, started from an empty dir ###"
$r = Invoke-Game -WorkDir $EMPTY -GameArgs ('-datapath "' + $DATA + '"')
Check 'switch seen' (Has $r.Log "SdlBackendParseCmdLine: -datapath '") ''
Check 'switch reports the directory it took' (Has $r.Log "datapath '[^']*pilots data dir'") ''
Check 'data file resolved into -datapath' ((Resolved $r.Log) -like '*pilots data dir\PILOTS.EXE') "(fullPath=$(Resolved $r.Log))"
Check 'data file opened'  ((OpenFd $r.Log) -gt 0) "(fd=$(OpenFd $r.Log))"
Check 'reached the main loop' $r.Reached ''

"### 3. no switch, started from an empty dir (the case -datapath exists for) ###"
$r = Invoke-Game -WorkDir $EMPTY
Check 'data file resolved against the empty CWD' ((Resolved $r.Log) -like '*pilots empty dir\PILOTS.EXE') "(fullPath=$(Resolved $r.Log))"
Check 'data file refused' ((OpenFd $r.Log) -le 0) "(fd=$(OpenFd $r.Log))"
Check 'did NOT reach the main loop' (-not $r.Reached) ''

"### 4. -datapath pointing nowhere falls back instead of dying ###"
$GONE = Join-Path $env:TEMP 'pilots no such dir'
if (Test-Path $GONE) { Remove-Item $GONE -Recurse -Force }
$r = Invoke-Game -WorkDir $EMPTY -GameArgs ('-datapath "' + $GONE + '"')
Check 'reports that it could not take the directory' (Has $r.Log 'cannot use it, keeping the current directory') ''
Check 'fell back to the CWD' ((Resolved $r.Log) -like '*pilots empty dir\PILOTS.EXE') "(fullPath=$(Resolved $r.Log))"

"### 5. -fullscreen still parses after the tokenizer rewrite ###"
$r = Invoke-Game -WorkDir $RUN -GameArgs '-fullscreen'
Check '-fullscreen recognised' (Has $r.Log 'SdlBackendParseCmdLine: -fullscreen') ''
Check 'reached the main loop' $r.Reached ''

if ($r.Dialog) { "note: startup dialog seen: $($r.Dialog)" }
""
if ($fail -eq 0) { "PASS"; exit 0 }
"FAIL ($fail check(s))"; exit 1
