# Scratch run directory for the game, shared by the harnesses.
#
# Dot-source it, then call New-GameRunDir:
#
#   . "$PSScriptRoot\rundir.ps1"
#   $RUN = New-GameRunDir -ExeName 'PILOTS_RE_x64.EXE'
#   $LOG = Join-Path $RUN 'PILOTS_RE_x64_debug.log'
#
# The exe lives in SRC\out\<arch> (the bat's per-arch output: out\x86, out\x64),
# the game data in the SRC root - New-GameRunDir takes both from where they are.
# A CMake build puts its exe somewhere else entirely (see Get-CMakeOutDirForExe),
# so -Build/-FromDir exist: the run dir itself does not care where the exe came
# from, it only hardlinks whatever it is given.
#
# Why a scratch dir at all. main.c resolves .\PILOTS.EXE (the game's own data file)
# and the .\movies\... video lookup against the process CWD, so the exe has to be
# started from a directory that holds the game data. That directory deliberately
# does NOT hold MOVIES, so the intro is skipped and the run is deterministic:
#
#  - on 64-bit Windows the AVI MCI path faults inside winmm (see host.c,
#    MciSendCommandGuarded). host.c now catches that and skips every video, so
#    this is no longer about surviving the startup - but a test that has to
#    measure timing should not depend on whether the videos happen to be
#    installed either way.
#
#  - a 32-bit build DOES play the intro, and then spends its whole measurement
#    window on the roll, so x86check's timing markers (GameMainLoop) shift
#    purely because the videos are present.
#
# Files are hardlinked, not copied: the data file is ~15 MB and the directory is
# rebuilt on every run. mklink /H needs no elevation (only symlinks do).
$__TestsDir = $PSScriptRoot

function Get-OutDirForExe {
  # SRC\out\<arch> — where build-vs2022.bat drops a given exe. The bat splits the
  # arches into out\x86 and out\x64 so they stop overwriting each other (shared
  # SDL2.dll name used to mean only the last-built arch could run). The subdir
  # follows from the exe's own name: PILOTS_RE_x64.* -> x64, else x86.
  param([string]$SRC, [string]$ExeName)
  $arch = if ($ExeName -match '_x64') { 'x64' } else { 'x86' }
  return (Join-Path $SRC (Join-Path 'out' $arch))
}

function Get-CMakeOutDirForExe {
  # Where a CMake build of a given arch puts its exe. Unlike the bat there is no
  # single answer: the Visual Studio generator adds a per-config subdir
  # (build-win32\Debug\PILOTS_RE.EXE) while Ninja writes straight into the build
  # dir (build-win32\PILOTS_RE.EXE), and the build dir itself is a convention
  # (build-win32/build-win64), not something CMake fixes. So probe the layout
  # instead of assuming it, Debug first - the baseline markers are debug-log
  # lines, and a Release build has no log at all. -Config pins the subdir when
  # the caller knows which config it just built: without it a Debug dir left over
  # from an earlier run would be handed back for a Release build, and the harness
  # would measure the wrong exe while believing it had just built the right one.
  param([string]$RepoRoot, [string]$ExeName, [string]$BuildDir, [string]$Config)
  $arch = if ($ExeName -match '_x64') { 'win64' } else { 'win32' }
  if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot "build-$arch" }
  $subs = if ($Config) { @($Config, '.') } else { @('Debug', 'Release', '.') }
  foreach ($sub in $subs) {
    $p = if ($sub -eq '.') { $BuildDir } else { Join-Path $BuildDir $sub }
    if (Test-Path (Join-Path $p $ExeName)) { return $p }
  }
  return $null
}

function Find-CMake {
  # cmake is not in this machine's PATH - what is installed is the copy bundled
  # with Visual Studio (Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin).
  # Look for the bundled one as a fallback rather than demanding cmake on PATH.
  $c = Get-Command cmake -ErrorAction SilentlyContinue
  if ($c) { return $c.Source }
  $vs = 'C:\Program Files\Microsoft Visual Studio\2022\Community'
  $bundled = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
  if (Test-Path $bundled) { return $bundled }
  return $null
}

