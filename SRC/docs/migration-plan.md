# План миграции SDL2-порта на перестроенный main (ветка port-main)

Это отчёт о разведке и карте архитектуры переписанной кодовой базы `main` (коммит `f4daf8e`, ветка `port-main`) — реконструкции проигрывателя Game Maker Player, на которую мигрирует SDL2-порт (ветка `sdl2`). Отчёт лежит в основе плана фаз внизу.

## A. Карта модулей

В `SRC/` плоский набор из 16 трансляционных единиц; каждый `.c` включает ровно один заголовок — `gm.h` (зонт: типы формата, глобальное состояние, прототипы). Окружение — чистый Win32: `<windows.h>`, `<mmsystem.h>`, CRT `_open/_read/_write`.

| Файл | Назначение |
|------|-----------|
| `gm.h` | Типы формата игры, все глобальные переменные (`extern`), все прототипы; `_Static_assert` размеров структур; x86-static-assert на `gm.h:20` |
| `data.c` | Определения глобальных переменных и постоянных таблиц (включая таблицу 57 встроенных функций `g_NativeFunctions`) |
| `host.c` | Платформенный шов: `WinMain` и главный цикл, главное окно и `Host_MainWndProc`, палитра и вывод кадра через DIB-секцию, AVI через MCI, файл данных игры, файлы сохранений, справка WinHelp, запуск внешних программ |
| `game.c` | `GmGame_Initialize`/`GmGame_Tick`/`GmGame_Shutdown`, список грязных прямоугольников и `GmRender_Flush`, вход в комнату и сохранение её состояния |
| `rules.c` | Генератор случайных чисел, преобразования клеток правил (симметрии квадрата), выполнение правила, проход правил за такт |
| `objects.c` | Пул записей, спрайты, экземпляры, фокус, тайлы, ход, `GmInput_Dispatch`, курсор (`GmCursor_*`) |
| `vm.c` | Машина скриптов `GmVm_Run` (байт-код, стек — собственный массив) |
| `natives.c` | Встроенные функции `Native_*` и переходники `Invoke_*` к таблице |
| `path.c` | Карта проходимости `GmWalkMap_*`, волновой поиск пути `GmPath_*` |
| `text.c` | Печать текста глифами шрифта, ввод строки |
| `heap.c` | Собственная куча движка `GmHeap_*` поверх арены |
| `load.c` | Чтение файла данных, теговый поток ресурсов, применение записей `GmLoad_ApplyRecord`, LZSS |
| `save.c` | Автосохранение (`PILOTS<слот>.<ext>`) и сохранения игры (`.SAV`), XOR-шифрование блоков переменных |
| `frame.c` | Кадровый буфер: копирование фона, вывод спрайтов с прозрачностью и отражениями, заливка, вывод в окно |
| `movie.c` | Проигрыватель собственных фильмов (дельта-кадры, RLE, LZSS) и добавочные блиттеры |
| `audio.c` | Каналы звука, микшер 8-разрядного PCM, выбор звукового слоя через таблицу `g_pfn*` |
| `wave.c` | Звуковые слои: waveOut, музыка в собственном MIDI-формате через midiOut, шлюз Win32s GMW.DLL |
| `build.bat`, `build-vs2022.bat` | Сборка MSVC x86 (C17, `/W4 /WX`, glob `*.c`), линкует `kernel32 user32 gdi32 winmm`, результат `out\PILOTS_RE.EXE` |
| `pilots.rc`, `pilots.ico` | Ресурсы: значок окна |

Документация подсистем — `SRC/docs/*.md` (рус.), таблица модулей в `SRC/docs/README.md:22-42`.

## B. Платформенный шов `host.c`: API и места вызовов

Всё, что трогает Win32/GDI/MCI/CRT-файлы, живёт в `host.c` за семейством `Host_*` (прототипы `gm.h:1729-1778`). Это и есть шов для SDL-порта.

**Ошибки.** `Host_MessageBox` (`host.c:41`), `Host_ErrorBox` (`host.c:48`), `Host_ShowPendingError` (`host.c:54`), `Host_SetError` (`host.c:64` — запоминает сообщение в `g_Window.errorMessage`, возвращает `false`). Особым значением `HOST_QUIT_MARK = (const char*)1` (`host.c:17`) помечается «получен `WM_QUIT` во время фильма» (`host.c:1081`, проверка в цикле `host.c:1349`).

**Окно, справка, завершение.** `Host_SetHelpPath` (`host.c:76`), `Host_CloseWindow` (`host.c:84`), `Host_Shutdown` (`host.c:95` — закрывает игру, файл данных, палитру, DIB, арену), `Host_GetCursor` (`host.c:119` — системная стрелка только если `header->systemCursor`), `Host_ShowHelp` (`host.c:127`, вызывается из `Native_ShowHelp` `natives.c:650`), `Host_RunProgram` (`host.c:141`, из `Native_RunProgram` `natives.c:696`).

