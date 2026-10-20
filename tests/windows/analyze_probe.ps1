param([string]$LogDir = "$env:TEMP\opencode")
$logs = @("$LogDir\probe_x64_rel.log", "$LogDir\probe_x64_dbg.log")
foreach ($p in $logs) {
  Write-Output "==== $(Split-Path $p -Leaf) ===="
  if (-not (Test-Path $p)) { Write-Output "MISSING"; continue }
  $lines = Get-Content $p
  $lines | Select-String 'CL_EXIT|WARNING:|NO_VCVARS|fatal error' | ForEach-Object { $_.Line }
  $warns = $lines | Select-String ' warning C(\d+):'
  $errs  = $lines | Select-String ' error C\d+:| fatal error'
  Write-Output ("warnings total: {0}; errors total: {1}" -f $warns.Count, $errs.Count)
  $hist = @{}
  foreach ($w in $warns) { $c = $w.Matches[0].Groups[1].Value; $hist[$c] = 1 + $hist[$c] }
  $hist.GetEnumerator() | Sort-Object Name | ForEach-Object { Write-Output ("  C{0}: {1}" -f $_.Key, $_.Value) }
  $ptr = $warns | Where-Object {
    $_.Line -match 'C(4311|4312|4302|4313|4826)\b' -or
    ($_.Line -match 'C(4047|4133|4024|4113|4022|4028|4029|4090)\b' -and $_.Line -match '\*')
  }
  Write-Output ("pointer-class warnings: {0}" -f $ptr.Count)
  $byfile = @{}
  foreach ($w in $ptr) {
    if ($w.Line -match '^(?<f>[^(]+)\((\d+)\):') {
      $k = Split-Path $Matches.f -Leaf
      $byfile[$k] = 1 + $byfile[$k]
    }
  }
  $byfile.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Write-Output ("  {0}: {1}" -f $_.Key, $_.Value) }
  Write-Output "  -- samples (first 15) --"
  $ptr | Select-Object -First 15 | ForEach-Object { Write-Output ("  " + $_.Line) }
}
