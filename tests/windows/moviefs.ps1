# Alt+Enter ВО ВРЕМЯ РОЛИКА: ребёнок mciavi32 (AVIWnd32) обязан переехать за
# картинкой. Репорт: на Windows при Alt+Enter во время ролика видео НЕ
# отцентровывается - AVI-окно остаётся на старых клиентских px (в углу экрана),
# а фон (BMP в кадровом буфере) уже нарисован по новой раскладке. Причина:
# ребёнок ставится ОДИН раз - при первом WM_PAINT (host.c, g_AviWindowPlaced),
# - а хук перестановки v0.1 (VideoRealignMciChild) потерялся при переезде MCI
# в host.c; вернул его Host_RealignAviWindow, вызываемый из SdlUpdateLayout на
# каждом изменении rect'а картинки.
#
#   & "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File tests\windows\moviefs.ps1 -Arch x64
#
# Сценарий (игра стартует в ОБЫЧНОМ окне, без -fullscreen):
#   1. дождаться ролика: PlayAvi: MCI device + AVIWnd32 с непустым rect;
#   2. baseline: центр AVIWnd32 == центру клиентской области (rect'а ролика не
#      предполагается, он читается из PlayAvi: start - интро стоит по центру);
#   3. Alt+Enter ПОСРЕДИ фильма (PostMessage WM_SYSKEYDOWN, фокус не нужен):
#      ждём SdlApplyFullscreen: on, маркер PlayAvi: realign child -> и строку
#      SdlUpdateLayout ... (native movie) at (px,py);
#   4. САМ АССЕРТ БАГА: центр AVIWnd32 снова совпал с центром client (физ. px,
#      допуск +-8) И rect ребёнка изменился - без фикса он остался в углу,
#      центр мимо на сотни пикселей;
#   5. Alt+Enter обратно: маркер реалайна >= 2, центр снова по центру обычного
#      окна (вторая ветка SdlUpdateLayout - identity);
#   6. без исключений.
#
# Ассерт ЦЕНТРА, а не абсолютных координат: харнесс DPI-aware, игра - нет
# (физ. клиент 800x600 при логических 640x480), центр совпадает в обоих
# пространствах при любом масштабе. Единственный харнесс, который проверяет
# геометрию MCI-ребёнка ИМЕННО во время играющего фильма после переключения.

param([ValidateSet('x86','x64')][string]$Arch = 'x64')

Add-Type 'using System; using System.Runtime.InteropServices;
public class MF {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, UIntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr FindWindowEx(IntPtr parent, IntPtr after, IntPtr cls, IntPtr title);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  public struct RECT { public int left, top, right, bottom; }
  public struct POINT { public int x, y; }
}'
[void][MF]::SetProcessDPIAware()

$ErrorActionPreference = 'Stop'
$SRC  = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$exe  = if ($Arch -eq 'x64') { 'PILOTS_RE_x64.EXE' } else { 'PILOTS_RE.EXE' }

$failed = @()
function Check([string]$name, [bool]$ok) {
  "{0,-52} {1}" -f $name, $(if ($ok) { 'PASS' } else { 'FAIL' })
  if (-not $ok) { $script:failed += $name }
}