**Окно и wndproc.** `Host_CreateWindow` (`host.c:805`, вызывается из `Host_InitVideo` `host.c:894`) — регистрирует класс с `Host_MainWndProc` (`host.c:856`), фиксирует клиентскую область размером с экран игры через `WM_GETMINMAXINFO` (`host.c:751-757`). `Host_ShowWindowOrIntro` (`host.c:783`, из `GmGame_Initialize` `game.c:282`) — проверка «экран не меньше окна», затем AVI-заставка или показ окна.

**Видео/палитра/презент.** `Host_InitVideo` (`host.c:888`, из `load.c:326`) — DIB-секция (`DIB_PAL_COLORS`), кадровый буфер = биты DIB (`arena->frameBuffer = g_DibBits`, `host.c:917`), иначе буфер выделит `GmFrame_Init` (`frame.c:33-46`) из арены; арену даёт `GlobalAlloc(GMEM_MOVEABLE)` (`host.c:921-925`) c запасом `HOST_ARENA_RESERVE = 80000` (`host.c:28`). `Host_SetFrameRate` (`host.c:931`; из `game.c:283`, `load.c:437`, `objects.c:830`). `Host_SetPalette` (`host.c:951`; из `host.c:455` (BMP заставки), `game.c:451` (комната), `movie.c:309` (палитра фильма)) — логическая палитра + обмен R/B для цветовой таблицы DIB (`host.c:1003-1012`). `Host_BlitRect` (`host.c:1021`) — `BitBlt` из DIB либо `StretchDIBits`; `Host_Present` (`host.c:1033`, единственная точка вызова — `GmFrame_Present` `frame.c:159`) — прямоугольник или 16-проходный раствор 16×16-плитками с шагом 64 (`host.c:1039-1052`); `Host_Flush` (`host.c:1062`; из `game.c:196`, `game.c:253`, `movie.c:299`).

**Сообщения.** `Host_MainWndProc` (`host.c:665`) — классическая wndproc (см. раздел D). `Host_PumpMessages` (`host.c:1070`) — выборка сообщений с возвратом события такта; используется **только** проигрывателем фильмов (`movie.c:252`, `movie.c:262`, `movie.c:287`). `Host_DrainMessages` (`host.c:503`) — выбросить накопленные ввод-сообщения после AVI.

**AVI через MCI.** `Host_MciCheck` (`host.c:163`), `Host_MciSend` (`host.c:175`), `Host_MciSendNoParams` (`host.c:183`), `Host_AviPlay` (`host.c:197` — `dwCallback = (uint16_t)(uintptr_t)g_MainWindow`, обрезка до слова, как в оригинале), `Host_AviWndProc` (`host.c:240` — wndproc окна на время AVI; `MM_MCINOTIFY` гасит `g_AviPlaying` `host.c:314-316`), `Host_AviStopByMouse` (`host.c:231`), поиск файлов `Host_TakeDriveOfType`/`Host_NextSearchDrive`/`Host_FindAviOnDrives`/`Host_FindAviNextToExe` (`host.c:330-411`), фон `Host_LoadAviBackdrop` (`host.c:417` — BMP читается прямо в кадровый буфер), `Host_AviOpen` (`host.c:469` — `MCI_OPEN` с `WS_CHILD` и родителем `g_MainWindow` `host.c:483-488`, окно AVI добывается через `MCI_DGV_STATUS_HWND` `host.c:492-494`), `Host_PlayAvi` (`host.c:527` — модальный `GetMessage`-цикл до конца `host.c:571-576`; вызывается из `Host_ShowWindowOrIntro` `host.c:792` и `Native_PlayAvi` `natives.c:684`).

**Файл данных.** `Host_OpenDataFile` (`host.c:1098`, из `GmLoad_OpenGameFile` `load.c:280`), `Host_DataTell` (`host.c:1105`), `Host_DataSeek` (`host.c:1111`; `load.c:90`, `movie.c:41`, `movie.c:206`, `movie.c:336`, `movie.c:351`), `Host_DataRead` (`host.c:1117`; `load.c:105`, `load.c:282`, `load.c:289`, `load.c:784`, `load.c:789`, `movie.c:75`, `movie.c:78`, `movie.c:347`, `movie.c:373`), `Host_DataReadSome` (`host.c:1123`), `Host_CloseDataFile` (`host.c:1129`, из `Host_Shutdown` `host.c:99`). Путь: `Host_ResolveGameFilePath` (`host.c:1166`) — если у пересобранного EXE нет хвоста с данными (проверка хвоста `=VS=` в `Host_HasGameData` `host.c:1143`), берётся `PILOTS.EXE` рядом с ним.

