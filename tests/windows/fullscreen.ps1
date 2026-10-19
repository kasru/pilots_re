# -fullscreen / Alt+Enter: borderless-полноэкранный режим и целостность координат.
#
#   & "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File tests\windows\fullscreen.ps1 -Arch x64
#
# Проверки (каждая - по строке debug-лога; в release DBG_* это no-ops, потому только debug-сборка):
#
#   1. опция дошла до движка:        SdlBackendParseCmdLine: -fullscreen
#   2. окно стало размером с экран:  SdlApplyFullscreen: on -> ... плюс GetWindowRect
#      действительно равен границам экрана;
#   3. во время ролика mciavi32 рисует свой дочерний виджет 1:1 и растянуть его нельзя,
#      поэтому картинка падает в естественный размер НО остаётся отцентрованной:
#      SdlBackendSetNativeMovie: 1 -> picture 640x480 at (x,y), (x,y) != (0,0);
#   4. клик в центр картинки доходит до процесса и доходит ПРАВИЛЬНЫМИ координатами:
#      MciChildForwardProc (дочернее окно -> клиентское) и
#      SdlGameWndProc: 0x201 client (...) -> game (320,240);
#      прерывание ролика подтверждает VideoWndProc: skip request;
#   5. после ролика картинка масштабирована с сохранением пропорций и отцентрована
#      (SdlUpdateLayout: window ... -> picture WxH at (x,y), WxH != 640x480);
#   6. Alt+Enter выключает и включает режим обратно, окно возвращается к клиенту 640x480.
#   7. САМИ ПИКСЕЛИ, а не только расчёт: SDL 2.30 УЗНАЁТ новый размер окна ТОЛЬКО из
#      WM_WINDOWPOSCHANGED (его обработчик WM_SIZE шлёт лишь RESTORED/MINIMIZED/
#      MAXIMIZED), а мы отдавали это сообщение одной игре. Поэтому swap chain навсегда
#      оставался 640x480 - размер окна при создании, - а SDL_GetRendererOutputSize
#      отвечал живыми 1536x864: картинка рисовалась в буфер 640x480, который DXGI
#      растягивал на весь клиент (2.4x по горизонтали, 1.8x по вертикали, обрезка краем
#      буфера), а все проверки строками проходили. Поэтому:
#      7a. последняя строка SdlDrawGameFrame: вьюпорт равен выходу рендерера;
#      7b. PrintWindow окна (не зависит от z-порядка: полноэкранное окно почти всегда
#          под чужим) - bbox непустой области картинки равен rect'у из раскладки
#          (логическому или пересчитанному в физические пиксели) и пропорция области
#          4:3. Одной долей непустых точек внутри ожидаемого rect'а это не ловилось:
#          сдвиг вправо и обрезка краем буфера давали те же 59% и PASS.
#
# Клик стараемся делать реальным курсором: цепочка начинается с hit-test'а (mciavi32
# принимает WM_LBUTTONDOWN в своём AVIWnd32), а PostMessage этот обход минует - см.
# clickskip.ps1. Но полноэкранное окно игры может оказаться ПОД чужим окном (в фоне
# SetForegroundWindow часто не даёт результата), и mciavi32 может переставить своё
# дочернее окно, так что реальный курсор используется только если под ним ИМЕННО
# AVIWnd32; иначе харнесс шлёт WM_LBUTTONDOWN прямо в AVIWnd32 (координатный
# конвейер при этом проверяется полностью, hit-test - отдельной проверкой 3b).
# Харнесс обязан быть DPI-aware: игра - нет, её клиент в физических пикселях
# 800x600 при логических 640x480 (масштаб 1.25), а полноэкранный клиент - 1536x864.

param([ValidateSet('x86','x64')][string]$Arch = 'x64')

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Text; using System.Runtime.InteropServices;
public class FS {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint dx, uint dy, uint data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, UIntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT p);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, IntPtr cls, IntPtr title);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr insertAfter, int x, int y, int cx, int cy, uint flags);
  public struct RECT { public int left, top, right, bottom; }
  public struct POINT { public int x, y; }
}'
[void][FS]::SetProcessDPIAware()

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

