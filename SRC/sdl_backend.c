/*
 * SDL2-шов (фаза P1 миграции): окно, презент кадра, ввод и палитра через SDL.
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
static bool s_PresentedOnce;        /* первый показ уже случился (маркер P1) */
static WNDPROC s_OrigWndProc;       /* wndproc SDL до подкласса */

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

/* Инициализировать SDL (видео). Истина — успех. */
int SdlBackendStartup(void)
{
    SDL_SetMainReady();             /* главная функция наша, не SDL_main */
    if (SDL_Init(SDL_INIT_VIDEO) != 0)
    {
        DBG_ERR("SDL_Init failed: %s", SDL_GetError());
        return 0;
    }
    DBG_LOG("SDL video driver: %s", SDL_GetCurrentVideoDriver());
    return 1;
}

/* Уничтожить рендерер, окно и завершить SDL. */
void SdlBackendShutdown(void)
{
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
    SDL_SysWMinfo wmi;
    HWND hwnd;
    Uint32 flags = SDL_WINDOW_SHOWN;

    if (resizable)
        flags |= SDL_WINDOW_RESIZABLE;

    s_Window = SDL_CreateWindow(title ? title : "", SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED, clientW, clientH, flags);
    if (!s_Window)
    {
        DBG_ERR("SDL_CreateWindow failed: %s", SDL_GetError());
        return NULL;
    }

    SDL_VERSION(&wmi.version);
    if (!SDL_GetWindowWMInfo(s_Window, &wmi))
    {
        DBG_ERR("SDL_GetWindowWMInfo failed: %s", SDL_GetError());
        return NULL;
    }
    hwnd = (HWND)wmi.info.win.window;

    /* Подкласс: сохраняем wndproc SDL, свои сообщения перехватываем. */
    s_OrigWndProc = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)SdlGameWndProc);

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

/* Выбросить накопленные SDL-события. Игровой ввод идёт Win32-сообщениями через
   подкласс, поэтому здесь просто дренируем очередь SDL. */
void SdlBackendPumpEvents(void)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev))
    {
        /* Пусто: WM_CLOSE/WM_DESTROY игра получает через подклассwndproc. */
    }
}

/* Разбор командной строки. В P1 — заглушка (фичи порта придут в P5). */
void SdlBackendParseCmdLine(const char *cmdLine)
{
    (void)cmdLine;
}