**Сохранения.** `Host_BuildSavePath` (`host.c:1184`), `Host_BuildTempSavePath` (`host.c:1195`), `Host_OpenSave` (`host.c:1207`; `save.c:145`, `save.c:162`, `save.c:184`, `save.c:224`, `save.c:314`), `Host_DeleteSave` (`host.c:1223`; `save.c:205`), `Host_SaveRead` (`host.c:1236`; `save.c:21`, `save.c:56`, `save.c:105`, `save.c:263`, `save.c:265`, `save.c:270`, `save.c:283`), `Host_SaveWrite` (`host.c:1242`; `save.c:78`, `save.c:111`, `save.c:112`, `save.c:120`, `save.c:341`, `save.c:348`), `Host_SaveTell` (`host.c:1248`; `save.c:169`), `Host_SaveSeek` (`host.c:1254`; `save.c:150`, `save.c:194`), `Host_SaveTruncate` (`host.c:1260`; `save.c:159`, `save.c:228`), `Host_CloseSave` (`host.c:1266`).

**Точка входа.** `WinMain` (`host.c:1286`): инициализация полей `g_Window`, `GmGame_Initialize` (`host.c:1329`), затем главный цикл `host.c:1338-1380` (см. раздел D), `Host_Shutdown` и возврат `message.wParam` последнего выбранного сообщения (`host.c:1387` — особенность оригинала, не `WM_QUIT`).

## C. Презент и потоки данных

Кадровый буфер — 8-разрядный индексный, «снизу вверх»: строка *y* лежит по смещению `(g_FrameHeight − 1 − y) · g_FrameStride` (`FramePixel` `frame.c:15-18`). Заводится он в двух местах: `Host_InitVideo` подсовывает биты DIB-секции (`host.c:917`), а если DIB не создался — `GmFrame_Init` режет `frameBufferSize` байт с начала арены (`frame.c:38-44`).

Поток кадра:

1. `GmGame_Tick` (`game.c:304`) после такта правил зовёт `GmCursor_Update` и `GmRender_Flush` (`game.c:351`).
2. `GmRender_Flush` (`game.c:47-200`): собирает видимые спрайты пула `g_PoolTop … g_PoolBottom` (признаки `USED|VISIBLE`, без `OBJECT`, `game.c:63-67`), сортирует по слою (больший слой ниже, `game.c:69-80`), последним добавляет курсор-спрайт (`game.c:82-83`); для каждого грязного прямоугольника: клип по `g_ClipWidth/Height` и скроллу, `GmFrame_CopyRect` фона комнаты (`game.c:112`), вывод обрезанных спрайтов `GmFrame_DrawSprite*` (`frame.c:108-139`, прозрачность по байту 0), `GmFrame_Present` прямоугольника (`game.c:188`); в конце `Host_Flush()` (`game.c:196`) и очистка списка.
3. `GmFrame_Present` (`frame.c:157-161`) → `Host_Present(g_FrameBits, …, g_FrameStride, g_FrameHeight, dissolve)` (`host.c:1033`) → раствором (16 проходов) или одним `Host_BlitRect` (`host.c:1055`) → `BitBlt` из DIB / `StretchDIBits` (`host.c:1021-1028`).

Грязные прямоугольники: `GmDirty_Add` (`game.c:359-440`) — слияние пересекающихся, узлы из кучи. Источники rect'ов: `WM_PAINT` добавляет область обновления (`host.c:675-678`), смена палитры — всё окно (`Host_RealizePalette` `host.c:655`), `GmRoom_RepaintAll` (`game.c:446-454` — палитра фона + растворение `g_DissolvePending` + весь вид), спрайты через `GmSprite_Invalidate` (`objects.c:122`), и вручную в `GmRoom_ResetView` (`game.c:240-253`, при `blitRects` — с прелиzentом).

Палитра: `Host_SetPalette` (`host.c:951`) перед сменой заливает окно чёрным или сеткой 8×8 (`g_TransitionGridColors`, стиль перехода), строит `LOGPALETTE`, дублирует цвета в цветовую таблицу DIB с обменом R/B (`host.c:1003-1012`) и шлёт `WM_QUERYNEWPALETTE` (`host.c:1014-1015`).

## D. Ввод

Ввод живёт в `Host_MainWndProc` (`host.c:665-779`) и хранится в полях `g_Window` (`GM_WINDOW`, `gm.h:88-120`): `mouseX/mouseY`, `clickX/clickY`, `event` (`GM_EVENT`), `actionKey` (0..7 + `GMDIR8_SHIFT_HELD = 8`), `lastChar`. Главный цикл раз в такт передаёт их в `GmGame_Tick` (`host.c:1371-1375`) и после такта обнуляет (`host.c:1377-1379`); «пустое» значение `GMEV_NONE = 0xE` пишется во все три поля при старте (`host.c:1322-1325`).

