/*
 * SDL2-шов: окно, презент кадра, ввод и палитра через SDL.
 * Имена Host_* сохранены в host.c; здесь — минимальный слой над SDL, который
 * эти тела вызывают. Окно создаёт SDL; wndproc подклассируется так, что
 * игровой набор сообщений уходит в Host_MainWndProc (как в оригинале), а
 * всё остальное отдаётся оригинальному wndproc SDL. MCI-видео и winmm-звук
 * остаются Win32 и ходят обычными сообщениями через g_MainWindow.
 *
 * Презент: 8-битный индексный кадр (строки снизу вверх, как DIB) конвертируется
 * через палитруный LUT в теневой ARGB-буфер, затем SDL_UpdateTexture +
 * RenderCopy + RenderPresent. Показ — только при «грязном» кадре, поэтому
 * Host_Flush после уже показанного кадра становится no-op.
 */
#include "gm.h"

/* WinMain наш, SDL_main не нужен: объявляем управление главной функцией сами. */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>

#ifndef _WIN32
#include <signal.h>    /* SIGUSR1: headless-прогон завершается чисто (scripts/diag_hang.sh) */
#include "adlmidi.h"   /* OPL3-синтез за midiOut-шимом: музыка игры (SdlMidi_*) */
#endif

/* ========================================================================== */
/* Состояние                                                                  */
/* ========================================================================== */

static SDL_Window *s_Window;
static SDL_Renderer *s_Renderer;
static SDL_Texture *s_Texture;      /* стриминговая, ARGB8888, texW×texH */
static uint32_t *s_Shadow;          /* теневой кадр ARGB, texW×texH */
static uint32_t s_PaletteLut[256];  /* индекс палитры → ARGB */
static int s_TexW, s_TexH;
static bool s_Dirty;                /* теневой кадр ждёт показа */
static bool s_PresentedOnce;        /* первый показ уже случился */
static WNDPROC s_OrigWndProc;       /* wndproc SDL до подкласса */

/* ---- безрамочный fullscreen (фича порта: -fullscreen / Alt+Enter) ----
   В обычном (оконном) режиме, как в оригинале, картинка И есть окно: каждое
   преобразование ниже — тождественное, игра ничего не замечает. В fullscreen
   окно покрывает экран, и игровая картинка масштабируется под него с
   сохранением пропорций и центрируется: s_PicX/Y/W/H — где она лежит в
   клиентских px окна, а мышиные координаты проходят через этот rect, поэтому
   игра продолжает жить в своём пространстве 640×480 — поле вокруг картинки
   ведёт себя ровно как «мышь ушла из окна» в оригинале (сообщение не шлётся).
   Режим native-фильма: на Windows кино играет в настоящем дочернем окне
   mciavi32, которое рисует 1:1 и не умеет масштабироваться, поэтому на время
   фильма картинка держит натуральный размер и только центрируется — фон и
   ребёнок остаются выровненными. */
static int s_Fullscreen;            /* запрошен SDL_WINDOW_FULLSCREEN_DESKTOP */
static int s_NativeMovie;           /* держать картинку натурального размера (MCI-ребёнок Windows) */
static int s_StretchPic;            /* '-' (фича порта): во весь экран, пропорции не сохраняются */
static int s_DataPathGiven;         /* -datapath (или Switch-фоллбэк) сменил CWD: путь игры
                                       строится от текущего каталога, как в оригинале */
static int s_PicX;                  /* начало картинки, клиентские px окна */
static int s_PicY;
static int s_PicW;                  /* размер картинки, клиентские px окна */
static int s_PicH;
static int s_GameClientW;           /* закреплённый логический клиент игры: */
static int s_GameClientH;           /*   g_Window.clientWidth/Height обнуляются собственным WM_SIZE
                                       игры при сворачивании (SIZE_MINIMIZED -> 0,0) и не могут
                                       служить источником для восстановления */

static void SdlUpdateLayout(void);  /* куда ложится картинка */
static void SdlApplyFullscreen(void); /* SDL-сторона s_Fullscreen (no-op до окна) */
static int SdlPictureTransformed(void);
static int SdlMapMousePoint(int *x, int *y); /* px окна -> px игры; 0 = вне картинки */
static int SdlFloorDiv(int a, int b);

/* ---- Оверлей кликабельного (фича порта): Ctrl зажат — отметить объекты, в
   которые попал бы hit-test клика. Клавиша читается через GetAsyncKeyState:
   на Windows это настоящий user32 (там wndproc отдаёт игре WM_KEYDOWN и SDL
   своего состояния не видит), на остальных платформах — шим на SDL_GetModState
   (win32_shim.c); polling опубликованного сообщения никогда бы не совпал. */
#define HOTSPOT_MAX 128
static int s_HotspotHeld;             /* Ctrl зажат на последнем пампе */
static int s_HotspotPrev;             /* ... и на пампе перед ним */
static int s_HotspotCount;            /* маркеры к следующей отрисовке */
static int s_HotspotX[HOTSPOT_MAX];   /* центр объекта, px игры (экран) */
static int s_HotspotY[HOTSPOT_MAX];
/* R на консоли — второй источник «зажато», та же картинка, что даёт Ctrl.
   Вне __SWITCH__ его никто не выставляет; один байт, читается безусловно. */
static int s_PadOverlayHeld;

static void SdlCollectHotspots(void); /* наполнить s_HotspotX/Y из пула объектов */
static void SdlDrawHotspots(void);    /* белые маркеры поверх показанного кадра */
static void SdlUpdateHotspots(void);  /* сэмпл Ctrl раз в памп */

/* Точка -> LPARAM мышиного сообщения (x в младшем слове, y в старшем). Общий
   путь: wndproc на Windows и очередь шима на остальных платформах. */
static LPARAM SdlPackPoint(int x, int y)
{
    return (LPARAM)(((DWORD)(Uint16)y << 16) | (Uint16)x);
}

#ifndef _WIN32
static SDL_Texture *s_VideoTex;     /* кадр фильма (SdlBackendVideoFrame) */
static int s_VideoTexW, s_VideoTexH;
static SDL_AudioDeviceID s_WaveDev; /* SDL-устройство за waveOut-шимом (win32_shim.c) */
static SDL_AudioDeviceID s_MidiDev; /* ...а за midiOut-шимом: своё, только при PILOTS_SHARE_AUDIO=0;
                                      в дележе синтез пишет в своё кольцо s_MidiRing */

/* Общий микшер (фича порта): одно SDL-устройство на все источники — волну игры,
   MIDI и звук фильма. Что это не теория, показано на Switch: у консоли один
   вывод на процесс, второй OpenAudioDevice отклоняется («Audio device already
   open»), и до динамика доходит только источник, открывшийший первым. Отсюда и
   «movie asked first» в логе: интро запрашивает звук раньше любых эффектов.
   Форматы — синтезовые (44100/S16/стерео), конвертация игровой волны ложится
   на продюсера (SdlWaveAppendShared), а колбэк только суммирует кольца. */
#define SDL_MIX_RATE   44100                    /* частота микшера: музыка без ресемплинга */
#define SDL_MIX_RATIO  4                        /* SDL_MIX_RATE / игровые 11025 */
#define SDL_MIX_SAMPLES 1024
#define SDL_MIX_RING   32768u                   /* кадров на кольцо, степень двойки: 0.74 с */
#define SDL_MIX_MASK   (SDL_MIX_RING - 1)

typedef struct
{
    short *pcm;                 /* стерео в перемежении, SDL_MIX_RING кадров */
    volatile unsigned int w;    /* монотонные счётчики кадров: читается только их
                                   разность, так что переполнение 2^32 безвредно */
    volatile unsigned int r;
} MixRing;

static int s_SharedAudio;             /* 1 = одно устройство, кольцо на источник */
static SDL_AudioDeviceID s_MixDev;    /* это устройство, когда источники делят его */
static MixRing s_WaveRing;
static MixRing s_MidiRing;            /* под будущий MIDI: создаётся сразу, пока пустует */
static MixRing s_MovieRing;           /* PCM фильма, см. SdlMix_MovieOpen */
static volatile unsigned int s_MovieRate;   /* частота фильма, 0 = кино без звука */
static unsigned int s_MovieSrcFrames;       /* сэмплов источника сдано с начала фильма */
static volatile unsigned int s_WaveFramesQueued;  /* оба монотонны: разность / RATIO — */
static volatile unsigned int s_WaveFramesEaten;   /* длина очереди в игровых байтах */
static volatile int s_WaveHeld;       /* волна удержана в микшере, а не на устройстве */

static volatile sig_atomic_t s_Usr1Quit;
#ifdef _DEBUG
static int s_FingerLogged;          /* первые касания — в лог, для отладки */
#endif
#ifdef __SWITCH__
/* Курсор со стика консоли: клиентские px игры, -1 до первого опроса (тогда
   паркуется по центру картинки). Ход мышью от стика троттлится — см.
   комментарий у SdlPollSwitchControls. */
static int s_StickX = -1;
static int s_StickY;
static unsigned int s_LastMovePost; /* мс последнего отправленного хода */
static int s_MovePending;           /* стик ушёл после последнего отправленного */
static int s_RLedgesLogged;         /* L без привязки: репорт о ребре ограничен */
#endif
#endif

/* ========================================================================== */
/* Оконная процедура: игровой набор сообщений → Host_MainWndProc, остальное → SDL */
/*                                                                            */
/* Только Windows: подкласс реального окна — это единственный путь, которым    */
/* игра получает сообщения на той платформе. На остальных сообщения падают в  */
/* очередь шима (SdlDispatchSdlEvent делает это из SDL-событий).               */
/* ========================================================================== */
#ifdef _WIN32

/* Сообщения, которые движок обрабатывает сам (Host_MainWndProc / Host_AviWndProc).
   MM_WOM_DONE и MM_MCINOTIFY — это уведомления winmm/MCI, обязательны. */
static bool SdlIsGameMessage(UINT msg)
{
    switch (msg)
    {
    case WM_PAINT:
    case WM_CLOSE:
    case WM_DESTROY:
    case WM_SETCURSOR:
    case WM_SIZE:
    case WM_GETMINMAXINFO:
    case WM_PARENTNOTIFY:
    case WM_CHAR:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_PALETTECHANGED:
    case WM_QUERYNEWPALETTE:
    case MM_WOM_DONE:      /* 0x3BB */
    case MM_MCINOTIFY:     /* 0x3B9 */
        return true;
    default:
        return false;
    }
}

static LRESULT CALLBACK SdlGameWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    /* WM_PAINT: игра валидирует область и ставит грязный прямоугольник, но под
       SDL она ничего не рисует в окно — поэтому после неё сами показываем
       текущий теневой кадр (закрашивает и repaint, и repaint за AVI). */
    if (msg == WM_PAINT)
    {
        Host_MainWndProc(hWnd, msg, wParam, lParam);
        SdlBackendFlushPresent();
        return 0;
    }

    /* '-' (фича порта): картинка во весь экран вместо сохранения пропорций.
       Проглатывается: у игры нет привязки к этой клавише, переключение — дело
       порта. Alt+Enter обрабатывается в WM_SYSKEYDOWN ниже, так же. */
    if (msg == WM_KEYDOWN && (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT))
    {
        SdlBackendToggleStretch();
        return 0;
    }

    /* Alt+Enter: переключатель безрамочного fullscreen (фича порта). */
    if (msg == WM_SYSKEYDOWN && wParam == VK_RETURN)
    {
        SdlBackendToggleFullscreen();
        return 0;                              /* проглочено: игра не должна увидеть Enter */
    }
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSCHAR)
    {
        if (msg == WM_SYSCHAR && wParam == 13)
            return 0;                          /* TranslateMessage достраивает это из Alt+Enter */
        return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
    }

    /* Мышиные сообщения: px окна -> px игры (тождественность в обычном окне).
       Точка вне картинки — как будто мышь ушла из окна: сообщение не шлётся. */
    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP
        || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP)
    {
        int cx = (int)(short)LOWORD(lParam);
        int cy = (int)(short)HIWORD(lParam);
        int mx = cx;
        int my = cy;
        if (!SdlMapMousePoint(&mx, &my))
            return 0;                           /* поле вокруг картинки: мышь «ушла» */
#ifdef _DEBUG
        if ((msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) && (mx != cx || my != cy))
            DBG_LOG("SdlGameWndProc: 0x%X client (%d,%d) -> game (%d,%d)",
                    msg, cx, cy, mx, my);       /* интересно, только пока картинка трансформирована */
#endif
        return Host_MainWndProc(hWnd, msg, wParam, SdlPackPoint(mx, my));
    }

    /* Размер и мин/макс рамки: сначала игра (обновляет clientWidth/Height,
       ограничивает рамку), затем SDL, чтобы его bookkeeping размера совпал.
       В fullscreen игровой размер пинить нечем: оконный размер — экранной, а
       клиентские поля игры обязаны остаться логическими 640×480 (иначе они
       станут clip-границей за фиксированным backbuffer'ом). */
    if (msg == WM_GETMINMAXINFO && s_Fullscreen)
        return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);

    if (msg == WM_SIZE)
    {
        if (s_Fullscreen)
            Host_MainWndProc(hWnd, msg, wParam, SdlPackPoint(s_GameClientW, s_GameClientH));
        else
            Host_MainWndProc(hWnd, msg, wParam, lParam);
        SdlUpdateLayout();                      /* окно теперь fullscreen-размера */
        return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
    }

    if (msg == WM_GETMINMAXINFO)
    {
        Host_MainWndProc(hWnd, msg, wParam, lParam);
        return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
    }

    if (msg == WM_WINDOWPOSCHANGED)
    {
        /* Игра первой — это её repaint-путь, WM_SIZE/WM_MOVE переотправляются
           из него же. Затем SDL: для него это ЕДИНСТВЕННОЕ сообщение, из
           которого SDL 2.30 узнаёт новый размер окна (его обработчик WM_SIZE
           шлёт только RESTORED/MINIMIZED/MAXIMIZED, а resize'бэкфера
           IDXGISwapChain_ResizeBuffers рождается именно из WM_WINDOWPOSCHANGED). */
        LRESULT gameResult = Host_MainWndProc(hWnd, msg, wParam, lParam);
        CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
        return gameResult;
    }

    if (msg == WM_CHAR && (unsigned int)wParam > 255)
    {
        /* Окна SDL — Unicode; оригинальное ANSI-окно отдавало байты cp1251 в wParam. */
        WCHAR wch = (WCHAR)wParam;
        char ansi = '?';
        WideCharToMultiByte(CP_ACP, 0, &wch, 1, &ansi, 1, 0, 0);
        wParam = (WPARAM)(unsigned char)ansi;
    }

    if (SdlIsGameMessage(msg))
        return Host_MainWndProc(hWnd, msg, wParam, lParam);

    /* Всё прочее отдаём SDL: тот внутри вызовет DefWindowProc, который
       пришлёт WM_SIZE назад в этот же подкласс. */
    return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
}

