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
# Признаки в debug-логе (port-main: подкласса дочернего окна НЕТ - и не нужен:
# щелчок по AVIWnd32 гонит WM_PARENTNOTIFY через DefWindowProc до родителя, и
# ветка WM_PARENTNOTIFY в Host_AviWndProc останавливает воспроизведение сама -
# измерено этим же харнессом; раньше все прогоны кликали мимо в чужое окно
# консоли и цепочка никогда не проверялась):
#   1. AVIWnd32 виден среди детей окна игры и его rect покрывает точку клика -
#      дочернее окно mciavi32 существует и перекрывает центр картинки;
#   2. PlayAvi: click on the AVI child (WM_PARENTNOTIFY ...) - щелчок дошёл до
#      родителя и ветка стопа сработала;
#   3. 'click L: ' в логе НЕ появляется - решение принимает Host_AviWndProc, а не
#      обычный путь окна игры;
#   4. PlayAvi: end playing= приходит за секунды после клика, а не через ~34 с -
#      MCI_STOP дошёл, MM_MCINOTIFY погасил модальный цикл.
#
# Без цепочки остаётся только доигрывание: клик в центр молча проглатывается
# и ролик идёт все 34 с.

param([ValidateSet('x86','x64')][string]$Arch = 'x64', [ValidateSet('bat','cmake')][string]$Build = 'bat')

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
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  public static string ClassName(IntPtr h) {
    var sb = new StringBuilder(64); GetClassNameW(h, sb, 64); return sb.ToString();
  }
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
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    public struct RECT { public int left, top, right, bottom; }
    // Screen rect of the first child with the given class - the AVI child may sit
    // somewhere other than where the picture is, which is exactly what a click
    // that hits nothing looks like.
    public static string ChildRect(IntPtr parent, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, delegate(IntPtr c, IntPtr l) {
            var cn = new StringBuilder(64); GetClassNameW(c, cn, 64);
            if (cn.ToString() == cls) { found = c; return false; }
            return true;
        }, IntPtr.Zero);
        if (found == IntPtr.Zero) return "no " + cls;
        RECT r; GetWindowRect(found, out r);
        return string.Format("0x{0:X} screen rect {1},{2}-{3},{4}",
                             found.ToInt64(), r.left, r.top, r.right, r.bottom);
    }
}
'@
}

$ErrorActionPreference = 'Stop'
$SRC  = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$exe  = if ($Arch -eq 'x64') { 'PILOT_RE_x64.EXE' } else { 'PILOT_RE.EXE' }
$SHOT = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $SHOT | Out-Null

$failed = @()
function Check([string]$name, [bool]$ok) {
  "{0,-52} {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })
  if (-not $ok) { $script:failed += $name }
}

# Убить остатки процесса ДО сборки: link не откроет занятый exe - напечатает
# BUILD FAILED, и тест молча продолжит на предыдущем бинарнике. Собирается именно
# та архитектура, что гоняется (у каждой свой каталог out\x86 / out\x64 со своей
# SDL2.dll, так что чужой бинарник тут уже не помешает).
Get-Process PILOT_RE,PILOT_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

. "$PSScriptRoot\rundir.ps1"
$exeDir = Build-GameExe -ExeName $exe -Config Debug -Build $Build   # out\x86 / out\x64 | build-win32 / build-win64

$RUN = New-GameRunDir -ExeName $exe -FromDir $exeDir
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
  if (LogHas 'PlayAvi: MCI device') { $ready = $true }
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
# и перекрывает картинку: без него клик в центр ушёл бы в окно игры мимо всей цепочки.
# Спрашивать надо ДО клика: как только ролик закончен, Host_AviClose закрывает это окно.
$children = [CSEx]::ChildClasses($h)
"AVIWnd child before the click: '$children'"
"AVI child: $([CSEx]::ChildRect($h, 'AVIWnd32'))"

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
$under = [CS]::WindowFromPoint($pt)
"under the point: '$([CS]::ClassName($under))' 0x$([Convert]::ToString($under.ToInt64(),16))"

