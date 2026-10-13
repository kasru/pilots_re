# Carve the application icon out of the original PILOTS.EXE and write it as a
# plain .ico next to it (SRC\pilots.ico), so the Windows build can link it.
#
# PILOTS.EXE is the ORIGINAL GAME's data file - it is gitignored like every
# other piece of the game, and so is the icon carved out of it. The build bats
# call this script when SRC\pilots.ico is missing, which is why it stays on
# plain Windows PowerShell 5.1 syntax: it is run through powershell.exe from
# the bats, not through pwsh 7.
#
# The original carries a single RT_GROUP_ICON (id 100): one 32x32, 4 bpp image
# referencing RT_ICON id 1. An .ico is just that group resource with the
# directory widened from 14-byte GRPICONDIRENTRY to 16-byte ICONDIRENTRY, so
# the image blobs are copied verbatim.
param(
    [string]$Source = (Join-Path $PSScriptRoot '..\SRC\PILOTS.EXE'),
    [string]$Out    = (Join-Path $PSScriptRoot '..\SRC\pilots.ico')
)

$ErrorActionPreference = 'Stop'

if ( -not ('PilotsIcon' -as [type]) ) {
    Add-Type -Namespace Pilots -Name Icon -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern IntPtr LoadLibraryExA(string f, IntPtr h, uint flags);
[DllImport("kernel32.dll")]
public static extern bool FreeLibrary(IntPtr m);
[DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Ansi)]
public static extern IntPtr FindResourceA(IntPtr m, IntPtr name, IntPtr type);
[DllImport("kernel32.dll")]
public static extern IntPtr LoadResource(IntPtr m, IntPtr r);
[DllImport("kernel32.dll")]
public static extern IntPtr LockResource(IntPtr l);
[DllImport("kernel32.dll")]
public static extern uint SizeofResource(IntPtr m, IntPtr r);
[DllImport("kernel32.dll", CharSet=CharSet.Ansi)]
public static extern bool EnumResourceNamesA(IntPtr m, IntPtr type, EnumResNameProc cb, IntPtr p);
public delegate bool EnumResNameProc(IntPtr m, IntPtr type, IntPtr name, IntPtr p);
'@
}

$RT_GROUP_ICON = [IntPtr]14
$RT_ICON       = [IntPtr]3

# LOAD_LIBRARY_AS_DATAFILE: the module is mapped as data, so a 32-bit PE can be
# read from 64-bit PowerShell without running any of its code.
$mod = [Pilots.Icon]::LoadLibraryExA((Resolve-Path $Source).Path, [IntPtr]::Zero, 0x2)
if ( $mod -eq [IntPtr]::Zero ) {
    throw "cannot map $Source as data (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
}

try {
    function Read-Resource([IntPtr]$id, [IntPtr]$type) {
        $res = [Pilots.Icon]::FindResourceA($mod, $id, $type)
        if ( $res -eq [IntPtr]::Zero ) { return $null }
        $ldr = [Pilots.Icon]::LoadResource($mod, $res)
        if ( $ldr -eq [IntPtr]::Zero ) { return $null }
        $buf = New-Object byte[] ([Pilots.Icon]::SizeofResource($mod, $res))
        # Unary comma: a bare `return $buf` would enumerate the array into the
        # pipeline and hand the caller Object[], not byte[].
        [Runtime.InteropServices.Marshal]::Copy([Pilots.Icon]::LockResource($ldr), $buf, 0, $buf.Length)
        return ,$buf
    }

    # EnumResourceNamesA calls back into PowerShell, so the delegate has to be
    # held by a variable for the whole call - otherwise the GC pulls it away
    # mid-enumeration and only the first name ever comes back.
    $groupIds = New-Object System.Collections.ArrayList
    $cb = [Pilots.Icon+EnumResNameProc]{ param($m, $t, $name, $p) [void]$groupIds.Add($name); return $true }
    [void][Pilots.Icon]::EnumResourceNamesA($mod, $RT_GROUP_ICON, $cb, [IntPtr]::Zero)

    if ( $groupIds.Count -eq 0 ) { throw "$Source carries no RT_GROUP_ICON" }
    if ( $groupIds.Count -gt 1 ) {
        Write-Warning "$Source carries $($groupIds.Count) RT_GROUP_ICON resources; using the first (id $($groupIds[0].ToInt64()))"
    }

    $grp = Read-Resource $groupIds[0] $RT_GROUP_ICON
    if ( -not $grp -or $grp.Length -lt 6 ) { throw "cannot read RT_GROUP_ICON id $($groupIds[0].ToInt64())" }

    $count = [BitConverter]::ToUInt16($grp, 4)
    if ( $count -eq 0 ) { throw 'the group icon resource is empty' }

    $icoEntries  = New-Object System.Collections.Generic.List[byte]
    $blobs    = New-Object System.Collections.Generic.List[byte]
    $offset   = 6 + 16 * $count            # ICONDIR + ICONDIRENTRY[count]

    for ( $i = 0; $i -lt $count; $i++ ) {
        $off = 6 + 14 * $i                 # GRPICONDIRENTRY is 14 bytes
        $iconId = [BitConverter]::ToUInt16($grp, $off + 12)   # the RT_ICON id

        $img = Read-Resource ([IntPtr]$iconId) $RT_ICON
        if ( -not $img ) { throw "RT_ICON id $iconId referenced by the group is missing" }

        # ICONDIRENTRY = the GRPICONDIRENTRY fields, with the trailing RT_ICON id
        # replaced by the blob's offset in the file.
        # 8 bytes only - the GRPICONDIRENTRY's own cbBytes (at +8) must NOT be
        # copied across: ICONDIRENTRY carries its own 4-byte length, and taking
        # 12 makes the entry 20 bytes and shifts the whole file.
        for ( $k = 0; $k -lt 8; $k++ ) { $icoEntries.Add($grp[$off + $k]) }
        $icoEntries.AddRange([BitConverter]::GetBytes([UInt32]$img.Length))
        $icoEntries.AddRange([BitConverter]::GetBytes([UInt32]$offset))

        $blobs.AddRange($img)
        $offset += $img.Length
    }

    # NB: not `$out` - that is this script's own [string] parameter (PowerShell
    # names are case-insensitive), and the assignment would stringify the list.
    $ico = New-Object System.Collections.Generic.List[byte]
    $ico.AddRange([BitConverter]::GetBytes([UInt16]0))   # idReserved, always 0
    $ico.AddRange([BitConverter]::GetBytes([UInt16]1))   # idType: 1 = icon
    $ico.AddRange([BitConverter]::GetBytes([UInt16]$count))
    $ico.AddRange($icoEntries)
    $ico.AddRange($blobs)

    # GetFullPath, not Join-Path: $Out is already rooted, and Join-Path would
    # glue it onto the current directory instead of normalising it.
    $full = [IO.Path]::GetFullPath($Out)
    [IO.File]::WriteAllBytes($full, $ico.ToArray())

    $img0 = Read-Resource ([IntPtr]([BitConverter]::ToUInt16($grp, 18))) $RT_ICON
    $w = [BitConverter]::ToInt32($img0, 4)
    $h = [BitConverter]::ToInt32($img0, 8) / 2
    $bpp = [BitConverter]::ToUInt16($img0, 14)
    Write-Output ("icon: {0} image(s), first {1}x{2} {3} bpp -> {4}" -f $count, $w, $h, $bpp, $full)
}
finally {
    [void][Pilots.Icon]::FreeLibrary($mod)
}