#endif /* _WIN32 */

/* ========================================================================== */
/* Жизненный цикл SDL и окно                                                  */
/* ========================================================================== */

#ifndef _WIN32
/* SIGUSR1: headless-прогон (контейнер без WM) завершается через обычный
   выход из главного цикла — обработчик лишь поднимает флаг, а pump превращает
   его в PostQuitMessage. */
static void SdlSigUsr1Handler(int sig)
{
    (void)sig;
    s_Usr1Quit = 1;
}
#endif

/* Инициализировать SDL (видео; на не-Windows ещё и аудио — за waveOut-шимом).
   Истина — успех. */
int SdlBackendStartup(void)
{
#ifdef __SWITCH__
    /* Самое первое: живой лог (stderr -> nxlink-хост, если запущено через
       netloader) и отчёт о памяти процесса — читать их можно с любой точки
       дальше, а упасть можно в любой. */
    SdlSwitchAttachDebugChannel();
    SdlSwitchReportEnvironment();
#endif
    SDL_SetMainReady();             /* главная функция наша, не SDL_main */
#ifndef _WIN32
    s_Usr1Quit = 0;
    signal(SIGUSR1, SdlSigUsr1Handler);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0)
#else
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
#endif
    {
        DBG_ERR("SDL_Init failed: %s", SDL_GetError());
#ifndef _WIN32
        /* Без звука игра всё равно идёт — пробуем только видео. */
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0)
        {
            DBG_ERR("SDL_InitSubSystem(VIDEO) failed: %s", SDL_GetError());
            return 0;
        }
#else
        return 0;
#endif
    }
#ifdef __SWITCH__
    /* Касания конвертируются в мышиные сообщения сами, в SdlDispatchSdlEvent —
       там палец проходит через те же координатные пути, что и настоящая мышь.
       Синтез SDL выключаем, иначе один тап дошёл бы до игры дважды. */
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    DBG_LOG("SDL video driver: %s", SDL_GetCurrentVideoDriver());
#ifndef _WIN32
    DBG_LOG("SDL audio driver: %s", SDL_GetCurrentAudioDriver());
#endif
    return 1;
}

/* Уничтожить рендерер, окно и завершить SDL. */
void SdlBackendShutdown(void)
{
#ifndef _WIN32
    /* Сначала музыка: SdlMidi_Close гасит синтез и его устройство, если оно
       своё (в дележе кольцо сбрасывается, общее устройство закроется ниже).
       Обычно она уже отработана через midiOutClose (ShutdownMultimedia) —
       вызов идемпотентен. Дальше волна: SDL_CloseAudioDevice дожидается
       коллбэка, дальше можно гасить видео. */
    SdlMidi_Close();
    if (s_WaveDev)
    {
        SDL_CloseAudioDevice(s_WaveDev);
        s_WaveDev = 0;
    }
    if (s_MixDev)
    {
        /* Общее устройство уходит после своего: закрытие устройства — это и
           есть остановка колбэка, который читает кольца, так что снимать их
           можно только потом. */
        SDL_CloseAudioDevice(s_MixDev);
        s_MixDev = 0;
        s_SharedAudio = 0;
        free(s_WaveRing.pcm);
        s_WaveRing.pcm = NULL;
        free(s_MidiRing.pcm);
        s_MidiRing.pcm = NULL;
        free(s_MovieRing.pcm);
        s_MovieRing.pcm = NULL;
    }
    if (s_VideoTex)
    {
        SDL_DestroyTexture(s_VideoTex);
        s_VideoTex = NULL;
        s_VideoTexW = s_VideoTexH = 0;
    }
#endif
    if (s_Shadow)
    {
        SDL_free(s_Shadow);
        s_Shadow = NULL;
    }
    if (s_Texture)
    {
        SDL_DestroyTexture(s_Texture);
        s_Texture = NULL;
    }
    if (s_Renderer)
    {
        SDL_DestroyRenderer(s_Renderer);
        s_Renderer = NULL;
    }
    if (s_Window)
    {
        SDL_DestroyWindow(s_Window);
        s_Window = NULL;
    }
    SDL_Quit();
#ifdef __SWITCH__
    /* Самым последним: алиас низкого пула держит страницы, которые hbloader
       забирает себе, когда приложение возвращает управление — незакрытый алиас
       роняет сам лоадер, и его отчёт без единого смещения PILOT_RV читается
       как «игра упала на выходе». См. SdlSwitchUnmapLowMemory. */
    SdlSwitchUnmapLowMemory();
#endif
}

/* Создать окно клиентского размера clientW×clientH с заголовком title, подклассить
   его wndproc и создать рендерер/текстуру/теневой кадр. Возвращает HWND окна. */
HWND SdlBackendCreateWindow(int clientW, int clientH, const char *title, int resizable)
{
#ifdef _WIN32
    SDL_SysWMinfo wmi;
#endif
    HWND hwnd;
    /* Окно создаётся скрытым: игра сама показывает его через ShowWindow
       (Host_ShowWindowOrIntro / Host_PlayAvi), и этот переход hidden→visible как
       в оригинале порождает первый WM_PAINT, которым мчитается окно AVI-ребёнка. */
    Uint32 flags = SDL_WINDOW_HIDDEN;

    if (resizable)
        flags |= SDL_WINDOW_RESIZABLE;

    s_Window = SDL_CreateWindow(title ? title : "", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, clientW, clientH, flags);
    if (!s_Window)
    {
        DBG_ERR("SDL_CreateWindow failed: %s", SDL_GetError());
        return NULL;
    }

#ifndef _WIN32
    /* HWND для игры — просто жетон: сообщения до неё доходят через очередь шима
       (PostMessageA + DispatchMessageA -> Host_MainWndProc), подкласс не нужен, а
       сам жетон обязан пережить усечение до слова, которое host.c кладёт в
       MCI_PLAY_PARMS.dwCallback (там его узнаёт MciResolveCallbackHwnd по низкому
       слову WinShimMainWindow()). X11-идентификатор для этого не годится. */
    hwnd = WinShimMainWindow();
#else
    SDL_VERSION(&wmi.version);
    if (!SDL_GetWindowWMInfo(s_Window, &wmi))
    {
        DBG_ERR("SDL_GetWindowWMInfo failed: %s", SDL_GetError());
        return NULL;
    }
    hwnd = (HWND)wmi.info.win.window;

    /* Подкласс: сохраняем wndproc SDL, свои сообщения перехватываем. */
    s_OrigWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)SdlGameWndProc);
#endif

    s_Renderer = SDL_CreateRenderer(s_Window, -1, SDL_RENDERER_ACCELERATED);
    if (!s_Renderer)
        s_Renderer = SDL_CreateRenderer(s_Window, -1, 0);
    if (!s_Renderer)
    {
        DBG_ERR("SDL_CreateRenderer failed: %s", SDL_GetError());
        return hwnd;
    }

    s_TexW = clientW;
    s_TexH = clientH;
    s_Texture = SDL_CreateTexture(s_Renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, s_TexW, s_TexH);
    if (s_Texture)
    {
        s_Shadow = (uint32_t *)SDL_malloc((size_t)s_TexW * (size_t)s_TexH * sizeof(uint32_t));
        if (s_Shadow)
            SDL_memset(s_Shadow, 0, (size_t)s_TexW * (size_t)s_TexH * sizeof(uint32_t));
    }

    DBG_LOG("SdlBackendCreateWindow: client=%dx%d resizable=%d hwnd=%p",
            clientW, clientH, resizable, (void *)hwnd);

    /* Закрепить логический клиент игры (см. комментарий у s_GameClientW) и,
       если -fullscreen уже разобран, применить его теперь — окно есть. */
    s_GameClientW = clientW;
    s_GameClientH = clientH;
    SdlUpdateLayout();                           /* обычное окно: записать тождественный rect */
    SdlApplyFullscreen();                        /* no-op без -fullscreen */
    return hwnd;
}

/* ========================================================================== */
/* Палитра и переходы                                                         */
/* ========================================================================== */

/* Перестроить LUT из count троек RGB (порядок R,G,B). Остальные индесы — чёрные. */
void SdlBackendUpdatePalette(const uint8_t *rgb, int count)
{
    int i;

    if (count > 256)
        count = 256;
    for (i = 0; i < count; i++)
    {
        s_PaletteLut[i] = 0xFF000000u
            | ((uint32_t)rgb[3 * i + 0] << 16)
            | ((uint32_t)rgb[3 * i + 1] << 8)
            | (uint32_t)rgb[3 * i + 2];
    }
    for (; i < 256; i++)
        s_PaletteLut[i] = 0xFF000000u;
    s_Dirty = true;                 /* цвета изменились — надо перепоказать */
}

/* Залить весь кадр чёрным и показать (переход без сетки). */
void SdlBackendClearBlack(void)
{
    if (!s_Shadow)
        return;
    SDL_memset(s_Shadow, 0, (size_t)s_TexW * (size_t)s_TexH * sizeof(uint32_t));
    SdlBackendFlushPresent();
}

/* Расчерчить сетку 8×8 цвета colorref поверх текущего кадра и показать
   (переход со сеткой). Повторяет GDI-цикл Host_SetPalette. */
void SdlBackendDrawGrid(int colorref)
{
    uint32_t color;
    int phase, x, y;

    if (!s_Shadow)
        return;
    color = 0xFF000000u
        | ((uint32_t)GetRValue(colorref) << 16)
        | ((uint32_t)GetGValue(colorref) << 8)
        | (uint32_t)GetBValue(colorref);
    for (phase = 0; phase < 8; phase++)
    {
        for (x = phase; x < s_TexW; x += 8)
            for (y = 0; y < s_TexH; y++)
                s_Shadow[(size_t)y * s_TexW + x] = color;
        for (y = phase; y < s_TexH; y += 8)
            for (x = 0; x < s_TexW; x++)
                s_Shadow[(size_t)y * s_TexW + x] = color;
    }
    SdlBackendFlushPresent();
}

/* ========================================================================== */
/* Презент кадра                                                              */
/* ========================================================================== */

/* Положить прямоугольник (x,y,w,h) индексного кадра bits (строки снизу вверх,
   высота bmpHeight, шаг stride) в теневой ARGB-кадр через LUT и пометить кадр
   грязным. Никакого показа здесь нет — его делает SdlBackendFlushPresent. */
void SdlBackendPresentRect(const void *bits, int bmpHeight, int stride, int x, int y, int w, int h)
{
    const uint8_t *src = (const uint8_t *)bits;
    int row;

    if (!s_Shadow || !bits || stride <= 0 || bmpHeight <= 0)
        return;
    /* Обрезка к границам кадра (как BitBlt к DC окна). */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s_TexW) w = s_TexW - x;
    if (y + h > s_TexH) h = s_TexH - y;
    if (w <= 0 || h <= 0)
        return;

    for (row = 0; row < h; row++)
    {
        /* Игровая строка (y+row) лежит в буфере снизу вверх: r = bmpHeight-1-(y+row). */
        int srcRow = bmpHeight - 1 - (y + row);
        const uint8_t *s = src + (size_t)srcRow * stride + x;
        uint32_t *d = s_Shadow + (size_t)(y + row) * s_TexW + x;
        int col;
        for (col = 0; col < w; col++)
            d[col] = s_PaletteLut[s[col]];
    }
    s_Dirty = true;
}

/* Кликабельное, как его видит сама игра: пул объектов тем же обходом, что
   GmObject_ProcessInput у hitObject (objects.c) — instance-объекты, чей класс
   вообще реагирует на указатель (POINTABLE — hover/клик, FOCUSABLE — фокус;
   v0.1 читал те же два из канала спрайта). Положение — центр спрайта в px
   карты минус скролл, т.е. на экране. Только чтение и без побочных эффектов:
   в отличие от настоящего прохода сюда ничего не пишется, игра не замечает,
   что мы тут были. */
static void SdlCollectHotspots(void)
{
    GM_POOL_ENTRY *entry;

    s_HotspotCount = 0;
    if (!g_PoolTop || !g_PoolBottom)
        return;                                    /* ещё ничего не загружено */
    for (entry = g_PoolTop; entry > g_PoolBottom; --entry)
    {
        GM_OBJECT *obj = &entry->object;
        const GM_CLASS *cls;
        const GM_SPRITE *sprite;
        const GM_IMAGE *image;
        int x, y;

        if ((obj->flags & (GMOBJ_USED | GMOBJ_INSTANCE)) != (GMOBJ_USED | GMOBJ_INSTANCE))
            continue;
        cls = &g_Classes[obj->classIndex];
        if (cls->kind != GMCLASS_POINTABLE && cls->kind != GMCLASS_FOCUSABLE)
            continue;
        if (obj->spriteIndex == GM_POOL_NONE)
            continue;                              /* объект без спрайта: не кликабелен */
        sprite = &g_PoolTop[-obj->spriteIndex].sprite;
        if (!sprite->frame)
            continue;
        /* Тот же слот-кадр, что читает GmObject_HitTest (objects.c) — оверлей
           обязан совпадать с hit-test'ом, который отражает, а не спорить с ним. */
        image = GM_FRAME_IMAGE(sprite->frame);
        if (!image->width || !image->height)
            continue;
        x = sprite->x + image->width / 2 - g_ScrollX;
        y = sprite->y + image->height / 2 - g_ScrollY;
        if (x < 0 || y < 0 || x >= s_GameClientW || y >= s_GameClientH)
            continue;                              /* центр за кадром: нечего помечать */
        if (s_HotspotCount >= HOTSPOT_MAX)
            break;
        s_HotspotX[s_HotspotCount] = x;            /* центр попадает внутрь того, что кликабельно */
        s_HotspotY[s_HotspotCount] = y;
        ++s_HotspotCount;
    }
}