[void][CS]::ShowWindow($h, 9)
[void][CS]::BringWindowToTop($h)
# Windows отказывает фоновому процессу стать foreground, и под точкой может остаться
# чужое окно (измерено: VirtualConsoleClass консоли харнесса). Обход - клавиша Alt
# рядом с SetForegroundWindow (см. Raise-And-Check в fullscreen.ps1), иначе реальный
# курсор кликает не в игру и проверка молча измеряет не ту цепочку.
[CS]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)      # ALT down
[void][CS]::SetForegroundWindow($h)
[CS]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)      # ALT up
Start-Sleep -Milliseconds 300
$under = [IntPtr]::Zero
for ($i=0; $i -lt 6 -and ([CS]::ClassName($under) -ne 'AVIWnd32'); $i++) {
  [void][CS]::SetCursorPos($pt.x, $pt.y)
  Start-Sleep -Milliseconds 200
  $under = [CS]::WindowFromPoint($pt)
  "under the point: '$([CS]::ClassName($under))' 0x$([Convert]::ToString($under.ToInt64(),16))"
  if ([CS]::ClassName($under) -ne 'AVIWnd32') {
    [void][CS]::BringWindowToTop($h)
    Start-Sleep -Milliseconds 200
  }
}
if ([CS]::ClassName($under) -eq 'AVIWnd32') {
  [void][CS]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
  Start-Sleep -Milliseconds 60
  [void][CS]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
  "clicked with the real cursor"
} else {
  # Запасной путь: щелчок прямо в окно игры (его ветка WM_LBUTTONDOWN в
  # Host_AviWndProc останавливает воспроизведение сама - вторая половина
  # оригинального механизма). В дочернее окно не шлём: без харнесса ОС
  # hit-test mciavi32 может съесть такой постинг молча.
  $lp = [IntPtr]((((240 * $scale) -shl 16) -bor [int](320 * $scale)))
  "AVIWnd32 is not under the cursor - posted WM_LBUTTONDOWN to the game window"
  [void][CS]::PostMessage($h, 0x201, [UIntPtr]1, $lp)
}

$t0 = Get-Date
# Ни одного 'click L: ' - значит клик съел AVIWnd32, а не окно игры (см. header).
$ateClick = $false
for ($i=0; $i -lt 10; $i++) {
  Start-Sleep -Milliseconds 200
  if (LogHas 'click L: ') { $ateClick = $true; break }
}
"after click: game saw its own button-down: $ateClick"

# Host_PlayAvi уходит из модального цикла только по MM_MCINOTIFY, то есть либо по
# концу ролика, либо после MCI_STOP по нашему клику (WM_PARENTNOTIFY от AVIWnd32).
# INTRO.AVI длится ~34 с, так что дедлайн в 20 с отличает пропуск от доигрывания
# однозначно.
$adv = $false; $dt = 0
for ($i=0; $i -lt 100 -and -not $adv; $i++) {        # 20 с - прерванный ролик кончается за доли секунды
  Start-Sleep -Milliseconds 200
  $adv = LogHas 'PlayAvi: end playing='
  if ($p.HasExited) { break }
}
if ($adv) { $dt = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1) }
"video over ${dt}s after the click (INTRO.AVI plays ~34s)"
Snap "${Arch}_click_after"

$skippedVia = if (LogHas 'PlayAvi: click on the AVI child') { 'AVI child (WM_PARENTNOTIFY)' }
              elseif (LogHas 'PlayAvi: click on the game window') { 'game window' }
              else { '<none>' }
"stop branch: $skippedVia"
Check 'video opened'                       $true
Check 'AVIWnd32 child exists'              ($children -match 'AVIWnd32')
Check 'the click went to AVIWnd32, not the game window' (-not $ateClick)
Check 'the stop branch fired'              ($skippedVia -ne '<none>')
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
