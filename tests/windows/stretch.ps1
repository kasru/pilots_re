# '-' (port feature): the picture between the game's own aspect and stretched over the screen.
# Windows half; the same key is handled in the SDL event path on Linux/Switch (SDL_SCANCODE_MINUS).
#
# The assertions are the layout lines the port itself logs, because those lines ARE the feature:
# windowed is the identity, fullscreen letterboxes 4:3 and centres, and '-' replaces that rect with
# the whole client - which is the one case where the picture deliberately does NOT keep 4:3, so the
# rect alone is the proof. Alt+Enter still exits fullscreen, and the stretch is left as the player
# left it (a run that never touches '-' must show the old behaviour, which is what x86check and
# fullscreen.ps1 keep asserting).
#
#   & "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File tests\windows\stretch.ps1 -Arch x86
param([ValidateSet('x86','x64')][string]$Arch = 'x86',
      # 'none' (default) keeps this harness as it was - it runs whatever exe is
      # already in out\<arch> and never builds; 'bat'/'cmake' build first.
      [ValidateSet('none','bat','cmake')][string]$Build = 'none')

. "$PSScriptRoot\rundir.ps1"
$ExeName = if ($Arch -eq 'x64') { 'PILOTS_RE_x64.EXE' } else { 'PILOTS_RE.EXE' }
$exeDir = $null
if ($Build -ne 'none') {
  Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Seconds 1
  try { $exeDir = Build-GameExe -ExeName $ExeName -Config Debug -Build $Build }
  catch { "$($_.Exception.Message)"; exit 1 }
}
$RUN = New-GameRunDir -ExeName $ExeName -FromDir $exeDir
$LogName = ($ExeName -replace '\.EXE$', '') + '_debug.log'
$LOG = Join-Path $RUN $LogName

Add-Type 'using System; using System.Runtime.InteropServices;
public class ST {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, UIntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
}'

function KeyDown([IntPtr]$h, [uint32]$vk) {
  [void][ST]::PostMessage($h, 0x0100, [UIntPtr]$vk, [IntPtr][long]0x00010001)
  Start-Sleep -Milliseconds 150
  [void][ST]::PostMessage($h, 0x0101, [UIntPtr]$vk, [IntPtr][long]0xC0010001)
  Start-Sleep -Milliseconds 400
}

$p = Start-Process -FilePath "$RUN\$ExeName" -WorkingDirectory $RUN -PassThru
Start-Sleep -Seconds 12
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { $h = [IntPtr]$p.Id }   # never happens; keeps the checks from lying

$rc = New-Object ST+RECT
[void][ST]::GetClientRect($h, [ref]$rc)
$cw = $rc.right; $ch = $rc.bottom

$FAILED = @()
function Check([bool]$ok, [string]$name, [string]$what) {
  if ($ok) { Write-Host ("  PASS  {0}  [{1}]" -f $name, $what) }
  else { Write-Host ("  FAIL  {0}  [{1}]" -f $name, $what); $script:FAILED += $name }
}

Write-Host "=== windowed client $cw x $ch; pressing '-' (VK_OEM_MINUS = 0xBD) three times ==="
KeyDown $h 0xBD
Check (-not $p.HasExited) "the game survived the first press" "windowed -> fullscreen stretched"
# The screen the picture has to fill is the FULLSCREEN client, which only exists after that first
# press - measuring it before is what made the first version of this harness compare 1152 against
# the windowed 640 and call a correct letterbox a failure.
$rc2 = New-Object ST+RECT
[void][ST]::GetClientRect($h, [ref]$rc2)
$fw = $rc2.right; $fh = $rc2.bottom
Write-Host "=== fullscreen client $fw x $fh ==="
KeyDown $h 0xBD
KeyDown $h 0xBD
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

$lines = @(Select-String -Path $LOG -Pattern 'SdlBackendToggleStretch: ' | ForEach-Object { $_.Line })
Write-Host "=== what the port logged ==="
$lines | ForEach-Object { Write-Host "  $_" }
$lay = @(Select-String -Path $LOG -Pattern 'SdlUpdateLayout: ')
$lay | Select-Object -Last 4 | ForEach-Object { Write-Host ("  " + $_.Line) }

Check ($lines.Count -ge 3) "the key reached the port three times" "($($lines.Count) toggle lines)"

function RectOf([string]$line) {
  $m = [regex]::Match($line, 'picture (\d+)x(\d+) at \((-?\d+),(-?\d+)\)')
  if (-not $m.Success) { return $null }
  return @{ w = [int]$m.Groups[1].Value; h = [int]$m.Groups[2].Value
            x = [int]$m.Groups[3].Value; y = [int]$m.Groups[4].Value }
}
$r1 = if ($lines.Count -ge 1) { RectOf $lines[0] } else { $null }
$r2 = if ($lines.Count -ge 2) { RectOf $lines[1] } else { $null }
$r3 = if ($lines.Count -ge 3) { RectOf $lines[2] } else { $null }

# 1st press: windowed -> fullscreen, and the stretch applies at once: the picture is the client.
Check ($r1 -and $r1.w -eq $fw -and $r1.h -eq $fh -and $r1.x -eq 0 -and $r1.y -eq 0) `
  "the first press filled the screen" "$($r1.w)x$($r1.h) in a $fw x $fh client"

# 2nd press: still fullscreen, back to the game's aspect - 4:3, narrower, and centred. The aspect
# is the whole point of the second half of the key, so it is asserted as a ratio, not a size.
$aspect = if ($r2) { [math]::Abs($r2.w * 3 - $r2.h * 4) } else { 9999 }
Check ($r2 -and $aspect -le 4 -and $r2.w -lt $fw -and $r2.x -gt 0) `
  "the second press restored the game's 4:3 and centred it" "$($r2.w)x$($r2.h) at ($($r2.x),$($r2.y)), 4:3 off by $aspect"

# 3rd press: stretched again - the key toggles, it does not run one way.
Check ($r3 -and $r3.w -eq $fw -and $r3.h -eq $fh) "the third press stretched again" "$($r3.w)x$($r3.h)"

# And the game is still alive and well behind all that: no exceptions in the log.
$exc = @(Select-String -Path $LOG -Pattern 'EXCEPTION: 0xC000').Count
Check ($exc -eq 0) "no exceptions" "$exc in the log"

if ($FAILED.Count) { Write-Host "RESULT: FAILED - $($FAILED -join ', ')"; exit 1 }
Write-Host "RESULT: PASS"
