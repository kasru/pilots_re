# Builds the two icons the port ships. Both are the same picture - a propeller in
# a ring, drawn from arithmetic in this file and nothing else. The game's own icon
# (a PE resource inside PILOTS.EXE) is not used anywhere: everything this
# repository carries is either code or art we can account for.
#
#   assets/switch_icon.bmp   the 128x128 8bpp icon elf2nro puts into PILOTS_RE.nro
#                            and hbmenu draws. nacptool wants exactly that size, 8
#                            bits per pixel, uncompressed and indexed.
#   SRC/pilots.ico           the Windows exe icon, referenced by pilots.rc's single
#                            ICON statement. 16/32/48/128, 32 bpp with alpha, so
#                            Explorer picks the size it needs and small sizes stay
#                            legible.
#
# Usage (either PowerShell works - this file uses nothing newer than 5.1 and no
# System.Drawing, so it runs on Windows and on the Linux box that builds the nro):
#
#   powershell.exe -NoProfile -File scripts\make_icons.ps1
#   pwsh -NoProfile -File scripts/make_icons.ps1
#
#   ... [-Bmp <path>] [-Ico <path>]     write somewhere else
#
# WHY GENERATED RATHER THAN DRAWN BY HAND: the BMP is a format nacptool rejects
# unless every field is right, the ICO must carry several sizes, and a binary
# asset has no readable history. Drawing both from the same function keeps them
# consistent and makes the diff reviewable.
#
# WHY THE BMP IS HAND-WRITTEN: GDI+ cannot render into an indexed bitmap at all.
# The picture is drawn into a truecolour buffer with 3x3 supersampling (that is
# the antialiasing), and only then written out as an indexed BMP. Its palette is
# not quantized: the colours that actually come out of the supersampling are
# collected and become the palette, so the file is lossless by construction.
# Should the drawing ever grow photografic enough to exceed 256 colours, the
# script says so and stops rather than quietly dithering. The ICO has no such
# limit - it is 32 bpp - so there the supersampling coverage goes into alpha.

param(
    [string]$Bmp = "",
    [string]$Ico = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Bmp) {
    $Bmp = Join-Path (Join-Path $repo "assets") "switch_icon.bmp"
}
if (-not $Ico) {
    $Ico = Join-Path (Join-Path $repo "SRC") "pilots.ico"
}

$W = 128
$H = 128
$SS = 3                       # supersample factor per axis: 9 samples a pixel

# The whole vocabulary of the picture. Flat colours only - see the note above.
$cEdge   = @(0x0A, 0x11, 0x20)   # outside the rounded square
$cBg     = @(0x10, 0x1E, 0x33)   # the square itself
$cAmber  = @(0xF2, 0xC1, 0x4E)   # ring and hub
$cLight  = @(0xE9, 0xEF, 0xF7)   # propeller blades

$C  = 64.0                      # centre, in sample space where pixel i spans [i,i+1)
$inset   = 4.0                  # rounded square: inset and corner radius
$corner  = 24.0
$ringIn  = 43.0                 # ring annulus
$ringOut = 47.0
$hubR    = 11.0                 # hub disc
$bladeRt = 8.0                  # blade: root (inside the hub, so they join), tip
$bladeTp = 41.0
$bladeW  = 7.0                  # blade half-thickness at the root
$bladeTk = 10.0                 # over how far before the tip it tapers

$ang = 35.0 * [Math]::PI / 180.0
$ux = [Math]::Cos($ang)
$uy = [Math]::Sin($ang)

# Returns the colour at a point, or $null outside the rounded square - the BMP
# has to paint that part, the ICO leaves it transparent.
function Sample([double]$x, [double]$y) {
    $qx = [Math]::Abs($x - $C) - ($W / 2.0 - $inset - $corner)
    $qy = [Math]::Abs($y - $C) - ($H / 2.0 - $inset - $corner)
    if ($qx -gt 0 -and $qy -gt 0) {
        $d = [Math]::Sqrt($qx * $qx + $qy * $qy)
        if ($d -gt $corner) { return $null }
    }

    $dx = $x - $C
    $dy = $y - $C
    $r  = [Math]::Sqrt($dx * $dx + $dy * $dy)

    if ($r -le $hubR) { return $cAmber }

    # Two blades along one axis, opposite each other. Half-thickness varies along
    # the axis so they read as blades and not as a bar: full width over the
    # middle, rounded away at root and tip. sin() over the span does that in one
    # line; the 0.35 floor keeps the root from vanishing into a point.
    $along  = $dx * $ux + $dy * $uy
    $across = [Math]::Abs($dx * $uy - $dy * $ux)
    $absA   = [Math]::Abs($along)
    if ($absA -ge $bladeRt -and $absA -le $bladeTp) {
        $u    = ($absA - $bladeRt) / ($bladeTp - $bladeRt)
        $half = $bladeW * (0.35 + 0.65 * [Math]::Sin([Math]::PI * $u))
        if ($across -le $half) { return $cLight }
    }

    if ($r -ge $ringIn -and $r -le $ringOut) { return $cAmber }
    return $cBg
}