# Остатки процесса убираются ДО сборки: link не откроет занятый exe (см. clickskip.ps1)
Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$bat = if ($Arch -eq 'x64') { 'debug x64' } else { 'debug x86' }
"building $Arch DEBUG (build-vs2022.bat $bat)..."
& cmd /c "`"$SRC\build-vs2022.bat $bat`"" | Select-Object -Last 1
if (-not (Test-Path (Join-Path $SRC "out\$exe"))) { "no exe: $SRC\out\$exe"; exit 1 }

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

# Старт БЕЗ -fullscreen: пользовательское репро - обычное окно, Alt+Enter посреди фильма.
$p = Start-Process -FilePath (Join-Path $RUN $exe) -WorkingDirectory $RUN -PassThru
$t0 = Get-Date

# Лог читается в кэш: игровой цикл пишет десятки тысяч строк в секунду, сплошное
# перечитывание файла на каждый Poll - многоминутное ожидание вместо секунд.
$script:logLines = @()
function Read-Log { $script:logLines = @(Get-Content $LOG -ErrorAction SilentlyContinue) }
function Test-Log([string]$pat) {
  foreach ($ln in $script:logLines) { if ($ln.Contains($pat)) { return $true } }
  return $false
}
function LogHas([string]$pat) { Read-Log; Test-Log $pat }
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

# Дочернее окно AVI-виджета: перечисление детей главного окна, класс - строкой.
function Get-AviChild([IntPtr]$h) {
  $after = [IntPtr]::Zero
  for (;;) {
    $c = [MF]::FindWindowEx($h, $after, [IntPtr]::Zero, [IntPtr]::Zero)
    if ($c -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
    $after = $c
    if ((Get-WindowClass $c) -eq 'AVIWnd32') { return $c }
  }
}

# Центр AVI-ребёнка в физических client px игры (+ сам client и сам rect для
# вывода). $null, если окна нет или оно пустое.
function Get-ChildCenter([IntPtr]$h, [IntPtr]$child) {
  $wr = New-Object MF+RECT
  if (-not [MF]::GetWindowRect($child, [ref]$wr)) { return $null }
  if (($wr.right - $wr.left) -le 0) { return $null }
  $o = New-Object MF+POINT; $o.x = 0; $o.y = 0
  if (-not [MF]::ClientToScreen($h, [ref]$o)) { return $null }
  $cr = New-Object MF+RECT
  if (-not [MF]::GetClientRect($h, [ref]$cr)) { return $null }
  [pscustomobject]@{
    cx   = ($wr.left + $wr.right) / 2.0 - $o.x
    cy   = ($wr.top + $wr.bottom) / 2.0 - $o.y
    cw   = $cr.right
    ch   = $cr.bottom
    rect = "$($wr.left),$($wr.top)-$($wr.right),$($wr.bottom)"
  }
}

# Раскладка из последней строки SdlUpdateLayout: смещение картинки и логический
# клиент. $null => обычное окно: картинка и есть клиент, лог. клиент 640x480
# (оконная ветка SdlUpdateLayout строки в лог не пишет вовсе).
function Get-Layout([string]$layoutLine) {
  $r = @{ picX = 0; picY = 0; logW = 640; logH = 480 }
  if ($layoutLine -and $layoutLine -match 'window (\d+)x(\d+) -> picture \d+x\d+ at \((-?\d+),(-?\d+)\)') {
    $r.logW = [int]$Matches[1]; $r.logH = [int]$Matches[2]
    $r.picX = [int]$Matches[3]; $r.picY = [int]$Matches[4]
  }
  return $r
}

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

# --- 1. ролик пошёл, ребёнок поставлен ----------------------------------------
$mci = $false
for ($i=0; $i -lt 200 -and -not $mci; $i++) {          # 40 с - запас на медленный диск
  Start-Sleep -Milliseconds 200
  $mci = LogHas 'PlayAvi: MCI device'
  if ($p.HasExited) { break }
}
Check 'movie opened (PlayAvi: MCI device)' $mci

$child = [IntPtr]::Zero; $m0 = $null
for ($i=0; $i -lt 50 -and -not $m0; $i++) {            # 10 с - до первого WM_PAINT
  Start-Sleep -Milliseconds 200
  $child = Get-AviChild $h
  if ($child -ne [IntPtr]::Zero) { $m0 = Get-ChildCenter $h $child }
  if ($p.HasExited) { break }
}
Check 'AVIWnd32 child placed (non-empty rect)' ([bool]$m0)

$mavi = LogLast 'PlayAvi: start name='
$aviRectOk = $mavi -and ($mavi -match 'rect=(-?\d+),(-?\d+) (\d+)x(\d+)')
Check 'PlayAvi: start rect parsed from the log' $aviRectOk
$aviX = 0; $aviY = 0; $aviW = 0; $aviH = 0
if ($aviRectOk) {
  $aviX = [int]$Matches[1]; $aviY = [int]$Matches[2]
  $aviW = [int]$Matches[3]; $aviH = [int]$Matches[4]
  # Интро логирует rect с нулевым размером (замерено: rect=160,120 0x0), а код
  # ставит/переставляет ребёнка с HOST_AVI_DEFAULT_320x240 - харнесс повторяет
  # тот же дефолт, иначе ожидаемый центр съезжает в точку (aviX,aviY).
  if ($aviW -le 0) { $aviW = 320 }
  if ($aviH -le 0) { $aviH = 240 }
  "  movie rect (game px): ($aviX,$aviY) ${aviW}x${aviH}"
}

# --- 2. baseline: обычное окно, ребёнок по центру ------------------------------
if ($m0) {
  "  child screen rect: $($m0.rect)  centre in client: ($([int]$m0.cx),$([int]$m0.cy)) of $($m0.cw)x$($m0.ch)"
  $l = Get-Layout $null
  $eX = ($l.picX + $aviX + $aviW / 2.0) * ($m0.cw / [double]$l.logW)
  $eY = ($l.picY + $aviY + $aviH / 2.0) * ($m0.ch / [double]$l.logH)
  Check 'child centred in the windowed client (baseline)' `
        ([math]::Abs($m0.cx - $eX) -le 8 -and [math]::Abs($m0.cy - $eY) -le 8)
}

