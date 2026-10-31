# Патчи сторонних зависимостей

Здесь лежат патчи, которые применяются к **исходникам зависимости** (не к нашему коду) при сборке платформы macOS. В наш репозиторий они идут только как `.patch`-файлы — сами пропатченные исходники зависимости у нас не хранятся.

## `sdl2-2.26.5-i386-objc-synthesize.patch`

Патчит Cocoa-бэкенд SDL2 **2.26.5** (`src/video/cocoa/`, 8 файлов) для сборки под **32-бит macOS (i386)**.

### Проблема

На 32-бит macOS clang применяет *fragile* Objective-C ABI и **не автосинтезирует** `@property`: если в `@interface` объявлено свойство, а в `@implementation` нет явного `@synthesize`, то геттер/сеттер **не создаются** вовсе (диагностируется предупреждением `-Wobjc-property-implementation`), и первый же вызов свойства падает в рантайме:

```
-[SDL_VideoData key_layout]: unrecognized selector sent to instance ...
```

На 64-бит macOS то же самое работает (там *non-fragile* ABI, автосинтез есть), поэтому апстрим этот баг не видит — и **никогда не увидит**: SDL официально сдал 32-бит macOS (см. «Статус апстрима» ниже).

### Что делает патч

Классическое «старое» решение — везде, где есть `@property` без синтеза:

1. явный ivar-блок `{ ... }` в `@interface` (с префиксом `_`, ровно те типы, что у свойств);
2. `@synthesize prop = _prop;` в `@implementation`.

Затронутые классы: `SDL_VideoData` (8 свойств), `SDL_WindowData` (11, включая `egl_surface` под `#if SDL_VIDEO_OPENGL_EGL`), `SDL_ShapeData` (3), `SDL_CocoaClosure` (3), `SDLOpenGLContext` (только `openglPixelFormat` — у остальных ivar'ы уже были явные).

Патч чисто механический и безопасен: игра обращается к этим полям только через аксессоры (проверено — ноль обращений вида `->_`).

### Применение

```sh
tar xzf SDL2-2.26.5.tar.gz
cd SDL2-2.26.5
patch -p1 < /путь/к/pilots_re/patches/sdl2-2.26.5-i386-objc-synthesize.patch
```

Проверено: `patch --dry-run -p1` на чистом tarball'е SDL2-2.26.5 проходит без ошибок.

После применения собрать SDL2 под i386:

```sh
cmake -S . -B build-i386 -DCMAKE_OSX_ARCHITECTURES=i386 -DCMAKE_OSX_SYSROOT=/ \
      -DSDL_SHARED=ON -DSDL_STATIC=OFF
cmake --build build-i386 --config Release
```

(`-DCMAKE_OSX_SYSROOT=/` — системные заголовки 10.14 SDK ещё универсальные и содержат i386.)

### Статус апстрима (libsdl-org/SDL)

- **Issue #3106** («SDL 2.0.9 prebuilt binaries doesn't support i386») — закрыта как **wontfix**: Xcode 11 убрал i386, macOS 10.15+ не запускает 32-бит. То есть апстрим осознанно не поддерживает 32-бит macOS, и баг автосинтеза никем не заведён именно поэтому — такие сборки просто никто не делает.
- **Issue #3884** — обсуждение ARC и 32-битности Cocoa-кода (Alex Szpakowski: «SDL for macOS doesn't use ARC because of 32 bit compatibility») — ближайшее по теме, но про ARC, а не про автосинтез свойств.
- Прямого issue/PR про `@property` без `@synthesize` в трекере **нет** (поиск по `synthesize` / `auto-synthesis` / `unrecognized selector` — ноль совпадений).

Вывод: PR в апстрим с этим патчем практически наверняка закроют как out-of-scope («мы не поддерживаем i386 macOS»), поэтому патч живёт здесь и на маке-сборщике, а не в апстриме.

### Как это соотносится с нашим репозиторием

Сама macOS-сборка в `CMakeLists.txt` ничего про этот патч не знает — патч применяется к исходникам SDL2 **до** того, как наш проект находит SDL через pkg-config. То есть это локальная «заплатка» сборочного окружения мака, а не часть проекта; на Linux/Switch/Windows она не нужна.

## Порядок сборки macOS (i386, High Sierra) — для полноты

Рабочая последовательность (все пути — на маке-сборщике):

1. **CMake** — собран из исходников в `/usr/local` (на High Sierra нужен >= 3.20 для `CMAKE_OSX_ARCHITECTURES=i386` + modern clang); лежит в `~/tools/CMake.app/Contents/bin/cmake`.
2. **pkg-config** — pkgconf 3.0.10 собран из исходников, симлинк `pkg-config` создан вручную.
3. **SDL2 2.26.5 i386** — распаковать tarball, применить `sdl2-2.26.5-i386-objc-synthesize.patch` (см. выше), собрать cmake'ом с `-DCMAKE_OSX_ARCHITECTURES=i386 -DCMAKE_OSX_SYSROOT=/`.
4. **ffmpeg 5.1.6 i386** — минимальная конфигурация под наш AVI-пайплайн:
   `--disable-everything --enable-demuxer=avi --enable-decoder=msrle,pcm_u8,pcm_s16le --enable-protocol=file --disable-x86asm --cc='clang -m32'`.
5. **Наш проект**:
   ```sh
   cmake -S . -B build-mac32 -DCMAKE_OSX_ARCHITECTURES=i386 -DCMAKE_OSX_SYSROOT=/ \
         -DCMAKE_PREFIX_PATH=<каталоги SDL2 и ffmpeg>
   cmake --build build-mac32
   ```
6. **Данные игры** — положить рядом с бинарником: `PILOTS.EXE` и `MOVIES/`. Запуск:
   ```sh
   cd build-mac32 && ./PILOTS_RE
   ```

Экзотика, о которой стоит помнить:

- **64-бит macOS невозможен**: движок хранит адреса в 32-бит `int` и требует арену ниже 4 ГБ, а XNU в 64-битном процессе не отдаёт память ниже 4 ГБ (`mmap` с hint игнорирует hint, `MAP_FIXED` → ENOMEM). В i386-процессе hint работает (арена получает `0x10000010`). High Sierra — последняя macOS с поддержкой 32-бит.
- **fragile ABI** — суть этого патча; `-fobjc-runtime=macosx-10.{5,6,7}` НЕ помогает (линковка падает на `_OBJC_CLASS_$_NSObject`), помогает только явный ivar + `@synthesize`.
- `GetModuleFileNameA` в шиме использует `_NSGetExecutablePath()` (коммит `95aff52`) — иначе движок не находит свои данные.