# Остатки процесса убираются ДО сборки: link не откроет занятый exe (см. clickskip.ps1)
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

# Ролики копируются и убираются в finally - контракт общего каталога запуска: без них
# (см. rundir.ps1), иначе маркеры x86check съедает играющее интро.
$movies = @()
try {
foreach ($f in Get-ChildItem (Join-Path $SRC 'MOVIES') -File) {
  $dst = Join-Path $RUN $f.Name
  Copy-Item $f.FullName $dst -Force
  $movies += $dst
}

$p = Start-Process -FilePath (Join-Path $RUN $exe) -WorkingDirectory $RUN -PassThru `
        -ArgumentList '-fullscreen'
$t0 = Get-Date

# Лог читается в кэш и переиспользуется: игровой цикл пишет в него десятки тысяч строк
# в секунду (GameMainLoop_ELSE / LoadSpriteData), а раньше каждый PollAll перечитывал файл
# 12 раз по одному на проверку - харнесс уходил в многоминутное ожидание вместо секунд.
$script:logLines = @()
function Read-Log { $script:logLines = @(Get-Content $LOG -ErrorAction SilentlyContinue) }
function Test-Log([string]$pat) {
  foreach ($ln in $script:logLines) { if ($ln.Contains($pat)) { return $true } }
  return $false
}
function LogHas([string]$pat) { Read-Log; Test-Log $pat }
function LogLine([string]$pat) {
  Read-Log
  foreach ($ln in $script:logLines) { if ($ln.Contains($pat)) { return $ln } }
  return $null
}
function LogLast([string]$pat) {
  $r = $null
  Read-Log
  foreach ($ln in $script:logLines) { if ($ln.Contains($pat)) { $r = $ln } }
  return $r
}
function LogCount([string]$pat) {
  $n = 0
  Read-Log
  foreach ($ln in $script:logLines) { if ($ln.Contains($pat)) { $n++ } }
  return $n
}
# Все верхнеуровневые окна процесса: нужно, чтобы поймать пересоздание главного окна -
# харнесс держит его хэндл один раз (MainWindowHandle) и молча кликает/спрашивает уже в пустоту.
function Proc-TopWindows([int]$gamePid) {
  $out = @(); $after = [IntPtr]::Zero
  for (;;) {
    $w = [FS]::FindWindowEx([IntPtr]::Zero, $after, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($w -eq [IntPtr]::Zero) { break }
    $after = $w
    $tp = [uint32]0
    [void][FS]::GetWindowThreadProcessId($w, [ref]$tp)
    if ($tp -eq [uint32]$gamePid) {
      $r = New-Object FS+RECT; $gr = [FS]::GetWindowRect($w, [ref]$r)
      $out += ("0x{0:X8} '{1}' rect {2},{3}-{4},{5}" -f $w.ToInt64(), (Get-WindowClass $w),
               $r.left, $r.top, $r.right, $r.bottom) + $(if (-not $gr) { ' (GetWindowRect FAILED)' } else { '' })
    }
  }
  $out
}
function Snap($n) {
  $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
  $bmp = New-Object System.Drawing.Bitmap $b.Width, $b.Height
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.CopyFromScreen($b.Location, [System.Drawing.Point]::Empty, $b.Size)
  $g.Dispose(); $bmp.Save((Join-Path $SHOT "$n.png")); $bmp.Dispose()
}

# Хронометраж: каждая строка запоминается ровно один раз, с секундами от старта
# процесса. Без этого "ролик прерван вовремя" и "ролик доиграл сам" неразличимы -
# в логе нет таймстемпов, а буферизация строк отсутствует (DbgPrintf пишет и сбрасывает
# каждую строку), так что порядок строк в файле совпадает с порядком событий.
$watch = @(
  'SdlBackendParseCmdLine: -fullscreen',
  'SdlApplyFullscreen: on ->',
  'SdlApplyFullscreen: off ->',
  'OpenMCIVideoDevice: MoveWindow(',
  'MciChildForwardClicks: subclassed ',
  'PlayIntroVideo: g_MCIDeviceID=1',
  'PlayIntroVideo: PlayMCIVideo -> ',
  'VideoWndProc: MM_MCINOTIFY reason=',
  'PlayIntroVideo: wait loop ended',
  'VideoWndProc: skip request',
  'SdlBackendSetNativeMovie: 1 ->',
  'SdlBackendSetNativeMovie: 0 ->'
)
$marks = @{}
function SeeCached([string]$pat) {
  $t = Test-Log $pat
  if ($t -and -not $marks.ContainsKey($pat)) {
    $marks[$pat] = [math]::Round(((Get-Date) - $t0).TotalSeconds, 1)
  }
  return $t
}
function See([string]$pat) { Read-Log; SeeCached $pat }

function PollAll { Read-Log; foreach ($w in $watch) { [void](SeeCached $w) } }

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
"  top-level windows: " + ((Proc-TopWindows $p.Id) -join ' | ')

# --- 1. опция командной строки ------------------------------------------------
$fsOpt = $false
for ($i=0; $i -lt 150 -and -not $fsOpt; $i++) {
  Start-Sleep -Milliseconds 200
  $fsOpt = See 'SdlBackendParseCmdLine: -fullscreen'
  if ($p.HasExited) { break }
}
Check '-fullscreen parsed' $fsOpt

# --- 2. окно стало размером с экрана ------------------------------------------
$onLine = ''
for ($i=0; $i -lt 150 -and -not $onLine; $i++) {
  Start-Sleep -Milliseconds 200
  PollAll
  $onLine = LogLine 'SdlApplyFullscreen: on ->'
  if ($p.HasExited) { break }
}
Check 'SdlApplyFullscreen: on logged' ([bool]$onLine)
if ($onLine) { "  $($onLine.ToString().Trim())" }

$scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$wr = New-Object FS+RECT
$winFs = $false
for ($i=0; $i -lt 50 -and -not $winFs; $i++) {
  Start-Sleep -Milliseconds 200
  PollAll
  [void][FS]::GetWindowRect($h, [ref]$wr)
  $winFs = ($wr.left -le 0 -and $wr.top -le 0 -and
            ($wr.right - $wr.left) -ge ($scr.Width * 0.95) -and
            ($wr.bottom - $wr.top) -ge ($scr.Height * 0.95))
}
"  window rect ($($wr.left),$($wr.top))-($($wr.right),$($wr.bottom)) vs screen $($scr.Width)x$($scr.Height)"
Check 'window covers the screen' $winFs

# --- 3. ролик: естественный размер картинки, но отцентрованной ----------------
"waiting for the video device..."
$ready = $false
for ($i=0; $i -lt 300 -and -not $ready; $i++) {
  Start-Sleep -Milliseconds 200
  if (See 'OpenMCIVideoDevice: MoveWindow(') { $ready = $true }
  if ($p.HasExited) { break }
}
if (-not $ready) {
  "video never opened (exe exited=$($p.HasExited)) - nothing to click on"
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  exit 1
}
Start-Sleep -Seconds 2                        # AVIWnd32 must be up and painting
PollAll
Snap "${Arch}_fs_movie"

# Естественный размер не означает (0,0): картинка отцентрована в клиенте полноэкранного
# окна, и дочернее окно AVIWnd32 стоит ровно в её центре - иначе клик мимо. Строка
# SetNativeMovie: 1 пишется в OpenMCIVideoDevice, то есть ДО ShowWindow, пока клиент
# ещё 640x480 и смещения нет; итоговое состояние - последняя строка SdlUpdateLayout.
$native = LogLine 'SdlBackendSetNativeMovie: 1 ->'
$layNat = LogLast 'SdlUpdateLayout: '
$natOk = $false
if ($layNat -and $layNat.ToString() -match 'picture 640x480 at \((\d+),(\d+)\) \(native movie\)') {
  $natOk = ([int]$Matches[1] -ne 0) -or ([int]$Matches[2] -ne 0)
}
Check 'movie: picture natural 640x480, centered' $natOk
if ($native) { "  $($native.ToString().Trim())" }
if ($layNat) { "  $($layNat.ToString().Trim())" }

# --- 4. клик в центр картинки -------------------------------------------------
# Центр клиентской области: и в масштабированном, и в естественном режиме картинка
# отцентрована, так что центр окна = центр картинки = логические (320,240).
$rc = New-Object FS+RECT
[void][FS]::GetClientRect($h, [ref]$rc)
$pt = New-Object FS+POINT
$pt.x = [int]($rc.right / 2); $pt.y = [int]($rc.bottom / 2)
[void][FS]::ClientToScreen($h, [ref]$pt)
"client $($rc.right)x$($rc.bottom) -> screen click at ($($pt.x),$($pt.y))"

# Полноэкранное окно игры обычно НАХОДИТСЯ под чужим окном: харнесс запущен фоном,
# и Windows отказывает фоновому процессу в праве стать foreground. Обход - клавиша Alt
# (keybd_event) рядом с SetForegroundWindow + явный подъём в z-порядке; после этого
# перепроверяем WindowFromPoint - кликать можно только в своё окно, иначе курсор
# уйдёт в чужое и ни одна проверка координат не сработает.
function Raise-And-Check {
  [void][FS]::ShowWindow($h, 9)
  [void][FS]::BringWindowToTop($h)
  [FS]::keybd_event(0x12, 0, 0, 0)                     # ALT down: иначе Windows не даёт фокус
  [void][FS]::SetForegroundWindow($h)
  [FS]::keybd_event(0x12, 0, 2, 0)                     # ALT up
  [void][FS]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0001 -bor 0x0002 -bor 0x0004)  # HWND_TOP, no move/size/activate
  Start-Sleep -Milliseconds 400
  $w = [FS]::WindowFromPoint($pt)
  $pid_ = [uint32]0
  [void][FS]::GetWindowThreadProcessId($w, [ref]$pid_)
  return @{ hwnd = $w; pid = $pid_; cls = $(if ($w -ne [IntPtr]::Zero) { Get-WindowClass $w } else { '' }) }
}

$u = Raise-And-Check
$under = $u.hwnd; $underPid = $u.pid; $underCls = $u.cls
"  window at that point: 0x$([Convert]::ToString($under.ToInt64(),16)) pid=$underPid class='$underCls' (game pid=$($p.Id))"
Check 'our window is under the cursor' ($underPid -eq [uint32]$p.Id)
Check 'AVIWnd32 child under the cursor' ($underCls -eq 'AVIWnd32')

# Где дочерние окна на самом деле: AVIWnd32 обязан стоять по центру картинки
# (608,312) 320x240 в логических координатах движка, а мciavi32 может переставить
# своё окно само после начала воспроизведения - тогда клик в центр попадёт в
# родительское окно. Печатаем все rect'ы, чтобы отказ был объяснимым.
$ch = [FS]::FindWindowEx($h, [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
while ($ch -ne [IntPtr]::Zero) {
  $chr = New-Object FS+RECT
  [void][FS]::GetWindowRect($ch, [ref]$chr)
  "  child 0x$([Convert]::ToString($ch.ToInt64(),16)) '$(Get-WindowClass $ch)' rect ($($chr.left),$($chr.top))-($($chr.right),$($chr.bottom))"
  $ch = [FS]::FindWindowEx($h, $ch, [IntPtr]::Zero, [IntPtr]::Zero)
}

$viaReal = $false
if ($underCls -eq 'AVIWnd32') {
  [void][FS]::SetCursorPos($pt.x, $pt.y)
  Start-Sleep -Milliseconds 150
  [void][FS]::mouse_event(0x0002, 0, 0, 0, [UIntPtr]::Zero)   # LEFTDOWN
  Start-Sleep -Milliseconds 60
  [void][FS]::mouse_event(0x0004, 0, 0, 0, [UIntPtr]::Zero)   # LEFTUP
  $viaReal = $true
  "  clicked with the real cursor"
} else {
  # Запасной путь: шлём в AVIWnd32 сами. MciChildForwardProc обязан пересчитать
  # дочерние координаты в клиентские (ClientToScreen/ScreenToClient) и переслать
  # в окно игры - это ровно та конверсия, ради которой клик и нужен.
  $childLine = LogLine 'MciChildForwardClicks: subclassed '
  $child = [IntPtr]::Zero
  if ($childLine -and $childLine.ToString() -match 'subclassed (0x[0-9A-Fa-f]+)') {
    $child = [IntPtr]([Convert]::ToInt64($Matches[1], 16))
  }
  if ($child -ne [IntPtr]::Zero) {
    $crc = New-Object FS+RECT
    [void][FS]::GetClientRect($child, [ref]$crc)
    $x = [int]($crc.right / 2); $y = [int]($crc.bottom / 2)
    $lp = [IntPtr]((([uint32]($y -band 0xFFFF) -shl 16) -bor [uint32]($x -band 0xFFFF)))
    "  posted WM_LBUTTONDOWN ($x,$y) to the child 0x$([Convert]::ToString($child.ToInt64(),16))"
    [void][FS]::PostMessage($child, 0x201, [UIntPtr]1, $lp)
  } else {
    "  no child window - posted WM_LBUTTONDOWN to the game window"
    $x = [int]($rc.right / 2); $y = [int]($rc.bottom / 2)
    $lp = [IntPtr]((([uint32]($y -band 0xFFFF) -shl 16) -bor [uint32]($x -band 0xFFFF)))
    [void][FS]::PostMessage($h, 0x201, [UIntPtr]1, $lp)
  }
}

$relay = $false; $skip = $false; $mapped = ''
for ($i=0; $i -lt 50 -and -not ($relay -and $skip); $i++) {
  Start-Sleep -Milliseconds 200
  PollAll
  if (-not $relay) { $relay = See 'MciChildForwardProc: Msg=0x201' }
  if (-not $skip)  { $skip  = See 'VideoWndProc: skip request' }
  if (-not $mapped) {
    $m = LogLine '-> game (320,240)'
    if ($m) { $mapped = $m.ToString().Trim() }
  }
  if ($p.HasExited) { break }
}
"after click: viaReal=$viaReal  child relay=$relay  skip=$skip"
if ($mapped) { "  $mapped" }

# INTRO.AVI = 528 кадров по 66660 мкс = 35.2 с. Прерванный ролик заканчивается за
# доли секунды, доигранный - примерно через 35 с; и то и другое мы просто измеряем,
# чтобы увидеть разницу в таймстемпах ниже.
$adv = See 'PlayIntroVideo: wait loop ended'
for ($i=0; $i -lt 200 -and -not $adv; $i++) {        # 40 с - больше полной длины ролика
  Start-Sleep -Milliseconds 200
  PollAll
  $adv = See 'PlayIntroVideo: wait loop ended'
  if ($p.HasExited) { break }
}
Snap "${Arch}_fs_after_movie"

Check 'click reached the game process' ($viaReal -or $relay)
Check 'AVIWnd32 child exists'                 (See 'MciChildForwardClicks: subclassed ')
Check 'click mapped to game (320,240)'        ([bool]$mapped)
Check 'child relay (Msg=0x201)'               $relay
Check 'VideoWndProc skip request'             $skip
Check 'video playback finished'               $adv

$naturalOff = LogLine 'SdlBackendSetNativeMovie: 0 ->'
Check 'movie closed, picture scaled again'    ([bool]$naturalOff -and -not $naturalOff.ToString().Contains('picture 640x480 at (0,0)'))
if ($naturalOff) { "  $($naturalOff.ToString().Trim())" }

# --- 5. картинка после ролика: масштаб с сохранением пропорций -----------------
$lay = LogLast 'SdlUpdateLayout: '
$layOk = $false
if ($lay -and $lay.ToString() -match 'window (\d+)x(\d+) -> picture (\d+)x(\d+) at \((-?\d+),(-?\d+)\)') {
  $ww = [int]$Matches[1]; $wh = [int]$Matches[2]
  $pw = [int]$Matches[3]; $ph = [int]$Matches[4]; $px = [int]$Matches[5]; $py = [int]$Matches[6]
  # 4:3 картинка в 16:9 клиенте: ширина меньше окна, высота равна окну, (0,0) невозможен,
  # и пропорции картинки - игровые (640/480), а не оконные.
  $layOk = ($pw -ne $ww) -and ($ph -eq $wh) -and (($px -ne 0) -or ($py -ne 0)) -and
           ([math]::Abs(($pw / [double]$ph) - (640.0 / 480.0)) -lt 0.01)
  "  layout: $($lay.ToString().Trim())"
}
Check 'picture scaled, aspect kept, centered' $layOk

# --- 5b. вьюпорт и ПИКСЕЛИ ------------------------------------------------------
# 7a. Вьюпорт SDL обязан покрывать весь выход рендерера: это ровно то, что сломалось
# (и что ни одна строка раскладки не видит).
$vg = LogLast 'SdlDrawGameFrame: '
$vpOk = $false
if ($vg -and $vg.ToString() -match 'output (\d+)x(\d+) viewport \((-?\d+),(-?\d+) (\d+)x(\d+)\)') {
  $vpOk = ([int]$Matches[3] -eq 0) -and ([int]$Matches[4] -eq 0) -and
          ([int]$Matches[5] -eq [int]$Matches[1]) -and ([int]$Matches[6] -eq [int]$Matches[2])
  "  $($vg.ToString().Trim())"
}
Check 'viewport covers the renderer output' $vpOk

# 7b. Сами пиксели. PrintWindow с PW_RENDERFULLCONTENT идёт через DWM, то есть берёт
# ровно тот кадр, что на экране, и ему всё равно, под чужим окном или нет наше.
# Сверяется bbox ОБЛАСТИ КАРТИНКИ, а не просто "непусто внутри ожидаемого rect":
# картинка, сдвинутая вправо и обрезанная краем буфера (см. WM_WINDOWPOSCHANGED в
# sdl_backend.c), внутри ожидаемого rect'а была непуста на 59% - и ни одна строковая
# проверка этого не видела. Bbox сравнивается и с логическим rect'ом из лога, и с тем
# же rect'ом, пересчитанным в физические пиксели через клиентский прямоугольник,
# каким его видит DPI-aware харнесс: пространство PrintWindow держать в голове не
# нужно, нужно совпадение с ОДНИМ из двух. И независимый от пространства инвариант -
# пропорция области 4:3: растянутый по одной оси кадр его ломает в любом пространстве.
# Курсор в кадре - маленькая яркая запятая, поэтому колонка/строка считается
# содержимым при непустой доле >= 25%, а не при одном ярком пикселе.
function Grab-Window([IntPtr]$hh) {
  $r = New-Object FS+RECT
  [void][FS]::GetWindowRect($hh, [ref]$r)
  $bw = $r.right - $r.left; $bh = $r.bottom - $r.top
  if ($bw -le 0 -or $bh -le 0) { return $null }
  $bmp = New-Object System.Drawing.Bitmap $bw, $bh
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $hdc = $g.GetHdc()
  [void][FS]::PrintWindow($hh, $hdc, 0x2)
  $g.ReleaseHdc($hdc); $g.Dispose()
  return $bmp
}
$painted = -1.0; $boxTxt = ''; $shaped = $false
for ($try = 0; $try -lt 6 -and $painted -lt 0.5; $try++) {
  $bmp = Grab-Window $h
  if ($bmp) {
    # $bmW/$bmH, а НЕ $W/$H: PowerShell регистронезависим, "$H = $bmp.Height" перезаписало бы
    # $h - хэндл главного окна, - и весь Alt+Enter-раздел ниже кликал бы в несуществующее окно
    # (ровно то и было: IsWindow=False при живом процессе).
    $bmW = $bmp.Width; $bmH = $bmp.Height
    $colN = New-Object int[] $bmW
    $rowN = New-Object int[] $bmH
    for ($yy = 0; $yy -lt $bmH; $yy += 4) {
      for ($xx = 0; $xx -lt $bmW; $xx += 4) {
        $c = $bmp.GetPixel($xx, $yy)
        if (($c.R + $c.G + $c.B) -gt 40) { $colN[$xx]++; $rowN[$yy]++ }
      }
    }
    $colNeed = [math]::Max(1, [int](($bmH / 4) * 0.25))
    $rowNeed = [math]::Max(1, [int](($bmW / 4) * 0.25))
    $bx0 = -1; $bx1 = -1; $by0 = -1; $by1 = -1
    for ($xx = 0; $xx -lt $bmW; $xx++) { if ($colN[$xx] -ge $colNeed) { if ($bx0 -lt 0) { $bx0 = $xx }; $bx1 = $xx } }
    for ($yy = 0; $yy -lt $bmH; $yy++) { if ($rowN[$yy] -ge $rowNeed) { if ($by0 -lt 0) { $by0 = $yy }; $by1 = $yy } }
    if ($bx0 -ge 0 -and $by0 -ge 0) {
      $cr2 = New-Object FS+RECT
      [void][FS]::GetClientRect($h, [ref]$cr2)
      $sx = $cr2.right / [double]$ww; $sy = $cr2.bottom / [double]$wh
      $cand = @(
        @{ n = 'logical'; x0 = $px; y0 = $py; x1 = $px + $pw - 1; y1 = $py + $ph - 1 },
        @{ n = 'phys'; x0 = [int]([math]::Round($px * $sx)); y0 = [int]([math]::Round($py * $sy))
                       x1 = [int]([math]::Round(($px + $pw) * $sx)) - 1
                       y1 = [int]([math]::Round(($py + $ph) * $sy)) - 1 }
      )
      $hit = $null
      foreach ($cd in $cand) {
        if ([math]::Abs($bx0 - $cd.x0) -le 6 -and [math]::Abs($bx1 - $cd.x1) -le 6 -and
            [math]::Abs($by0 - $cd.y0) -le 6 -and [math]::Abs($by1 - $cd.y1) -le 6) { $hit = $cd }
      }
      if (-not $hit) { $hit = $cand[1] }        # для доли окраски берём ожидаемый физический rect
      $bw = $bx1 - $bx0 + 1; $bh = $by1 - $by0 + 1
      $aspect = $bw / [double]$bh
      $aspectOk = [math]::Abs($aspect - (640.0 / 480.0)) -lt 0.02
      $matched = $hit.n -ne 'phys' -or ($bx0 -eq $hit.x0 -and $by0 -eq $hit.y0 -and $bx1 -eq $hit.x1 -and $by1 -eq $hit.y1)
      $lit = 0; $tot = 0
      for ($yy = [math]::Max(0, $hit.y0); $yy -le [math]::Min($bmH - 1, $hit.y1); $yy += 16) {
        for ($xx = [math]::Max(0, $hit.x0); $xx -le [math]::Min($bmW - 1, $hit.x1); $xx += 16) {
          $c = $bmp.GetPixel($xx, $yy); $tot++
          if (($c.R + $c.G + $c.B) -gt 48) { $lit++ }
        }
      }
      if ($tot -gt 0) { $painted = $lit / [double]$tot }
      $boxTxt = "content $bx0,$by0 - $bx1,$by1 (${bw}x${bh}) aspect {0:N3} in {1} rect {2},{3} - {4},{5}" -f `
                $aspect, $(if ($matched) { $hit.n } else { 'MISMATCH vs ' + $hit.n }), $hit.x0, $hit.y0, $hit.x1, $hit.y1
      $shaped = ($matched -and $aspectOk)
    }
    $bmp.Save((Join-Path $SHOT "${Arch}_fs_pixels.png"), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
  }
  if ($painted -lt 0.5) { Start-Sleep -Milliseconds 400 }
}
"  painted fraction inside picture rect: {0:N2}" -f $painted
"  $boxTxt"
Check 'picture pixels actually painted' ($painted -ge 0.5)
Check 'picture bbox matches layout rect, 4:3' $shaped

