$SRC = (Resolve-Path (Join-Path $PSScriptRoot '..\..\SRC')).Path
# The four combinations of build-vs2022.bat: [debug|release] [x86|x64].
$combos = @('debug x86','release x86','release x64','debug x64')
foreach ($c in $combos) {
  "=== build-vs2022.bat $c ==="
  $out = cmd /c "`"$SRC\build-vs2022.bat $c`"" 2>&1
  $out | Select-String -Pattern 'SUCCESS|BUILD FAILED|error [A-Z]|LNK[0-9]' | ForEach-Object { $_.Line }
}
