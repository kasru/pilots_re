# relfuzz.ps1 = a smoke run of the release build (no DBG_LOG) - the closest thing
# to a real user. -Build bat|cmake picks where that release exe comes from:
# SRC\out\x64 via build-vs2022.bat, or build-win64 via CMake.
param([ValidateSet('bat','cmake')][string]$Build = 'bat')

Add-Type 'using System; using System.Runtime.InteropServices;
public class G5 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  public static IntPtr LP(int x,int y){ return (IntPtr)(((uint)y<<16)|((uint)x&0xFFFF)); }
}'

. "$PSScriptRoot\rundir.ps1"
$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
Get-Process PILOT_RE,PILOT_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

"### building x64 RELEASE ###"
try {
  $OUT = Build-GameExe -ExeName 'PILOT_RE_x64.EXE' -Config Release -Build $Build
} catch {
  "$($_.Exception.Message)"; exit 1
}
if (-not $OUT) { "build produced no exe dir"; exit 1 }

$p = Start-Process -FilePath (Join-Path $OUT 'PILOT_RE_x64.EXE') -WorkingDirectory $SRC -PassThru
$h = [IntPtr]::Zero
for ($i=0; $i -lt 150 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle } }
"window handle: $h"
if ($h -eq [IntPtr]::Zero) { "no window - release build did not start"; exit }

function ClickL($lx,$ly) {
  [void][G5]::PostMessage($h,0x0200,[IntPtr]::Zero,[G5]::LP($lx,$ly))
  [void][G5]::PostMessage($h,0x0201,[IntPtr]1,[G5]::LP($lx,$ly))
  [void][G5]::PostMessage($h,0x0202,[IntPtr]::Zero,[G5]::LP($lx,$ly))
  Start-Sleep -Milliseconds 70
}
function KeyL($vk) {
  [void][G5]::PostMessage($h,0x0100,[IntPtr]$vk,[IntPtr]::Zero)
  [void][G5]::PostMessage($h,0x0101,[IntPtr]$vk,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 50
}

Start-Sleep -Seconds 16
"### entering scene 1 ###"
ClickL 42 448
Start-Sleep -Seconds 5
if ($p.HasExited) { "CRASHED entering scene 1 (release)"; exit }

$crashed=$false; $done=0; $lastStage=''
:outer for ($pass=1; $pass -le 4; $pass++) {
  for ($y=30; $y -le 410; $y += 30) {
    for ($x=25; $x -le 625; $x += 40) {
      ClickL $x $y; $done++
      if ($p.HasExited) { $lastStage="pass=$pass n=$done at scene ($x,$y)"; $crashed=$true; break outer }
    }
    foreach ($vk in 0x26,0x28,0x25,0x27,0x20,0x0D,0x1B,0x31,0x32,0x33) { KeyL $vk; if ($p.HasExited) { $lastStage="pass=$pass after key 0x$($vk.ToString('X'))"; $crashed=$true; break outer } }
  }
  foreach ($tx in 52,180,310,440,604) { ClickL $tx 448; if ($p.HasExited) { $lastStage="toolbar ($tx,448) pass=$pass"; $crashed=$true; break outer } }
  Start-Sleep -Seconds 1
  ClickL 42 448
  Start-Sleep -Seconds 3
  if ($p.HasExited) { $lastStage="re-entering scene 1 pass=$pass"; $crashed=$true; break outer }
  "  pass $pass ok (clicks=$done)"
}
"total clicks=$done  crashed=$crashed  exited=$($p.HasExited)  stage=$lastStage"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
