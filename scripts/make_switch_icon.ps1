# Builds assets/switch_icon.bmp: the 128x128 8bpp icon that elf2nro puts into
# PILOTS_RE.nro and hbmenu then shows in its grid.
#
# Usage (either PowerShell works - this file uses nothing newer than 5.1 and no
# System.Drawing, so it runs on Windows and on the Linux box that builds the nro):
#
#   powershell.exe -NoProfile -File scripts\make_switch_icon.ps1
#   pwsh -NoProfile -File scripts/make_switch_icon.ps1
#
# WHY GENERATE AN ICON INSTEAD OF USING THE GAME'S OWN: the original game icon is
# a PE resource inside PILOTS.EXE, i.e. game data, so it cannot come near this
# repository - which is also why SRC/pilots.ico is gitignored and carved out at
# build time by extract_icon.ps1 (see the Build section of AGENTS.md). The Windows
# exe may wear the game's icon because that happens on the user's own copy at
# build time; a .nro, on the other hand, has the icon baked into the file we hand
# out, so that file has to carry art of our own. Hence a generated picture: a
# propeller in a ring, drawn from arithmetic in this script and nothing else.
#
# WHY HAND-WRITEN BYTES: nacptool wants an UNCOMPRESSED 8-bit INDEXED BMP at
# exactly 128x128, and GDI+ cannot render into an indexed bitmap anyway. So the
# picture is drawn into a truecolour buffer with 3x3 supersampling (that is the
# antialiasing), and only then written out as an indexed BMP.
#
# The palette is not quantized: the colours that actually come out of the
# supersampling are collected and become the palette, so the file is lossless by
# construction. That is safe here because the picture is only four flat colours -
# averaging 9 samples of them can produce at most 4 + 3*8 = 28 distinct values,
# and a 256-entry table has room to spare. Should the drawing ever grow
# photografic enough to exceed 256 colours, the script says so and stops rather
# than quietly dithering.

param(
    [string]$Path = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
if (-not $Path) {
    $Path = Join-Path (Join-Path $repo "assets") "switch_icon.bmp"
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

function Sample([double]$x, [double]$y) {
    # Rounded-square border: the picture is not masked by anything on the
    # console, so the corners are painted darker to read as rounded.
    $qx = [Math]::Abs($x - $C) - ($W / 2.0 - $inset - $corner)
    $qy = [Math]::Abs($y - $C) - ($H / 2.0 - $inset - $corner)
    if ($qx -gt 0 -and $qy -gt 0) {
        $d = [Math]::Sqrt($qx * $qx + $qy * $qy)
        if ($d -gt $corner) { return $cEdge }
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

# ---- draw --------------------------------------------------------------------
# Distinct colours, in first-seen order, plus their index.
$palette = New-Object System.Collections.Generic.List[int]
$index   = @{}
$pixels  = New-Object 'int[]' ($W * $H)
$step    = 1.0 / $SS

for ($py = 0; $py -lt $H; $py++) {
    for ($px = 0; $px -lt $W; $px++) {
        $sr = 0; $sg = 0; $sb = 0
        for ($sy = 0; $sy -lt $SS; $sy++) {
            for ($sx = 0; $sx -lt $SS; $sx++) {
                $col = Sample ($px + ($sx + 0.5) * $step) ($py + ($sy + 0.5) * $step)
                $sr += $col[0]; $sg += $col[1]; $sb += $col[2]
            }
        }
        $n   = $SS * $SS
        # Round, then pack: the averages of nine flat colours are rarely whole
        # numbers, and -shl on a double would let PowerShell round it on its own.
        $key = ([int][Math]::Round($sr / $n) -shl 16) -bor `
               ([int][Math]::Round($sg / $n) -shl 8) -bor `
               [int][Math]::Round($sb / $n)
        if (-not $index.ContainsKey($key)) {
            if ($palette.Count -ge 256) {
                throw "make_switch_icon: more than 256 distinct colours ($($palette.Count)+), the drawing outgrew an 8bpp palette"
            }
            $index[$key] = $palette.Count
            $palette.Add($key)
        }
        $pixels[$py * $W + $px] = $index[$key]
    }
}
Write-Host "colours: $($palette.Count)"

# ---- write an 8bpp indexed BMP -----------------------------------------------
$hdr    = 14
$infohdr = 40
$palsz  = 256 * 4                       # always the full table, unused slots black
$rowbytes = $W                          # 128 already a multiple of 4, no padding
$off    = $hdr + $infohdr + $palsz
$bytes  = New-Object 'byte[]' ($off + $rowbytes * $H)

function PutBE16([byte[]]$b, [int]$p, [int]$v) { $b[$p] = $v -band 0xFF; $b[$p+1] = ($v -shr 8) -band 0xFF }
function PutBE32([byte[]]$b, [int]$p, [int]$v) {
    $b[$p]   =  $v         -band 0xFF
    $b[$p+1] = ($v -shr  8) -band 0xFF
    $b[$p+2] = ($v -shr 16) -band 0xFF
    $b[$p+3] = ($v -shr 24) -band 0xFF
}

# BITMAPFILEHEADER: 'BM', whole size, reserved, pixel-data offset
$bytes[0] = 0x42; $bytes[1] = 0x4D
PutBE32 $bytes 2  ($off + $rowbytes * $H)
PutBE32 $bytes 10 $off
# BITMAPINFOHEADER
PutBE32 $bytes 14 $infohdr
PutBE32 $bytes 18 $W
PutBE32 $bytes 22 $H                          # positive = bottom-up rows
$bytes[26] = 1; $bytes[27] = 0                # planes
$bytes[28] = 8; $bytes[29] = 0                # bpp
PutBE32 $bytes 30 0                           # BI_RGB: uncompressed, as nacptool wants
PutBE32 $bytes 34 ($rowbytes * $H)
PutBE32 $bytes 38 2835                        # ~72 dpi; nobody reads it
PutBE32 $bytes 42 2835
PutBE32 $bytes 46 0                           # biClrUsed 0 = "all 256"
PutBE32 $bytes 50 0
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

$dir = Split-Path -Parent $Path
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
[System.IO.File]::WriteAllBytes($Path, $bytes)
Write-Host "wrote $Path ($($bytes.Length) bytes)"
