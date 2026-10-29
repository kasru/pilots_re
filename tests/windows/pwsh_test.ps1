"version          : $($PSVersionTable.PSVersion)  ($($PSVersionTable.PSEdition))"
"OS               : $($PSVersionTable.OS)"

$t = 1 -eq 1 ? "yes" : "no"
$f = 1 -eq 2 ? "yes" : "no"
"ternary operator : OK  (true->$t, false->$f)"

$u = [UIntPtr]1
"UIntPtr cast     : OK  value=$u"

$p = Join-Path $env:TEMP "enc_test.txt"
[System.IO.File]::WriteAllText($p, "привет без BOM`n", [System.Text.UTF8Encoding]::new($false))
$b = [System.IO.File]::ReadAllBytes($p)[0..3]
"UTF-8 no BOM     : first bytes = $($b -join ',')  (no 239,187,191 => OK)"
Remove-Item $p

Add-Type -AssemblyName System.Windows.Forms,System.Drawing
Add-Type 'using System; using System.Runtime.InteropServices;
public class T {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr LP(int x,int y){ return (IntPtr)(((uint)y<<16)|((uint)x&0xFFFF)); }
}'
"Add-Type P/Invoke: OK   LP(45,448)=$([T]::LP(45,448).ToInt64())  (expect 2949165 = 0x1C0002D)"
"SetProcessDPIAware(): $([T]::SetProcessDPIAware())"
"Screen bounds    : $([System.Windows.Forms.Screen]::PrimaryScreen.Bounds)"

$g = Get-Process PILOTS_RE,PILOTS_RE_x64 -ErrorAction SilentlyContinue
"game processes   : " + $(if ($g) { ($g | ForEach-Object { "$($_.ProcessName)($($_.Id))" }) -join ', ' } else { 'none' })
