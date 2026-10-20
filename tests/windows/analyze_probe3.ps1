param([string]$LogDir = "$env:TEMP\opencode")
foreach ($name in 'probe_x64_rel.log','probe_x64_dbg.log') {
  $p = Join-Path $LogDir $name
  Write-Output "==== $name ===="
  if (-not (Test-Path $p)) { Write-Output "MISSING"; continue }
  $lines = Get-Content $p
  $lines | Select-String 'CL_EXIT' | ForEach-Object { $_.Line }
  # strict pointer-class (star-based filter)
  $ptr = $lines | Select-String 'C(4311|4312|4302|4313|4826):|C(4047|4133|4024|4022|4113):[^\r\n]*\*'
  Write-Output ("pointer-class strict: {0}" -f $ptr.Count)
  $byfile = @{}
  foreach ($w in $ptr) { if ($w -match '^(?<f>[^(]+)\(') { $k = Split-Path $Matches.f -Leaf; $byfile[$k] = 1 + $byfile[$k] } }
  $byfile.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Write-Output ("  {0}: {1}" -f $_.Key, $_.Value) }
  # INT_PTR-related mismatches WITHOUT star (pending prototype fixes)
  $ip = $lines | Select-String 'C(4022|4024|4133|4047|4113):[^\r\n]*INT_PTR'
  Write-Output ("INT_PTR-mismatch (pending shared edits): {0}" -f $ip.Count)
  $by2 = @{}
  foreach ($w in $ip) { if ($w -match '^(?<f>[^(]+)\(') { $k = Split-Path $Matches.f -Leaf; $by2[$k] = 1 + $by2[$k] } }
  $by2.GetEnumerator() | Sort-Object Value -Descending | ForEach-Object { Write-Output ("  {0}: {1}" -f $_.Key, $_.Value) }
  # any remaining decl/def mismatch mentioning INT_PTR anywhere
  $misc = $lines | Select-String 'C(4022|4024|4113|4133):' | Where-Object { $_.Line -notmatch '\*' -and $_.Line -notmatch 'INT_PTR' }
  Write-Output ("other decl/def mismatches (no star, no INT_PTR): {0}" -f $misc.Count)
  $misc | Select-Object -First 12 | ForEach-Object { Write-Output ("  " + $_.Line) }
}
