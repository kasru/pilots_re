Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class GP {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  public static IntPtr LP(int x,int y){ return (IntPtr)(((uint)y<<16)|((uint)x&0xFFFF)); }
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
  [void][GP]::ShowWindow($h,9); [void][GP]::BringWindowToTop($h)
  [GP]::keybd_event(0x12,0,0,[UIntPtr]::Zero); [void][GP]::SetForegroundWindow($h); [GP]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
  Start-Sleep -Milliseconds 150
}
function ClickL($lx,$ly) {
  [void][GP]::PostMessage($h,0x0200,[IntPtr]::Zero,[GP]::LP($lx,$ly))
  [void][GP]::PostMessage($h,0x0201,[IntPtr]1,[GP]::LP($lx,$ly))
  [void][GP]::PostMessage($h,0x0202,[IntPtr]::Zero,[GP]::LP($lx,$ly))
}
function KeyL($vk) {
  [void][GP]::PostMessage($h,0x0100,[IntPtr]$vk,[IntPtr]::Zero)
  [void][GP]::PostMessage($h,0x0101,[IntPtr]$vk,[IntPtr]::Zero)
}
function Snap($n){
  $b=[System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp=New-Object System.Drawing.Bitmap $b.Width,$b.Height
  $g=[System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location,[System.Drawing.Point]::Empty,$b.Size)
  $g.Dispose(); $bmp.Save((Join-Path $SHOT "$n.png")); $bmp.Dispose()
}
function Probe($tag) {
  $e = 0
  if (Test-Path $LOG) { $e = (Get-Content $LOG -ErrorAction SilentlyContinue | Select-String -SimpleMatch 'EXCEPTION: 0xC000').Count }
  if ($p.HasExited) { "CRASHED at $tag   exitcode=$($p.ExitCode)  exceptions=$e"; return $true }
  return $false
}

Start-Sleep -Seconds 16
FocusWin
$script:stage = 'start'
$script:done  = $false

"### entering scene 1 ###"
ClickL 42 448; Start-Sleep -Seconds 6
if (Probe 'scene1 entry') { $script:done=$true }

if (-not $script:done) {
  # realistic object clicks: click, then let the game react for 4 seconds
  $objects = @(
    @(500,330,'tuba-player'), @(540,370,'green-figure'), @(250,330,'barrel'),
    @(180,300,'door'),        @(470,285,'bell'),         @(400,300,'gate'),
    @(300,250,'street'),      @(120,250,'house'),        @(600,300,'right-side'),
    @(80,350,'left-ground'),  @(350,380,'mid-ground'),   @(200,200,'sky-left')
  )
  for ($pass = 1; $pass -le 3 -and -not $script:done; $pass++) {
    "  pass $pass"
    foreach ($o in $objects) {
      ClickL $o[0] $o[1]
      Start-Sleep -Seconds 4
      if (Probe "pass$pass $($o[2])") { $script:done=$true; $script:stage="$($o[2]) pass$pass"; break }
      # typing: in case a text prompt opened
      foreach ($k in 0x41,0x42,0x43,0x0D,0x1B,0x08) { KeyL $k }
      Start-Sleep -Milliseconds 400
      if (Probe "pass$pass keys after $($o[2])") { $script:done=$true; $script:stage="keys pass$pass"; break }
    }
    if ($script:done) { break }
    # inventory / help / exit row with dwells
    foreach ($tx in 130,255,360,470,604) {
      ClickL $tx 448; Start-Sleep -Seconds 3
      if (Probe "pass$pass toolbar $tx") { $script:done=$true; $script:stage="toolbar $tx pass$pass"; break }
    }
    if ($script:done) { break }
    ClickL 42 448; Start-Sleep -Seconds 5
    if (Probe "pass$pass re-enter scene1") { $script:done=$true; $script:stage="re-enter pass$pass"; break }
    Snap "play_p$pass"
  }
}

if (-not $script:done) {
  "### sweeping the other scenes ###"
  foreach ($tx in 180,310,440,604) {
    ClickL $tx 448; Start-Sleep -Seconds 6
    if (Probe "scene-sweep $tx") { $script:done=$true; $script:stage="sweep $tx"; break }
    Snap "sweep_$tx"
  }
}

if (-not $script:done) {
  "### map flags ###"
  ClickL 604 448; Start-Sleep -Seconds 5   # exit to map
  if (-not $script:done) {
    foreach ($f in @(568,218,'flag1'), @(424,246,'flag2')) {
      ClickL $f[0] $f[1]; Start-Sleep -Seconds 6
      if (Probe "map $($f[2])") { $script:done=$true; $script:stage="map $($f[2])"; break }
      Snap "map_$($f[2])"
      ClickL 604 448; Start-Sleep -Seconds 4
      if (Probe "map back from $($f[2])") { $script:done=$true; $script:stage="map back $($f[2])"; break }
    }
  }
}

"finished: crashed=$($script:done) stage=$($script:stage) exited=$($p.HasExited)"
Snap 'play_end'
Start-Sleep -Seconds 2
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 500
""
"### exceptions ###"
if (Test-Path $LOG) {
  $ex = Get-Content $LOG | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation'
  if ($ex) { $ex | Select-Object -First 6 | ForEach-Object { $_.Line -replace '^\[PILOTS\] ','' } } else { "(none)" }
  "log lines = $((Get-Content $LOG).Count)"
} else { "(no log)" }
