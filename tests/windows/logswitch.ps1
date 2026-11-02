# -log [file] - when the log is written, and into which file.
#
# Release builds must be silent by default: DBG_* is compiled in but gated on
# g_DbgLogOn (debug_log.h), and DbgLogInit returns before opening anything when
# the key is absent - so a plain release run creates no file at all. The key
# turns it on:
#   -log                 -> the usual <exe name>_debug.log, next to the image
#   -log <file>          -> that file, resolved against the LAUNCH directory
#
# Why the launch directory matters: DbgLogInit reads the key before
# SdlBackendParseCmdLine can run -datapath's SetCurrentDirectoryA, which is
# also why the -datapath log lines exist at all. Every case below therefore
# passes -datapath and asserts that the file appears HERE (next to the exe)
# and NOT in the data directory - that is the ordering, measured.
#
# Debug builds keep their old behaviour: the log is on either way, and -log
# merely redirects it.
#
# The harness builds BOTH flavours itself (Release first, Debug second, each
# copied into its own scratch dir before the next build overwrites it), so it
# never depends on which one happens to sit in out\<arch>.
#
#   & "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File tests\windows\logswitch.ps1
#   ... -Arch x86
#   ... -Build cmake
param([ValidateSet('x86','x64')][string]$Arch = 'x64',
      [ValidateSet('bat','cmake')][string]$Build = 'bat')

$ExeName = if ($Arch -eq 'x64') { 'PILOTS_RE_x64.EXE' } else { 'PILOTS_RE.EXE' }
$Stem = [IO.Path]::GetFileNameWithoutExtension($ExeName)
$RunSeconds = 6

$fail = 0
function Check([string]$what, [bool]$ok, [string]$detail = '') {
  if ($ok) { "  ok   $what $detail" } else { "  FAIL $what $detail"; $script:fail++ }
}

. "$PSScriptRoot\rundir.ps1"
Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path

# Copy exe + its own SDL2.dll into a private dir: the scratch must not share
# files with out\<arch>, because the second build overwrites both.
function Copy-ExeTo([string]$FromDir, [string]$ToDir) {
  # Скатать старое содержимое: прошлый прогон оставил бы journal.txt, и кейс
  # «нечего писать» упал бы на чужом файле.
  Remove-Item $ToDir -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Path $ToDir -Force | Out-Null
  Copy-Item (Join-Path $FromDir $ExeName) $ToDir -Force
  Copy-Item (Join-Path $FromDir 'SDL2.dll') $ToDir -Force
}

# One run: start the copied exe with the given keys, then look at what landed
# where. $Want says what the launch dir must contain: 'none', 'default'
# (<stem>_debug.log) or a literal file name; $AlsoNone are files that must NOT
# be there. The log must carry real markers, not just exist.
function Run-Case {
  param([string]$Label, [string]$LaunchDir, [string[]]$ArgList, [string]$Want)
  $work = Join-Path $env:TEMP ('pilots-log-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
  New-Item -ItemType Directory -Path $work | Out-Null
  # Журналов в каталоге данных быть не должно: сравниваем состав до и после,
  # чтобы упавший прогон не обвинял в своём же файле.
  $dataBefore = @(Get-ChildItem -Path $SRC -Filter '*.log' -File -ErrorAction SilentlyContinue |
                  ForEach-Object { $_.Name })
  try {
    Copy-ExeTo -FromDir $LaunchDir -ToDir $work
    $all = @($ArgList) + @('-datapath', $SRC)
    $p = Start-Process -FilePath (Join-Path $work $ExeName) -ArgumentList $all `
        -WorkingDirectory $work -PassThru
    Start-Sleep -Seconds $RunSeconds
    $p.Refresh()
    if (-not $p.HasExited) { $p.Kill(); $p.WaitForExit() }

    $files = @(Get-ChildItem -Path $work -File | Where-Object { $_.Name -notin @($ExeName, 'SDL2.dll') })
    $names = @($files | ForEach-Object Name)
    $detail = 'files=' + $(if ($names) { ($files | ForEach-Object { "$($_.Name)($($_.Length))" }) -join ',' } else { '-' })

    switch ($Want) {
      'none'    { Check $Label ($files.Count -eq 0) $detail }
      'default' { Check $Label ($names -contains "${Stem}_debug.log") $detail }
      default   {
        $ok = ($names -contains $Want) -and ($names -notcontains "${Stem}_debug.log")
        Check $Label $ok $detail
      }
    }

    # В каталоге данных журнала появиться не должно: -log разбирается раньше,
    # чем -datapath успевает сменить рабочий каталог.
    $dataAfter = @(Get-ChildItem -Path $SRC -Filter '*.log' -File -ErrorAction SilentlyContinue |
                   ForEach-Object { $_.Name })
    $newData = @($dataAfter | Where-Object { $_ -notin $dataBefore })
    Check "$Label : no log in the data dir" ($newData.Count -eq 0) ($newData -join ',')

    if ($Want -ne 'none') {
      $text = ($files | ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join ''
      Check "$Label : markers present" ($text.Contains('ENTER: WinMain') -and
            $text.Contains('First present: 640x480')) ("len=" + $text.Length)
    }
  } finally {
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  }
}

try {
  $relDir = Build-GameExe -ExeName $ExeName -Config Release -Build $Build
  $relCopy = Join-Path $env:TEMP ('pilots-logrel-' + $Arch)
  Copy-ExeTo -FromDir $relDir -ToDir $relCopy
  $dbgDir = Build-GameExe -ExeName $ExeName -Config Debug -Build $Build
  $dbgCopy = Join-Path $env:TEMP ('pilots-logdbg-' + $Arch)
  Copy-ExeTo -FromDir $dbgDir -ToDir $dbgCopy
} catch {
  "$($_.Exception.Message)"
  exit 1
}

"release ($relDir):"
Run-Case 'release: no key writes nothing'    -LaunchDir $relCopy -ArgList @()                    -Want 'none'
Run-Case 'release: -log writes default file' -LaunchDir $relCopy -ArgList @('-log')              -Want 'default'
Run-Case 'release: -log <file> writes it'    -LaunchDir $relCopy -ArgList @('-log','journal.txt') -Want 'journal.txt'
Run-Case 'release: -log "name with a space"' -LaunchDir $relCopy -ArgList @('-log','"my log.txt"') -Want 'my log.txt'

"debug ($dbgDir):"
Run-Case 'debug: no key writes default file' -LaunchDir $dbgCopy -ArgList @()                     -Want 'default'
Run-Case 'debug: -log <file> redirects'      -LaunchDir $dbgCopy -ArgList @('-log','journal.txt') -Want 'journal.txt'

Remove-Item $relCopy, $dbgCopy -Recurse -Force -ErrorAction SilentlyContinue

if ($fail) { "$fail check(s) FAILED"; exit 1 }
'all checks passed'
