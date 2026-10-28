# Music / sound toggle test for the bottom panel buttons.
#
# The level-select screen (the game's home screen) carries FIVE panel buttons,
# each one a picture plus a vertical caption to its right, at logical y=448:
#
#   1 ИГРА (x~66)   - starts a scene and swaps the whole panel for the inventory bar
#   2 МУЗЫКА (x~196) - MIDI music on/off          (saxophonist; his pose changes)
#   3 ЗВУК  (x~324) - sound effects on/off        (cat with a microphone; it lies
#                                                  down when sound is off)
#   4 ПОИСК (x~452)
#   5 ПОХОД (x~582)
#
# Only a button's CENTRE is a hit - clicking its caption or its edge does nothing.
#
# The game logic is bytecode, so the debug log is the only observable state. In
# the v0.1 build each button ended up in the engine's Native_SetSystemFlag with
# the volume ramping through SetWaveVolume / SetMidiVolume; port-main keeps the
# game code untouched and logs none of that. What IS observable here:
#
#   - click L: x,y        the button-down reached the game at these coordinates;
#   - the button PICTURE  the script reacts to the flag by redrawing the sprite
#     (the saxophonist changes pose, the cat lies down) - a pixel diff, which is
#     the engine's own proof that the flag flipped.
#
# What the log CANNOT show on Windows: the audio output itself. wave.c/midi.c
# open the REAL winmm devices here (waveOutOpen/midiOutShortMsg) and log nothing
# of their own; the port's mixer - whose SdlAudioMix / SdlWave_Queue lines make
# the output assertable - is the non-Windows shim path, covered by
# tests/linux/midisynth.sh and movieaudio.sh. So this harness asserts every
# toggle reaching the game, the game reacting to it, the scene round-trip and
# the absence of crashes in either flag state.
#
# The test asserts three things the panel promises:
#   1. the button picture changes on every click,
#   2. the toggles ALTERNATE - off, on (they used to only ever turn off),
#   3. the game survives a scene in both states without a single exception.
#
# Usage:
#   pwsh -NoProfile -File tests\windows\audio.ps1            # run the test (x64)
#   pwsh -NoProfile -File tests\windows\audio.ps1 -Arch x86  # same script on x86
#   pwsh -NoProfile -File tests\windows\audio.ps1 -Build     # rebuild that arch's debug first
#   pwsh -NoProfile -File tests\windows\audio.ps1 -Build -BuildVia cmake   # ...through CMake instead
#   pwsh -NoProfile -File tests\windows\audio.ps1 -Scan      # UI timeline capture only
#
# -Arch exists to compare the two builds under the SAME scenario: several MIDI
# values the log prints look arch-specific but are really scenario/timing
# specific, and only an identical click script on both arches can tell them apart.
#
# Exit code 0 = every check passed. Reproduce on a debug build only: in release
# DBG_* are no-ops (debug_log.h), so there is nothing to assert against.
param(
    [switch]$Build,
    # Which build path -Build takes (kept separate from -Build, which here means
    # "rebuild at all", unlike the other harnesses where -Build IS the switch on
    # the path): -Build -BuildVia cmake measures the CMake build instead of the bat.
    [ValidateSet('bat','cmake')][string]$BuildVia = 'bat',
    [int]$DwellMs = 900,
    [switch]$Scan,
    [int]$ScanSec = 20,
    [ValidateSet('x86','x64')][string]$Arch = 'x64'
)

$ExeName = if ($Arch -eq 'x86') { 'PILOT_RE.EXE' } else { 'PILOT_RE_x64.EXE' }
$LogName = $ExeName -replace '\.EXE$', '_debug.log'

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class GA {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte sc, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  public struct POINT { public int X, Y; }
  public struct RECT { public int Left, Top, Right, Bottom; }
  public static IntPtr LP(int x,int y){ return (IntPtr)(((uint)y<<16)|((uint)x&0xFFFF)); }
}'

$SRC  = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$SHOT = Join-Path $env:TEMP 'shots'
New-Item -ItemType Directory -Force -Path $SHOT | Out-Null

Get-Process PILOT_RE,PILOT_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

if ($Build) {
  . "$PSScriptRoot\rundir.ps1"
  $exeDir = Build-GameExe -ExeName $ExeName -Config Debug -Build $BuildVia
}