function Stage-GameDataForExe {
  # Put PILOTS.EXE (the game's data file, 48 MB) next to the exe just built.
  #
  # The engine does NOT take its data from the CWD: WinMain resolves the path
  # from GetModuleFileNameA and Host_ResolveGameFilePath replaces the exe's own
  # name with PILOTS.EXE IN THE EXE'S OWN DIRECTORY. src\out\x64 has had that
  # file sitting there since the bat era, which is why every harness that runs
  # the exe straight out of out\<arch> worked - a CMake build dir has no such
  # file, so the game raised its "no resources" box and the harness reported a
  # crash for what was really an undismissed dialog.
  #
  # A hardlink, exactly like New-GameRunDir does: no 48 MB copy per build dir,
  # and the file is game data, never a build product, so it is not cleaned.
  # Run-dir based harnesses are unaffected - they hardlink the exe into $RUN and
  # stage PILOTS.EXE there themselves.
  param([string]$SrcDir, [string]$ExeDir)
  $dst = Join-Path $ExeDir 'PILOTS.EXE'
  if (Test-Path $dst) { return }
  $src = Join-Path $SrcDir 'PILOTS.EXE'
  if (-not (Test-Path $src)) { throw "game data missing: $src" }
  cmd /c "mklink /H `"$dst`" `"$src`"" | Out-Null
  if (-not (Test-Path $dst)) { Copy-Item $src $dst -Force }   # fallback, e.g. other volume
}

