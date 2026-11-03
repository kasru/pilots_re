# Собирает архивы релиза из уже собранных бинарников. Всё делает 7-Zip.
#
#   pwsh scripts/make_release_archives.ps1 -Dist <каталог с бинарниками> [-Out <куда>]
#
# Архив — это и есть дистрибутив: внутрь кладутся бинарники, необходимые для
# запуска, README.md и LICENSE, больше ничего. Раскладка в -Dist:
#
#   x64/PILOTS_RE_x64.EXE, SDL2.dll     windows_amd64
#   x86/PILOTS_RE.EXE, SDL2.dll         windows_386
#   linux/PILOTS_RE_x64                 linux_amd64
#   linux386/PILOTS_RE                  linux_386
#   mac/PILOTS_RE                       macos_i386
#   switch/PILOTS_RE.nro                nintendo_switch
#
# Форматы: .zip там, где tar не нужен (Windows, Switch — их распаковывают в
# проводнике), .tar.gz на Linux и macOS. Версия берётся из SRC/version.h — того
# же файла, что читают rc.exe (VERSIONINFO) и CMake (NACP, Info.plist), так что
# имя архива не разойдётся с содержимым.
#
# Два решения, ради которых устроено именно так:
#
#   - zip создаётся с -mx=0 (Stored, method 0). Такой архив читается в браузере
#     без inflate: установщик распаковывает его обычным срезом байтов, а
#     дефлейт от .exe/.dll почти не выигрывает — проверено, method = 0.
#   - tar.gz получается через промежуточный .tar (7z не умеет tar.gz одним
#     шагом). Права в нём 0777: 7-Zip на Windows не знает unix-прав файла и
#     пишет все биты, зато бит запуска у бинарника есть всегда. Читатель
#     архива нормализует права по имени файла (см. lib/archive.js), так что
#     README в итоговом пакете не становится исполняемым, а бинарники —
#     0755, а не 0777.

param(
    [Parameter(Mandatory = $true)][string]$Dist,
    [string]$Out = '',
    [string]$SevenZip = 'C:\Program Files (x86)\7-Zip\7z.exe'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent

if (-not (Test-Path $SevenZip)) { throw "нет 7-Zip: $SevenZip" }
if ($Out -eq '') { $Out = "$Dist-archives" }

# Версия — из SRC/version.h, единого источника (см. этот файл).
$verLine = Get-Content (Join-Path $repo 'SRC\version.h') | Where-Object { $_ -match '^#define PILOTS_VERSION\s+"' } | Select-Object -First 1
$version = if ($verLine) { ($verLine -replace '^#define PILOTS_VERSION\s+"([^"]+)".*', '$1') } else { '' }
if (-not $version) { throw 'не удалось прочитать PILOTS_VERSION из SRC/version.h' }

$readme = Join-Path $repo 'README.md'
$license = Join-Path $repo 'LICENSE'
if (-not (Test-Path $readme)) { throw "нет $readme" }
if (-not (Test-Path $license)) { throw "нет $license" }

# Заметка попадает только в 32-битный linux-архив: в отличие от остальных, он
# динамический — SDL2 и ffmpeg в него не вшиты (см. текст ниже и README).
$linux386Note = @'
> [!NOTE]
> Эта сборка **динамическая**: SDL2 и ffmpeg в неё не вшиты, поэтому кроме
> glibc нужны ещё i386-пакеты — `libSDL2-2.0.so.0:i386`, `libavcodec59:i386`,
> `libavformat59:i386`, `libavutil57:i386` (названия пакетов для Debian/Ubuntu;
> на других дистрибутивах — 32-битные версии тех же библиотек). Если ставить
> их не хочется — возьмите `linux_amd64`, он самодостаточный.

'@

$platforms = @(
    @{ name = 'windows_amd64';   from = 'x64';      format = 'zip';   files = @('PILOTS_RE_x64.EXE', 'SDL2.dll') }
    @{ name = 'windows_386';     from = 'x86';      format = 'zip';   files = @('PILOTS_RE.EXE', 'SDL2.dll') }
    @{ name = 'linux_amd64';     from = 'linux';    format = 'targz'; files = @('PILOTS_RE_x64') }
    @{ name = 'linux_386';       from = 'linux386'; format = 'targz'; files = @('PILOTS_RE'); note = $linux386Note }
    @{ name = 'macos_i386';      from = 'mac';      format = 'targz'; files = @('PILOTS_RE') }
    @{ name = 'nintendo_switch'; from = 'switch';   format = 'zip';   files = @('PILOTS_RE.nro') }
)

New-Item -ItemType Directory -Path $Out -Force | Out-Null
$made = @()

foreach ($p in $platforms) {
    $src = Join-Path $Dist $p.from
    if (-not (Test-Path $src)) { throw "нет каталога $src" }

    $stage = Join-Path ([IO.Path]::GetTempPath()) ("pilots-arch-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Path $stage -Force | Out-Null
    try {
        foreach ($f in $p.files) {
            $from = Join-Path $src $f
            if (-not (Test-Path $from)) { throw "нет файла $from" }
            Copy-Item $from (Join-Path $stage $f) -Force
        }
        # README и LICENSE едут в каждый архив: игрок начинает с распаковки,
        # инструкция обязана приехать вместе с бинарниками.
        $readmeText = Get-Content $readme -Raw
        if ($p.note) { $readmeText = $p.note + $readmeText }
        Set-Content -Path (Join-Path $stage 'README.md') -Value $readmeText -Encoding utf8 -NoNewline
        Copy-Item $license (Join-Path $stage 'LICENSE') -Force

        $ext = if ($p.format -eq 'zip') { 'zip' } else { 'tar.gz' }
        $outFile = Join-Path $Out "pilots-re_$($version)_$($p.name).$ext"
        Remove-Item $outFile -Force -ErrorAction SilentlyContinue
        $inner = @($p.files) + @('README.md', 'LICENSE')

        Push-Location $stage
        try {
            if ($p.format -eq 'zip') {
                # -mx=0: Stored, method 0 — иначе браузеру понадобится inflate
                & $SevenZip a -tzip -mx=0 $outFile @inner | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "7z (zip) вернул $LASTEXITCODE" }
            } else {
                $tmpTar = Join-Path $stage 'inner.tar'
                & $SevenZip a -ttar $tmpTar @inner | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "7z (tar) вернул $LASTEXITCODE" }
                & $SevenZip a -tgzip $outFile $tmpTar | Out-Null
                if ($LASTEXITCODE -ne 0) { throw "7z (gzip) вернул $LASTEXITCODE" }
                Remove-Item $tmpTar -Force
            }
        } finally {
            Pop-Location
        }

        $item = Get-Item $outFile
        $hash = (Get-FileHash $outFile -Algorithm SHA256).Hash.ToLower()
        $made += [pscustomobject]@{ Name = $item.Name; Size = $item.Length; Sha256 = $hash }
        "{0,-34} {1,10} Б  {2}" -f $item.Name, $item.Length, $hash
    } finally {
        Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

"`nсобрано $($made.Count) архивов в $Out"
$made | ConvertTo-Json | Set-Content (Join-Path $Out 'archives.json') -Encoding utf8