# Scratch dir holding the data but NOT MOVIES: keeps the run deterministic
# instead of depending on whether the videos are installed (see rundir.ps1).
# Build first, so the hardlink picks up the freshly built exe.
. "$PSScriptRoot\rundir.ps1"
$RUN = New-GameRunDir -ExeName $ExeName -FromDir $exeDir
$LOG = Join-Path $RUN $LogName

Remove-Item $LOG -ErrorAction SilentlyContinue
$p = Start-Process -FilePath "$RUN\$ExeName" -WorkingDirectory $RUN -PassThru
$h = [IntPtr]::Zero
# The handle only becomes valid once SDL has created and painted the window;
# grabbing it at the first non-zero poll returns a half-made window and every
# later PrintWindow of it is blank. Require it to stay put across polls.
$stable = 0; $prev = [IntPtr]::Zero
for ($i = 0; $i -lt 200 -and $stable -lt 3; $i++) {
  Start-Sleep -Milliseconds 200
  $p.Refresh()
  $cur = $p.MainWindowHandle
  if ($cur -ne [IntPtr]::Zero -and $cur -eq $prev) { $stable++ } else { $stable = 0 }
  $prev = $cur; $h = $cur
}
if ($h -eq [IntPtr]::Zero) { Report-StartupFailure $p; if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }; exit 1 }
"window: $h"

function FocusWin() {
  if ([GA]::GetForegroundWindow() -eq $h) { return }
  [void][GA]::ShowWindow($h, 9); [void][GA]::BringWindowToTop($h)
  [GA]::keybd_event(0x12,0,0,[UIntPtr]::Zero)
  [void][GA]::SetForegroundWindow($h)
  [GA]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
  Start-Sleep -Milliseconds 200
}
# PrintWindow paints the WHOLE window (title bar + borders) into the DC, so the
# bitmap must be window-sized and the client area starts at an offset - a
# client-sized bitmap silently chops the bottom rows, which is exactly where the
# toolbar lives.
$script:OX = 0; $script:OY = 0; $script:CW = 640; $script:CH = 480
function Snap($n) {
  $wr = New-Object GA+RECT
  $cr = New-Object GA+RECT
  [void][GA]::GetWindowRect($h, [ref]$wr)
  [void][GA]::GetClientRect($h, [ref]$cr)
  $co = New-Object GA+POINT; $co.X = 0; $co.Y = 0
  [void][GA]::ClientToScreen($h, [ref]$co)
  $script:OX = $co.X - $wr.Left                     # client origin inside the window bitmap
  $script:OY = $co.Y - $wr.Top
  $script:CW = $cr.Right; $script:CH = $cr.Bottom
  $w = $wr.Right - $wr.Left; $ht = $wr.Bottom - $wr.Top
  if ($w -le 0 -or $ht -le 0) { return }
  # PrintWindow hands back a blank frame while the SDL surface is not paintable
  # yet - retry until a sample of the client area is not all one colour.
  for ($try = 0; $try -lt 12; $try++) {
    $bmp = New-Object System.Drawing.Bitmap $w, $ht
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $hdc = $g.GetHdc()
    $ok = [GA]::PrintWindow($h, $hdc, 2)      # 2 = PW_RENDERFULLCONTENT
    $g.ReleaseHdc($hdc); $g.Dispose()
    if (-not $ok) { $bmp.Dispose(); return }
    $probe = @(@(90,110),@(170,250),@(330,300),@(500,380),@(70,470))
    $flat = $true
    $c0 = $bmp.GetPixel($script:OX + $probe[0][0], $script:OY + $probe[0][1])
    foreach ($pt in $probe[1..($probe.Count-1)]) {
      if ($bmp.GetPixel($script:OX + $pt[0], $script:OY + $pt[1]).ToArgb() -ne $c0.ToArgb()) { $flat = $false; break }
    }
    if (-not $flat) { break }
    Start-Sleep -Milliseconds 250
  }
  $bmp.Save((Join-Path $SHOT "$n.png")); $bmp.Dispose()
}