function Build-GameExe {
  # Build one exe and return the directory holding it - the one function every
  # harness goes through, so -Build bat|cmake means the same thing everywhere.
  # 'bat' (default) is SRC\build-vs2022.bat into SRC\out\<arch>, the proven
  # Windows baseline; 'cmake' builds the same arch through CMake into
  # build-win32/build-win64 and returns THAT dir. Neither side has to know the
  # other's layout: the bat's output dir comes from Get-OutDirForExe, CMake's
  # from Get-CMakeOutDirForExe. $Config is CMake's spelling (Debug/Release) and
  # is lowercased for the bat, which only distinguishes 'debug' from everything
  # else (/i, so case does not matter to it either way).
  param([string]$ExeName, [string]$Config = 'Debug',
        [ValidateSet('bat','cmake')][string]$Build = 'bat')

  $SRC = (Resolve-Path (Join-Path $__TestsDir '..\..\SRC')).Path
  $repo = (Resolve-Path (Join-Path $__TestsDir '..\..')).Path
  $arch = if ($ExeName -match '_x64') { 'x64' } else { 'x86' }

  if ($Build -eq 'cmake') {
    $cmake = Find-CMake
    if (-not $cmake) { throw 'cmake not found (not on PATH, no bundled VS copy either)' }
    $bdir = Join-Path $repo ("build-win" + $(if ($arch -eq 'x64') { '64' } else { '32' }))
    $plat = if ($arch -eq 'x64') { 'x64' } else { 'Win32' }

    # Everything a human wants to see goes to the host stream, NOT the output
    # stream: this function's only output-stream value is the returned dir, and a
    # bare "=== building ===" here would be captured by `$dir = Build-GameExe`
    # and then handed to New-GameRunDir as the exe's directory.
    Write-Host "=== building $arch $Config (CMake, $bdir) ==="
    # Configure only if this dir was never configured: -A is a first-configure
    # option, and re-running it against a cache that belongs to another
    # generator/arch is a hard CMake error, not a no-op.
    if (-not (Test-Path (Join-Path $bdir 'CMakeCache.txt'))) {
      & $cmake -S $repo -B $bdir -A $plat 2>&1 | Select-Object -Last 5 | ForEach-Object { Write-Host $_ }
    }
    $bld = & $cmake --build $bdir --config $Config 2>&1
    # Match link ERRORS only: a bare 'LNK[0-9]' also matched warnings - notably
    # LNK4281 ("undesirable base address 0x10000000 for x64 image"), which this
    # project emits by design (/BASE:0x10000000 /DYNAMICBASE:NO, deliberate).
    # MSVC spells the two severities differently: "path : error LNK2019: ..." and
    # "LINK : fatal error LNK1120: ...", while a warning is "LINK : warning
    # LNK4281: ...". That false positive stayed hidden as long as the build was
    # up to date (no relink, hence no warning); the exe rename forced a relink.
    $errs = $bld | Select-String -Pattern 'error [A-Z]|error LNK[0-9]|CMake Error'
    if ($errs) { $errs | ForEach-Object { Write-Host $_.Line }; throw "BUILD FAILED (CMake $bdir $Config)" }
    $dir = Get-CMakeOutDirForExe -RepoRoot $repo -ExeName $ExeName -BuildDir $bdir -Config $Config
    if (-not $dir) { throw "no $ExeName under $bdir after a $Config build" }
    Stage-GameDataForExe -SrcDir $SRC -ExeDir $dir
    return $dir
  }

  Write-Host "=== building $arch $Config (build-vs2022.bat) ==="
  $out = cmd /c "`"$SRC\build-vs2022.bat $($Config.ToLower()) $arch`"" 2>&1
  # 'error LNK' rather than 'LNK[0-9]', same as the cmake branch: the x64 link
  # prints LNK4281 (the deliberate /BASE:0x10000000) and it is not a problem.
  $out | Select-String -Pattern 'SUCCESS|BUILD FAILED|error [A-Z]|error LNK[0-9]' | ForEach-Object { Write-Host $_.Line }
  $dir = Get-OutDirForExe $SRC $ExeName
  if (-not (Test-Path (Join-Path $dir $ExeName))) { throw "no exe at $dir\$ExeName after build-vs2022.bat" }
  Stage-GameDataForExe -SrcDir $SRC -ExeDir $dir
  return $dir
}

function New-GameRunDir {
  param([string]$ExeName, [string]$FromDir = $null)

  $SRC = (Resolve-Path (Join-Path $__TestsDir '..\..\SRC')).Path
  if (-not $FromDir) { $FromDir = Get-OutDirForExe $SRC $ExeName }   # the bat's per-arch output dir
  if (-not (Test-Path (Join-Path $FromDir $ExeName))) {
    throw "no exe at $FromDir\$ExeName - build it first (SRC\build-vs2022.bat, or CMake into build-win32/build-win64)"
  }
  Clear-StartupDialogs $ExeName
  $RUN = Join-Path $env:TEMP 'pilots_rundir'
  New-Item -ItemType Directory -Force -Path $RUN | Out-Null

  # This directory's contract is "no movies" (see the header), but it is never
  # actually emptied - clickskip.ps1 drops MOVIES here and removes them in a
  # finally, so a killed run leaves loose .AVI/.BMP behind. Those are exactly the
  # names the engine looks for relative to the CWD (.\intro.AVI, case aside), and
  # a 32-bit build then spends x86check's whole measurement window on the intro,
  # which shows up as a bogus GameMainLoop FAIL rather than as
  # leftover files. Enforce the contract on every run.
  Get-ChildItem $RUN -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -ieq '.avi' -or $_.Extension -ieq '.bmp' } |
    Remove-Item -Force -ErrorAction SilentlyContinue

  # SDL2.dll ships one copy per arch (the bat copies it into out\x86 and out\x64
  # separately, CMake copies it next to its own exe), so FromDir has the matching
  # one. The exe and the DLL are build outputs; PILOTS.EXE is game data and always
  # comes from the SRC root - it is not a build product.
  foreach ($f in @($ExeName, 'SDL2.dll', 'PILOTS.EXE')) {
    $s = if ($f -ieq 'PILOTS.EXE') { Join-Path $SRC $f } else { Join-Path $FromDir $f }
    if (-not (Test-Path $s)) { throw "missing game file: $s" }
    $d = Join-Path $RUN $f
    if (Test-Path $d) { Remove-Item $d -Force -ErrorAction SilentlyContinue }
    cmd /c "mklink /H `"$d`" `"$s`"" | Out-Null
    if (-not (Test-Path $d)) { Copy-Item $s $d -Force }   # fallback, e.g. other volume
  }
  return $RUN
}