# --- 3. Alt+Enter посреди фильма ----------------------------------------------
[void][MF]::PostMessage($h, 0x104, [UIntPtr]13, [IntPtr]::Zero)   # WM_SYSKEYDOWN, VK_RETURN
$on = $false; $re = $false; $nat = $false
for ($i=0; $i -lt 100 -and -not ($on -and $re -and $nat); $i++) {
  Start-Sleep -Milliseconds 100
  $on  = LogHas 'SdlApplyFullscreen: on ->'
  $re  = LogHas 'PlayAvi: realign child ->'
  $lay = LogLast 'SdlUpdateLayout: window'
  if ($lay -and $lay.ToString().Contains('(native movie)')) { $nat = $true }
  if ($p.HasExited) { break }
}
Check 'Alt+Enter switched to fullscreen (SdlApplyFullscreen: on)' $on
Check 'layout after the switch keeps the movie natural' $nat
Check 'realign marker logged (PlayAvi: realign child)' $re

# --- 4. САМ АССЕРТ БАГА: ребёнок переехал за картинкой ------------------------
$m1 = $null
for ($i=0; $i -lt 50 -and -not $m1; $i++) {
  Start-Sleep -Milliseconds 100
  $c1 = Get-AviChild $h
  if ($c1 -ne [IntPtr]::Zero) { $m1 = Get-ChildCenter $h $c1 }
  if ($p.HasExited) { break }
}
if ($m1) {
  $lay = LogLast 'SdlUpdateLayout: window'
  $l = Get-Layout $(if ($lay) { $lay.ToString() } else { $null })
  $eX = ($l.picX + $aviX + $aviW / 2.0) * ($m1.cw / [double]$l.logW)
  $eY = ($l.picY + $aviY + $aviH / 2.0) * ($m1.ch / [double]$l.logH)
  "  fullscreen: client $($m1.cw)x$($m1.ch), picture at logical ($($l.picX),$($l.picY))"
  "  child screen rect: $($m1.rect)  centre: ($([int]$m1.cx),$([int]$m1.cy)), expected ($([int]$eX),$([int]$eY))"
  Check 'child followed the picture after Alt+Enter (centred)' `
        ([math]::Abs($m1.cx - $eX) -le 8 -and [math]::Abs($m1.cy - $eY) -le 8)
  # Смещение ОТНОСИТЕЛЬНО client'а, не экранные координаты: само окно игры при
  # fullscreen переезжает на (0,0), и по экранным px ребёнок "меняется" даже без
  # перестановки (до фикса центр в client стоял неподвижно на (400,300)).
  Check 'child moved inside the client on the switch' `
        ([math]::Abs($m1.cx - $m0.cx) -gt 8 -or [math]::Abs($m1.cy - $m0.cy) -gt 8)
} else {
  Check 'child followed the picture after Alt+Enter (centred)' $false
}

# --- 5. Alt+Enter обратно: вторая ветка SdlUpdateLayout ------------------------
[void][MF]::PostMessage($h, 0x104, [UIntPtr]13, [IntPtr]::Zero)
$off = $false
for ($i=0; $i -lt 100 -and -not $off; $i++) {
  Start-Sleep -Milliseconds 100
  $off = LogHas 'SdlApplyFullscreen: off ->'
  if ($p.HasExited) { break }
}
Check 'Alt+Enter back to the windowed mode' $off
Check 'realign ran on both switches (count >= 2)' ((LogCount 'PlayAvi: realign child ->') -ge 2)

$m2 = $null
for ($i=0; $i -lt 50 -and -not $m2; $i++) {
  Start-Sleep -Milliseconds 100
  $c2 = Get-AviChild $h
  if ($c2 -ne [IntPtr]::Zero) { $m2 = Get-ChildCenter $h $c2 }
  if ($p.HasExited) { break }
}
if ($m2) {
  $l = Get-Layout $null                            # оконная ветка: rect и есть клиент
  $eX = ($l.picX + $aviX + $aviW / 2.0) * ($m2.cw / [double]$l.logW)
  $eY = ($l.picY + $aviY + $aviH / 2.0) * ($m2.ch / [double]$l.logH)
  "  back to windowed: client $($m2.cw)x$($m2.ch), child centre ($([int]$m2.cx),$([int]$m2.cy)), expected ($([int]$eX),$([int]$eY))"
  Check 'child centred again after leaving fullscreen' `
        ([math]::Abs($m2.cx - $eX) -le 8 -and [math]::Abs($m2.cy - $eY) -le 8)
} else {
  Check 'child centred again after leaving fullscreen' $false
}

Check 'no exception in the log' (-not (Get-Content $LOG -EA SilentlyContinue | Select-String 'EXCEPTION: 0xC000|Access violation' | Select-Object -First 1))

} finally {
  if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
  foreach ($f in $movies) { Remove-Item $f -ErrorAction SilentlyContinue }
}

if ($failed) { "FAILED: $($failed -join ', ')"; exit 1 }
"PASSED"
exit 0
