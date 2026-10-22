Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class G {
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

$SRC  = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$LOG  = "$SRC\out\PILOTS_RE_x64_debug.log"
$SHOT = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $SHOT | Out-Null

Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

Remove-Item $LOG -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$SRC\out\PILOTS_RE_x64.EXE" -WorkingDirectory $SRC -PassThru
$h = [IntPtr]::Zero
for ($i=0; $i -lt 150 -and $h -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 200
  $p.Refresh()
  if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle }
}
"window handle: $h"

function FocusWin($tag) {
  if ([G]::GetForegroundWindow() -eq $h) { return $true }
  [void][G]::ShowWindow($h, 9)              # SW_RESTORE
  [void][G]::BringWindowToTop($h)
  [G]::keybd_event(0x12,0,0,[UIntPtr]::Zero)   # ALT down  (unlocks SetForegroundWindow)
  [void][G]::SetForegroundWindow($h)
  [G]::keybd_event(0x12,0,2,[UIntPtr]::Zero)   # ALT up
  Start-Sleep -Milliseconds 250
  $ok = ([G]::GetForegroundWindow() -eq $h)
  "  focus[$tag] = $ok"
  return $ok
}
function Snap($n){
  $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height
  $g=[System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size)
  $g.Dispose(); $bmp.Save((Join-Path $SHOT "$n.png")); $bmp.Dispose()
}
function ClickL($lx,$ly,$tag) {
  $null = FocusWin $tag
  $o = New-Object G+POINT; $o.X = [int]($lx*1.25); $o.Y = [int]($ly*1.25)
  [void][G]::ClientToScreen($h,[ref]$o)
  [void][G]::SetCursorPos($o.X,$o.Y)
  Start-Sleep -Milliseconds 120
  [void][G]::PostMessage($h,0x0200,[IntPtr]::Zero,[G]::LP($lx,$ly))
  Start-Sleep -Milliseconds 120
  [void][G]::PostMessage($h,0x0201,[IntPtr]1,[G]::LP($lx,$ly))
  Start-Sleep -Milliseconds 70
  [void][G]::PostMessage($h,0x0202,[IntPtr]::Zero,[G]::LP($lx,$ly))
  Start-Sleep -Milliseconds 500
  # confirm the game actually received the button-down at these coords
  $last = Get-Content $LOG -ErrorAction SilentlyContinue | Select-String -SimpleMatch 'click L: ' | Select-Object -Last 1
  $want = "$lx,$ly"
  $got  = if ($last) { ($last.Line -replace '^.*click L: ','') } else { '<none>' }
  "  click[$tag] ($lx,$ly)  game got '$got'   want '$want'   match=$($got -eq $want)"
}

Start-Sleep -Seconds 16
"started: log=$((Get-Content $LOG -ErrorAction SilentlyContinue).Count) lines"

"### entering scene 1 (first toolbar square) ###"
ClickL 42 448 'scene1'
Start-Sleep -Seconds 5
Snap 'f_zoo'
"after scene1: exited=$($p.HasExited)  log=$((Get-Content $LOG).Count) lines"
if ($p.HasExited) { "CRASHED entering scene 1"; exit }

"### clicking scene objects ###"
$points = @(
  @(500,330,'tuba-player'), @(540,370,'green-figure'), @(600,300,'right-side'),
  @(250,330,'barrel'),      @(180,300,'door'),         @(470,285,'bell'),
  @(400,300,'gate'),        @(300,250,'street'),       @(120,250,'house'),
  @(42,448,'slot1'),        @(130,448,'slot2'),        @(255,448,'slot3'),
  @(604,448,'book-BEDA'),   @(360,448,'slot4'),        @(470,448,'slot5')
)
foreach ($pt in $points) {
  ClickL $pt[0] $pt[1] $pt[2]
  if ($p.HasExited) { "CRASHED after $($pt[2]) at ($($pt[0]),$($pt[1]))"; break }
}
Start-Sleep -Seconds 4
Snap 'f_end'
"final: exited=$($p.HasExited)  log=$((Get-Content $LOG).Count) lines"
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 400
""
"### exceptions ###"
$ex = Get-Content $LOG | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation'
if ($ex) { $ex | Select-Object -First 5 | ForEach-Object { $_.Line -replace '^\[PILOTS\] ','' } } else { "(none)" }