# --- 6. Alt+Enter -------------------------------------------------------------
# Диагностика до и после: если окно/процесс уже мертвы, PostMessage уходит в пустоту,
# GetWindowRect возвращает нули, и обе последующие проверки ("окно уменьшилось",
# "снова полноэкранное") проходят ложно - нулевой rect меньше 95% экрана, а LogLine
# ищет по всему файлу и находит ЕДИНСТВЕННУЮ строку "SdlApplyFullscreen: on".
$valid0 = [FS]::IsWindow($h); $ex0 = $p.HasExited
"  before Alt+Enter: IsWindow=$valid0 procExited=$ex0  handle=$h"
"  top-level windows: " + ((Proc-TopWindows $p.Id) -join ' | ')
$pm = [FS]::PostMessage($h, 0x104, [UIntPtr]13, [IntPtr]::Zero)   # WM_SYSKEYDOWN, VK_RETURN
"  PostMessage(WM_SYSKEYDOWN,VK_RETURN) -> $pm"
$off = ''
for ($i=0; $i -lt 100 -and -not $off; $i++) {
  Start-Sleep -Milliseconds 100
  PollAll
  $off = LogLine 'SdlApplyFullscreen: off ->'
  if ($p.HasExited) { break }
}
Check 'Alt+Enter leaves fullscreen' ([bool]$off)
if ($off) { "  $($off.ToString().Trim())" }
$valid1 = [FS]::IsWindow($h); $ex1 = $p.HasExited
"  after Alt+Enter: IsWindow=$valid1 procExited=$ex1"
$wr2 = New-Object FS+RECT
$winOff = $false
for ($i=0; $i -lt 50 -and -not $winOff; $i++) {
  Start-Sleep -Milliseconds 100
  PollAll
  $gr = [FS]::GetWindowRect($h, [ref]$wr2)      # false на мертвом HWND, rect при этом нулевой
  $winOff = ($gr -and (($wr2.right - $wr2.left) -lt ($scr.Width * 0.95)))
}
"  window rect now ($($wr2.left),$($wr2.top))-($($wr2.right),$($wr2.bottom))"
Check 'window back to windowed size' $winOff
Snap "${Arch}_fs_windowed"

[void][FS]::PostMessage($h, 0x104, [UIntPtr]13, [IntPtr]::Zero)   # WM_SYSKEYDOWN, VK_RETURN
$on3 = $false
for ($i=0; $i -lt 100 -and -not $on3; $i++) {
  Start-Sleep -Milliseconds 100
  PollAll
  $on3 = (LogCount 'SdlApplyFullscreen: on ->') -ge 2   # первая строка - сам запуск с -fullscreen
  if ($p.HasExited) { break }
}
Check 'Alt+Enter returns to fullscreen' $on3
Snap "${Arch}_fs_back"

Check 'no exception in the log' (-not (Get-Content $LOG -EA SilentlyContinue | Select-String 'EXCEPTION: 0xC000|Access violation' | Select-Object -First 1))

"timeline, seconds since the process started:"
foreach ($kv in ($marks.GetEnumerator() | Sort-Object Value)) {
  "  {0,7:N1}  {1}" -f $kv.Value, $kv.Key
}

Start-Sleep -Seconds 1
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Milliseconds 500
} finally {
  foreach ($m in $movies) { Remove-Item $m -Force -ErrorAction SilentlyContinue }
}

if ($failed) { "FAILED: $($failed -join ', ')"; exit 1 }
"PASSED"
exit 0