- `WM_KEYDOWN` → `Host_OnKeyDown` (`host.c:614-637`): 8 клавиш направлений из `settings->dirKeys` → `Host_OnDirectionKey` (`host.c:604-611`, Shift через `GetAsyncKeyState(VK_SHIFT)` даёт разряд 3; точка челчка = текущая мышь), 4 особые клавиши из `settings->specialKeys` → события `GMEV_ACTION1/ACTION2/NEXT_FOCUS/HELP` + `lastChar` (код клавиши). Проверка `GMDEV_KEYBOARD` (`host.c:618`).
- `WM_CHAR` → `lastChar` при включённой клавиатуре (`host.c:692-695`).
- `WM_LBUTTONDOWN/WM_RBUTTONDOWN` → `GMEV_MOUSE_DOWN` + точка из lParam (`Host_OnMouseButton` `host.c:640-645`), при `GMDEV_MOUSE` (`host.c:701-706`); заодно сбрасывается `s_swallowButtonUp` (`host.c:703`).
- `WM_LBUTTONUP` → `GMEV_ACTION1` + `lastChar = specialKeys[0]` (`host.c:708-719`); `WM_RBUTTONUP` → `GMEV_ACTION2` + `lastChar = specialKeys[1]` (`host.c:721-732`). Отпускание, «проглоченное» щелчком, остановившим AVI, отбрасывается (`s_swallowButtonUp` `host.c:228`, `host.c:709-713`) — **осознанное отклонение от оригинала** (комментарий `host.c:224-227`).
- `WM_MOUSEMOVE` → `mouseX/mouseY` + `GmCursor_OnMouseMove` (`host.c:742-749`; `objects.c:1090-1102`) — при движке-курсоре мгновенно инвалидирует спрайт курсора и зовёт `GmRender_Flush()` прямо из wndproc.
- `WM_SETCURSOR` → стрелка, если `systemCursor` (`host.c:734-740`, `Host_GetCursor` `host.c:119`), иначе NULL — курсор рисует движок.
- `WM_GETMINMAXINFO`/`WM_SIZE` — размеры клиентской области (`host.c:751-762`).

Раздача по объектам: `GmGame_Tick` → `GmInput_Dispatch` (`game.c:329`; `objects.c:841-951`) — обход пула экземпляров, класс-зависимая раздача `input`-битов (`GMIN_HOVER/PRESSED/RELEASED/KEY`, `objects.c:885-898`), hit-тест `GmObject_HitTest` (`objects.c:490`, вызов `objects.c:900`) с выбором верхнего слоя, запоминание `g_ClickEvent/g_ClickTileX/Y/Class` при `ACTION1/2` (`objects.c:938-944`). Курсор: `GmCursor_Update` (`objects.c:1021-1060`) анимирует спрайт курсора и ставит `g_GameRunning`; `GmCursor_SetGraphic` (`objects.c:1065-1086`) — смена графики курсора (`g_CursorSprite`, `gm.h:1170`; в оригинале — `g_CursorObject`).

Клип «курсор в окне»: главный цикл сам спрашивает положение курсора через `GetCursorPos`/`ScreenToClient` и передаёт `cursorInWindow` в такт (`host.c:1361-1370`).

Во время AVI ввод перехватывается `Host_AviWndProc` (`host.c:240-326`): щелчок (в т.ч. в дочернем окне через `WM_PARENTNOTIFY`, `host.c:264-268`), пробел и Enter (`host.c:275-278`) останавливают MCI, `WM_CLOSE` — останавливает и просит выход (`host.c:318-321`).

## E. Видео

Два независимых канала:

1. **Собственные фильмы движка** (`movie.c`): читаются из файла данных (`GmMovie_Begin` `movie.c:19` → `Host_DataSeek`), куски `GM_MOVIE_CHUNK`, кадры — дельта-прямоугольники, прибавляемые к кадровому буферу (`GmMovie_BlitDelta`, добавочные блиттеры `GmAddBlit_*` `gm.h:1673-1676`), RLE/LZSS. Такт фильма сам крутит цикл сообщений через `Host_PumpMessages` (`movie.c:252`, `movie.c:262`, `movie.c:287`), палитру ставит `Host_SetPalette` (`movie.c:309`), звук/музыку фильма мешает общий микшер. Запуск — `Native_PlayMovie` → `GmMovie_Play`.
2. **AVI через MCI** (`host.c`): `Host_PlayAvi` (`host.c:527`) → `Host_AviOpen` (`host.c:469`, `MCI_ANIM_OPEN_WS|PARENT`, окно `AVIWnd32` получается через `MCI_DGV_STATUS_HWND`) → `Host_AviPlay` (`host.c:197`, `MCI_NOTIFY`, `dwCallback` — **младшее слово** HWND, `host.c:202`) → модальный цикл `GetMessage` до `MM_MCINOTIFY` (`host.c:314-316`, `host.c:571-576`). Фон — BMP рядом с AVI, читается прямо в кадровый буфер (`Host_LoadAviBackdrop` `host.c:417-463`); поиск — каталог EXE, затем корни дисков (fixed→cd→remote→ramdisk, `host.c:346-395`). После фильма — `GmRoom_RepaintAll`, сброс ввода и «подёргивание» курсора (`host.c:587-594`); `WM_CLOSE` во время AVI закрывает и игру (`host.c:583-586`).

