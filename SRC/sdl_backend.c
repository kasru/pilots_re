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

#ifndef _WIN32
static SDL_Texture *s_VideoTex;     /* кадр фильма (SdlBackendVideoFrame) */
static int s_VideoTexW, s_VideoTexH;
static SDL_AudioDeviceID s_WaveDev; /* SDL-устройство за waveOut-шимом (win32_shim.c) */
static volatile sig_atomic_t s_Usr1Quit;
#endif

/* ========================================================================== */
/* Оконная процедура: игровой набор сообщений → Host_MainWndProc, остальное → SDL */
/* ========================================================================== */

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

    /* Размер и мин/макс рамки: сначала игра (обновляет clientWidth/Height,
       ограничивает рамку), затем SDL, чтобы его bookkeeping размера совпал. */
    if (msg == WM_SIZE || msg == WM_GETMINMAXINFO)
    {
        Host_MainWndProc(hWnd, msg, wParam, lParam);
        return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
    }

    if (SdlIsGameMessage(msg))
        return Host_MainWndProc(hWnd, msg, wParam, lParam);

    /* WM_WINDOWPOSCHANGED и всё прочее отдаём SDL: тот внутри вызовет
       DefWindowProc, который пришлёт WM_SIZE назад в этот же подкласс. */
    return CallWindowProcA(s_OrigWndProc, hWnd, msg, wParam, lParam);
}

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
    /* Сначала волна: SDL_CloseAudioDevice дожидается коллбэка, дальше можно
       гасить видео. */
    if (s_WaveDev)
    {
        SDL_CloseAudioDevice(s_WaveDev);
        s_WaveDev = 0;
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

/* Показать теневой кадр, если он грязный (иначе no-op). Один RenderPresent
   на кадр; Host_Flush после уже показанного кадра ничего не делает. */
void SdlBackendFlushPresent(void)
{
    if (!s_Texture || !s_Renderer || !s_Shadow || !s_Dirty)
        return;
    SDL_UpdateTexture(s_Texture, NULL, s_Shadow, s_TexW * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(s_Renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_Renderer);
    SDL_RenderCopy(s_Renderer, s_Texture, NULL, NULL);
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

/* Разбор командной строки. Заглушка: разбор (в т.ч. -datapath) добавляется отдельно. */
void SdlBackendParseCmdLine(const char *cmdLine)
{
    (void)cmdLine;
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
            WPARAM vk = SdlKeysymToVk(ev->key.keysym.sym);
            if (!vk)
                break;
            PostMessageA((HWND)g_MainWindow,
                         ev->type == SDL_KEYDOWN ? WM_KEYDOWN : WM_KEYUP,
                         vk, 0);
        }
        break;

    case SDL_MOUSEMOTION:
        /* WM_SETCURSOR перед каждым движением — так приходит настоящая мышь и
           так игра знает, что курсор в клиенте. */
        PostMessageA((HWND)g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
        PostMessageA((HWND)g_MainWindow, WM_MOUSEMOVE, 0,
                     (LPARAM)(((DWORD)(Uint16)ev->motion.y << 16) | (Uint16)ev->motion.x));
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

            if (right)
                msg = ev->type == SDL_MOUSEBUTTONDOWN ? WM_RBUTTONDOWN : WM_RBUTTONUP;
            else
                msg = ev->type == SDL_MOUSEBUTTONDOWN ? WM_LBUTTONDOWN : WM_LBUTTONUP;
            if (ev->type == SDL_MOUSEBUTTONDOWN)
                wp = right ? 2 : 1;              /* MK_RBUTTON / MK_LBUTTON */
            PostMessageA((HWND)g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
            PostMessageA((HWND)g_MainWindow, msg, wp,
                         (LPARAM)(((DWORD)(Uint16)ev->button.y << 16) | (Uint16)ev->button.x));
        }
        break;

    case SDL_WINDOWEVENT:
        if (ev->window.event == SDL_WINDOWEVENT_CLOSE && g_MainWindow)
            PostMessageA((HWND)g_MainWindow, WM_CLOSE, 0, 0);
        break;

    default:
        break;
    }
}

/* Слить SDL в очередь шима: события -> WM_*, затем соник-службы — фильм
   (WinShim_VideoPump, mci_ffmpeg.c) и волна (WinShim_WavePump, win32_shim.c).
   Именно сюда встаёт и главный цикл игры, и модальный цикл Host_PlayAvi. */
int WinShim_PumpSDLToQueue(void)
{
    SDL_Event ev;

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
    if (!s_WaveDev || !data || !len)
        return 0;
    return SDL_QueueAudio(s_WaveDev, data, len) == 0;
}

void SdlWave_Clear(void)
{
    if (s_WaveDev)
        SDL_ClearQueuedAudio(s_WaveDev);
}

void SdlWave_Pause(int pause)
{
    if (s_WaveDev)
        SDL_PauseAudioDevice(s_WaveDev, pause ? 1 : 0);
}

unsigned int SdlWave_Queued(void)
{
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
    dst.x = x;
    dst.y = y;
    dst.w = w;
    dst.h = h;
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
void SdlBackendClientToGame(int *x, int *y)
{
    (void)x;
    (void)y;
}

void SdlBackendGameToClient(int *x, int *y)
{
    (void)x;
    (void)y;
}

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