# Logical 640x480 click: PostMessage the button trio the game's own window proc
# expects (move -> down -> up). Needs no focus and moves no cursor, so it works
# even with the game window occluded. Returns the log lines the click produced
# plus whether the game acknowledged the button-down at these coordinates.
function ClickL($lx,$ly,$tag,$dwell) {
  if (-not $dwell) { $dwell = $DwellMs }
  FocusWin
  $mark = (Get-Content $LOG -ErrorAction SilentlyContinue | Measure-Object).Count
  [void][GA]::PostMessage($h,0x0200,[IntPtr]::Zero,[GA]::LP($lx,$ly))
  Start-Sleep -Milliseconds 60
  [void][GA]::PostMessage($h,0x0201,[IntPtr]1,[GA]::LP($lx,$ly))
  Start-Sleep -Milliseconds 40
  [void][GA]::PostMessage($h,0x0202,[IntPtr]::Zero,[GA]::LP($lx,$ly))
  Start-Sleep -Milliseconds $dwell
  # The game logs the click at the coordinates IT received (windowed mode is the
  # identity mapping, so they equal the logical ones posted here).
  $last = Get-Content $LOG -ErrorAction SilentlyContinue | Select-String -SimpleMatch 'click L: ' | Select-Object -Last 1
  $want = "$lx,$ly"
  $got  = if ($last) { ($last.Line -replace '^.*click L: ','') } else { '<none>' }
  $after = Get-Content $LOG -ErrorAction SilentlyContinue
  $new   = if ($after.Count -gt $mark) { @($after[$mark..($after.Count-1)]) } else { @() }
  return [pscustomobject]@{ Tag = $tag; Delivered = ($got -eq $want); Lines = $new }
}

# Pixel-diff a logical 640x480 button box between two shots. This answers
# requirement #1 ("the picture on the button must change") objectively rather
# than by eye. Arguments are logical client coords; the shots also carry the
# title bar, hence the OX/OY offset.
function DiffBox($a, $b, $x0, $y0, $x1, $y1) {
  $pa = Join-Path $SHOT "$a.png"; $pb = Join-Path $SHOT "$b.png"
  if (-not (Test-Path $pa) -or -not (Test-Path $pb)) { return -1 }
  $ia = [System.Drawing.Bitmap]::FromFile($pa)
  $ib = [System.Drawing.Bitmap]::FromFile($pb)
  $n = 0
  for ($y = $y0 + $script:OY; $y -lt $y1 + $script:OY; $y += 2) {
    for ($x = $x0 + $script:OX; $x -lt $x1 + $script:OX; $x += 2) {
      if ($x -ge $ia.Width -or $y -ge $ia.Height) { continue }
      if ($x -ge $ib.Width -or $y -ge $ib.Height) { continue }
      if ($ia.GetPixel($x, $y).ToArgb() -ne $ib.GetPixel($x, $y).ToArgb()) { $n++ }
    }
  }
  $ia.Dispose(); $ib.Dispose()
  return $n
}

# Whole-frame comparison over the client area only: answers "did the screen
# change at all", as opposed to DiffBox's "did this button change".
function FullDiff($a, $b) {
  $pa = Join-Path $SHOT "$a.png"; $pb = Join-Path $SHOT "$b.png"
  if (-not (Test-Path $pa) -or -not (Test-Path $pb)) { return "  $a -> ${b}: missing shot" }
  $ia = [System.Drawing.Bitmap]::FromFile($pa)
  $ib = [System.Drawing.Bitmap]::FromFile($pb)
  $n = 0; $fy = -1; $ly = -1
  $y1 = [Math]::Min($script:OY + $script:CH, $ia.Height)
  $x1 = [Math]::Min($script:OX + $script:CW, $ia.Width)
  for ($y = $script:OY; $y -lt $y1; $y++) {
    for ($x = $script:OX; $x -lt $x1; $x++) {
      if ($y -ge $ib.Height -or $x -ge $ib.Width) { continue }
      if ($ia.GetPixel($x, $y).ToArgb() -ne $ib.GetPixel($x, $y).ToArgb()) { $n++; if ($fy -lt 0) { $fy = $y - $script:OY }; $ly = $y - $script:OY }
    }
  }
  $ia.Dispose(); $ib.Dispose()
  return "  $a -> ${b}: client changed=$n$(if ($fy -ge 0) { " (client y[$fy..$ly])" })"
}

# Snapshot the screen every 2 s from startup: the bottom panel is not the same
# on every screen, so the toggles can only be located by watching which screen
# shows up when. Diagnostic mode - exits without running the test.
if ($Scan) {
  for ($t = 0; $t -le $ScanSec; $t += 2) {
    Snap "scan_$t"
    Start-Sleep -Seconds 2
  }
  "### screen timeline (client pixels that changed) ###"
  for ($t = 2; $t -le $ScanSec; $t += 2) { FullDiff ("scan_" + ($t-2)) ("scan_$t") }
  if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
  "shots in $SHOT"
  exit 0
}