Вызовы AVI: заставка (`Host_ShowWindowOrIntro` `host.c:792`, режим `GMINTRO_AVI`) и встроенная функция №55 `Native_PlayAvi` (`natives.c:672-687` — звук останавливается на время, строка `"имя x y ширина высота"` через `sscanf` без ограничения длины, особенность оригинала `natives.c:682`).

Замечание для порта: в отличие от старого `video.c` здесь **нет** SEH-обёртки вокруг `mciSendCommandA` и нет `_WIN64`-фикса `dwCallback`/`WS_CHILD` — переносить соответствующие решения порта на `Host_AviOpen`/`Host_AviPlay`.

## F. Звук и MIDI

Подсистема в два слоя. Верхний — `audio.c`: каналы `g_SoundChannels[8]` (8-разрядный unsigned PCM, тишина 0x80), микшер `GmAudio_Mix` (`audio.c:212-254`) мешает 2048-байтовые куски в свободный буфер `g_WaveBuffers[8][2048]`; насыщение `GmMix_AddSaturated` (`audio.c:179-204`) со штатной для оригинала особенностью «вверх зажимается в 0xF0» (`audio.c:193-195`). Микшер дёргается из `MM_WOM_DONE` в wndproc (`host.c:764-766`) и при запуске звука (`GmAudio_PlaySound` `audio.c:312-343`).

Нижний слой подключается таблицей указателей `g_pfn*` (`gm.h:1205-1216`): `GmAudio_OpenWinMM` (`audio.c:145-168`) ставит `GmWave_*`/`GmMidi_*`, `GmAudio_OpenGmw` (`audio.c:93-132`) — шлюз Win32s в 16-разрядную GMW.DLL (`wave.c:392-477`), после `GmAudio_Shutdown` (`audio.c:389-404`) — заглушки `GmAudioStub_*`. Выбор слоя по `GetVersion()` (`audio.c:266-278`); окно для уведомлений waveOut (`CALLBACK_WINDOW`) кладётся в первое двойное слово wave-буфера (`audio.c:272`).

`wave.c`: waveOut 8-разрядный моно 11025 Гц (`wave.c:61-106`), громкость звуков — программная, к отсчётам перед выводом (`ScaleWaveBuffer` `wave.c:36-42`, комментарий `wave.c:19-31` о том, почему не `waveOutSetVolume`). Музыка — собственный MIDI-подобный формат: `GmMidi_Play` (`wave.c:339-362`) ставит 10-мс `timeSetEvent`, `GmMidi_TimerProc` (`wave.c:266-290`) → `GmMidi_Pump` (`wave.c:195-260`) → `midiOutShortMsg`; громкость музыки — сила ноты (`wave.c:240-241`), повтор дорожки через `g_MidiLoopDelay` тиков.

Для SDL-порта это естественный шов: `GmWave_*`/`GmMidi_*` заменяются на SDL-колбэк/очередь и libADLMIDI, `MM_WOM_DONE` уходит (микшер в колбэке), а верхний слой `audio.c` не меняется.

## G. Логирование

**В новой кодовой базе его нет.** Ни одного `printf`/`OutputDebugString`/`fopen` лог-файла в `SRC/*.c` (grep пуст, кроме `wsprintfA` для путей сохранений `host.c:1191`, `host.c:1202`). Нет и `debug_log.h`/`DBG_*` из старого порта. Маркеры вида `auxMainLoop: startResourceId=1`, `ENTER: GameMainLoop`, `EXCEPTION: 0xC000`, `SdlUpdateLayout`, `SdlLoopProfileTick` и т. п. в этой базе не печатаются — вся диагностика старого порта (включая харнесс-ассерты `tests/windows/*.ps1`) при миграции должна быть восстановлена заново, на новых точках: `GmGame_Tick`/`GmRender_Flush`/`Host_Present`/`GmAudio_Mix`/`Host_MainWndProc`. Фаза P1 плана включает возврат `debug_log.h` и дебаг-маркеров.

## H. Переименования: старое → новое

