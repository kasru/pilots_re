# Клик ПО РОЛИКУ должен прерывать его — как клик РЯДОМ с роликом.
#
#   & "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File tests\windows\clickskip.ps1 -Arch x64
#
# Собирает указанный exe последним (иначе в SRC лежит SDL2.dll другой разрядности и
# процесс не стартует вообще), кладёт в черновой каталог SRC\MOVIES (rundir.ps1 его
# намеренно не кладёт, чтобы остальные харнессы были детерминированными), ждёт, пока
# движок откроет MCI-устройство, и бьёт реальным курсором в центр картинки 320x240,
# стоящей в клиенте на (160,120).
#
# Ровно реальным курсором и ровно потому, что тестируемая цепочка начинается с
# hit-test'а: mciavi32 рисует в собственное окно AVIWnd32, дочернее относительно
# окна игры, и оно съедает WM_LBUTTONDOWN. PostMessage прямо в окно игры этот обход
# минует и доказал бы ровно ничего.
#
# Три объективных признака в debug-логе:
#   1. MciChildForwardProc: Msg=0x201  - подкласс дочернего окна переслал клик окну игры;
#   2. VideoWndProc: skip request     - игровое окно приняло его и увело MCI_STOP;
#   3. PlayIntroVideo: wait loop ended- цикл ожидания ролика закончился за секунды,
#      а не через ~34 с, на которые рассчитан INTRO.AVI.
#
# Без подкласса (т.е. до фикса) остаётся только #3: клик в центр молча проглатывается
# и ролик доигрывает до конца.

param([ValidateSet('x86','x64')][string]$Arch = 'x64')

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Text; using System.Runtime.InteropServices;
public class CS {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  public struct RECT { public int left, top, right, bottom; }
  public struct POINT { public int x, y; }
}'
# Харнесс обязан быть DPI-aware, иначе GetClientRect вернёт виртуализированные 640x480,
# ClientToScreen - физические экранные, и клик уйдёт мимо картинки. С SetProcessDPIAware
# все три работают в физических пикселях, как и у игры (которая awareness не имеет,
# отсюда её клиент 800x600 при логических 640x480).
[void][CS]::SetProcessDPIAware()
if (-not ('CSEx' -as [type])) {
Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class CSEx {
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    public static string ChildClasses(IntPtr parent) {
        var sb = new StringBuilder();
        EnumChildWindows(parent, delegate(IntPtr c, IntPtr l) {
            var cls = new StringBuilder(64); GetClassNameW(c, cls, 64);
            var txt = new StringBuilder(256); GetWindowTextW(c, txt, 256);
            sb.Append(cls.ToString()); sb.Append('"'); sb.Append(txt.ToString()); sb.Append('"');
            sb.Append(' ');
            return true;
        }, IntPtr.Zero);
        return sb.ToString();
    }
}
'@
}

$ErrorActionPreference = 'Stop'
$SRC  = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$exe  = if ($Arch -eq 'x64') { 'PILOT_RV_x64.EXE' } else { 'PILOT_RV.EXE' }
$SHOT = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $SHOT | Out-Null

$failed = @()
function Check([string]$name, [bool]$ok) {
  "{0,-52} {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })
  if (-not $ok) { $script:failed += $name }
}