/* Белые маркеры ПОВЕРХ показанного кадра, в клиентских px окна: игровой радиус
   в fullscreen растянулся бы по X и Y по-разному и диск превратился бы в
   эллипс, а один радиус в оконных px остаётся кругом и растёт вместе с
   картинкой. У SDL нет круглая примитива — диск заливается построчно, как
   SdlBackendDrawGrid рисует линиями. 8-битный буфер, палитра и dirty-регионы
   здесь не трогаются: рендерер пересоздаётся на каждый презент, так что
   маркеры исчезают просто тем, что их не нарисовали снова (и появляются —
   принудительным презентом в SdlUpdateHotspots на пустающей картинке). */
static void SdlDrawHotspots(void)
{
    int r, r2, i;

    if (!s_HotspotCount)
        return;
    /* ~4.5 игровых px: масштабируется вместе с картинкой, зажат к разумному —
       крайние значения либо невидимы, либо клякса. Один радиус по X и Y —
       чтобы был круг. */
    r = s_GameClientH > 0 ? (9 * s_PicH + s_GameClientH) / (2 * s_GameClientH) : 5;
    if (r < 2)
        r = 2;
    if (r > 16)
        r = 16;
    r2 = r * r;
    SDL_SetRenderDrawColor(s_Renderer, 255, 255, 255, 255);
    for (i = 0; i < s_HotspotCount; ++i)
    {
        int cx = s_HotspotX[i], cy = s_HotspotY[i];
        int dy;

        SdlBackendGameToClient(&cx, &cy);
        for (dy = -r; dy <= r; ++dy)
        {
            int half = 0;

            while ((half + 1) * (half + 1) + dy * dy <= r2)
                ++half;
            SDL_RenderDrawLine(s_Renderer, cx - half, cy + dy, cx + half, cy + dy);
        }
    }
}

/* Сэмпл Ctrl и обновление списка маркеров, раз в памп. Принудительный презент
   на смене состояния — то, что делает фичу рабочей: игра презентует по блиту,
   а на покоящемся экране блита нет, — без него маркеры либо никогда не
   появились бы, либо никогда не исчезли. Оба ребра в этом нуждаются: вниз —
   показать, вверх — стереть. */
static void SdlUpdateHotspots(void)
{
    int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    int held = ctrl || s_PadOverlayHeld;          /* и руль консоли — второй источник */
    int prevCount = s_HotspotCount;
    int toggled = held != s_HotspotPrev;
    static const char *s_Src = "Ctrl";            /* имя на спуске, остаётся и на подъёме */

    if (held)
        s_Src = ctrl ? "Ctrl" : "pad R";
    s_HotspotHeld = held;
    if (held)
        SdlCollectHotspots();
    else
        s_HotspotCount = 0;
    if (toggled || s_HotspotCount != prevCount)
        DBG_LOG("SdlUpdateHotspots: %s %s, %d interactive object(s)",
                s_Src, held ? "down" : "up", s_HotspotCount);
    s_HotspotPrev = held;
    if (toggled && s_Texture && s_Renderer)
    {
        s_Dirty = true;                            /* форс: сцена ничего не блитовала */
        SdlBackendFlushPresent();
    }
}

/* Показать теневой кадр, если он грязный (иначе no-op). Один RenderPresent
   на кадр; Host_Flush после уже показанного кадра ничего не делает. Кадр
   кладётся в rect картинки: в обычном окне это всё окно (как в оригинале),
   в fullscreen — letterboxed-пятно, масштаб рендерером. */
void SdlBackendFlushPresent(void)
{
    SDL_Rect dst;

    if (!s_Texture || !s_Renderer || !s_Shadow || !s_Dirty)
        return;
    SDL_UpdateTexture(s_Texture, NULL, s_Shadow, s_TexW * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(s_Renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_Renderer);
    dst.x = s_PicX;
    dst.y = s_PicY;
    dst.w = s_PicW > 0 ? s_PicW : s_TexW;
    dst.h = s_PicH > 0 ? s_PicH : s_TexH;
    SDL_RenderCopy(s_Renderer, s_Texture, NULL, &dst);
    if (s_HotspotHeld && s_HotspotCount)          /* Ctrl зажат: отметить кликабельное */
        SdlDrawHotspots();
    SDL_RenderPresent(s_Renderer);
    s_Dirty = false;
    if (!s_PresentedOnce)
    {
        s_PresentedOnce = true;
        DBG_LOG("First present: %dx%d", s_TexW, s_TexH);
    }
}

/* ========================================================================== */
/* События                                                                    */
/* ========================================================================== */

/* Выбросить накопленные SDL-события. На Windows игровые сообщения идут через
   подкласс wndproc, поэтому здесь просто дренируем очередь SDL; на остальных
   платформах очередь шима — единственная, так что сюда встаёт полный насос
   (перевод SDL-событий в WM_* + фильм + волна). */
void SdlBackendPumpEvents(void)
{
    SdlUpdateHotspots();                           /* Ctrl зажат — отметить кликабельное */
#ifndef _WIN32
    WinShim_PumpSDLToQueue();
#else
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
        /* Пусто: WM_CLOSE/WM_DESTROY игра получает через подклассwndproc. */
    }
#endif
}

#ifdef __SWITCH__
/* hbmenu не передаёт командную строку, так что -datapath на консоли недоступен,
   а движок ищет .\PILOTS.EXE от текущего каталога (при запуске из hbmenu это
   корень SD). Фоллбэк на стандартное homebrew-место — но только если данные
   действительно в нём: кто держит их в другом месте, получает прежнее
   поведение, а если там ничего нет — текущий каталог не трогается (фоллбэк
   только добавляет случай, ничего не отнимая). Путь идёт через шим
   (GetFileAttributesA/SetCurrentDirectoryA в win32_shim.c), как и весь путь
   игры: обход без учёта регистра один на все файлы. */
static void SdlSwitchDefaultDataPath(void)
{
    char probe[MAX_PATH];

    strcpy(probe, "sdmc:/switch/pilots");
    strcat(probe, "/PILOTS.EXE");
    if (GetFileAttributesA(probe) == (DWORD)-1)
        return;
    if (SetCurrentDirectoryA("sdmc:/switch/pilots"))
    {
        s_DataPathGiven = 1;              /* путь игры от CWD, как после -datapath */
        DBG_LOG("SdlBackendParseCmdLine: no -datapath on the Switch, using sdmc:/switch/pilots");
    }
}
#endif

/* Следующий токен командной строки. Токен в кавычках может содержать пробелы,
   и кавычки в него не входят. Возвращает 0, когда строка исчерпана, и 1,
   когда токен прочитан — *len может быть 0 для пустого "" (поэтому вызывающий
   не должен считать 0 признаком «ничего не найдено»). */
static int SdlNextToken(const char **pp, const char **ptok, int *len)
{
    const char *p = *pp;
    const char *tok;

    while (*p == ' ' || *p == '\t')
        ++p;
    if (!*p)
        return 0;
    if (*p == '"')
    {
        ++p;
        tok = p;
        while (*p && *p != '"')
            ++p;
        *len = (int)(p - tok);
        if (*p == '"')
            ++p;
    }
    else
    {
        tok = p;
        while (*p && *p != ' ' && *p != '\t')
            ++p;
        *len = (int)(p - tok);
    }
    *ptok = tok;
    *pp = p;
    return 1;
}

/* Весь токен — «-name» (или «/name»), без учёта регистра, и ничего больше. */
static int SdlArgIs(const char *tok, int len, const char *name)
{
    int i;

    for (i = 0; name[i]; ++i)
    {
        char a = (i + 1 < len) ? tok[i + 1] : 0;   /* +1 пропускает ведущий - или / */
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (a != name[i])
            return 0;
    }
    return len == i + 1;
}

/* -datapath <каталог> (фича порта): сменить рабочий каталог процесса. Всё, что
   движок ищет, — относительно CWD (.\PILOTS.EXE, .\movies\..., сохранения), так
   что один SetCurrentDirectoryA уводит за ключом всё разом, и код игры ничего
   не узнаёт об этом. Без ключа не трогается ничего — как в оригинале. Два
   файла намеренно следуют exe, а не каталогу: debug-лог и .hlp — оба
   выводятся из GetModuleFileNameA. SetCurrentDirectoryA на Windows —
   настоящий kernel32, на остальных платформах — обёртка над chdir из
   win32_shim.c (там же регистронезависимый обход пути). */
static void SdlUseDataPath(const char *path, int len)
{
    char dir[MAX_PATH];

    if (len >= (int)sizeof(dir))
        len = (int)sizeof(dir) - 1;
    memcpy(dir, path, len);
    dir[len] = '\0';
    if (!SetCurrentDirectoryA(dir))
        DBG_LOG("SdlBackendParseCmdLine: -datapath '%s': cannot use it, keeping the current directory", dir);
    else
    {
        s_DataPathGiven = 1;
        DBG_LOG("SdlBackendParseCmdLine: -datapath '%s': game resources are looked up here", dir);
    }
}

/* Был ли ключ -datapath (или применился ли Switch-фоллбэк): WinMain по этому
   признаку строит путь игры от текущего каталога (как оригинал), а не от
   каталога своего exe. */
int SdlBackendDataPathGiven(void)
{
    return s_DataPathGiven;
}

/* Разбор командной строки: -fullscreen и -datapath <каталог>. Зовётся из
   WinMain (host.c) ДО того, как движок открыл хоть один файл — именно это
   и делает -datapath работающим. На консоли командной строки нет вовсе,
   поэтому там просто фоллбэк пути данных. */
void SdlBackendParseCmdLine(const char *cmdLine)
{
    const char *p = cmdLine;
    const char *tok;
    int len;
    int found = 0;
    int dataPathGiven = 0;

    if (p)
    {
        while (SdlNextToken(&p, &tok, &len))
        {
            if (len <= 1 || (tok[0] != '-' && tok[0] != '/'))
                continue;
            if (SdlArgIs(tok, len, "fullscreen"))
                found = 1;
            else if (SdlArgIs(tok, len, "datapath"))
            {
                const char *val;
                int vlen;

                if (SdlNextToken(&p, &val, &vlen) && vlen > 0)
                {
                    dataPathGiven = 1;
                    SdlUseDataPath(val, vlen);
                }
                else
                    DBG_LOG("SdlBackendParseCmdLine: -datapath needs a directory, ignored");
            }
        }
    }
#ifdef __SWITCH__
    if (!dataPathGiven)
        SdlSwitchDefaultDataPath();
#else
    (void)dataPathGiven;
#endif
    if (found)
    {
        DBG_LOG("SdlBackendParseCmdLine: -fullscreen -> borderless desktop fullscreen, Alt+Enter toggles");
        SdlBackendSetFullscreen(1);               /* окна ещё нет — запишется, применится в CreateWindow */
    }
}

/* ========================================================================== */
/* Раскладка картинки: обычное окно (тождественность, как в оригинале) и      */
/* fullscreen (letterbox с сохранением пропорций, '-' — растянуть во весь)    */
/* ========================================================================== */

#define SDL_FS_MAX_PIN 16384                      /* замена пина максимального размера окна в fullscreen */

/* Деление со знаком вниз: точка слева/выше картинки обязана дать отрицательную
   игровую координату, а не 0 — главный цикл сравнивает её с клиентским rect. */
static int SdlFloorDiv(int a, int b)
{
    int q = a / b;
    if ((a % b) != 0 && (a < 0) != (b < 0))
        --q;
    return q;
}

static int SdlPictureTransformed(void)
{
    return s_PicW > 0 && s_PicH > 0
        && (s_PicX != 0 || s_PicY != 0 || s_PicW != s_GameClientW || s_PicH != s_GameClientH);
}

/* Куда ложится картинка. Обычное окно — это и есть клиент (тождественность,
   оригинальное поведение). Fullscreen — пятно окна с сохранением пропорций,
   центрированное; пока играет кино, натуральный размер с центрирующим
   смещением (MCI-ребёнок Windows рисует 1:1 и не следует масштабу). */
static void SdlUpdateLayout(void)
{
    int w = s_GameClientW;
    int h = s_GameClientH;
    int ww, wh;
    int picW, picH;
    int newX, newY;
    int changed;

    if (w <= 0 || h <= 0)
        return;
    if (!s_Fullscreen || !s_Window)
    {
        /* Обычное окно: картинка И есть клиент. Выход из fullscreen попадает
           сюда, поэтому путь обязан заметить изменение, а не только записать
           тождественный rect. */
        changed = (s_PicX != 0 || s_PicY != 0 || s_PicW != w || s_PicH != h);
        s_PicX = 0;
        s_PicY = 0;
        s_PicW = w;
        s_PicH = h;
        if (changed && s_Texture && s_Renderer)
            SdlBackendFlushPresent();             /* новый rect рисуется только следующим кадром */
        return;
    }
    /* Клиентский rect, а не SDL_GetWindowSize: на Windows SDL узнаёт новый
       размер только из WM_WINDOWPOSCHANGED, который приходит ПОСЛЕ WM_SIZE —
       пересчёт происходит внутри обработчика WM_SIZE, где у SDL ещё старый
       размер (а при старте окно ещё скрыто, когда применяется -fullscreen, и
       сообщения о размере не было вовсе). */
    ww = wh = 0;
#ifdef _WIN32
    {
        RECT rc;
        if (g_MainWindow && GetClientRect((HWND)g_MainWindow, &rc))
        {
            ww = rc.right;
            wh = rc.bottom;
        }
    }
#endif
    if (ww <= 0 || wh <= 0)
        SDL_GetWindowSize(s_Window, &ww, &wh);
    if (ww <= 0 || wh <= 0)
        return;                                  /* свёрнуто: оставить прошлую раскладку */
    if (s_NativeMovie)
    {
        picW = w;                                /* натуральный размер: MCI-ребёнок 1:1 */
        picH = h;
    }
    else if (s_StretchPic)
    {
        picW = ww;                               /* '-' (фича порта): во весь экран, пропорции
                                                    игры сознательно не сохраняются — в этом
                                                    смысл клавиши, и клики всё так же идут
                                                    обратно через этот rect */
        picH = wh;
    }
    else if ((long long)ww * h <= (long long)wh * w)
    {
        picW = ww;                               /* окно шире: вписываем по ширине */
        picH = (int)((long long)ww * h / w);
        if (picH < 1)
            picH = 1;
    }
    else
    {
        picH = wh;                               /* окно выше: вписываем по высоте */
        picW = (int)((long long)wh * w / h);
        if (picW < 1)
            picW = 1;
    }
    newX = (ww - picW) / 2;
    newY = (wh - picH) / 2;
    changed = (newX != s_PicX || newY != s_PicY || picW != s_PicW || picH != s_PicH);
    s_PicX = newX;
    s_PicY = newY;
    s_PicW = picW;
    s_PicH = picH;
    if (changed)
    {
        /* Выход рендерера — пространство, в котором SDL_RenderCopy реально
           рисует: на Windows оно НЕ гарантированно равно клиенту (DPI-масштаб,
           backbuffer, который SDL ещё не resize'нул). Когда оно меньше ww/wh,
           копия обрезается и окно показывает только левый верхний угол
           масштабированной картинки — потому оно и логируется рядом с rect'ом,
           которому обязано совпасть. */
        int outW = 0, outH = 0;
        if (s_Renderer && SDL_GetRendererOutputSize(s_Renderer, &outW, &outH) != 0)
            outW = outH = 0;
        DBG_LOG("SdlUpdateLayout: window %dx%d -> picture %dx%d at (%d,%d)%s (renderer output %dx%d)%s",
                ww, wh, s_PicW, s_PicH, s_PicX, s_PicY,
                s_NativeMovie ? " (native movie)" : "", outW, outH,
                s_StretchPic ? " [- stretched]" : "");
        if (s_Texture && s_Renderer)
            SdlBackendFlushPresent();             /* раскладка сдвинулась — кадр нужен сейчас */
    }
}