# --- startup failure dialog -------------------------------------------------
# If the exe cannot even load (a missing or wrong SDL2.dll next to it, a broken
# image), Windows puts up a modal box BEFORE main() runs - no window, no log, and
# the harness would otherwise just say "no window" or hang. Find that box, report
# its text and click its button. The classic case (the other arch's SDL2.dll,
# which used to be shared by name) is gone since each arch keeps its own copy in
# out\<arch>, but "no window, no log" is still best explained by this box, so it
# stays.
if (-not ('StartupDiag' -as [type])) {
  Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;

public static class StartupDiag
{
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc f, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);

    public static string ClassOf(IntPtr h)
    {
        var cls = new StringBuilder(64); GetClassNameW(h, cls, 64);
        return cls.ToString();
    }

    // Text of the first visible #32770 (dialog) box that either belongs to the
    // process or merely NAMES it in its title, and a BM_CLICK on its button.
    //
    // The pid clause alone is not enough: 0xC000007B is raised by the loader,
    // and the resulting "Ошибка при запуске приложения" box is owned by a
    // completely different process - so a filter on the game's pid finds
    // nothing. Match on the executable's name in the title instead.
    //
    // The button caption is localized, so take the first Button child (dialog
    // templates have OK first).
    public static string Dismiss(uint pid, string name)
    {
        string text = null;
        EnumWindows(delegate(IntPtr h, IntPtr l)
        {
            var cls = new StringBuilder(64); GetClassNameW(h, cls, 64);
            if (cls.ToString() != "#32770" || !IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512); GetWindowTextW(h, sb, 512);
            var t = sb.ToString();
            bool mine = false;
            // Match "<exe>.EXE" rather than the bare process name: "PILOTS_RE"
            // is a substring of "PILOTS_RE_x64", and a leftover box from the
            // other arch would otherwise be taken for this process' own.
            if (name != null && name.Length > 0 && t.IndexOf(name, StringComparison.OrdinalIgnoreCase) >= 0)
                mine = true;
            else { uint w; GetWindowThreadProcessId(h, out w); mine = (w == pid); }
            if (!mine) return true;
            text = t;
            // The loader's code ("Приложение не запущено (0xc000007b)") is in the
            // body, i.e. in a Static child - the title alone would not carry it.
            // Collect the texts first and click afterwards: BM_CLICK stops the
            // enumeration, so clicking inline would drop whatever follows.
            IntPtr ok = IntPtr.Zero;
            EnumChildWindows(h, delegate(IntPtr b, IntPtr l2)
            {
                var bc = new StringBuilder(32); GetClassNameW(b, bc, 32);
                var cn = bc.ToString();
                if (cn == "Button") { if (ok == IntPtr.Zero) ok = b; return true; }
                if (cn == "Static")
                {
                    var bt = new StringBuilder(512); GetWindowTextW(b, bt, 512);
                    if (bt.Length > 0) text += "\r\n" + bt.ToString();
                }
                return true;
            }, IntPtr.Zero);
            if (ok != IntPtr.Zero) SendMessage(ok, 0x00F5 /*BM_CLICK*/, IntPtr.Zero, IntPtr.Zero);
            return false;
        }, IntPtr.Zero);
        return text;
    }
}
'@
}

function Get-StartupDialog {
  # $null when no dialog can be found; otherwise its text (the box has already
  # been clicked shut by then). The box is matched on "<exe>.EXE" in its title
  # because it is NOT owned by the game process - see StartupDiag.
  param($Proc)
  try { if ($Proc.HasExited) { return $null } } catch { return $null }
  $name = ''
  try { $name = $Proc.ProcessName + '.EXE' } catch { }
  return [StartupDiag]::Dismiss([uint32]$Proc.Id, $name)
}

function Clear-StartupDialogs {
  # Clicks shut any visible box left over from an earlier run, so a stale one is
  # never mistaken for this run's. Takes the exe's file name ("PILOTS_RE.EXE").
  param([string]$ExeName)
  for ($i = 0; $i -lt 8; $i++) {
    if (-not [StartupDiag]::Dismiss([uint32]0, $ExeName)) { break }
    Start-Sleep -Milliseconds 100
  }
}

function Get-WindowClass {
  param([IntPtr]$Hwnd)
  if ($Hwnd -eq [IntPtr]::Zero) { return '' }
  return [StartupDiag]::ClassOf($Hwnd)
}

function Report-StartupFailure {
  # Call when the game window never appeared: explains WHY instead of "no window".
  # Pass -Dialog when the box was found (and clicked) by an earlier poll, so the
  # text is not lost - Dismiss() returns nothing once the box is gone.
  param($Proc, [string]$Dialog)
  if (-not $Dialog) { $Dialog = Get-StartupDialog $Proc }
  if (-not $Dialog) { "no window"; return }
  "startup dialog: $Dialog"
  if ($Dialog -match '0x[cC]000007[bB]') {
    "  -> 0xC000007B: SDL2.dll does not match the exe's arch (see AGENTS.md:"
    "     SDL2.dll is arch-shared, only the last-built flavor can run)."
  }
}
