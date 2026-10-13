# Hold Ctrl -> the port marks every object the engine's click hit test would accept;
# release -> nothing is drawn. Checks the marker count in the debug log.
#
# Why the log and not pixels: the scene animates on its own, so "screenshot before vs
# after" always differs a little even with the overlay off (measured: 34 px of a single
# animated object between two idle frames, against ~534 px of actual markers). The count
# is the honest signal, and it is exact.
#
# Ctrl is a real keybd_event, not a PostMessage: the overlay reads the PHYSICAL key state
# through GetAsyncKeyState (the only keyboard call that answers the same way on Windows
# - real user32 - and on Linux - SDL_GetModState shim, win32_shim.c), and a posted
# WM_KEYDOWN never changes that state.
#
#   & "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File tests\windows\hotspots.ps1
#   ... -Arch x86
param([ValidateSet('x86','x64')][string]$Arch = 'x64')

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class HS {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr LP(int x,int y){ return (IntPtr)(((uint)y<<16)|((uint)x&0xFFFF)); }
}'

$ExeName = if ($Arch -eq 'x64') { 'PILOT_RV_x64.EXE' } else { 'PILOT_RV.EXE' }
$VK_CONTROL = 0x11
$KEYEVENTF_KEYUP = 2
$shots = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $shots | Out-Null

$fail = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
  if ($ok) { "  ok   $what $detail" } else { "  FAIL $what $detail"; $script:fail++ }
}

. "$PSScriptRoot\rundir.ps1"
Get-Process PILOT_RV,PILOT_RV_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$RUN = New-GameRunDir -ExeName $ExeName
# DbgLogInit strips the exe extension: PILOT_RV_x64_debug.log / PILOT_RV_debug.log
$LOG = Join-Path $RUN (($ExeName -replace '\.EXE$','') + '_debug.log')
Remove-Item $LOG -ErrorAction SilentlyContinue

$p = Start-Process -FilePath (Join-Path $RUN $ExeName) -WorkingDirectory $RUN -PassThru
$h = [IntPtr]::Zero
for ($i=0; $i -lt 150 -and $h -eq [IntPtr]::Zero; $i++) {
  Start-Sleep -Milliseconds 200; $p.Refresh()
  if ($p.MainWindowHandle -ne [IntPtr]::Zero) { $h = $p.MainWindowHandle }
}
if ($h -ne [IntPtr]::Zero -and (Get-WindowClass $h) -eq '#32770') {
  Report-StartupFailure $p (Get-StartupDialog $p)
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  exit 1
}
if ($h -eq [IntPtr]::Zero) {
  Report-StartupFailure $p
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  exit 1
}
"$ExeName window: $h"

function Send-CtrlDown { [HS]::keybd_event($VK_CONTROL, 0, 0, [UIntPtr]::Zero) }
function Send-CtrlUp   { [HS]::keybd_event($VK_CONTROL, 0, $KEYEVENTF_KEYUP, [UIntPtr]::Zero) }
function ClickL($x,$y) {
  [void][HS]::PostMessage($h,0x0200,[IntPtr]::Zero,[HS]::LP($x,$y))
  [void][HS]::PostMessage($h,0x0201,[IntPtr]1,[HS]::LP($x,$y))
  [void][HS]::PostMessage($h,0x0202,[IntPtr]::Zero,[HS]::LP($x,$y))
}
# PrintWindow + PW_RENDERFULLCONTENT: takes the window through DWM, so it does not
# depend on the z-order (SetForegroundWindow is refused for a background process).
function Snap($n) {
  $wr = New-Object HS+RECT
  [void][HS]::GetWindowRect($h, [ref]$wr)
  $ww = $wr.Right - $wr.Left; $hh = $wr.Bottom - $wr.Top
  if ($ww -le 0 -or $hh -le 0) { return }
  $bmp = New-Object System.Drawing.Bitmap $ww,$hh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc(); [void][HS]::PrintWindow($h, $hdc, 0x2); $g.ReleaseHdc($hdc)
  $g.Dispose(); $bmp.Save((Join-Path $shots $n)); $bmp.Dispose()
}
function LastHotspotLine {
  Get-Content $LOG -ErrorAction SilentlyContinue |
    Select-String 'SdlUpdateHotspots: Ctrl (down|up), (\d+) interactive' |
    Select-Object -Last 1
}
function MarkerCount([string]$edge) {
  $m = LastHotspotLine
  if (-not $m) { return -1 }
  if ($m.Matches[0].Groups[1].Value -ne $edge) { return -1 }
  return [int]$m.Matches[0].Groups[2].Value
}
function AssertOverlay([string]$where) {
  Send-CtrlDown
  Start-Sleep -Milliseconds 1500
  $down = MarkerCount 'down'
  Snap ("hotspots_{0}_on.png" -f $where)
  Send-CtrlUp
  Start-Sleep -Milliseconds 1500
  $up = MarkerCount 'up'
  Snap ("hotspots_{0}_off.png" -f $where)
  Check "${where}: Ctrl down marks objects" ($down -gt 0) "(count=$down)"
  Check "${where}: Ctrl up clears them"     ($up -eq 0)  "(count=$up)"
}

# Same dwell as pfuzz.ps1: the window exists long before the menu's objects do, and
# measuring the overlay during that window just reads an empty stack.
Start-Sleep -Seconds 16

"### overlay on the startup screen ###"
# The main menu is interactive too, so this is a real scene check, not a smoke test.
AssertOverlay 'startup'

"### entering scene 1 (first toolbar square) ###"
ClickL 42 448
Start-Sleep -Seconds 6
if ($p.HasExited) { "CRASHED entering scene 1"; exit 1 }

"### overlay in scene 1 ###"
AssertOverlay 'scene1'

"### exceptions ###"
$ex = Get-Content $LOG -ErrorAction SilentlyContinue | Select-String 'EXCEPTION: 0xC000|Access violation'
Check 'no exceptions' (-not $ex) ($(if ($ex) { $ex | Select-Object -First 2 | ForEach-Object { $_.Line } } else { '' }))
if ($ex) { $ex | Select-Object -First 4 | ForEach-Object { $_.Line -replace '^\[PILOTS\] ','' } }

if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 300
""
if ($fail -eq 0) { "PASS (screenshots in $shots)"; exit 0 }
"FAIL ($fail check(s)); screenshots in $shots"; exit 1