$script:FAIL = @()
function Check($name, $cond, $detail) {
  if ($cond) { "  PASS  $name$(if ($detail) { "  [$detail]" })" }
  else       { $script:FAIL += $name; "  FAIL  $name$(if ($detail) { "  [$detail]" })" }
}

Start-Sleep -Seconds 16
"started: $((Get-Content $LOG -ErrorAction SilentlyContinue).Count) lines"

Snap 'map_0'

# --------------------------------------------------------------------------
# The toggles must ALTERNATE: turn both OFF here, prove the gate in a scene,
# then turn them back ON and prove the sound returns. Each click is asserted
# delivered AND redrawing its button - the sprite change is the engine's own
# proof that the flag flipped (port-main logs no flag lines of its own).
# --------------------------------------------------------------------------
$TOGGLES = @(
  @{ Name='button 2 МУЗЫКА'; X=196; Shot='music' },
  @{ Name='button 3 ЗВУК';  X=324; Shot='sound' }
)
$script:Prev = 'map_0'
$StateWord = @('off','on')
foreach ($round in 0,1) {
  foreach ($t in $TOGGLES) {
    $tag = "{0}_{1}" -f $t.Shot, $StateWord[$round]
    ""
    "### $($t.Name) -> $($StateWord[$round]) at x=$($t.X),y=448 ###"
    $r = ClickL $t.X 448 $tag
    Snap "map_$tag"
    $icon = DiffBox $script:Prev "map_$tag" ($t.X-45) 438 ($t.X+45) 480
    $script:Prev = "map_$tag"
    Check "$tag delivered" $r.Delivered "game saw the button-down"
    Check "$tag picture changes" ($icon -gt 0) "changed pixels: $icon"
  }
  if ($round -eq 0) {
    # Both flags off: a silent run through the scene - the engine's own audio
    # path with nothing to assert in the log, but it must not crash either.
    ""
    "### scene 1 with music OFF and sound OFF ###"
    $r = ClickL 66 448 'goto1-off'
    Check 'goto-scene1 (off) delivered' $r.Delivered 'button 1 starts a scene'
    Start-Sleep -Seconds 10
    Check 'no crash in the gated scene' (-not $p.HasExited) 'process alive'
    # (607,450) is the scene's exit back to the level map - the coordinates
    # pfuzz's walk measured as "the game returned to resource 1".
    $r = ClickL 607 450 'exit'
    Check 'exit back to the map delivered' $r.Delivered 'bottom-right exit button'
    Start-Sleep -Seconds 6
    Snap 'map_back'
    $script:Prev = 'map_back'
  }
}

# --------------------------------------------------------------------------
# With both flags back ON the same scene runs the engine's own audio path
# (real waveOut/midiOut): the spray below drives it through ~70 clicks.
# --------------------------------------------------------------------------
""
"### scene 1 with music ON and sound ON ###"
$r = ClickL 66 448 'goto1-on'             # button 1 ИГРА - swaps the panel for the inventory bar
Check 'goto-scene1 (on) delivered' $r.Delivered "button 1 starts a scene"
Start-Sleep -Seconds 10
Check 'no crash entering the scene' (-not $p.HasExited) 'process alive'
Snap 'scene1'

# Sound effects are attached to scene objects, so spray a grid over the scene
# (~7% of scene clicks in pfuzz hit one). Nothing to assert in the log on
# Windows - the point is that the wave path survives it.
"  spraying scene clicks (drives the engine's wave path)..."
$sfxClicks = 0
foreach ($y in @(100,140,180,220,260,300,340)) {
  foreach ($x in @(60,120,180,240,300,360,420,480,540,600)) {
    [void](ClickL $x $y 'sfx' 100)
    $sfxClicks++
  }
}
Check 'survived the spray' (-not $p.HasExited) "$sfxClicks clicks"

""
"### exceptions ###"
$ex = Get-Content $LOG | Select-String -Pattern 'EXCEPTION: 0xC000|Access violation'
Check 'no exceptions' (-not $ex) "$(@($ex).Count) in the log"
if ($ex) { $ex | Select-Object -First 5 | ForEach-Object { "  " + ($_.Line -replace '^\[PILOTS\] ','') } }

""
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
Start-Sleep -Milliseconds 400
"shots in $SHOT"
if ($script:FAIL.Count -eq 0) { "RESULT: PASS"; exit 0 }
"RESULT: FAIL ($($script:FAIL.Count) check(s)):"
$script:FAIL | ForEach-Object { "  - $_" }
exit 1
