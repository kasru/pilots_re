# Scratch run directory for the game, shared by the harnesses.
#
# Dot-source it, then call New-GameRunDir:
#
#   . "$PSScriptRoot\rundir.ps1"
#   $RUN = New-GameRunDir -ExeName 'PILOTS_RE_x64.EXE'
#   $LOG = Join-Path $RUN 'PILOTS_RE_x64_debug.log'
#
# The exe lives in SRC\out (the bat's output), the game data in the SRC root -
# New-GameRunDir takes both from where they are.
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

function New-GameRunDir {
  param([string]$ExeName, [string]$FromDir = $null)

  $SRC = (Resolve-Path (Join-Path $__TestsDir '..\..\SRC')).Path
  if (-not $FromDir) { $FromDir = Join-Path $SRC 'out' }   # the bat's output dir
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

  # SDL2.dll is arch-shared, so whatever the last build left in SRC\out is the
  # right one. The exe and the DLL are build outputs (SRC\out); PILOTS.EXE is
  # game data and always comes from the SRC root - it is not a build product.
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
# If the exe cannot even load (most often: SDL2.dll of the other arch, both
# being arch-shared by name), Windows puts up a modal box BEFORE main() runs -
# no window, no log, and the harness would otherwise just say "no window" or
# hang. Find that box, report its text and click its button.
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
