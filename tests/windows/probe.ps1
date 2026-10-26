# probe.ps1 = rebuild x64 debug, then run pfuzz against it - the one-shot wrapper
# for the workhorse. Same -Build bat|cmake as the other harnesses: 'bat' (default)
# is SRC\build-vs2022.bat into SRC\out\x64, 'cmake' is the same x64 debug built
# into build-win64. pfuzz itself builds nothing and just runs whatever is in
# SRC\out\x64 - so with cmake the child has to be pointed at that dir explicitly.
param([ValidateSet('bat','cmake')][string]$Build = 'bat')

. "$PSScriptRoot\rundir.ps1"

Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1

try {
  $exeDir = Build-GameExe -ExeName 'PILOTS_RE_x64.EXE' -Config Debug -Build $Build
} catch {
  "$($_.Exception.Message)"; exit 1
}

"=== running fuzz ==="
& "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File (Join-Path $PSScriptRoot 'pfuzz.ps1') -FromDir $exeDir
