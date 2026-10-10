$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
Get-Process PILOT_RV,PILOT_RV_x64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
"=== building x64 debug ==="
$out = cmd /c "`"$SRC\build_sdl_x64_debug.bat`"" 2>&1
$out | Select-String -Pattern 'SUCCESS|BUILD FAILED|error [A-Z]' | ForEach-Object { $_.Line }
"=== running fuzz ==="
& "$env:ProgramFiles\PowerShell\7\pwsh.exe" -NoProfile -File (Join-Path $PSScriptRoot 'pfuzz.ps1')