/* px окна -> px игры (тождественность, пока картинка — окно). */
void SdlBackendClientToGame(int *x, int *y)
{
    if (!SdlPictureTransformed())
        return;
    *x = SdlFloorDiv((*x - s_PicX) * s_GameClientW, s_PicW);
    *y = SdlFloorDiv((*y - s_PicY) * s_GameClientH, s_PicH);
    /* Результат вне картинки main.c читает как inputState=0 («стереть курсор,
       не перерисовывать») — падения на поле вокруг картинки сделаны нарочно,
       и охота за исчезающим курсором начинается здесь. */
}

/* px игры -> px окна (позиция MCI-ребёнка на Windows). */
void SdlBackendGameToClient(int *x, int *y)
{
    if (!SdlPictureTransformed())
        return;
    *x = s_PicX + (int)((long long)*x * s_PicW / s_GameClientW);
    *y = s_PicY + (int)((long long)*y * s_PicH / s_GameClientH);
}

/* Точка мыши для игры; 0 — вне картинки, тогда поле отбрасывается, а не
   шлётся: игра ведёт себя ровно как при уходе мыши из окна в оригинале
   (сообщения нет вообще, флаг ввода в главном цикле выключен). */
static int SdlMapMousePoint(int *x, int *y)
{
    if (!SdlPictureTransformed())
        return 1;
    SdlBackendClientToGame(x, y);
    return *x >= 0 && *x < s_GameClientW && *y >= 0 && *y < s_GameClientH;
}

/* SDL-сторона s_Fullscreen. Идемпотентна и не делает ничего, пока окна нет:
   WinMain записывает -fullscreen рано, а применяет CreateWindow. */
