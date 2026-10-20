Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class G4 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  public static IntPtr LP(int x,int y){ return (IntPtr)(((uint)y<<16)|((uint)x&0xFFFF)); }
}'

$SRC  = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$SHOT = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $SHOT | Out-Null

Get-Process PILOT_RV,PILOT_RV_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

# Scratch dir holding the data but NOT MOVIES: keeps the run deterministic
# instead of depending on whether the videos are installed (see rundir.ps1).
. "$PSScriptRoot\rundir.ps1"
$RUN = New-GameRunDir -ExeName 'PILOT_RV_x64.EXE'
$LOG = Join-Path $RUN 'PILOT_RV_x64_debug.log'
Remove-Item $LOG -ErrorAction SilentlyContinue

$p = Start-Process -FilePath "$RUN\PILOT_RV_x64.EXE" -WorkingDirectory $RUN -PassThru
$h = [IntPtr]::Zero
for ($i=0; $i -lt 150 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle } }
# When the exe cannot load, the OS box IS the process' only window, so
# MainWindowHandle hands us a #32770 dialog instead of the SDL game window.
if ($h -ne [IntPtr]::Zero -and (Get-WindowClass $h) -eq '#32770') {
  Report-StartupFailure $p (Get-StartupDialog $p); if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }; exit 1
}
"window handle: $h"
if ($h -eq [IntPtr]::Zero) { Report-StartupFailure $p; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }; exit 1 }

# best-effort focus, purely cosmetic: PostMessage does not depend on it
function FocusWin {
  [void][G4]::ShowWindow($h,9); [void][G4]::BringWindowToTop($h)
  [G4]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [void][G4]::SetForegroundWindow($h); [G4]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
  Start-Sleep -Milliseconds 150
}
function ClickL($lx,$ly) {
  [void][G4]::PostMessage($h,0x0200,[IntPtr]::Zero,[G4]::LP($lx,$ly))
  [void][G4]::PostMessage($h,0x0201,[IntPtr]1,[G4]::LP($lx,$ly))
  [void][G4]::PostMessage($h,0x0202,[IntPtr]::Zero,[G4]::LP($lx,$ly))
  Start-Sleep -Milliseconds 70
}
function KeyL($vk) {
  [void][G4]::PostMessage($h,0x0100,[IntPtr]$vk,[IntPtr]::Zero)
  [void][G4]::PostMessage($h,0x0101,[IntPtr]$vk,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 50
}
function ReceivedClicks { (Get-Content $LOG -ErrorAction SilentlyContinue | Select-String -SimpleMatch 'msg=0x201').Count }
function Snap($n){
  $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height
  $g=[System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size)
  $g.Dispose(); $bmp.Save((Join-Path $SHOT "$n.png")); $bmp.Dispose()
}

Start-Sleep -Seconds 16
FocusWin
"### entering scene 1 (first toolbar square) ###"
ClickL 42 448
Start-Sleep -Seconds 5
$rc = ReceivedClicks
"game received $rc button-down messages"
Snap 'pf_zoo'
if ($p.HasExited) { "CRASHED entering scene 1"; Get-Content $LOG | Select-String -Pattern 'EXCEPTION|Access violation' | Select-Object -First 3 | ForEach-Object { $_.Line }; exit }

"### fuzzing scene 1 (PostMessage only, cursor untouched) ###"
$crashed=$false; $done=0
:outer for ($pass=1; $pass -le 2; $pass++) {
  for ($y=30; $y -le 410; $y += 30) {
    for ($x=25; $x -le 625; $x += 40) {
      ClickL $x $y; $done++
      if ($p.HasExited) { "CRASHED pass=$pass n=$done at scene ($x,$y)"; $crashed=$true; break outer }
    }
    foreach ($vk in 0x26,0x28,0x25,0x27,0x20,0x0D,0x1B,0x31,0x32,0x33) { KeyL $vk; if ($p.HasExited) { "CRASHED pass=$pass after key 0x$($vk.ToString('X'))"; $crashed=$true; break outer } }
  }
  # toolbar: walk through every button and return with the first square
  foreach ($tx in 52,180,310,440,604) { ClickL $tx 448; if ($p.HasExited) { "CRASHED toolbar ($tx,448) pass=$pass"; $crashed=$true; break outer } }
  Start-Sleep -Seconds 1
  ClickL 42 448
  Start-Sleep -Seconds 3
  if ($p.HasExited) { "CRASHED re-entering scene 1 pass=$pass"; $crashed=$true; break outer }
  "  pass $pass ok (clicks=$done, game received=$((ReceivedClicks)) downs)"
}
"total clicks=$done  crashed=$crashed  exited=$($p.HasExited)"
Snap 'pf_end'
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500
""
"### exceptions ###"
$ex = Get-Content $LOG | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation'
if ($ex) { $ex | Select-Object -First 4 | ForEach-Object { $_.Line -replace '^\[PILOTS\] ','' } } else { "(none)" }
"### log lines = $((Get-Content $LOG).Count) ###"