# Renders the picture at $size x $size, top-down. $over is the colour the samples
# outside the rounded square get: the BMP needs one (it has no alpha), while $null
# leaves them out of the average and reports coverage in alpha instead.
# Returns @{ rgb = int[]; alpha = int[] }, alpha 0..255.
function Render([int]$size, $over) {
    $rgb   = New-Object 'int[]' ($size * $size)
    $alpha = New-Object 'int[]' ($size * $size)
    $step  = 1.0 / $SS
    $scale = $W / $size
    $n     = $SS * $SS
    for ($py = 0; $py -lt $size; $py++) {
        for ($px = 0; $px -lt $size; $px++) {
            $sr = 0; $sg = 0; $sb = 0; $cover = 0
            for ($sy = 0; $sy -lt $SS; $sy++) {
                for ($sx = 0; $sx -lt $SS; $sx++) {
                    $col = Sample (($px + ($sx + 0.5) * $step) * $scale) (($py + ($sy + 0.5) * $step) * $scale)
                    if ($null -eq $col) {
                        if ($null -eq $over) { continue }
                        $col = $over
                    }
                    $cover++
                    $sr += $col[0]; $sg += $col[1]; $sb += $col[2]
                }
            }
            $i = $py * $size + $px
            if ($cover -gt 0) {
                # Round, then pack: the averages of nine flat colours are rarely
                # whole numbers, and -shl on a double would round it on its own.
                $r = [int][Math]::Round($sr / $cover)
                $g = [int][Math]::Round($sg / $cover)
                $b = [int][Math]::Round($sb / $cover)
                $rgb[$i] = ($r -shl 16) -bor ($g -shl 8) -bor $b
            }
            $alpha[$i] = [int][Math]::Round(255.0 * $cover / $n)
        }
    }
    return @{ rgb = $rgb; alpha = $alpha }
}

# BMP is little-endian, so these write the low byte first.
function PutLE16([byte[]]$b, [int]$p, [int]$v) {
    $b[$p]   =  $v        -band 0xFF
    $b[$p+1] = ($v -shr 8) -band 0xFF
}
function PutLE32([byte[]]$b, [int]$p, [int]$v) {
    $b[$p]   =  $v         -band 0xFF
    $b[$p+1] = ($v -shr  8) -band 0xFF
    $b[$p+2] = ($v -shr 16) -band 0xFF
    $b[$p+3] = ($v -shr 24) -band 0xFF
}

# ---- assets/switch_icon.bmp: 128x128, 8 bpp, indexed -------------------------
# Distinct colours, in first-seen order, plus their index.
$picture = Render $W $cEdge
$palette = New-Object System.Collections.Generic.List[int]
$index   = @{}
$pixels  = New-Object 'int[]' ($W * $H)
for ($i = 0; $i -lt $W * $H; $i++) {
    $key = $picture.rgb[$i]
    if (-not $index.ContainsKey($key)) {
        if ($palette.Count -ge 256) {
            throw "make_icons: more than 256 distinct colours ($($palette.Count)+), the drawing outgrew an 8bpp palette"
        }
        $index[$key] = $palette.Count
        $palette.Add($key)
    }
    $pixels[$i] = $index[$key]
}
Write-Host "colours: $($palette.Count)"

$hdr     = 14
$infohdr = 40
$palsz   = 256 * 4                      # always the full table, unused slots black
$rowbytes = $W                          # 128 already a multiple of 4, no padding
$off     = $hdr + $infohdr + $palsz
$bytes   = New-Object 'byte[]' ($off + $rowbytes * $H)