Старый порт (ветка `sdl2`) и переписанный `main` — это один движок, разложенный по-разному. Соответствия ключевых символов (источники старого порта — `build/sdl_backend_dbg.c`, `SRC/PILOT_RV*_debug.log` в рабочем дереве старой ветки):

| Старый порт (`sdl2`) | Переписанный `main` |
|---|---|
| `ProcessInputAndRender` | `GmGame_Tick` (`game.c:304`) + `GmRender_Flush` (`game.c:47`) |
| `HandleGameEvent` | `Host_MainWndProc` (`host.c:665`) + `GmInput_Dispatch` (`objects.c:841`) |
| `UpdateCursorPosition` | `GmCursor_Update` (`objects.c:1021`) / `GmCursor_OnMouseMove` (`objects.c:1090`) |
| `g_VideoConfig` (одна структура) | разрезан на `g_Window` (`gm.h:88`), `g_GameHeader` (`gm.h:123`), `g_Settings` (`gm.h:356`) и кадровые глобальные `g_FrameBits/g_FrameStride/g_FrameHeight` (`gm.h:1326-1328`) |
| `g_CursorObject` | `g_CursorSprite` (`gm.h:1170`) |
| `AddObjectToDirtyRegions` | `GmSprite_Invalidate` (`objects.c:122`) |
| `CheckObjectHitTest` | `GmObject_HitTest` (`objects.c:490`) |
| `g_RenderObjectsStart` … `g_RenderObjectsEnd` | `g_PoolTop` … `g_PoolBottom` (`gm.h:1387-1388`), обход вниз, «номер записи = g_PoolTop − запись» |
| `SdlBackend*` (окно/презент/ввод) | `Host_*` (`host.c`) |
| `PlayIntroVideo` | `Host_PlayAvi` (`host.c:527`) |
| `OpenMCIVideoDevice` | `Host_AviOpen` (`host.c:469`) |
| `VideoWndProc` | `Host_AviWndProc` (`host.c:240`) |
| `MciSendCommandGuarded` (SEH) | отсутствует — надо добавить в `Host_MciSend` (`host.c:175`) |
| `LZSSDecompress` | `GmLzss_Unpack` (`gm.h:1678`) |
| `LoadDataFile` / чтение ресурсов | `GmLoad_*`/`GmRead_*` (`load.c`) поверх `Host_OpenDataFile/Host_DataRead/Host_DataSeek` |
| `SdlMix_*` (микшер) | `GmAudio_Mix`/`GmMix_AddSaturated` (`audio.c`) |
| `SdlMidi_*` (libADLMIDI) | слой `GmMidi_*` (`wave.c`) — заменить целиком на SDL/libADLMIDI |
| макросы `DBG_*`/`debug_log.h` | отсутствуют (раздел G) |

## I. Объём LP64-миграции

Новая база, как и старая до миграции `INT_PTR`, — строго 32-битная, и это зафиксировано компиляционно:

1. `_Static_assert(sizeof(void *) == 4, …)` в `gm.h:20` — первый барьер для любой x64-сборки.
2. ~47 `_Static_assert(sizeof(...) == N)` в `gm.h` фиксируют layout структур формата файла, включая структуры с указателями внутри: `GM_CLASS` (16, `gm.h:166-176` — `uint8_t *createScript`, `GM_RULE **rules`, `uint8_t *destroyScript`), `GM_SPRITE` (16, `gm.h:315-327` — `GM_FRAME *frame`), `GM_POOL_ENTRY` (16, `gm.h:614-621`), `GM_ARENA` (16, `gm.h:148-155`), `GM_OBJECT` (16, `gm.h:519-531` — `uint8_t *vars`). На LP64 все они не компилируются: 32-разрядный пул/формат предполагает указатели в 4 байта.
3. `GM_RULE_ITEM` (`gm.h:689-696`) — юнит 4 байт, где одно из полей `uint8_t *script`; загрузчик кладёт туда настоящие указатели (`load.c:484-493`).
4. Сохранения пишут указатели как есть: `Host_SaveWrite(room->savedState, …)` (`save.c:341`) пишет `GM_POOL_ENTRY` с полем `GM_FRAME *frame`, перед этим переbase'd в смещения от арены: `(int)(intptr_t)g_Arena.freePtr` — усечение до `int` (`save.c:273`, `save.c:336`), само переbase — `GmSave_RebaseSpriteFrames` (`save.c:127-134`).
5. `vm.c`: адреса в стеке машины — `int32_t`: `OP_RET_CALL`/`OP_RET_FREE` читают возврат как 32-бит (`vm.c:126`, `vm.c:131`), `OP_LEA_GLOBAL/OBJECT/LOCAL` кладут `(int32_t)(intptr_t)` адрес (`vm.c:170-182`), `OP_CALL_SCRIPT`/`OP_CALL_INDIRECT` пушат `(int32_t)(intptr_t)ip` (`vm.c:239-243`), косвенные загрузки/записи — через `(intptr_t)hi` (`vm.c:184-189`, `vm.c:230-235`). Т. е. адреса скриптов ходят как 32-битные значения по всему байт-коду.
6. `natives.c:707`: `#define ARG_PTR(type, value) ((type)(intptr_t)(value))` — 32-битные аргументы стека VM трактуются как указатели во всех `Invoke_*` (`natives.c:722-767`).
7. `GM_FORMAT_BUFFER`/`GM_INLINE_CALL` (`gm.h:740-755`) — инлайновый вызов со смещением `int`, адресуется от `g_GlobalVars`.

