# Unlock levels on the map by patching PILOTS0.230.
#
#   pwsh -File tests\windows\unlock_levels.ps1              # all 15 levels (default)
#   pwsh -File tests\windows\unlock_levels.ps1 -Count 6
#   pwsh -File tests\windows\unlock_levels.ps1 -Count 15 -Input <run dir>\PILOTS0.230 -Output <file>
#
# The game has 15 levels: -Count is capped at 15 and a larger value is rejected.
# (Measured, not assumed: value 14 draws exactly 15 flags, the Ctrl overlay on the
# map sees those 15 plus the 5 menu buttons, and no flag 16 appears for any value.)
#
# PILOTS0.230 is the engine's settings/progress save (fileio.c OpenSaveFile slot 0,
# extension "230"): 8-byte magic g_SaveMagic1, 0x34 bytes of engine state, then the
# script-defined custom regions (saveload.c WriteCustomSaveData) XOR-encrypted with
# byte_410298 (XorCryptData, key index = (regionOffset + i + 1) & 0x1F).
#
# This game registers ONE 20-byte region whose globals offset is 12 (mod 32) - verified
# by decrypting the original file into clean small dwords. Its layout:
#
#   +0  dword  1                       (unchanged here)
#   +4  dword  levelsOpen - 1          <-- THIS: 2 = flags 1..3, 5 = flags 1..6, 14 = flags 1..15
#   +8  dword  0
#   +12 dword  0
#   +16 dword  1                       (unchanged here)
#
# The game re-reads the file at startup and writes the same values back, so a patched
# file keeps its level count until the script itself changes it (finishing a level).
# Flag positions on the map: tests/LEVEL_MAP.md.
param(
  [int]$Count = 15,
  [string]$Input = '',
  [string]$Output = ''
)

$ErrorActionPreference = 'Stop'
$MAX_LEVELS = 15   # the game has 15 levels; the map draws no flag 16+
if ($Count -lt 1 -or $Count -gt $MAX_LEVELS) {
  throw "Count must be 1..$MAX_LEVELS (the game has $MAX_LEVELS levels), got $Count"
}
$REPO = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$SRC = Join-Path $REPO 'SRC'
if (-not $Input) { $Input = Join-Path $SRC 'PILOTS0.230' }
if (-not $Output) { $Output = Join-Path $SRC 'PILOTS0.230' }

# byte_410298 (game_globals_init.h), first 32 bytes - XorCryptData masks the index with 0x1F
$KEY = @(0xA7,0x15,0xF0,0x56,0xF3,0xFA,0x84,0x2C,0xFD,0x81,0x38,0xAC,0x73,0xD2,0x22,0x47,
         0xA0,0x12,0xB8,0x19,0x20,0x6A,0x26,0x7C,0x32,0x57,0xDD,0xB2,0x38,0xA7,0x95,0x7A)
$REGION_OFF = 12      # region globals offset, mod 32
$REGION_LEN = 20
$CUSTOM_AT  = 60      # 8 magic + 0x34 state

if (-not (Test-Path $Input)) { throw "no such file: $Input" }
$blob = [IO.File]::ReadAllBytes($Input)
if ($blob.Length -ne $CUSTOM_AT + $REGION_LEN) {
  throw ("unexpected size {0}, expected {1} - this save layout is not the known one" -f $blob.Length, ($CUSTOM_AT + $REGION_LEN))
}

# decrypt -> patch dword +4 -> re-encrypt in place
$plain = New-Object byte[] $REGION_LEN
for ($i = 0; $i -lt $REGION_LEN; $i++) {
  $plain[$i] = $blob[$CUSTOM_AT + $i] -bxor $KEY[($REGION_OFF + $i + 1) -band 0x1F]
}
$levels = [BitConverter]::ToInt32($plain, 0)
$open   = [BitConverter]::ToInt32($plain, 4)
[BitConverter]::GetBytes([int]($Count - 1)).CopyTo($plain, 4)
for ($i = 0; $i -lt $REGION_LEN; $i++) {
  $blob[$CUSTOM_AT + $i] = $plain[$i] -bxor $KEY[($REGION_OFF + $i + 1) -band 0x1F]
}
[IO.File]::WriteAllBytes($Output, $blob)

"read  : {0}" -f $Input
"before: levels field={0}, open field={1} -> {2} flags" -f $levels, $open, ($open + 1)
"after : open field={0} -> {1} flags" -f ($Count - 1), $Count
"write : {0}" -f $Output
"note  : put the file next to the exe (game CWD) and start the game; the map after the"
"        intro videos shows the flags - tests/LEVEL_MAP.md has their coordinates."