# BITMAPFILEHEADER: 'BM', whole size, reserved, pixel-data offset
$bytes[0] = 0x42; $bytes[1] = 0x4D
PutLE32 $bytes 2  ($off + $rowbytes * $H)
PutLE32 $bytes 10 $off
# BITMAPINFOHEADER
PutLE32 $bytes 14 $infohdr
PutLE32 $bytes 18 $W
PutLE32 $bytes 22 $H                    # positive = bottom-up rows
$bytes[26] = 1; $bytes[27] = 0          # planes
$bytes[28] = 8; $bytes[29] = 0          # bpp
PutLE32 $bytes 30 0                     # BI_RGB: uncompressed, as nacptool wants
PutLE32 $bytes 34 ($rowbytes * $H)
PutLE32 $bytes 38 2835                  # ~72 dpi; nobody reads it
PutLE32 $bytes 42 2835
PutLE32 $bytes 46 0                     # biClrUsed 0 = "all 256"
PutLE32 $bytes 50 0
for ($i = 0; $i -lt $palette.Count; $i++) {
    $p = $hdr + $infohdr + $i * 4
    $bytes[$p]     =  $palette[$i]         -band 0xFF   # blue
    $bytes[$p + 1] = ($palette[$i] -shr 8) -band 0xFF   # green
    $bytes[$p + 2] = ($palette[$i] -shr 16) -band 0xFF  # red
    $bytes[$p + 3] = 0
}
for ($y = 0; $y -lt $H; $y++) {
    $dst = $off + ($H - 1 - $y) * $rowbytes   # bottom-up
    for ($x = 0; $x -lt $W; $x++) {
        $bytes[$dst + $x] = [byte]$pixels[$y * $W + $x]
    }
}

$dir = Split-Path -Parent $Bmp
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
[System.IO.File]::WriteAllBytes($Bmp, $bytes)
Write-Host "wrote $Bmp ($($bytes.Length) bytes)"

# ---- SRC/pilots.ico: 16/32/48/128, 32 bpp with alpha -------------------------
# Each image is a bottom-up DIB: a 40-byte header whose height is doubled, the
# BGRA pixels, then the 1bpp AND mask. The mask stays zero - the alpha channel is
# what Windows reads since Vista, and the mask is only there because the format
# has a slot for it.
$sizes  = @(16, 32, 48, 128)
$images = New-Object System.Collections.Generic.List[object]
foreach ($size in $sizes) {
    $one = Render $size $null
    $stride     = $size * 4
    $maskStride = [int]([math]::Floor((($size + 31) / 32)) * 4)
    $blob = New-Object 'byte[]' ($infohdr + $stride * $size + $maskStride * $size)
    PutLE32 $blob 0 $infohdr
    PutLE32 $blob 4 $size
    PutLE32 $blob 8 ($size * 2)         # ICO height spans XOR bitmap + AND mask
    $blob[12] = 1; $blob[13] = 0        # planes
    $blob[14] = 32; $blob[15] = 0       # bpp
    PutLE32 $blob 16 0                  # BI_RGB
    PutLE32 $blob 20 ($stride * $size)
    for ($y = 0; $y -lt $size; $y++) {
        $src = ($size - 1 - $y) * $size   # bottom-up
        $dst = $infohdr + $y * $stride
        for ($x = 0; $x -lt $size; $x++) {
            $i = $src + $x
            $p = $dst + $x * 4
            $blob[$p]     =  $one.rgb[$i]         -band 0xFF   # blue
            $blob[$p + 1] = ($one.rgb[$i] -shr 8)  -band 0xFF  # green
            $blob[$p + 2] = ($one.rgb[$i] -shr 16) -band 0xFF # red
            $blob[$p + 3] = [byte]$one.alpha[$i]
        }
    }
    $images.Add(@{ size = $size; blob = $blob })
}

$total = 6 + 16 * $sizes.Count
foreach ($image in $images) { $total += $image.blob.Length }
$icoBytes = New-Object 'byte[]' $total
PutLE16 $icoBytes 2 1                   # type: icon
PutLE16 $icoBytes 4 $sizes.Count
$offset = 6 + 16 * $sizes.Count
$at = 6
foreach ($image in $images) {
    if ($image.size -ge 256) {
        $icoBytes[$at] = 0; $icoBytes[$at + 1] = 0   # 0 means 256
    } else {
        $icoBytes[$at] = $image.size; $icoBytes[$at + 1] = $image.size
    }
    PutLE16 $icoBytes ($at + 4) 1                 # planes
    PutLE16 $icoBytes ($at + 6) 32                # bpp
    PutLE32 $icoBytes ($at + 8) $image.blob.Length
    PutLE32 $icoBytes ($at + 12) $offset
    [Array]::Copy($image.blob, 0, $icoBytes, $offset, $image.blob.Length)
    $offset += $image.blob.Length
    $at += 16
}

$dir = Split-Path -Parent $Ico
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
[System.IO.File]::WriteAllBytes($Ico, $icoBytes)
Write-Host "wrote $Ico ($($icoBytes.Length) bytes, $($sizes.Count) sizes: $($sizes -join '/'))"
