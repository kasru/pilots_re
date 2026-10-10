$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
$bats = @('build_sdl_debug.bat','build_sdl_release.bat','build_sdl_x64.bat','build_sdl_x64_debug.bat')
foreach ($b in $bats) {
  "=== $b ==="
  $out = cmd /c "`"$SRC\$b`"" 2>&1
  $out | Select-String -Pattern 'SUCCESS|BUILD FAILED|error [A-Z]|LNK[0-9]' | ForEach-Object { $_.Line }
}