Вывод: LP64 — это не только снятие `static_assert`'ов, а согласованная замена «адрес как int32» по всей цепочке VM→natives→load→save; старая ветка уже прошла этот путь через `INT_PTR`-миграцию, и опыт переносится, но в новых именах и структурах. В плане это фаза P2.

## J. Сюрпризы при переносе

1. **Главный цикл — инлайн в `WinMain`** (`host.c:1338-1380`), а не отдельная функция: PeekMessage → `errorMessage`/`HOST_QUIT_MARK` → `IsIconic` → пейсинг `frameDelayMs + g_LastFrameTime` → `GetCursorPos`/`ScreenToClient` → `GmGame_Tick`. SDL-порту придётся раздеть цикл из `WinMain` (как старый порт сделал со своим `main.c`), сохранив пейсинг и «свёрнутое окно = нет тактов».
2. **Два wndproc**: `Host_MainWndProc` делегирует всё в `Host_AviWndProc`, пока `g_AviActive` (`host.c:670-671`) — SDL subclass должен учитывать обе.
3. **Презент крутится вокруг грязных прямоугольников и палитры**: 8-разрядный индексный кадр, `DIB_PAL_COLORS`, логическая палитра 256, смена палитры = перерисовка всего окна. SDL-текстура должна строиться как палитровая (как в старом порте).
4. **AVI-шные «падения оригинала» сохранены**: `dwCallback = (uint16_t)g_MainWindow` (`host.c:202`), не проверяемый результат `Host_LoadAviBackdrop` (`host.c:496-498`), неопределённый возврат `Host_MciSendNoParams` при закрытом устройстве (`host.c:187-191`) — но **нет** ни SEH-защиты `mciSendCommandA`, ни `_WIN64`-фиксации `hWnd`/`dwCallback`, ни `MciChildForwardClicks` — всё это из старого `video.c` придётся переносить поверх `Host_AviOpen`/`Host_AviPlay`/`Host_AviWndProc`.
5. **`s_swallowButtonUp`** (`host.c:228`) — уже готовое (и задокументированное как отклонение) решение проблемы «щелчок по AVI: отпускание не доходит до игры», за которое старый порт боролся через `MciChildForwardClicks`.
6. **Аудио-шов — таблица `g_pfn*`** (`gm.h:1205-1216`): слой `audio.c` трогать не нужно, менять только `GmWave_*`/`GmMidi_*` (`wave.c`). Обратите внимание на сохранённую оригинальную семантику: «вверх зажимается в 0xF0» (`audio.c:193-195`), первый канал копируется, остальные суммируются (`audio.c:228-238`), громкость — программная (`wave.c:19-31`).
7. **Файл данных — хвост собственного EXE**: `Host_ResolveGameFilePath` (`host.c:1166-1176`) уже умеет «пересобранный EXE без данных читает `PILOTS.EXE` рядом» — это аналог старого `.\PILOTS.EXE` CWD-резолва; `-datapath` и case-insensitive `WinShim_ResolvePath` здесь ещё нет (нужны в P3 для Linux).
8. **Сохранения содержат абсолютные указатели кадров**, переbase'd через `(int)` арену (`save.c:273`, `save.c:336`) — формат `.SAV` не зависит от базы, но зависит от 32-битности указателя; LP64 обязан сохранить байт-формат (P2).
9. **Отладочного логирования нет совсем** (раздел G) — харнессам `tests/` не на что опираться, пока P1 не вернёт `debug_log.h`.
10. **Сборка — glob `*.c`** (`build-vs2022.bat:28`): новый файл порта (`sdl_backend.c` и т. п.) подхватится автоматически MSVC-глобом, но `gm.h` тянет `<windows.h>` — SDL-слой придётся要么 вынести в отдельный TU с другим набором заголовков,要么 продолжить включать windows-заголовки (как делает старый `sdl_backend.c` с `SDL_syswm.h`). `gm.h:12` (`WIN32_LEAN_AND_MEAN` + `<windows.h>`) — ещё один LP64/кроссплатформенный барьер: на не-Windows он весь уходит в `win32_shim.h`-аналог.

## План миграции

