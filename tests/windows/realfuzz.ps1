Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class G3 {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, uint d, UIntPtr e);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  public struct POINT { public int X, Y; }
}'

$SRC  = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
. "$PSScriptRoot\rundir.ps1"
$OUT  = Get-OutDirForExe $SRC 'PILOTS_RE_x64.EXE'   # out\x64
$LOG  = Join-Path $OUT 'PILOTS_RE_x64_debug.log'
$SHOT = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $SHOT | Out-Null

Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Remove-Item $LOG -ErrorAction SilentlyContinue

$p = Start-Process -FilePath (Join-Path $OUT 'PILOTS_RE_x64.EXE') -WorkingDirectory $SRC -PassThru
$h = [IntPtr]::Zero
for ($i=0; $i -lt 150 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle } }
"window handle: $h"
if ($h -eq [IntPtr]::Zero) { "no window"; exit }

function FocusWin {
  if ([G3]::GetForegroundWindow() -eq $h) { return $true }
  [void][G3]::ShowWindow($h,9); [void][G3]::BringWindowToTop($h)
  [G3]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [void][G3]::SetForegroundWindow($h); [G3]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
  Start-Sleep -Milliseconds 250
  return ([G3]::GetForegroundWindow() -eq $h)
}
$script:origin = $null
function ToScreen($lx,$ly) {
  if (-not $script:origin) {
    FocusWin | Out-Null
    $o = New-Object G3+POINT; $o.X=0; $o.Y=0
    [void][G3]::ClientToScreen($h,[ref]$o)
    $script:origin = $o
  }
  return @([int]($script:origin.X + $lx*1.25), [int]($script:origin.Y + $ly*1.25))
}
function RealClick($lx,$ly,$button) {
  if (-not (FocusWin)) { "FOCUS LOST - abort"; exit }
  $s = ToScreen $lx $ly
  [void][G3]::SetCursorPos($s[0],$s[1])
  Start-Sleep -Milliseconds 60
  switch ($button) {
    'L' { [G3]::mouse_event(0x0002,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 50; [G3]::mouse_event(0x0004,0,0,0,[UIntPtr]::Zero) }
    'R' { [G3]::mouse_event(0x0008,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 50; [G3]::mouse_event(0x0010,0,0,0,[UIntPtr]::Zero) }
    'D' { [G3]::mouse_event(0x0002,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 40; [G3]::mouse_event(0x0004,0,0,0,[UIntPtr]::Zero)
          Start-Sleep -Milliseconds 40
          [G3]::mouse_event(0x0002,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 40; [G3]::mouse_event(0x0004,0,0,0,[UIntPtr]::Zero) }
    'M' { [G3]::mouse_event(0x0002,0,0,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 40
          [G3]::mouse_event(0x0001,40,20,0,[UIntPtr]::Zero); Start-Sleep -Milliseconds 60   # move while down
          [G3]::mouse_event(0x0004,0,0,0,[UIntPtr]::Zero) }
  }
  Start-Sleep -Milliseconds 80
}
function Snap($n){
  $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height
  $g=[System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size)
  $g.Dispose(); $bmp.Save((Join-Path $SHOT "$n.png")); $bmp.Dispose()
}

Start-Sleep -Seconds 16
FocusWin | Out-Null
"origin=$($script:origin.X),$($script:origin.Y)"
"### entering scene 1 ###"
RealClick 52 448 'L'
Start-Sleep -Seconds 5
Snap 'real_zoo'
if ($p.HasExited) { "CRASHED entering scene 1"; Get-Content $LOG | Select-String -Pattern 'EXCEPTION|Access violation' | Select-Object -First 3 | ForEach-Object { $_.Line }; exit }

"### real-click fuzz ###"
$crashed=$false; $done=0
:outer for ($pass=1; $pass -le 3; $pass++) {
  for ($y=30; $y -le 400; $y += 35) {
    for ($x=30; $x -le 620; $x += 45) {
      $btn = 'L'; if (($done % 17) -eq 5) { $btn='D' }; if (($done % 23) -eq 9) { $btn='R' }; if (($done % 31) -eq 13) { $btn='M' }
      RealClick $x $y $btn; $done++
      if ($p.HasExited) { "CRASHED pass=$pass n=$done ($x,$y) btn=$btn"; $crashed=$true; break outer }
    }
    [G3]::keybd_event(0x26,0,0,[UIntPtr]::Zero); [G3]::keybd_event(0x26,0,2,[UIntPtr]::Zero)
    [G3]::keybd_event(0x28,0,0,[UIntPtr]::Zero); [G3]::keybd_event(0x28,0,2,[UIntPtr]::Zero)
    [G3]::keybd_event(0x20,0,0,[UIntPtr]::Zero); [G3]::keybd_event(0x20,0,2,[UIntPtr]::Zero)
    [G3]::keybd_event(0x0D,0,0,[UIntPtr]::Zero); [G3]::keybd_event(0x0D,0,2,[UIntPtr]::Zero)
    if ($p.HasExited) { "CRASHED pass=$pass after keys y=$y"; $crashed=$true; break outer }
  }
  foreach ($tx in 52,180,310,440,604) { RealClick $tx 448 'L'; if ($p.HasExited) { "CRASHED toolbar ($tx,448) pass=$pass"; $crashed=$true; break outer } }
  Start-Sleep -Seconds 1
  RealClick 52 448 'L'
  Start-Sleep -Seconds 3
  if ($p.HasExited) { "CRASHED re-entering scene 1 pass=$pass"; $crashed=$true; break outer }
  "  pass $pass done (total=$done)"
}
"total=$done crashed=$crashed exited=$($p.HasExited)"
Snap 'real_end'
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500
""
"### exceptions ###"
$ex = Get-Content $LOG | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation'
if ($ex) { $ex | Select-Object -First 4 | ForEach-Object { $_.Line -replace '^\[PILOTS\] ','' } } else { "(none)" }
"### log lines = $((Get-Content $LOG).Count) ###"
