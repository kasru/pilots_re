# План миграции SDL2-порта на перестроенный main — план и исполнение

Живой трекер: здесь план фаз и его исполнение. Анализ кодовой базы (секции A–J) —
в соседнем `migration-plan.md`. Ветка `port-main`; референс — ветка `sdl2` с тэгом `v0.1`.

## План фаз

- **P0** — ветка `port-main` от `main`, инфраструктура (`tests/`, `third_party`, `.gitignore`, `AGENTS.md`), разведка архитектуры — **выполнено**.
- **P1** — SDL2 на Windows x86: окно/презент/ввод через SDL на основе `host.c`, восстановление `debug_log.h` и дебаг-маркеров, сборка с SDL2, окно 640×480. MCI-видео и winmm-звук остаются как есть — **выполнено**.
- **P2** — LP64/x64: снятие 32-битных `_Static_assert`, pointer-as-int, x64-сборка.
- **P3** — Linux (CMake, win32_shim/mci_ffmpeg, SDL-аудио).
- **P4** — Switch (`switch_libnx`, пад, low-memory).
- **P5** — фичи порта по одной с харнессами (fullscreen/stretch, hotspots, cursor keepalive, movie sound, libADLMIDI MIDI, `-datapath`, load-индикатор, профиль лупы, coalescing, стик-курсор).
- **P6** — полная матрица тестов зелёная (Windows x86/x64, Linux, Switch).

## Исполнение

### P1 (выполнен)

Шов собран в `SRC/sdl_backend.c` (только SDL) и точечных правках `host.c`:

- **Окно**: `Host_CreateWindow` вызывает `SdlBackendCreateWindow` (SDL создаёт окно, `SDL_GetWindowWMInfo` → HWND, subclass через `SetWindowLongPtr(GWLP_WNDPROC)` с сохранением `s_OrigWndProc`). Подкласс `SdlGameWndProc` отдаёт игровой набор сообщений (клавиатура/мышь/палитра/`WM_PAINT`/`WM_SIZE`/`WM_GETMINMAXINFO`/`MM_WOM_DONE`/`MM_MCINOTIFY`/close) в неизменённый `Host_MainWndProc`, остальное — `CallWindowProc(orig)` SDL. `g_MainWindow` = этот HWND, поэтому MCI/waveOut работают как есть. Окно 640×480 (флаг `SDL_WINDOW_RESIZABLE` по `header->resizable`).
- **Презент**: DIB-секция не создаётся (`Host_InitVideo`), кадровый буфер из арены (`GmFrame_Init`). `Host_BlitRect` → `SdlBackendPresentRect` (8-битный bottom-up прямоугольник через палитруный LUT → теневой ARGB, без показа), `Host_Flush` → `SdlBackendFlushPresent` (один `SDL_UpdateTexture`+`RenderCopy`+`RenderPresent`, только если кадр «грязный»). Растворение (dissolve) сводится к одному кадру — принятое отклонение.
- **Палитра**: `Host_SetPalette` → `SdlBackendUpdatePalette` (LUT без R/B-свопа) + `SdlBackendClearBlack`/`SdlBackendDrawGrid` вместо GDI-заливки чёрным/сеткой. GDI-палитра создаётся по-прежнему (нужна AVI-путям), `SetDIBColorTable`-блок пропускается сам (`g_hDibDC` NULL).
- **Ввод**: SDL-события просто дренируются (`SdlBackendPumpEvents` в главном цикле); реальный игровой ввод идёт Win32-сообщениями через подкласс (как в оригинале).
- **Диагностика**: `debug_log.h` перенесён с `sdl2` (добавлены `#include <string.h>` и `#pragma warning(disable:4505)` для `/W4 /WX`), лог-глобалы — в `data.c` под `#ifdef _DEBUG`, `#include "debug_log.h"` — в `gm.h`. Маркеры: окно (размеры), `SetPalette` (count/grid), первый present, `PlayAvi` start/end, `GmGame_Initialize` ok, `click L` (ввод), `ENTER: WinMain`/`GameMainLoop`.
- **Билд**: `build-vs2022.bat` **и** `build.bat` синхронно получили `/external:W0 /external:I "%SDL2%\include"`, линковку `SDL2.lib` и копию `SDL2.dll` в `out\`. Обе конфигурации (debug и release) собираются чисто под `/W4 /WX`.

Проверено запуском (debug и release): окно 640×480, картинка уровня через SDL в полном цвете, курсор рисуется движком при вводе, `click L: …` доходит до `Host_MainWndProc`, клик по кнопке нижней панели перезагружает палитру (переход экрана), ноль `EXCEPTION`. AVI-заставки в тестовом запуске не открывались (нет каталога `MOVIES`) — уходят в ShowWindow-путь, как и ожидалось.

Замечание: главный цикл считает `cursorInWindow` по реальному положению курсора (`GetCursorPos`+`ScreenToClient`), поэтому опубликованный (`PostMessage`) клик обрабатывается, только если реальный курсор зажат над окном — это логика игры, а не шва.

## Текущий статус

P0 и P1 выполнены; SDL2-шов (`sdl_backend.c` + правки `host.c`/`gm.h`/`data.c`/`debug_log.h`) собран и проверен запуском — `SRC\build-vs2022.bat [debug]` → `SUCCESS: out\PILOTS_RE.EXE`, окно 640×480 рендерит картинку, ввод доходит до игры, ноль исключений. Ветка `port-main`. Дальше — P2 (LP64/x64). Заблокировано: деплой debug-NRO на консоль (спит); пуш веток/тэга `v0.1` (интерактивная аутентификация GitHub).