static void SdlApplyFullscreen(void)
{
    int want = s_Fullscreen ? 1 : 0;
    int isFs;

    if (!s_Window)
        return;
    isFs = (SDL_GetWindowFlags(s_Window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
    if (want != isFs)
    {
        if (want)
        {
            /* Оба пина размера обязаны уйти первыми: собственный максимум SDL
               (выставленный при создании окна) и пин игрового WM_GETMINMAXINFO
               ужали бы fullscreen-резайз обратно к размеру игры. */
            SDL_SetWindowMaximumSize(s_Window, SDL_FS_MAX_PIN, SDL_FS_MAX_PIN);
#ifndef _WIN32
            /* На X11 desktop-fullscreen от SDL — только ПРОСЬБА к оконному
               менеджеру (_NET_WM_STATE_FULLSCREEN) и bump собственной мысли о
               размере до границ экрана; само окно он не resize'ит. С WM этого
               хватает; без него (голый Xvfb — каждый харнесс в tests/linux) X-окно
               остаётся 640×480 в (320,120), SDL отвечает 1280×720, и весь
               rect-математике считается по несуществующему окну — доказано
               отдельным SDL-пробой без единой строчки кода игры. Поэтому
               геометрию двигаем сами и пока окно ещё обычное: после поднятия
               флага полноэкранности SDL игнорирует SetWindowSize. Безрамочный
               fullscreen И ЕСТЬ «окно размером с экран в (0,0)», с WM или без. */
            {
                SDL_DisplayMode dm;
                int di = SDL_GetWindowDisplayIndex(s_Window);
                if (di < 0)
                    di = 0;
                if (SDL_GetDesktopDisplayMode(di, &dm) == 0 && dm.w > 0 && dm.h > 0)
                {
                    SDL_SetWindowPosition(s_Window, 0, 0);  /* SDL центрирует окна без WM */
                    SDL_SetWindowSize(s_Window, dm.w, dm.h);
                    DBG_LOG("SdlApplyFullscreen: forced X geometry %dx%d at (0,0)", dm.w, dm.h);
                }
            }
#endif
            if (SDL_SetWindowFullscreen(s_Window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
                DBG_LOG("SdlApplyFullscreen: SDL_SetWindowFullscreen(DESKTOP) failed: %s", SDL_GetError());
        }
        else
        {
            SDL_SetWindowFullscreen(s_Window, 0);
#ifndef _WIN32
            /* Зеркало входа: флаг SDL снял, а запомненный обычный размер —
               тот, что мы навязали fullscreen'ом: вернуть игровое окно. */
            if (s_GameClientW > 0 && s_GameClientH > 0)
            {
                SDL_SetWindowSize(s_Window, s_GameClientW, s_GameClientH);
                SDL_SetWindowPosition(s_Window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
            }
#endif
            if (s_GameClientW > 0 && s_GameClientH > 0)
                SDL_SetWindowMaximumSize(s_Window, s_GameClientW, s_GameClientH);
        }
        SdlUpdateLayout();
        DBG_LOG("SdlApplyFullscreen: %s -> picture %dx%d at (%d,%d)",
                want ? "on" : "off", s_PicW, s_PicH, s_PicX, s_PicY);
        if (g_MainWindow)
            InvalidateRect((HWND)g_MainWindow, 0, 1); /* repaint всего окна на новой геометрии */
    }
    SdlUpdateLayout();
}

void SdlBackendSetFullscreen(int on)
{
    int was = s_Fullscreen ? 1 : 0;

    on = on ? 1 : 0;
    if (on == was && s_Window)
        return;
    s_Fullscreen = on;
    SdlApplyFullscreen();
}

int SdlBackendToggleFullscreen(void)
{
    SdlBackendSetFullscreen(!s_Fullscreen);
    return s_Fullscreen;
}

/* '-' (фича порта): между игровой картинкой (letterbox, пропорции сохранены)
   и той же картинкой, растянутой на весь экран. Второе состояние живёт только
   в fullscreen, поэтому клавиша подразумевает его: из обычного окна уводит в
   fullscreen и сразу растянутое, дальше лишь щёлкает между двумя картинками.
   Alt+Enter остаётся путём обратно в окно, а оконная картинка всегда была и
   остаётся тождественной. */
int SdlBackendToggleStretch(void)
{
    s_StretchPic = !s_StretchPic;
    if (s_StretchPic && !s_Fullscreen)
        SdlBackendSetFullscreen(1);               /* он применяет раскладку и видит stretch */
    else
        SdlUpdateLayout();
    DBG_LOG("SdlBackendToggleStretch: %s -> picture %dx%d at (%d,%d)",
            s_StretchPic ? "stretched over the screen" : "the game's own aspect, letterboxed",
            s_PicW, s_PicH, s_PicX, s_PicY);
    return s_StretchPic;
}

/* Кино Windows: дочернее окно mciavi32 рисует 1:1 и не масштабируется вслед
   за картинкой, поэтому на время фильма картинка падает до натурального
   размера и держит только центрирующее смещение. На остальных платформах
   кино рисует сам порт (SdlBackendVideoFrame) и масштабируется само — здесь
   пусто. */
void SdlBackendSetNativeMovie(int on)
{
#ifdef _WIN32
    on = on ? 1 : 0;
    if (on == (s_NativeMovie != 0))
        return;
    s_NativeMovie = on;
    SdlUpdateLayout();
    DBG_LOG("SdlBackendSetNativeMovie: %d -> picture %dx%d at (%d,%d)",
            on, s_PicW, s_PicH, s_PicX, s_PicY);
#else
    (void)on;
#endif
}

/* ========================================================================== */
/* Не-Windows: SDL-события -> WM_* в очередь шима                             */
/*                                                                            */
/* Очередь сообщений здесь одна — та, что держит win32_shim.c: PeekMessageA   */
/* её дренирует, DispatchMessageA зовёт Host_MainWndProc напрямую. Ни одного  */
/* реального окна нет, так что весь ввод — это перевод SDL-событий в WM_*.    */
/* Координаты везде тождественны (окно всегда 640×480 в оконном виде;         */
/* полноэкранный layout с масштабированием не переносится).                     */
/* ========================================================================== */

#ifndef _WIN32

/* SDL keysym -> Win32 виртуальный код. Буквы/цифры/знаки препинания совпадают
   с ASCII, а тот в свою очередь совпадает с VK — остаётся только специальные. */
static WPARAM SdlKeysymToVk(SDL_Keycode k)
{
    switch (k)
    {
    case SDLK_RETURN:     return VK_RETURN;
    case SDLK_ESCAPE:     return VK_ESCAPE;
    case SDLK_BACKSPACE:  return VK_BACK;
    case SDLK_TAB:        return VK_TAB;
    case SDLK_SPACE:      return VK_SPACE;
    case SDLK_DELETE:     return VK_DELETE;
    case SDLK_INSERT:     return VK_INSERT;
    case SDLK_HOME:       return VK_HOME;
    case SDLK_END:        return VK_END;
    case SDLK_PAGEUP:     return VK_PRIOR;
    case SDLK_PAGEDOWN:   return VK_NEXT;
    case SDLK_RIGHT:      return VK_RIGHT;
    case SDLK_LEFT:       return VK_LEFT;
    case SDLK_UP:         return VK_UP;
    case SDLK_DOWN:       return VK_DOWN;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:     return VK_SHIFT;
    case SDLK_LCTRL:
    case SDLK_RCTRL:      return VK_CONTROL;
    case SDLK_LALT:
    case SDLK_RALT:       return VK_MENU;
    case SDLK_CAPSLOCK:   return VK_CAPITAL;
    case SDLK_PAUSE:      return VK_PAUSE;
    case SDLK_NUMLOCKCLEAR: return VK_NUMLOCK;
    case SDLK_SCROLLLOCK: return VK_SCROLL;
    default: break;
    }
    if (k >= SDLK_F1 && k <= SDLK_F12)
        return (WPARAM)(VK_F1 + (k - SDLK_F1));
    if (k >= 0 && k < 128)
        return (WPARAM)k;
    return 0;
}

/* Одно SDL-событие -> WM_* в очередь шима (те же сообщения, что на Windows
   прислал бы реальный ввод в реальное окно). */

/* Просьба выйти: то же, что закрыл окно пользователем. Пока идёт фильм,
   закрывается AVI-ребёнок, а не главное окно: только так фильм останавливается
   чисто (WM_CLOSE -> g_AviQuitRequested -> MCI_STOP -> MM_MCINOTIFY -> выход из
   модального цикла Host_PlayAvi). Один WM_QUIT в этот цикл попадает зря -
   GetMessageA вернёт 0, а сообщение будет снято и потеряно (так написан
   оригинал), после чего игра продолжит работу и уже никогда не закроется. */
static void SdlPostQuitRequest(void)
{
    static int s_QuitRequested;

    if (s_QuitRequested)
        return;
    s_QuitRequested = 1;
    if (g_hAviWindow)
    {
        DBG_LOG("SdlPostQuitRequest: WM_CLOSE -> AVI child");
        PostMessageA((HWND)g_hAviWindow, WM_CLOSE, 0, 0);
    }
    else if (g_MainWindow)
    {
        DBG_LOG("SdlPostQuitRequest: WM_CLOSE -> main window");
        PostMessageA((HWND)g_MainWindow, WM_CLOSE, 0, 0);
    }
    else
        PostQuitMessage(0);
}

static void SdlDispatchSdlEvent(const SDL_Event *ev)
{
    switch (ev->type)
    {
    case SDL_QUIT:
        /* Крестик окна. Игра закрывается своим путём (WM_CLOSE -> Host_CloseWindow
           -> WM_DESTROY -> PostQuitMessage); во время фильма закрывается AVI-ребёнок. */
        SdlPostQuitRequest();
        break;

    case SDL_TEXTINPUT:
        /* Каждый байт — WM_CHAR, как его выдал бы TranslateMessage. */
        {
            const char *p = ev->text.text;
            while (*p)
                PostMessageA((HWND)g_MainWindow, WM_CHAR, (WPARAM)(unsigned char)*p++, 0);
        }
        break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
        {
            WPARAM vk;
            if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_RETURN
                && (ev->key.keysym.mod & KMOD_ALT))
            {
                /* Alt+Enter: переключатель fullscreen (фича порта) — на том же
                   месте, где на Windows сидит WM_SYSKEYDOWN. Игра Enter не
                   видит. */
                SdlBackendToggleFullscreen();
                break;
            }
            if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_MINUS)
            {
                /* '-' (фича порта): картинка во весь экран — то же, что
                   WM_KEYDOWN/VK_OEM_MINUS на Windows. */
                SdlBackendToggleStretch();
                break;
            }
            vk = SdlKeysymToVk(ev->key.keysym.sym);
            if (!vk)
                break;
            PostMessageA((HWND)g_MainWindow,
                         ev->type == SDL_KEYDOWN ? WM_KEYDOWN : WM_KEYUP,
                         vk, 0);
        }
        break;

    case SDL_MOUSEMOTION:
        /* WM_SETCURSOR перед каждым движением — так приходит настоящая мышь и
           так игра знает, что курсор в клиенте. Координаты проходят через rect
           картинки (тождественность в обычном окне); точка вне картинки —
           поле вокруг letterbox — отбрасывается, как «мышь ушла из окна». */
        {
            int mx = (int)ev->motion.x;
            int my = (int)ev->motion.y;
            if (!SdlMapMousePoint(&mx, &my))
                break;
            PostMessageA((HWND)g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
            PostMessageA((HWND)g_MainWindow, WM_MOUSEMOVE, 0, SdlPackPoint(mx, my));
        }
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        {
            /* Средняя кнопка — та же правая: у игры их всего две. wParam несёт
               модификаторы остальных кнопок, но host.c читает из мышиных
               сообщений только координаты из lParam. */
            int right = ev->button.button != SDL_BUTTON_LEFT;
            UINT msg;
            WPARAM wp = 0;
            int mx = (int)ev->button.x;
            int my = (int)ev->button.y;
            int cx = mx, cy = my;

            if (!SdlMapMousePoint(&mx, &my))
                break;                          /* поле вокруг картинки: клик «мимо» */
#ifdef _DEBUG
            if (ev->type == SDL_MOUSEBUTTONDOWN && (mx != cx || my != cy))
                DBG_LOG("SdlDispatchSdlEvent: client (%d,%d) -> game (%d,%d)",
                        cx, cy, mx, my);        /* интересно, только пока картинка трансформирована */
#endif
            if (right)
                msg = ev->type == SDL_MOUSEBUTTONDOWN ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            else
                msg = ev->type == SDL_MOUSEBUTTONDOWN ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            if (ev->type == SDL_MOUSEBUTTONDOWN)
                wp = right ? 2 : 1;              /* MK_RBUTTON / MK_LBUTTON */
            PostMessageA((HWND)g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
            PostMessageA((HWND)g_MainWindow, msg, wp, SdlPackPoint(mx, my));
        }
        break;

    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
        {
            /* Тачскрин: SDL отдаёт палец нормированным к окну, дальше это
               мышиный путь — WM_SETCURSOR, ход, и для касания ещё и клик
               (палец есть левая кнопка, а не наведение). Своя конвертация;
               синтез SDL из пальца выключен в SdlBackendStartup. */
            int mx, my, ww = s_TexW, wh = s_TexH;

            if (!g_MainWindow)
                break;
            if (s_Window)
                SDL_GetWindowSize(s_Window, &ww, &wh);
            mx = (int)(ev->tfinger.x * ww);
            my = (int)(ev->tfinger.y * wh);
#ifdef _DEBUG
            if (s_FingerLogged < 6)
            {
                s_FingerLogged++;
                DBG_LOG("touch: type=%d window %dx%d -> (%d,%d)", ev->type, ww, wh, mx, my);
            }
#endif
            /* Дальше палец идёт мышиным путём: клиент px окна -> px игры (в
               обычном окне — тождественность); точка вне картинки, как и мышь,
               отбрасывается. */
            if (!SdlMapMousePoint(&mx, &my))
                break;
            PostMessageA(g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
            PostMessageA(g_MainWindow, WM_MOUSEMOVE, 0, SdlPackPoint(mx, my));
#ifdef __SWITCH__
            s_StickX = mx;              /* после пальца стик продолжает от него */
            s_StickY = my;
#endif
            if (ev->type == SDL_FINGERDOWN)
                PostMessageA(g_MainWindow, WM_LBUTTONDOWN, 0, SdlPackPoint(mx, my));
            else if (ev->type == SDL_FINGERUP)
                PostMessageA(g_MainWindow, WM_LBUTTONUP, 0, SdlPackPoint(mx, my));
        }
        break;

    case SDL_WINDOWEVENT:
        switch (ev->window.event)
        {
        case SDL_WINDOWEVENT_CLOSE:
            if (g_MainWindow)
                PostMessageA((HWND)g_MainWindow, WM_CLOSE, 0, 0);
            break;
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            /* Окно сменило размер — картинка должна переложиться. На Windows
               то же приходит через WM_SIZE/WM_WINDOWPOSCHANGED в подклассе
               wndproc, здесь это единственная ниточка. */
            SdlUpdateLayout();
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}

#ifdef __SWITCH__
/* Стик и кнопки консоли -> те же мышиные сообщения, что шлёт настоящая мышь
   (SdlDispatchSdlEvent делает то же для SDL-событий): игра не отличает одно от
   другого — тот же WM_SETCURSOR перед ходом, те же координаты в LPARAM.

   Поток ХОДОВ троттлится, и вся история в этом слове. Каждый WM_MOUSEMOVE
   гоняет wndproc по UpdateCursorPosition -> RenderDirtyRegions СИНХРОННО, а
   стик шлёт сообщение на каждый памп, пока держат: замерено 343-650
   сообщений/с, и на том уровне один проход dirty-region замораживал анимацию
   сцены под двигающимся курсором (сам цикл оставался здоровым). MOVE_MIN_INTERVAL_MS
   сводит поток к частоте настоящей мыши: курсор перекрашивается в её ритме, а
   сцена живёт.

   Скорость стика интегрируется по МИЛЛИСЕКУНДАМ, а не по пампу: памп на
   консоли что угодно — 50-60/с в покое, до 143924/с на загруженной карте
   уровней — и «пиксель за памп» прибивал курсор к краю картинки за 37 мс
   (замер: «дёргано, с рывками, кончаясь у краёв»). Здесь скорость курсора —
   свойство одного стика, памп её не касается. */
#define STICK_DEADZONE 3000         /* из 0x7FFF: покоящийся стик дрейфует на сотни */
#define STICK_SPEED_PX_PER_SEC 640  /* px/секунду при полном отклонении: пересекает картинку за секунду */
#define MOVE_MIN_INTERVAL_MS 50     /* троттл-интервал: 50 мс — частота отчёта настоящей мыши */

/* Клик кнопкой пада по позиции КУРСОРА (не кнопки — движок щупает именно её).
   Нажатие несёт WM_MOUSEMOVE перед собой: ход может отстать на один интервал
   троттла, а клик должен попасть туда, куда целится игрок. */
static void SdlPostPadClick(UINT downMsg, UINT upMsg, int down)
{
    if (!down)
    {
        PostMessageA(g_MainWindow, upMsg, 0, SdlPackPoint(s_StickX, s_StickY));
        return;
    }
    PostMessageA(g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
    PostMessageA(g_MainWindow, WM_MOUSEMOVE, 0, SdlPackPoint(s_StickX, s_StickY));
    PostMessageA(g_MainWindow, downMsg, 0, SdlPackPoint(s_StickX, s_StickY));
}

static void SdlPollSwitchControls(void)
{
    static unsigned int s_PrevButtons;
    static unsigned int s_LastStickPoll;       /* мс предыдущего опроса — стик
                                                  интегрируется по ВРЕМЕНИ */
    static long long s_StickAccX, s_StickAccY; /* недобор субпикселя: слабое
                                                  отклонение должно ползти, а не
                                                  стоять до выплаты целого пикселя */
    int sx, sy;
    unsigned int buttons;
    int dx, dy, nx, ny;
    unsigned int now;
    int dt;
    long long ax, ay;

    if (!g_MainWindow)
        return;
    if (s_StickX < 0)
    {
        s_StickX = s_TexW / 2; /* центр — единственная точка, гарантированно внутри */
        s_StickY = s_TexH / 2;
    }

    SdlSwitchPollPad(&sx, &sy, &buttons);

    if (sx > -STICK_DEADZONE && sx < STICK_DEADZONE)
        sx = 0;                                /* курсор, ползущий сам по себе, хуже мёртвой зоны */
    if (sy > -STICK_DEADZONE && sy < STICK_DEADZONE)
        sy = 0;
    now = SDL_GetTicks();
    dt = (int)(now - s_LastStickPoll);
    s_LastStickPoll = now;
    if (dt < 0 || dt > 100)
        dt = 100;                              /* первый опрос и зависание одинаково: никогда не швырять */
    ax = s_StickAccX + (long long)sx * STICK_SPEED_PX_PER_SEC * dt;
    ay = s_StickAccY - (long long)sy * STICK_SPEED_PX_PER_SEC * dt; /* стик вверх, y клиента вниз */
    dx = (int)(ax / (1000 * 0x7FFF));
    dy = (int)(ay / (1000 * 0x7FFF));
    s_StickAccX = ax - (long long)dx * (1000 * 0x7FFF);
    s_StickAccY = ay - (long long)dy * (1000 * 0x7FFF);

    nx = s_StickX + dx;
    ny = s_StickY + dy;
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx > s_TexW - 1)
        nx = s_TexW - 1;
    if (ny > s_TexH - 1)
        ny = s_TexH - 1;
    if (nx != s_StickX || ny != s_StickY)
    {
        s_StickX = nx;
        s_StickY = ny;
        s_MovePending = 1;                     /* уходит ниже, как разрешит троттл */
    }
    /* Троттл-ход: позиция СЕЙЧАС, никогда хвост: одного сообщения хватает сколько
       угодно пропущенных пампов, а флаг добьёт конечную точку в пределах одного
       интервала после того, как стик отпустили. */
    if (s_MovePending && SDL_GetTicks() - s_LastMovePost >= MOVE_MIN_INTERVAL_MS)
    {
        s_MovePending = 0;
        s_LastMovePost = SDL_GetTicks();
        PostMessageA(g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
        PostMessageA(g_MainWindow, WM_MOUSEMOVE, 0, SdlPackPoint(s_StickX, s_StickY));
    }

    /* Каждое ребро — строка в лог: «игра закрылась сама» должно читаться рядом
       с тем, что делал игрок. A — левый клик, B — правый. X/Y пока без
       привязки, но мёртвая кнопка тихой быть не должна. */
    if ((buttons & SWITCH_PAD_A) != (s_PrevButtons & SWITCH_PAD_A))
    {
        DBG_LOG("SdlSwitchControls: A %s -> left click at (%d,%d)",
                (buttons & SWITCH_PAD_A) ? "down" : "up", s_StickX, s_StickY);
        SdlPostPadClick(WM_LBUTTONDOWN, WM_LBUTTONUP, (buttons & SWITCH_PAD_A) != 0);
    }
    if ((buttons & SWITCH_PAD_B) != (s_PrevButtons & SWITCH_PAD_B))
    {
        DBG_LOG("SdlSwitchControls: B %s -> right click at (%d,%d)",
                (buttons & SWITCH_PAD_B) ? "down" : "up", s_StickX, s_StickY);
        SdlPostPadClick(WM_RBUTTONDOWN, WM_RBUTTONUP, (buttons & SWITCH_PAD_B) != 0);
    }
    if ((buttons & (SWITCH_PAD_X | SWITCH_PAD_Y)) != (s_PrevButtons & (SWITCH_PAD_X | SWITCH_PAD_Y)))
        DBG_LOG("SdlSwitchControls: X/Y edge, prev=0x%X now=0x%X (no binding, ignored)",
                s_PrevButtons & (SWITCH_PAD_X | SWITCH_PAD_Y),
                buttons & (SWITCH_PAD_X | SWITCH_PAD_Y));
    /* '-' (минус Joy-Con): собственный переключатель картинки порта, на том же
       месте, где на других платформах клавиша '-': игра привязки не имеет и
       ничего не узнаёт. Только по фронту нажатия, чтобы удержание не мигало
       между двумя режимами. Из обычного окна первое нажатие уводит в
       fullscreen и сразу растянутое — «во весь экран» имеет смысл только на
       всём экране. */
    if ((buttons & SWITCH_PAD_MINUS) && !(s_PrevButtons & SWITCH_PAD_MINUS))
    {
        DBG_LOG("SdlSwitchControls: '-' (minus) pressed -> the port's picture toggle");
        SdlBackendToggleStretch();
    }
    /* R (правый плечевой) — оверлей кликабельного, та же картинка, что даёт
       Ctrl: состояние держится, пока кнопка зажата, а строку «pad R down»
       пишет и рисует SdlUpdateHotspots следующим пампом (он идёт до опроса
       пада — один памп задержки, как v0.1). L — по-прежнему без привязки, но
       мёртвая кнопка тихой быть не должна: репорт о ребре ограничен. */
    s_PadOverlayHeld = (buttons & SWITCH_PAD_R) != 0;
    if ((buttons & SWITCH_PAD_L) != (s_PrevButtons & SWITCH_PAD_L)
        && s_RLedgesLogged < 8)
    {
        ++s_RLedgesLogged;
        DBG_LOG("SdlSwitchControls: L edge, prev=0x%X now=0x%X (no binding); %d edge line(s) of 8",
                s_PrevButtons & SWITCH_PAD_L,
                buttons & SWITCH_PAD_L,
                s_RLedgesLogged);
    }

    s_PrevButtons = buttons;
}
#endif /* __SWITCH__ */

#ifdef __SWITCH__
/* Стрелка, которую движок перерисовывает только при движении указателя. На Windows и X11
   стрелку система рисует поверх окна сама; на Switch её нет вовсе (в штатном драйвере
   видео SDL2 не существует ShowCursor — замерено nm'ом по библиотеке), так что там
   стрелка — это только то, что движок последним нарисовал, и всё, чем сцена закрасит её
   место, отбирает её до первого движения игрока. Порт не изобретает своего ремонта, а
   заставляет движок вызвать сам себя: GmSprite_Invalidate(&g_CursorSprite) — тот же
   вызов, что делает objects.c при движке указателя, — и стрелка возвращается собственным
   проходом движка по грязным прямоугольникам, в том же кадре, что и памп, без движения
   и потому без восстановления фона под старым местом. Три промерянные попытки стоят за
   этой фразой (см. AGENTS.md): слишком ранний вызов рисовал стрелку в (0,0), пока ни одно
   сообщение мыши не приходило; однопиксельный ход мыши возвращал стрелку, но на анимации
   восстановление фона было неверным и игрок видел мигание; счётчик блитов с порогом 16
   превратился в метроном — ремонт сам был блитом и подпитывал своё же условие, лог
   показывал ровный полусекундный ритм. Решение — сравнивать саму картинку: квадрат 65×65
   из теневого кадра (того же, что уходит в текстуру) вокруг позиции, которую движок видит
   (g_Window.mouseX/Y — host.c пишет из WM_MOUSEMOVE после маппинга порта в px игры).

   Снимок сознательно НЕ обновляется при расхождении, а пампом позже — тем кадром, что
   дал ремонт; обновить его из стёртого кадра значило бы снова кормить условие его же
   результатом. Покоящаяся сцена не меняется вовсе → ничего не шлётся; анимированная
   меняется каждый кадр → стрелку просят back каждый кадр, и каждый показанный кадр её
   содержит — форма, при которой мигание невозможно в принципе. */
#define CURSOR_BOX      32       /* половина наблюдаемого квадрата, px игры: стрелка влезает */
#define CURSOR_BOX_PX   (2 * CURSOR_BOX + 1)
#define CURSOR_DIFF_MIN 8        /* пикселей должно разойтись, чтобы считать «закрасили» */

static uint32_t s_CursorSnap[CURSOR_BOX_PX * CURSOR_BOX_PX];

static void SdlCursorKeepAlive(void)
{
    static int s_X = -1;           /* позиция, вокруг которой снят снимок */
    static int s_Y = -1;
    static int s_WantSnap;         /* ремонт состоялся: перепривязаться следующим пампом */
    int cx, cy, x0, y0, w, h, row, col, diff = 0;

    if (!s_Shadow || !g_CursorSprite.frame)
        return;                    /* кадра-указателя ещё нет — нечего держать */
    /* Указатель как ЕГО видит игра, а не как порт последний раз его послал: заглоченное
       по дороге сообщение (в поле вокруг картинки, в буксите) оставляет игру в другом
       месте, а перекрашивают как раз её точку. */
    cx = g_Window.mouseX;
    cy = g_Window.mouseY;
    if (cx <= 0 && cy <= 0)
        return;                    /* игра указатель ещё не видела */
    x0 = cx - CURSOR_BOX;
    y0 = cy - CURSOR_BOX;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    w = CURSOR_BOX_PX;
    h = CURSOR_BOX_PX;
    if (x0 + w > s_TexW)
        w = s_TexW - x0;
    if (y0 + h > s_TexH)
        h = s_TexH - y0;
    if (w <= 0 || h <= 0)
        return;

    if (s_WantSnap || cx != s_X || cy != s_Y)
    {
        /* Кадр, который движок нарисовал после хода указателя или после ремонта: стрелка
           в нём есть — с ним и сравнивать следующие. */
        s_X = cx;
        s_Y = cy;
        s_WantSnap = 0;
        for (row = 0; row < h; ++row)
            for (col = 0; col < w; ++col)
                s_CursorSnap[(size_t)row * CURSOR_BOX_PX + col] =
                    s_Shadow[(size_t)(y0 + row) * s_TexW + x0 + col];
        return;
    }

    for (row = 0; row < h; ++row)
        for (col = 0; col < w; ++col)
            diff += (s_CursorSnap[(size_t)row * CURSOR_BOX_PX + col] !=
                     s_Shadow[(size_t)(y0 + row) * s_TexW + x0 + col]);
    if (diff < CURSOR_DIFF_MIN)
    {
        if (diff > 0)
        {
            for (row = 0; row < h; ++row)         /* пара пикселей: перепривязаться молча */
                for (col = 0; col < w; ++col)
                    s_CursorSnap[(size_t)row * CURSOR_BOX_PX + col] =
                        s_Shadow[(size_t)(y0 + row) * s_TexW + x0 + col];
        }
        return;                    /* поверх указателя не закрасили: не трогаем */
    }

    /* Собственный механизм движка, вызванный так, как движок его зовёт (objects.c,
       движение указателя): спрайт курсора — в грязный список, а проход следующего такта
       нарисует фон комнаты, спрайты и стрелку сверху (game.c:82 ставит её последней).
       Ничего не движется — фон под старым местом восстанавливать не надо. Если указатель
       вне картинки, game.c:82 стрелку в этот проход не добавит: движок сам её стёр, порт
       с ним не спорит. */
    GmSprite_Invalidate(&g_CursorSprite);
    s_WantSnap = 1;
}
#endif /* __SWITCH__ */

/* Слить SDL в очередь шима: события -> WM_*, затем соник-службы — фильм
   (WinShim_VideoPump, mci_ffmpeg.c) и волна (WinShim_WavePump, win32_shim.c).
   Именно сюда встаёт и главный цикл игры, и модальный цикл Host_PlayAvi. */
int WinShim_PumpSDLToQueue(void)
{
    SDL_Event ev;

#ifdef __SWITCH__
    /* Пад и здесь, а не только из главного цикла: именно в эту pump ходит
       модальный цикл Host_PlayAvi (ожидание g_VideoPlaying через PeekMessageA),
       и без опроса здесь стик с кнопками оглохли бы ровно на время фильма —
       а A это и есть пропуск интро. Стрелка — по той же причине: во время
       фильма памп главного цикла не крутится, и без этого вызова стрелка
       гасла бы на всю его длительность. */
    SdlPollSwitchControls();
    SdlCursorKeepAlive();
#endif
    if (s_Usr1Quit)
    {
        s_Usr1Quit = 0;
        DBG_LOG("WinShim_PumpSDLToQueue: SIGUSR1, quitting");
        SdlPostQuitRequest();               /* одна просьба на прогон — внутри флаг */
    }
    while (SDL_PollEvent(&ev))
        SdlDispatchSdlEvent(&ev);
    WinShim_VideoPump();                         /* MM_MCINOTIFY и кадры фильма */
    WinShim_WavePump();                          /* дозрели ли WAVEHDR'ы */
    return 0;
}

/* ========================================================================== */
/* Общий микшер: волна игры, MIDI и звук фильма на одном устройстве.          */
/*                                                                            */
/* Первая кого-то попросившая сторона открывает устройство, остальные          */
/* пользуются им же: форматы синтезовые (44100/S16/стерео), поэтому           */
/* конвертация игровой волны — на продюсере (SdlWaveAppendShared),            */
/* а колбэк только читает кольца и суммирует. Звук фильма — третий источник:   */
/* mci_ffmpeg.c читает аудио-чанки AVI сам (ffmpeg их в demuxer не показывает) */
/* и ведёт их по видеоклоку. Отдельное кольцо, а не волна, — чтобы skip-кино   */
/* сбрасывало своё, не трогая ни музыку, ни эффект.                           */
/*                                                                            */
/* Зачем делить одно устройство вообще: Switch имеет один вывод на процесс и  */
/* второй OpenAudioDevice отклоняет (мерено: «Audio device already open»), —   */
/* до динамика тогда доходит только открывший первым. PILOTS_SHARE_AUDIO=0/1   */
/* форсирует режим везде, Windows по умолчанию не делит (там звук фильма —     */
/* mciavi32, свой), Linux/Switch — делят (без микшера звука кино нет вовсе).   */
/* ========================================================================== */

static unsigned int MixRingFree(MixRing *ring)
{
    return SDL_MIX_RING - (ring->w - ring->r);
}

static int MixRing_New(MixRing *ring)
{
    ring->pcm = (short *)malloc(SDL_MIX_RING * 2 * sizeof(short));
    ring->w = 0;
    ring->r = 0;
    return ring->pcm != 0;
}

static void MixRing_Reset(MixRing *ring)
{
    ring->r = ring->w;               /* сбросить очередь, счётчики ведутся дальше */
}

/* Положить в кольцо N стерео-кадров из готового S16-буфера. Продюсер —
   синтез (SdlMidi_Pump на потоке таймера), потребитель — колбэк выше; ровно
   одна продюсерская запись на кольцо, читатель видит только кадры, дописанные
   до открутки w, так что volatile здесь и есть вся синхронизация. */
static unsigned int MixRingWrite(MixRing *ring, const short *frames, unsigned int n)
{
    unsigned int room = MixRingFree(ring);
    unsigned int i;
    unsigned int pos;

    if (n > room)
        n = room;
    pos = ring->w & SDL_MIX_MASK;
    for (i = 0; i < n; ++i)
    {
        ring->pcm[pos * 2] = frames[i * 2];
        ring->pcm[pos * 2 + 1] = frames[i * 2 + 1];
        pos = (pos + 1) & SDL_MIX_MASK;
    }
    ring->w += n;
    return n;
}

/* SDL-колбэк: читает кольца источников и пишет смешанный S16-поток. */
static void SdlAudioMix(void *userdata, Uint8 *stream, int len)
{
    short *out = (short *)stream;
    unsigned int frames = (unsigned int)len / 4;   /* два канала, два байта */
    unsigned int i;
#ifdef _DEBUG
    /* Учёт самого микшера: какой источник реально доходит до колбэка и с какой
       громкостью. «Музыка играет, эффекты — нет» бывает двух видов — в кольцо
       ничего не кладут или кладут неслышно малое, — а по логу консоли их не
       различить, потому-то и меряется. Пики — абсолютные значения смешиваемых
       сэмплов. */
    unsigned int waveFrames = 0, midiFrames = 0, movieFrames = 0;
    int wavePeak = 0, midiPeak = 0, moviePeak = 0, outPeak = 0;
    static unsigned int s_MixFrames;
    static unsigned int s_MixSumWave, s_MixSumMidi, s_MixSumMovie;  /* с прошлого отчёта */
    static int s_MixSumWavePeak, s_MixSumMidiPeak, s_MixSumMoviePeak, s_MixSumOutPeak;
    static unsigned s_MixLines;
    int v;
#endif

    (void)userdata;
    for (i = 0; i < frames; ++i)
    {
        int left = 0, right = 0;

        if (s_WaveRing.pcm && !s_WaveHeld && s_WaveRing.w != s_WaveRing.r)
        {
            unsigned int p = (s_WaveRing.r & SDL_MIX_MASK) * 2;
            left = s_WaveRing.pcm[p];
            right = s_WaveRing.pcm[p + 1];
            s_WaveRing.r += 1;
            s_WaveFramesEaten += 1;
#ifdef _DEBUG
            ++waveFrames;
            v = left < 0 ? -left : left;
            if (v > wavePeak)
                wavePeak = v;
            v = right < 0 ? -right : right;
            if (v > wavePeak)
                wavePeak = v;
#endif
        }
        if (s_MidiRing.pcm && s_MidiRing.w != s_MidiRing.r)
        {
            unsigned int p = (s_MidiRing.r & SDL_MIX_MASK) * 2;
            left += s_MidiRing.pcm[p];
            right += s_MidiRing.pcm[p + 1];
            s_MidiRing.r += 1;
#ifdef _DEBUG
            ++midiFrames;
            v = s_MidiRing.pcm[p] < 0 ? -s_MidiRing.pcm[p] : s_MidiRing.pcm[p];
            if (v > midiPeak)
                midiPeak = v;
#endif
        }
        if (s_MovieRing.pcm && s_MovieRing.w != s_MovieRing.r)
        {
            unsigned int p = (s_MovieRing.r & SDL_MIX_MASK) * 2;
            left += s_MovieRing.pcm[p];
            right += s_MovieRing.pcm[p + 1];
            s_MovieRing.r += 1;
#ifdef _DEBUG
            ++movieFrames;
            v = s_MovieRing.pcm[p] < 0 ? -s_MovieRing.pcm[p] : s_MovieRing.pcm[p];
            if (v > moviePeak)
                moviePeak = v;
#endif
        }
        if (left > 32767)
            left = 32767;
        else if (left < -32768)
            left = -32768;
        if (right > 32767)
            right = 32767;
        else if (right < -32768)
            right = -32768;
        out[i * 2] = (short)left;
        out[i * 2 + 1] = (short)right;
#ifdef _DEBUG
        /* И что уходит на устройство: пики источников могут быть честными, а
           сам поток — тихим (суммирование в переменную, которую не пишут,
           выглядит ровно как «источник никто не слышит»), — поэтому меряем
           там, где пишем. */
        {
            int o = left < 0 ? -left : left;
            int p = right < 0 ? -right : right;
            if (p > o)
                o = p;
            if (o > outPeak)
                outPeak = o;
        }
#endif
    }

#ifdef _DEBUG
    s_MixSumWave += waveFrames;
    s_MixSumMidi += midiFrames;
    s_MixSumMovie += movieFrames;
    if (wavePeak > s_MixSumWavePeak)
        s_MixSumWavePeak = wavePeak;
    if (midiPeak > s_MixSumMidiPeak)
        s_MixSumMidiPeak = midiPeak;
    if (moviePeak > s_MixSumMoviePeak)
        s_MixSumMoviePeak = moviePeak;
    if (outPeak > s_MixSumOutPeak)
        s_MixSumOutPeak = outPeak;
    s_MixFrames += frames;
    if (s_MixFrames >= SDL_MIX_RATE * 5u)         /* одна строка на 5 с звука */
    {
        /* Пишутся только окна, где была волна или кино, и потолок считает
           строки, а не время: иначе лимит выжжет музыка, а эффекты на консоли —
           то, ради чего смотрят, — начнутся позже, и все существующие строки
           будут говорить «wave 0». */
        if (s_MixLines < 40 && (s_MixSumWave || s_MixSumMovie))
        {
            ++s_MixLines;
            DBG_LOG("SdlAudioMix: %u s - wave %u frames (peak %d), midi %u frames (peak %d), movie %u frames (peak %d), out peak %d, rings %u/%u/%u frames",
                    (unsigned)(s_MixFrames / SDL_MIX_RATE), s_MixSumWave, s_MixSumWavePeak,
                    s_MixSumMidi, s_MixSumMidiPeak, s_MixSumMovie, s_MixSumMoviePeak, s_MixSumOutPeak,
                    s_WaveRing.w - s_WaveRing.r, s_MidiRing.w - s_MidiRing.r,
                    s_MovieRing.w - s_MovieRing.r);
        }
        s_MixFrames = 0;
        s_MixSumWave = 0;
        s_MixSumMidi = 0;
        s_MixSumMovie = 0;
        s_MixSumWavePeak = 0;
        s_MixSumMidiPeak = 0;
        s_MixSumMoviePeak = 0;
        s_MixSumOutPeak = 0;
    }
#endif
}

/* Делить одно устройство или нет. На Switch это не выбор (один вывод на
   процесс). Остальное — по умолчанию вслед за тем, что доступно: на Windows
   mciavi32 играет звук фильма сам, там двух-устройственный путь остаётся; на
   Linux и Switch звук кино существует только если существует микшер, там
   деление — по умолчанию. PILOTS_SHARE_AUDIO=0 или =1 принудительно в обе
   стороны — так измеряется shared-путь там, где вместо динамика число в логе. */
static int SdlShareAudioWanted(void)
{
#ifdef __SWITCH__
    return 1;
#else
    static int s_Share = -1;
    if (s_Share < 0)
    {
        char *e = getenv("PILOTS_SHARE_AUDIO");
#ifdef _WIN32
        s_Share = 0;
#else
        s_Share = 1;
#endif
        if (e && *e)
            s_Share = (*e != '0');
        DBG_LOG("SdlShareAudioWanted: one device for all sources = %d (platform default, PILOTS_SHARE_AUDIO='%s')",
                s_Share, e ? e : "");
    }
    return s_Share;
#endif
}

/* Первая попросившая сторона открывает единственное устройство, остальные
   пользуются им же. Оба формата — синтезовые, конвертация игровой PCM там,
   откуда она приходит. */
static int SdlMix_Open(const char *who)
{
    SDL_AudioSpec want;
    SDL_AudioSpec have;

    if (s_MixDev)
        return 1;
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
    {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        {
            DBG_LOG("SdlMix_Open: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return 0;
        }
    }
    SDL_zero(want);
    want.freq = SDL_MIX_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = SDL_MIX_SAMPLES;
    want.callback = SdlAudioMix;
    s_MixDev = SDL_OpenAudioDevice(NULL, SDL_FALSE, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
    if (!s_MixDev)
    {
        DBG_LOG("SdlMix_Open: %s could not open the shared device: %s", who, SDL_GetError());
        return 0;
    }
    if (have.freq != SDL_MIX_RATE || have.format != AUDIO_S16SYS || have.channels != 2)
    {
        DBG_LOG("SdlMix_Open: shared device gave %d Hz / format 0x%X / %d ch, this mixer only sums S16 stereo",
                have.freq, (unsigned int)have.format, have.channels);
        SDL_CloseAudioDevice(s_MixDev);
        s_MixDev = 0;
        return 0;
    }
    if (!s_WaveRing.pcm && !MixRing_New(&s_WaveRing))
        return 0;
    if (!s_MidiRing.pcm && !MixRing_New(&s_MidiRing))
        return 0;
    s_SharedAudio = 1;
    SDL_PauseAudioDevice(s_MixDev, 0);
    /* Имя драйвера и номер устройства идут с строкой успеха, потому что пик
       внутри колбэка доказывает работу микшера, а не то, что байты доходят до
       динамика: второе устройство там, где вывод один, или запасной драйвер —
       это ровно «колбэк работает, а тишина идёт». Имя драйвера и есть то, что
       различает два случая по логу. */
    DBG_LOG("SdlMix_Open: one device for both sources (%s asked first), %d Hz / %d ch / S16, %u frames per ring; driver '%s', device #%u, playing=%d",
            who, have.freq, have.channels, SDL_MIX_RING,
            SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)",
            (unsigned int)s_MixDev, (int)SDL_GetAudioDeviceStatus(s_MixDev));
    return 1;
}

/* Звук фильма, третий источник микшера. Своей AVI-читки у игры нет звука
   вообще, а mciavi32 на этих платформах не существует, — mci_ffmpeg.c читает
   аудио-чанки сам, пейся их по уже показанным кадрам, и сдаёт байты сюда. */
int SdlMix_MovieOpen(unsigned int rate)
{
    if (!rate || !SdlShareAudioWanted() || !SdlMix_Open("movie"))
    {
        DBG_LOG("SdlMix_MovieOpen: %u Hz refused - the movie's sound only exists through the mixer", rate);
        return 0;
    }
    if (!s_MovieRing.pcm && !MixRing_New(&s_MovieRing))
        return 0;
    MixRing_Reset(&s_MovieRing);
    s_MovieRate = rate;
    s_MovieSrcFrames = 0;
    DBG_LOG("SdlMix_MovieOpen: movie PCM at %u Hz into the mixer at %d Hz", rate, SDL_MIX_RATE);
    return 1;
}

/* Пересчёт частоты точными границами: выходной кадр o играет сэмпл источника
   floor(o * rate / SDL_MIX_RATE), т.е. каждый сэмпл копируется столько раз,
   сколько выходных кадров началось внутри него. Против 44100 игровые 22050 и
   11025 дают ровно 2 и 4 копии; другое отношение деградирует до повтора
   ближайшего сэмпла — как волна и так делает на своём фиксированном
   отношении. Возвращает число сэмплов источника, взятых меньше спрошенного,
   когда кольцо заполнено: остальное вызывающий оставит на следующий pump. */
unsigned int SdlMix_MovieWrite(const unsigned char *src, unsigned int len)
{
    unsigned int room = MixRingFree(&s_MovieRing);
    unsigned int pos = s_MovieRing.w & SDL_MIX_MASK;
    unsigned int emitted = 0;
    unsigned int i;

    if (!s_MovieRing.pcm || !s_MovieRate)
        return 0;
    for (i = 0; i < len; ++i)
    {
        unsigned int from = (unsigned int)(((unsigned long long)s_MovieSrcFrames * SDL_MIX_RATE) / s_MovieRate);
        unsigned int to = (unsigned int)(((unsigned long long)(s_MovieSrcFrames + 1) * SDL_MIX_RATE) / s_MovieRate);
        unsigned int n = to - from;
        short s = (short)(((int)src[i] - 128) * 257);
        unsigned int k;

        if (n > room)
            break;
        for (k = 0; k < n; ++k)
        {
            s_MovieRing.pcm[pos * 2] = s;
            s_MovieRing.pcm[pos * 2 + 1] = s;
            pos = (pos + 1) & SDL_MIX_MASK;
        }
        room -= n;
        emitted += n;
        ++s_MovieSrcFrames;
    }
    s_MovieRing.w += emitted;
    return i;
}

void SdlMix_MovieClose(void)
{
    if (s_MovieRing.pcm)
        MixRing_Reset(&s_MovieRing);   /* skip-кино: своё сбросить, чужое не тронуть */
    s_MovieRate = 0;
    s_MovieSrcFrames = 0;
}

/* ========================================================================== */
/* MIDI: музыка игры через libADLMIDI (OPL3-эмуляция) вместо midiOut           */
/*                                                                            */
/* Волна игры уже идёт через этот же шов (waveOut -> win32_shim.c -> SdlWave), */
/* и midiOut устроен так же: wave.c держит протокол WinMM байт в байт (от-     */
/* крытие, 10-мс timeSetEvent, midiOutShortMsg, сброс), а за ним стоит synth.  */
/* Поток тактовой линии: SDL_AddTimer (win32_shim.c) гоняет GmMidi_TimerProc   */
/* на своём потоке, тот разбирает дорожку и шлёт слова сюда, а SdlMidi_Pump в  */
/* том же тике (ShimTimerProc, win32_shim.c) подмазывает PCM в очередь — как  */
/* в v0.1, где таймерный колбэк «рендерит PCM поверх событий». Состояние       */
/* синтеза трогают оба потока (таймерный — ShortMsg/Pump, игровой — Open/      */
/* Close/Reset), поэтому каждая точка входа берёт s_MidiCS. Критическая секция */
/* заводится один раз и не удаляется никогда: таймерный колбэк может быть в    */
/* полёте, пока игра рушит звук (у оригинала была та же безобидная гонка с      */
/* закрытым midiOut-хэндлом, а удалённая CS была бы UB).                       */
/*                                                                            */
/* Отступления от v0.1 ровно два, оба — из устройства:                         */
/*  - у v0.1 флаг s_MidiFeed стоял между StartMidiPlayback и ResetMidi, а здесь */
/*    SdlMidi_Pump зовут только из ShimTimerProc, т.е. только пока жив таймер   */
/*    трека (Play..Halt) — сам таймер и есть этот флаг;                         */
/*  - громкость музыки v0.1 применял к PCM (midiOutSetVolume приходил из        */
/*    audio.c), а port-main применяет её к силе нот в wave.c и midiOutSetVolume  */
/*    не зовёт вовсе — масштабирование в Pump было бы мёртвым кодом.            */
/* ========================================================================== */

#define SDL_MIDI_RATE          44100
#define SDL_MIDI_BANK          68  /* «WOPL (4op bank by James Alan Nguyen and Wohlstand)», GM-набор;
                                      0 = AIL «The Fat Man» (заводской дефолт); см. adl_getBankNames() */
#define SDL_MIDI_CHUNK_SAMPLES 1024 /* adl_generate считает сэмплы, кадр = два: 512 кадров ~ 11.6 мс */
#define SDL_MIDI_QUEUE_TARGET  (128 * 1024) /* ~0.7 с PCM запаса поверх провалов игрового цикла */

static struct ADL_MIDIPlayer *s_Synth;
static CRITICAL_SECTION s_MidiCS;
static int s_MidiCSReady;
static short s_MidiChunk[SDL_MIDI_CHUNK_SAMPLES];
static unsigned int s_MidiMsgCount;      /* диагностика: сообщений с открытия/последнего сброса */
static int s_MidiFirstMsgLogged;
static int s_MidiFirstAudioLogged;

static void MidiLock(void)
{
    if (s_MidiCSReady)
        EnterCriticalSection(&s_MidiCS);
}

static void MidiUnlock(void)
{
    if (s_MidiCSReady)
        LeaveCriticalSection(&s_MidiCS);
}

/* Куда идёт готовый PCM синтеза: в устройство своё — в его очередь, в дележе —
   в кольцо микшера. Две формы одного и того же, чтобы три места, которым это
   знать, спросили один вопрос, а не проверяли оба условия каждый раз. */
static int MidiSinkReady(void)
{
    return s_MidiDev || (s_SharedAudio && s_MidiRing.pcm != 0);
}

static void MidiSinkClear(void)
{
    if (s_MidiDev)
        SDL_ClearQueuedAudio(s_MidiDev);
    else if (s_SharedAudio && s_MidiRing.pcm)
        MixRing_Reset(&s_MidiRing);
}

static int MidiSinkWrite(const short *frames, unsigned int n)
{
    if (s_MidiDev)
        return SDL_QueueAudio(s_MidiDev, frames, n * 2 * sizeof(short)) == 0;
    return MixRingWrite(&s_MidiRing, frames, n) == n;
}

static unsigned int MidiSinkBacklog(void)
{
    if (s_MidiDev)
        return SDL_GetQueuedAudioSize(s_MidiDev);
    return (s_MidiRing.w - s_MidiRing.r) * 4;  /* байты — те же единицы, что у очереди SDL */
}

/* midiOutOpen: открыть синтез. Неудача обязана быть неудачей: провал сюда ->
   GmMidi_Open откатывается, и игра вообще не начнёт дорожку — это другое (и
   худшее) поведение, чем тихая музыка. */
int SdlMidi_Open(void)
{
    SDL_AudioSpec want;
    SDL_AudioSpec have;

    if (s_Synth)
        return 1;
    if (!s_MidiCSReady)
    {
        InitializeCriticalSection(&s_MidiCS);
        s_MidiCSReady = 1;
    }
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
    {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        {
            DBG_LOG("SdlMidi_Open: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return 0;
        }
    }
    SDL_zero(want);
    want.freq = SDL_MIDI_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = 0;                   /* push-режим: PCM подмазывает SdlMidi_Pump */
    if (SdlShareAudioWanted())
    {
        /* Устройства своего нет: у консоли один вывод на процесс, а формат
           микшера и есть этот — PCM синтезу некуда конвертировать, нужно лишь
           кольцо для суммирования. Кто первый — тот открыл устройство. */
        if (!SdlMix_Open("midi"))
            return 0;
    }
    else
    {
        s_MidiDev = SDL_OpenAudioDevice(0, 0, &want, &have, 0); /* без подмены формата */
        if (!s_MidiDev)
        {
            DBG_LOG("SdlMidi_Open: SDL_OpenAudioDevice failed: %s", SDL_GetError());
            return 0;
        }
        if (have.freq != SDL_MIDI_RATE || have.format != AUDIO_S16SYS || have.channels != 2)
        {
            DBG_LOG("SdlMidi_Open: device gave %d Hz / 0x%X / %dch, refusing", have.freq, have.format, have.channels);
            SDL_CloseAudioDevice(s_MidiDev);
            s_MidiDev = 0;
            return 0;
        }
    }
    s_Synth = adl_init(SDL_MIDI_RATE);
    if (!s_Synth)
    {
        DBG_LOG("SdlMidi_Open: adl_init failed: %s", adl_errorString());
        if (s_MidiDev)
        {
            SDL_CloseAudioDevice(s_MidiDev);
            s_MidiDev = 0;
        }
        return 0;
    }
    if (adl_switchEmulator(s_Synth, ADLMIDI_EMU_NUKED) < 0)
        DBG_LOG("SdlMidi_Open: Nuked emulator unavailable (%s), keeping %s",
                adl_errorString(), adl_chipEmulatorName(s_Synth));
    if (adl_setBank(s_Synth, SDL_MIDI_BANK) < 0)
        DBG_LOG("SdlMidi_Open: bank %d unavailable (%s), keeping the default one",
                SDL_MIDI_BANK, adl_errorString());
    s_MidiMsgCount = 0;
    s_MidiFirstMsgLogged = 0;
    s_MidiFirstAudioLogged = 0;
    if (s_MidiDev)
        SDL_PauseAudioDevice(s_MidiDev, 0);
    DBG_LOG("SdlMidi_Open: libADLMIDI ready: emu=%s bank=%d rate=%d%s",
            adl_chipEmulatorName(s_Synth), SDL_MIDI_BANK, SDL_MIDI_RATE,
            s_SharedAudio ? ", sharing the wave device" : "");
    return 1;
}

/* midiOutClose: гаснуть (идемпотентно — ShutdownMultimedia мог закрыть раньше). */
void SdlMidi_Close(void)
{
    MidiLock();
    if (s_Synth)
    {
        adl_close(s_Synth);
        s_Synth = 0;
    }
    if (s_SharedAudio && s_MidiRing.pcm)
        MixRing_Reset(&s_MidiRing);      /* устройство общее: SdlBackendShutdown закроет его */
    if (s_MidiDev)
    {
        SDL_CloseAudioDevice(s_MidiDev);
        s_MidiDev = 0;
    }
    MidiUnlock();
}

/* midiOutReset: снять все звучащие ноты и выбросить уже отрендеренный PCM —
   GmMidi_Halt зовёт его уже убитым таймером, так что идущих Pump'ов после
   сброса не будет (кроме одного в полёте — он уйдёт в тишину паникнутого
   синтеза, ровно как у оригинала с закрытым устройством). */
void SdlMidi_Reset(void)
{
    MidiLock();
    if (s_Synth)
        adl_panic(s_Synth);
    if (MidiSinkReady())
        MidiSinkClear();
    if (s_MidiMsgCount)
    {
        /* Тайминг-зависимая диагностика, а НЕ инвариант: печатается только если
           10-мс таймер успел прокрутиться с прошлого сброса, поэтому между
           спаренными Stop/Start одна архитектура может молчать, пока другая
           поймает тик (и наоборот). Мерялось под одним скриптом кликов: обе
           архитектуры выдают идентичную последовательность сбросов и
           идентичные счётчики — различается лишь тик в щели stop/start.
           Не принимать за проверку архитектуры. */
        DBG_LOG("SdlMidi_Reset: %u messages processed", s_MidiMsgCount);
        s_MidiMsgCount = 0;
    }
    MidiUnlock();
}

/* Слово из GmMidi_Pump, упакованное как midiOutShortMsg: status | data1<<8 | data2<<16. */
void SdlMidi_ShortMsg(unsigned int packed)
{
    unsigned int status = packed & 0xFF;
    unsigned int data1 = (packed >> 8) & 0xFF;
    unsigned int data2 = (packed >> 16) & 0xFF;
    unsigned int channel = status & 0x0F;

    MidiLock();
    if (s_Synth)
    {
        ++s_MidiMsgCount;
        if (!s_MidiFirstMsgLogged)
        {
            s_MidiFirstMsgLogged = 1;
            DBG_LOG("SdlMidi_ShortMsg: first message 0x%02X", status);
        }
        switch (status & 0xF0)
        {
        case 0x80:                         /* note off (velocity отпускания в data2 отбрасывается) */
            adl_rt_noteOff(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
            break;
        case 0x90:                         /* note on; velocity 0 == note off, как в оригинале */
            if (data2)
                adl_rt_noteOn(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1, (ADL_UInt8)data2);
            else
                adl_rt_noteOff(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
            break;
        case 0xA0:                         /* полифонический aftertouch */
            adl_rt_noteAfterTouch(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1, (ADL_UInt8)data2);
            break;
        case 0xB0:                         /* control change */
            adl_rt_controllerChange(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1, (ADL_UInt8)data2);
            break;
        case 0xC0:                         /* program change (2-байтовое) */
            adl_rt_patchChange(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
            break;
        case 0xD0:                         /* channel aftertouch (2-байтовое) */
            adl_rt_channelAfterTouch(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
            break;
        case 0xE0:                         /* pitch bend: data1 = LSB, data2 = MSB */
            adl_rt_pitchBendML(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data2, (ADL_UInt8)data1);
            break;
        default:                           /* системные сюда не доходят (GmMidi_Pump их не шлёт) */
            break;
        }
    }
    MidiUnlock();
}

/* Подмазать PCM в очередь. Зовётся из ShimTimerProc (win32_shim.c) каждые
   10 мс, т.е. только пока жив таймер трека: этот поток кормит синтез даже
   когда игровой цикл занят загрузкой, а глубина очереди гасит дрожание
   таймера. Цикл доводит очередь до цели (саморегуляция: полная — пропуск,
   пустая — доливка не больше 16 чанков за тик). */
void SdlMidi_Pump(void)
{
    unsigned int queued;
    int guard = 0;
    int generated;

    MidiLock();
    if (!s_Synth || !MidiSinkReady())
    {
        MidiUnlock();
        return;
    }
    queued = MidiSinkBacklog();
    while (queued < SDL_MIDI_QUEUE_TARGET && guard < 16)
    {
        int i;
        generated = adl_generate(s_Synth, SDL_MIDI_CHUNK_SAMPLES, s_MidiChunk);
        if (generated <= 0)
            break;
        if (!s_MidiFirstAudioLogged)
        {
            /* Тоже тайминг-зависимое, не инвариант архитектуры: число ненулевых
               сэмплов в ПЕРВОМ чанке зависит от того, сколько MIDI-событий
               успело дойти до синтеза, т.е. от того, когда впервые выстрелил
               10-мс таймер. Тот же бинарь даёт 937 или 941 от прогона к
               прогону; накатавший 10 с холостых старт даёт 937 на ОБЕИХ
               архитектурах. Архитектурно-независимы — лог SdlMidi_Open
               (emu / bank / rate) и status-байт первого сообщения. */
            int nonzero = 0;
            for (i = 0; i < generated; ++i)
            {
                if (s_MidiChunk[i] != 0)
                    ++nonzero;
            }
            if (nonzero)
            {
                s_MidiFirstAudioLogged = 1;
                DBG_LOG("SdlMidi_Pump: first audible PCM (%d of %d samples nonzero)", nonzero, generated);
            }
        }
        if (!MidiSinkWrite(s_MidiChunk, (unsigned int)generated / 2))
            break;
        queued += (unsigned int)generated * sizeof(short);
        ++guard;
    }
    MidiUnlock();
}

/* Игровая PCM входит, микшер-кадры добавляются: один U8-сэмпл становится
   SDL_MIX_RATIO одинаковыми стерео-кадрами. Возвращает число игровых байт,
   взятых; полное кольцо отдаёт хвост, — это и есть «устройство отстало»
   двух-устройственно пути с игровой стороны. */
static unsigned int SdlWaveAppendShared(const unsigned char *src, unsigned int len)
{
    unsigned int samples = MixRingFree(&s_WaveRing) / SDL_MIX_RATIO;
    unsigned int pos;
    unsigned int i;

    if (len > samples)
        len = samples;
    pos = s_WaveRing.w & SDL_MIX_MASK;
    for (i = 0; i < len; ++i)
    {
        /* Сэмпл сам, а не «128 плюс громкость»: первая версия этой строки
           стартовала с 128 и лишь двигалась на громкость — каждый выход был 0,
           счётная активность при глухом звуке, пик 0 в пробе микшера, — так это
           и поймали. Громкость игра применяет к U8-сэмплам сама (wave.c),
           waveOutSetVolume — заглушка. */
        short s = (short)(((int)src[i] - 128) * 257);   /* та же распаковка U8->S16, что у SDL */
        unsigned int k;

        for (k = 0; k < SDL_MIX_RATIO; ++k)
        {
            s_WaveRing.pcm[pos * 2] = s;
            s_WaveRing.pcm[pos * 2 + 1] = s;
            pos = (pos + 1) & SDL_MIX_MASK;
        }
    }
    s_WaveRing.w += len * SDL_MIX_RATIO;
    s_WaveFramesQueued += len * SDL_MIX_RATIO;
    return len;
}

/* ========================================================================== */
/* SDL-устройство волны за waveOut-шимом (win32_shim.c)                       */
/*                                                                            */
/* Формат жёстко игровой: 11025 Гц, моно, U8 — wave.c другого не шлёт.        */
/* Устройство без колбэка: волна идёт в очередь, а дозрели заголовков         */
/* WinShim_WavePump меряет по SDL_GetQueuedAudioSize.                          */
/* ========================================================================== */

int SdlWave_Open(void)
{
    SDL_AudioSpec want, have;

    if (s_WaveDev)
        return 1;
    if (SdlShareAudioWanted())
    {
        /* Своего устройства здесь не будет: на этой платформе второго не открыть,
           а формат микшера не игровой — и не надо, конвертер SdlWaveAppendShared. */
        return SdlMix_Open("wave");
    }
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO))
    {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        {
            DBG_LOG("SdlWave_Open: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
            return 0;
        }
    }
    SDL_zero(want);
    want.freq = 11025;                           /* формат волны игры: 11025/U8/моно */
    want.format = AUDIO_U8;
    want.channels = 1;
    want.samples = 1024;
    want.callback = NULL;                        /* очередь, а не колбэк */
    s_WaveDev = SDL_OpenAudioDevice(NULL, SDL_FALSE, &want, &have, 0);
    if (!s_WaveDev)
    {
        DBG_LOG("SdlWave_Open: SDL_OpenAudioDevice failed: %s", SDL_GetError());
        return 0;
    }
    if (have.freq != want.freq || have.format != want.format || have.channels != want.channels)
    {
        DBG_LOG("SdlWave_Open: device gave %d Hz/%d/%d, want %d/%d/%d - closing",
                have.freq, have.format, have.channels,
                want.freq, want.format, want.channels);
        SDL_CloseAudioDevice(s_WaveDev);
        s_WaveDev = 0;
        return 0;
    }
    SDL_PauseAudioDevice(s_WaveDev, 0);
    DBG_LOG("SdlWave_Open: %d Hz, %d channels, %d samples/buffer",
            have.freq, have.channels, have.samples);
    return 1;
}

void SdlWave_Close(void)
{
    if (s_WaveDev)
    {
        SDL_CloseAudioDevice(s_WaveDev);
        s_WaveDev = 0;
    }
}

int SdlWave_Queue(const void *data, unsigned int len)
{
    if (!data || !len)
        return 0;
    if (s_SharedAudio)
    {
        unsigned int taken = SdlWaveAppendShared((const unsigned char *)data, len);

        if (taken != len)
        {
            /* Потребитель не поспевает: хвост буфера некуда класть. Каждый сброс
               с потолком в лог: одна просадка — неровность, сброс за буфером при
               полном кольце — уже отказ звука, и лимит их различает. */
            static unsigned int s_WaveDropLines;

            if (s_WaveDropLines < 40)
            {
                ++s_WaveDropLines;
                DBG_LOG("SdlWave_Queue: shared ring full, %u of %u game bytes dropped (drop line %u of 40)",
                        (unsigned)(len - taken), len, s_WaveDropLines);
            }
        }
        return taken == len;
    }
    if (!s_WaveDev)
        return 0;
    return SDL_QueueAudio(s_WaveDev, data, len) == 0;
}

void SdlWave_Clear(void)
{
    if (s_SharedAudio)
    {
        MixRing_Reset(&s_WaveRing);
        s_WaveFramesQueued = s_WaveFramesEaten;   /* очередь = 0, счётчики идут дальше */
        return;
    }
    if (s_WaveDev)
        SDL_ClearQueuedAudio(s_WaveDev);
}

void SdlWave_Pause(int pause)
{
    if (s_SharedAudio)
    {
        /* Удержание в микшере, а не на устройстве: то же устройство несёт
           музыку, а игра просит остановить эффекты, не саундтрек. Пока держим,
           колбэк кольцо не читает, — игровой счётчик догоняющего звука (его
           протокол завершения) замирает вместе с ним и после снятия паузы
           продолжается с того же места, ничего не потеряно. */
        s_WaveHeld = pause ? 1 : 0;
        return;
    }
    if (s_WaveDev)
        SDL_PauseAudioDevice(s_WaveDev, pause ? 1 : 0);
}

unsigned int SdlWave_Queued(void)
{
    if (s_SharedAudio)
        /* Игровые единицы (байты 11025/моно/U8): WAVEHDR в шиме считаются ими —
           четыре микшер-кадра на игровой байт, разность двух бегущих счётчиков
           (она и переживает их переполнение). */
        return (s_WaveFramesQueued - s_WaveFramesEaten) / SDL_MIX_RATIO;
    return s_WaveDev ? SDL_GetQueuedAudioSize(s_WaveDev) : 0;
}

/* ========================================================================== */
/* Кадр фильма (mci_ffmpeg.c) и мелкие хуки для win32_shim.c                  */
/* ========================================================================== */

/* Показать один кадр MCI-воспроизведения: под игрой (если теневой кадр грязный)
   и поверх — сам кадр в прямоугольнике окна AVI-ребёнка. Прямоугольник здесь
   игровой/оконный без пересчёта: без полноэкранного layout они совпадают. */
void SdlBackendVideoFrame(const void *bgra, int srcw, int srch, int x, int y, int w, int h)
{
    SDL_Rect dst;

    if (!s_Renderer || !bgra || srcw <= 0 || srch <= 0)
        return;
    if (w <= 0)
        w = srcw;                                /* MoveWindow(0×0) = натуральный размер */
    if (h <= 0)
        h = srch;
    if (!s_VideoTex || s_VideoTexW != srcw || s_VideoTexH != srch)
    {
        if (s_VideoTex)
            SDL_DestroyTexture(s_VideoTex);
        s_VideoTex = SDL_CreateTexture(s_Renderer, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING, srcw, srch);
        s_VideoTexW = srcw;
        s_VideoTexH = srch;
        if (!s_VideoTex)
        {
            DBG_ERR("SdlBackendVideoFrame: SDL_CreateTexture failed: %s", SDL_GetError());
            return;
        }
    }
    SDL_UpdateTexture(s_VideoTex, NULL, bgra, srcw * 4);
    if (s_Dirty)
        SdlBackendFlushPresent();                /* подложка (BMP-фон фильма) под кадром */
    /* Прямоугольник приходит в px игры (это то, во что MoveWindow поставил
       MCI-окно на Windows), а рендерер рисует в px окна — в обычном окне это
       тождественность, в fullscreen кадр должен попасть в rect картинки. */
    {
        int x2 = x + w;
        int y2 = y + h;
        SdlBackendGameToClient(&x, &y);
        SdlBackendGameToClient(&x2, &y2);
        dst.x = x;
        dst.y = y;
        dst.w = x2 - x;
        dst.h = y2 - y;
    }
    SDL_RenderCopy(s_Renderer, s_VideoTex, NULL, &dst);
    SDL_RenderPresent(s_Renderer);
}

void SdlBackendShowWindow(int show)
{
    if (!s_Window)
        return;
    if (show)
    {
        SDL_ShowWindow(s_Window);
        SDL_RaiseWindow(s_Window);
    }
    else
        SDL_HideWindow(s_Window);
}

int SdlBackendIsMinimized(void)
{
    return s_Window && (SDL_GetWindowFlags(s_Window) & SDL_WINDOW_MINIMIZED) != 0;
}

/* Стили оригинального окна: фиксированное окно с рамкой и заголовком. */
DWORD SdlBackendGetWinStyle(void)
{
    return 0x020A0000u;                          /* WS_OVERLAPPED | WS_VISIBLE | WS_CAPTION | WS_SYSMENU */
}

void SdlBackendSetCursorVisible(int visible)
{
    SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

void SdlBackendGetClientSize(int *w, int *h)
{
    int W = 0, H = 0;
    if (s_Window)
        SDL_GetWindowSize(s_Window, &W, &H);
    if (w)
        *w = W;
    if (h)
        *h = H;
}

/* Окно и игра в одном и том же пикселе: fullscreen-масштабирование не
   перенесено, до тех пор обе стороны — тождественные преобразования. */

/* Счётчик опросов указателя для профиля лупы — профиль тоже приедет отдельно. */
void SdlBackendNoteFramePoll(void)
{
}

/* Индикатор загрузки не перенесён; сейчас байты просто проходят мимо. */
void SdlBackendLoadBeat(unsigned int bytes)
{
    (void)bytes;
}

#endif /* !_WIN32 */
