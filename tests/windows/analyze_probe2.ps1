param([string]$LogDir = "$env:TEMP\opencode")
foreach ($name in 'probe_x64_rel.log','probe_x64_dbg.log') {
  $p = Join-Path $LogDir $name
  Write-Output "==== $name errors ===="
  $lines = Get-Content $p
  $errs = $lines | Select-String ': error C\d+:'
  Write-Output ("error lines: {0}" -f $errs.Count)
  $hist = @{}
  foreach ($e in $errs) { $c = $e.Matches[0].Value -replace '.*error (C\d+):.*','$1'; $hist[$c] = 1 + $hist[$c] }
  $hist.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Write-Output ("  {0}: {1}" -f $_.Key, $_.Value) }
  Write-Output "  -- first 20 error lines --"
  $errs | Select-Object -First 20 | ForEach-Object { Write-Output ("  " + $_.Line) }
}