- **P0** — ветка `port-main` от `main`, инфраструктура (`tests/`, `third_party`, `.gitignore`, `AGENTS.md`), разведка архитектуры и этот план — **выполнено**.
- **P1** — SDL2 на Windows x86: окно/презент/ввод через SDL на основе `host.c`, восстановление `debug_log.h` и дебаг-маркеров, сборка с SDL2, окно 640×480. MCI-видео и winmm-звук остаются как есть. — **выполнено**.
- **P2** — LP64/x64: снятие 32-битных `_Static_assert`, pointer-as-int, x64-сборка.
- **P3** — Linux (CMake, win32_shim/mci_ffmpeg, SDL-аудио).
- **P4** — Switch (`switch_libnx`, пад, low-memory).
- **P5** — фичи порта по одной с харнессами (fullscreen/stretch, hotspots, cursor keepalive, movie sound, libADLMIDI MIDI, `-datapath`, load-индикатор, профиль лупы, coalescing, стик-курсор).
- **P6** — полная матрица тестов зелёная (Windows x86/x64, Linux, Switch).

### P1 (выполнен)

Шов собран в `SRC/sdl_backend.c` (только SDL) и точечных правках `host.c`:

- **Окно**: `Host_CreateWindow` вызывает `SdlBackendCreateWindow` (SDL создаёт окно, `SDL_GetWindowWMInfo` → HWND, subclass через `SetWindowLongPtr(GWLP_WNDPROC)` с сохранением `s_OrigWndProc`). Подкласс `SdlGameWndProc` отдаёт игровой набор сообщений (клавиатура/мышь/палитра/`WM_PAINT`/`WM_SIZE`/`WM_GETMINMAXINFO`/`MM_WOM_DONE`/`MM_MCINOTIFY`/close) в неизменённый `Host_MainWndProc`, остальное — `CallWindowProc(orig)` SDL. `g_MainWindow` = этот HWND, поэтому MCI/waveOut работают как есть. Окно 640×480 (флаг `SDL_WINDOW_RESIZABLE` по `header->resizable`).
- **Презент**: DIB-секция не создаётся (`Host_InitVideo`), кадровый буфер из арены (`GmFrame_Init`). `Host_BlitRect` → `SdlBackendPresentRect` (8-битный bottom-up прямоугольник через палитруный LUT → теневой ARGB, без показа), `Host_Flush` → `SdlBackendFlushPresent` (один `SDL_UpdateTexture`+`RenderCopy`+`RenderPresent`, только если кадр «грязный»). Растворение (dissolve) сводится к одному кадру — принятое отклонение.
- **Палитра**: `Host_SetPalette` → `SdlBackendUpdatePalette` (LUT без R/B-свопа) + `SdlBackendClearBlack`/`SdlBackendDrawGrid` вместо GDI-заливки чёрным/сеткой. GDI-палитра создаётся по-прежнему (нужна AVI-путям), `SetDIBColorTable`-блок пропускается сам (`g_hDibDC` NULL).
- **Ввод**: SDL-события просто дренируются (`SdlBackendPumpEvents` в главном цикле); реальный игровой ввод идёт Win32-сообщениями через подкласс (как в оригинале).
- **Диагностика**: `debug_log.h` перенесён с `sdl2` (добавлены `#include <string.h>` и `#pragma warning(disable:4505)` для `/W4 /WX`), лог-глобалы — в `data.c` под `#ifdef _DEBUG`, `#include "debug_log.h"` — в `gm.h`. Маркеры: окно (размеры), `SetPalette` (count/grid), первый present, `PlayAvi` start/end, `GmGame_Initialize` ok, `click L` (ввод), `ENTER: WinMain`/`GameMainLoop`.
- **Билд**: `build-vs2022.bat` **и** `build.bat` синхронно получили `/external:W0 /external:I "%SDL2%\include"`, линковку `SDL2.lib` и копию `SDL2.dll` в `out\`. Обе конфигурации (debug и release) собираются чисто под `/W4 /WX`.

Проверено запуском (debug и release): окно 640×480, картинка уровня через SDL в полном цвете, курсор рисуется движком при вводе, `click L: …` доходит до `Host_MainWndProc`, клик по кнопке нижней панели перезагружает палитру (переход экрана), ноль `EXCEPTION`. AVI-заставки в тестовом запуске не открывались (нет каталога `MOVIES`) — уходят в ShowWindow-путь, как и ожидалось.

### Текущий статус

P0 и P1 выполнены; SDL2-шов (`sdl_backend.c` + правки `host.c`/`gm.h`/`data.c`/`debug_log.h`) собран и проверен запуском — `SRC\build-vs2022.bat [debug]` → `SUCCESS: out\PILOTS_RE.EXE`, окно 640×480 рендерит картинку, ввод доходит до игры, ноль исключений. Ветка `port-main`.
