# The four combinations of [debug|release] x [x86|x64] - all of them, back to
# back, printing SUCCESS/FAILED only. -Build bat (default) walks the four
# build-vs2022.bat combinations, which is what this script was written for;
# -Build cmake walks the same four through CMake, so both paths get built in one
# shot instead of one's four builds being trusted because the other's are green.
param([ValidateSet('bat','cmake')][string]$Build = 'bat')

$combos = @('Debug/x86','Release/x86','Release/x64','Debug/x64')
$failed = 0

. "$PSScriptRoot\rundir.ps1"
foreach ($c in $combos) {
  $cfg, $arch = $c -split '/'
  $exe = if ($arch -eq 'x64') { 'PILOT_RE_x64.EXE' } else { 'PILOT_RE.EXE' }
  try {
    $dir = Build-GameExe -ExeName $exe -Config $cfg -Build $Build
    "OK: $dir\$exe"
  } catch {
    "FAILED: $cfg $arch - $($_.Exception.Message)"
    $failed++
  }
}
"--- ${Build}: $($combos.Count - $failed) ok, $failed failed ---"
if ($failed) { exit 1 }
