# assets/switch_icon.bmp (128x128, 8 bpp — ровно тот, что вшит в .nro и рисует
# hbmenu) -> PNG для карточки в Homebrew App Store.
#
# Апскейл x2 методом ближайшего соседа: картинка нарисована по пикселям, и
# обычное сглаживание размазало бы её в кашу. Отдельный файл, потому что
# магазин ждёт PNG (BMP он не берёт), а формат для консоли менять нельзя.
#
#   pwsh scripts/make_store_icon.ps1
param(
    [string]$Src = '',
    [string]$Out = '',
    [int]$Scale = 2
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (-not $Src) { $Src = Join-Path $repo 'assets\switch_icon.bmp' }
if (-not $Out) { $Out = Join-Path $repo 'assets\store_icon.png' }
if (-not (Test-Path $Src)) { throw "нет $Src" }

Add-Type -AssemblyName System.Drawing
# Отдельное имя для картинки: $Src — параметр, а переменные PowerShell не
# различают регистр, и присваивание в $src затёрло бы путь самому себе.
$image = New-Object System.Drawing.Bitmap($Src)
try {
    $width = $image.Width * $Scale
    $height = $image.Height * $Scale
    $canvas = New-Object System.Drawing.Bitmap($width, $height)
    $g = [System.Drawing.Graphics]::FromImage($canvas)
    try {
        $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
        $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
        $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::None
        $g.DrawImage($image, 0, 0, $width, $height)
    } finally {
        $g.Dispose()
    }
    $canvas.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
    $canvas.Dispose()
} finally {
    $image.Dispose()
}
$item = Get-Item $Out
'{0}  {1}x{2}  {3} Б' -f $item.Name, $width, $height, $item.Length