# Убить остатки процесса ДО сборки: SDL2.dll общая по имени для обеих архитектур, так
# что собирать надо нужную последней, а link не откроет занятый exe - напечатает
# BUILD FAILED, и тест молча продолжит на предыдущем бинарнике.
Get-Process PILOT_RV,PILOT_RV_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$bat = if ($Arch -eq 'x64') { 'debug x64' } else { 'debug x86' }
"building $Arch DEBUG (build_sdl.bat $bat)..."
& cmd /c "`"$SRC\build_sdl.bat $bat`"" | Select-Object -Last 1
if (-not (Test-Path (Join-Path $SRC $exe))) { "no exe: $SRC\$exe"; exit 1 }

. "$PSScriptRoot\rundir.ps1"
$RUN = New-GameRunDir -ExeName $exe
$LOG = Join-Path $RUN (($exe -replace '\.EXE$','') + '_debug.log')
Remove-Item $LOG -ErrorAction SilentlyContinue

# MOVIES рядом с exe: main.c ищет .\movies\... и голые .\intro.AVI относительно CWD
# процесса. Каталог запуска ОБЩИЙ для всех харнессов и по контракту обязан быть без
# роликов (см. rundir.ps1): x86check меряет маркеры за 12 с, а играющее интро съедает
# это окно целиком. Поэтому копии убираются за собой - см. finally ниже.
$movies = @()
try {
foreach ($f in Get-ChildItem (Join-Path $SRC 'MOVIES') -File) {
  $dst = Join-Path $RUN $f.Name
  Copy-Item $f.FullName $dst -Force
  $movies += $dst
}

$p = Start-Process -FilePath (Join-Path $RUN $exe) -WorkingDirectory $RUN -PassThru
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
"game window: $h"

function Snap($n) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $g.Dispose(); $bmp.Save((Join-Path $SHOT "$n.png")); $bmp.Dispose()
}
function LogHas([string]$pat) { [bool](Get-Content $LOG -ErrorAction SilentlyContinue | Select-String -SimpleMatch $pat | Select-Object -First 1) }

"waiting for the video device..."
$ready = $false
for ($i=0; $i -lt 300 -and -not $ready; $i++) {
  Start-Sleep -Milliseconds 200
  if (LogHas 'OpenMCIVideoDevice: MoveWindow(') { $ready = $true }
  if ($p.HasExited) { break }
}
if (-not $ready) {
  "video never opened (exe exited=$($p.HasExited)) - nothing to click on"
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  exit 1
}
Start-Sleep -Seconds 2                              # AVIWnd32 must be up and painting
Snap "${Arch}_click_before"
# Классы детей окна - единственный прямой признак того, что AVIWnd32 и вправду существует
# и перекрывает картинку: без него клик в центр ушёл бы в окно игры мимо всего фикса.
# Спрашивать надо ДО клика: как только ролик закончен, PlayIntroVideo закрывает это окно.
"AVIWnd child before the click: '$([CSEx]::ChildClasses($h))'"

# Центр картинки: логическое (320,240) внутри клиентских 640x480. Харнесс помечен
# DPI-aware, так что клиентский прямоугольник, ClientToScreen и SetCursorPos все
# работают в физических пикселях - клиент игры здесь 800x600 при логических 640x480.
$rc = New-Object CS+RECT
[void][CS]::GetClientRect($h, [ref]$rc)
$scale = $rc.right / 640.0
$pt = New-Object CS+POINT
$pt.x = [int](320 * $scale); $pt.y = [int](240 * $scale)
[void][CS]::ClientToScreen($h, [ref]$pt)
"client $($rc.right)x$($rc.bottom) scale=$([math]::Round($scale,3)) -> screen click at ($($pt.x),$($pt.y))"

[void][CS]::ShowWindow($h, 9)
[void][CS]::BringWindowToTop($h)
[void][CS]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 300
[void][CS]::SetCursorPos($pt.x, $pt.y)
Start-Sleep -Milliseconds 150
[void][CS]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
Start-Sleep -Milliseconds 60
[void][CS]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP

$t0 = Get-Date
$relay = $false; $skip = $false
for ($i=0; $i -lt 50 -and -not ($relay -and $skip); $i++) {
  Start-Sleep -Milliseconds 200
  if (-not $relay) { $relay = LogHas 'MciChildForwardProc: Msg=0x201' }
  if (-not $skip)  { $skip  = LogHas 'VideoWndProc: skip request' }
  if ($p.HasExited) { break }
}
"after click: child relay=$relay  skip request=$skip"

# PlayIntroVideo уходит из цикла ожидания только по MM_MCINOTIFY, то есть либо по
# концу ролика, либо по нашему клику. INTRO.AVI длится ~34 с, так что дедлайн в 20 с
# отличает пропуск от доигрывания однозначно.
$adv = $false; $dt = 0
for ($i=0; $i -lt 100 -and -not $adv; $i++) {        # 20 с
  Start-Sleep -Milliseconds 200
  $adv = LogHas 'PlayIntroVideo: wait loop ended'
  if ($p.HasExited) { break }
}
if ($adv) { $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
"video over ${dt}s after the click (INTRO.AVI plays ~34s)"
Snap "${Arch}_click_after"

Check 'video opened'                       $true
Check 'AVIWnd32 child exists'              (LogHas 'MciChildForwardClicks: subclassed')
Check "MciChildForward relay (Msg=0x201)"  $relay
Check 'VideoWndProc skip request'          $skip
Check 'playback ended early'               ($adv -and $dt -lt 20)
Check 'no exception in the log'            (-not (Get-Content $LOG -EA SilentlyContinue | Select-String 'EXCEPTION: 0xC000|Access violation' | Select-Object -First 1))

Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 500
} finally {
  # Ролики убраны: общий каталог запуска снова без MOVIES, как и требует контракт rundir.ps1
  foreach ($m in $movies) { Remove-Item $m -Force -ErrorAction SilentlyContinue }
}

if ($failed) { "FAILED: $($failed -join ', ')"; exit 1 }
"PASSED"
exit 0
