Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class G2 {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  public struct POINT { public int X, Y; }
  public static IntPtr LP(int x,int y){ return (IntPtr)(((uint)y<<16)|((uint)x&0xFFFF)); }
}'

$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$LOG = "$SRC\out\PILOTS_RE_x64_debug.log"
$SHOT = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $SHOT | Out-Null

Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Remove-Item $LOG -ErrorAction SilentlyContinue

$p = Start-Process -FilePath "$SRC\out\PILOTS_RE_x64.EXE" -WorkingDirectory $SRC -PassThru
$h = [IntPtr]::Zero
for ($i=0; $i -lt 150 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200; $p.Refresh(); if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle } }
"window handle: $h"
if ($h -eq [IntPtr]::Zero) { "no window"; exit }

function FocusWin {
  if ([G2]::GetForegroundWindow() -eq $h) { return }
  [void][G2]::ShowWindow($h,9); [void][G2]::BringWindowToTop($h)
  [G2]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [void][G2]::SetForegroundWindow($h); [G2]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
  Start-Sleep -Milliseconds 200
}
function ClickL($lx,$ly) {
  FocusWin
  $o = New-Object G2+POINT; $o.X=[int]($lx*1.25); $o.Y=[int]($ly*1.25)
  [void][G2]::ClientToScreen($h,[ref]$o); [void][G2]::SetCursorPos($o.X,$o.Y)
  [void][G2]::PostMessage($h,0x0200,[IntPtr]::Zero,[G2]::LP($lx,$ly))
  [void][G2]::PostMessage($h,0x0201,[IntPtr]1,[G2]::LP($lx,$ly))
  [void][G2]::PostMessage($h,0x0202,[IntPtr]::Zero,[G2]::LP($lx,$ly))
  Start-Sleep -Milliseconds 90
}
function KeyL($vk) {
  [void][G2]::PostMessage($h,0x0100,[IntPtr]$vk,[IntPtr]::Zero)   # WM_KEYDOWN
  [void][G2]::PostMessage($h,0x0101,[IntPtr]$vk,[IntPtr]::Zero)   # WM_KEYUP
  Start-Sleep -Milliseconds 60
}

Start-Sleep -Seconds 16
FocusWin
"started, entering scene 1"
ClickL 42 448
Start-Sleep -Seconds 5
if ($p.HasExited) { "CRASHED entering scene 1"; Get-Content $LOG | Select-String -Pattern 'EXCEPTION|Access violation' | Select-Object -First 3 | ForEach-Object { $_.Line } ; exit }

"### fuzzing scene 1 ###"
$crashed = $false
$done = 0
:outer for ($pass = 1; $pass -le 3; $pass++) {
  # dense grid over the scene area
  for ($y = 30; $y -le 400; $y += 35) {
    for ($x = 30; $x -le 620; $x += 45) {
      ClickL $x $y; $done++
      if ($p.HasExited) { "CRASHED at pass=$pass click=$done  scene ($x,$y)"; $crashed = $true; break outer }
    }
    # sprinkle keyboard input between rows
    KeyL 0x26; KeyL 0x28; KeyL 0x25; KeyL 0x27; KeyL 0x20; KeyL 0x0D; KeyL 0x1B
    if ($p.HasExited) { "CRASHED at pass=$pass after keys row y=$y"; $crashed = $true; break outer }
  }
  # toolbar row: cycle through the scene buttons and come back
  foreach ($tx in 52,180,310,440,604) { ClickL $tx 448; if ($p.HasExited) { "CRASHED at toolbar ($tx,448) pass=$pass"; $crashed = $true; break outer } }
  Start-Sleep -Seconds 1
  ClickL 42 448   # back into scene 1 via the first square
  Start-Sleep -Seconds 3
  if ($p.HasExited) { "CRASHED re-entering scene 1 pass=$pass"; $crashed = $true; break outer }
  "  pass $pass done (total clicks=$done)"
}
"total clicks=$done  crashed=$crashed  exited=$($p.HasExited)"
Start-Sleep -Seconds 3
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500
""
"### exceptions ###"
$ex = Get-Content $LOG | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation'
if ($ex) { $ex | Select-Object -First 4 | ForEach-Object { $_.Line -replace '^\[PILOTS\] ','' } } else { "(none)" }
"### log lines = $((Get-Content $LOG).Count) ###"
