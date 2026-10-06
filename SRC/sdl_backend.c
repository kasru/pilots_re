#include "pilots_fwd.h"
#include <SDL_syswm.h>

#pragma pack(push, 2)

/* ---- SDL2 backend ----
   Replaces the original GDI presentation path (CreateDIBSection + BitBlt/StretchDIBits +
   SetDIBColorTable) with an SDL texture + renderer, and subclasses SDL's window proc so the
   game's own MainWndProc/VideoWndProc keep receiving the raw Win32 messages (VK codes,
   WM_CHAR, WM_PAINT update rects) exactly as the original did.
   Also wraps the SDL queue-audio device; audio.c keeps the original 8 x 2048 byte WAVEHDR
   protocol on top of it (SdlWavePump stands in for the MM_WOM_DONE callback).
   MIDI music (audio.c) and AVI playback (video.c) still use WinMM/MCI. */

static SDL_Window *s_SdlWindow;
static SDL_Renderer *s_Renderer;
static SDL_Texture *s_Texture;
static int s_TexWidth;                          /* texture (backing store) size in pixels */
static int s_TexHeight;
static WNDPROC s_OrigWndProc;                   /* SDL's original window proc, before our subclass */

static unsigned int s_PalRGB[256];              /* palette LUT: 8-bit index -> 0xAARRGGBB */
static unsigned int *s_Stage;                   /* staging buffer for rect conversion */
static int s_StagePixels;

static SDL_AudioDeviceID s_WaveDev;             /* 11025 Hz / AUDIO_U8 / mono queue-audio device */
static int s_WaveVolume = 255;                  /* 0..255, applied at queue time (u8 PCM centered at 128) */

/* Window proc subclass: game messages first, SDL for everything else.
   The message sets of MainWndProc and VideoWndProc (MainWndProc self-delegates to
   VideoWndProc while a movie plays) are handled by the game only; everything else goes to
   SDL's proc, which ends in DefWindowProc - same default behavior the game had. */
static LRESULT CALLBACK SdlGameWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  if ( (uMsg >= WM_KEYDOWN && uMsg <= WM_KEYLAST)      /* 0x0100-0x0109: keyboard incl. WM_CHAR */
    || (uMsg >= WM_LBUTTONDOWN && uMsg <= WM_RBUTTONDBLCLK) /* 0x0201-0x0205: mouse buttons */
    || uMsg == WM_SETFOCUS || uMsg == WM_KILLFOCUS || uMsg == WM_ACTIVATE
    || uMsg == WM_CLOSE || uMsg == WM_DESTROY || uMsg == WM_QUIT )
    DBG_LOG("SdlGameWndProc: msg=0x%X wp=0x%X lp=0x%X", uMsg, (unsigned)wParam, (unsigned)lParam);
  switch ( uMsg )
  {
    case WM_PAINT:                              /* dirty rect -> AddDirtyRegion + DefWindowProc (validated) */
    case WM_QUERYNEWPALETTE:
    case WM_PALETTECHANGED:
    case WM_KEYDOWN:                            /* VK codes from game data match wParam natively */
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_WINDOWPOSCHANGED:                   /* VideoWndProc repaint path; WM_SIZE/WM_MOVE are re-dispatched from it */
    case WM_MOUSEACTIVATE:
    case MM_MCINOTIFY:                          /* MCI movie callbacks */
    case MM_WOM_DONE:                           /* legacy waveOut callback (dead with SDL, kept faithful) */
    case WM_CLOSE:                              /* game destroys the window -> WM_DESTROY -> SDL_QUIT */
      return MainWndProc(hWnd, uMsg, wParam, lParam);

    case WM_CHAR:
      if ( (unsigned int)wParam > 255 )
      {
        /* SDL windows are Unicode; the original ANSI window delivered cp1251 bytes in wParam */
        WCHAR wch = (WCHAR)wParam;
        char ansi = '?';
        WideCharToMultiByte(CP_ACP, 0, &wch, 1, &ansi, 1, 0, 0);
        wParam = (WPARAM)(unsigned char)ansi;
      }
      return MainWndProc(hWnd, uMsg, wParam, lParam);

    case WM_SETCURSOR:
      if ( LOWORD(lParam) == HTCLIENT )         /* client area: game cursor (GetGameCursor), no SDL */
        return MainWndProc(hWnd, uMsg, wParam, lParam);
      return CallWindowProc(s_OrigWndProc, hWnd, uMsg, wParam, lParam);

    case WM_GETMINMAXINFO:                      /* game clamps min size to the game resolution, SDL adds its own */
    case WM_SIZE:                               /* game records client size, SDL tracks it too */
      MainWndProc(hWnd, uMsg, wParam, lParam);
      return CallWindowProc(s_OrigWndProc, hWnd, uMsg, wParam, lParam);

    case WM_DESTROY:                            /* game PostQuitMessage(0); push SDL_QUIT so the poll loop exits too */
      {
        LRESULT gameResult = MainWndProc(hWnd, uMsg, wParam, lParam);
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = SDL_QUIT;
        SDL_PushEvent(&ev);
        return gameResult;
      }

    default:
      return CallWindowProc(s_OrigWndProc, hWnd, uMsg, wParam, lParam);
  }
}

int SdlBackendStartup(void)
{
  if ( SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0 )
  {
    DBG_LOG("SdlBackendStartup: SDL_Init failed: %s", SDL_GetError());
    if ( SDL_InitSubSystem(SDL_INIT_VIDEO) != 0 )
    {
      DBG_LOG("SdlBackendStartup: SDL_InitSubSystem(VIDEO) failed: %s", SDL_GetError());
      return 0;
    }
  }
  return 1;
}

void SdlBackendShutdown(void)
{
  if ( s_Texture )
  {
    SDL_DestroyTexture(s_Texture);
    s_Texture = 0;
  }
  if ( s_Renderer )
  {
    SDL_DestroyRenderer(s_Renderer);
    s_Renderer = 0;
  }
  if ( s_SdlWindow )
  {
    /* CleanupHelpAndWindow already destroyed the HWND; SDL just frees its side (DestroyWindow on
       the stale handle fails benignly) */
    SDL_DestroyWindow(s_SdlWindow);
    s_SdlWindow = 0;
  }
  s_OrigWndProc = 0;
  if ( s_Stage )
  {
    free(s_Stage);
    s_Stage = 0;
    s_StagePixels = 0;
  }
  if ( s_WaveDev )
  {
    SDL_CloseAudioDevice(s_WaveDev);
    s_WaveDev = 0;
  }
  SDL_Quit();
}

/* Create the game window hidden (original CreateWindowEx style had no WS_VISIBLE - the window
   is shown by the game's own ShowWindow calls in InitDisplay/PlayIntroVideo), grab the native
   HWND for g_MainWindow and subclass over SDL's wndproc. x/y == CW_USEDEFAULT maps to
   SDL_WINDOWPOS_UNDEFINED. Returns the HWND, 0 on failure. */
HWND SdlBackendCreateWindow(int x, int y, int clientW, int clientH, int resizable, LPCSTR title)
{
  Uint32 flags = SDL_WINDOW_HIDDEN;
  SDL_SysWMinfo wmi;
  HWND hwnd;

  if ( !s_SdlWindow )
  {
    if ( resizable )
      flags |= SDL_WINDOW_RESIZABLE;
    s_SdlWindow = SDL_CreateWindow(
        title ? title : "",
        x == (int)CW_USEDEFAULT ? SDL_WINDOWPOS_UNDEFINED : x,
        y == (int)CW_USEDEFAULT ? SDL_WINDOWPOS_UNDEFINED : y,
        clientW, clientH, flags);
    if ( !s_SdlWindow )
    {
      DBG_LOG("SdlBackendCreateWindow: SDL_CreateWindow failed: %s", SDL_GetError());
      return 0;
    }
  }
  SDL_VERSION(&wmi.version);
  if ( !SDL_GetWindowWMInfo(s_SdlWindow, &wmi) || wmi.subsystem != SDL_SYSWM_WINDOWS )
  {
    DBG_LOG("SdlBackendCreateWindow: SDL_GetWindowWMInfo failed: %s", SDL_GetError());
    return 0;
  }
  hwnd = wmi.info.win.window;
  s_OrigWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SdlGameWndProc);
  if ( resizable )                              /* SDL's WM_GETMINMAXINFO handler preserves this */
    SDL_SetWindowMinimumSize(s_SdlWindow, clientW, clientH);
  return hwnd;
}

/* Cursor visibility must agree with GetGameCursor: the game toggles the shape with
   SetCursor from WM_SETCURSOR, SDL toggles the global ShowCursor counter here. */
void SdlBackendSetCursorVisible(int visible)
{
  SDL_ShowCursor(visible ? SDL_ENABLE : SDL_DISABLE);
}

int SdlBackendCreateVideoTargets(int clientW, int clientH)
{
  if ( !s_SdlWindow || clientW <= 0 || clientH <= 0 )
    return 0;
  if ( !s_Renderer )
  {
    /* No vsync: BlitRectToScreen presents after every blit (dissolve tiles must appear one by one) */
    s_Renderer = SDL_CreateRenderer(s_SdlWindow, -1, 0);
    if ( !s_Renderer )
      s_Renderer = SDL_CreateRenderer(s_SdlWindow, -1, SDL_RENDERER_SOFTWARE);
    if ( !s_Renderer )
    {
      DBG_LOG("SdlBackendCreateVideoTargets: SDL_CreateRenderer failed: %s", SDL_GetError());
      return 0;
    }
    SDL_SetRenderDrawColor(s_Renderer, 0, 0, 0, 255);
  }
  if ( s_Texture )
  {
    SDL_DestroyTexture(s_Texture);
    s_Texture = 0;
  }
  s_Texture = SDL_CreateTexture(s_Renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, clientW, clientH);
  if ( !s_Texture )
  {
    DBG_LOG("SdlBackendCreateVideoTargets: SDL_CreateTexture failed: %s", SDL_GetError());
    return 0;
  }
  s_TexWidth = clientW;
  s_TexHeight = clientH;
  return 1;
}

/* Show the current texture (1:1, top-left - mirrors the original BitBlt which never scaled).
   Outside the texture the renderer is cleared to black, like the window background. */
static void SdlPresentCurrent(void)
{
  SDL_Rect dst;

  if ( !s_Texture || !s_Renderer )
    return;
  SDL_SetRenderDrawColor(s_Renderer, 0, 0, 0, 255);
  SDL_RenderClear(s_Renderer);
  dst.x = 0;
  dst.y = 0;
  dst.w = s_TexWidth;
  dst.h = s_TexHeight;
  SDL_RenderCopy(s_Renderer, s_Texture, 0, &dst);
  SDL_RenderPresent(s_Renderer);
}

/* Convert an 8-bit bottom-up rect through the palette LUT, upload it and present.
   Mirrors the original BitBlt/StretchDIBits(srcY = bmp_height - y - cy) row flip. */
void SdlBackendPresentRect(const void *bits, int bmp_height, int stride, int x, int y, int w, int h)
{
  const unsigned char *src = (const unsigned char *)bits;
  unsigned int *dst;
  SDL_Rect r;
  int row;

  if ( !s_Texture || !bits || stride <= 0 || bmp_height <= 0 )
    return;
  /* Clip to the texture (and the source buffer), like BitBlt clipping to the window DC */
  if ( x < 0 )
  {
    w += x;
    x = 0;
  }
  if ( y < 0 )
  {
    h += y;
    y = 0;
  }
  if ( w > stride - x )
    w = stride - x;
  if ( h > bmp_height - y )
    h = bmp_height - y;
  if ( x + w > s_TexWidth )
    w = s_TexWidth - x;
  if ( y + h > s_TexHeight )
    h = s_TexHeight - y;
  if ( w <= 0 || h <= 0 )
    return;

  if ( w * h > s_StagePixels )
  {
    if ( s_Stage )
      free(s_Stage);
    s_Stage = (unsigned int *)malloc((size_t)w * h * 4);
    s_StagePixels = s_Stage ? w * h : 0;
    if ( !s_Stage )
      return;
  }
  dst = s_Stage;
  for ( row = 0; row < h; ++row )
  {
    /* bottom-up source: screen row y+row lives at buffer row bmp_height-1-(y+row) */
    const unsigned char *srow = src + stride * (bmp_height - 1 - (y + row)) + x;
    int col;
    for ( col = 0; col < w; ++col )
      *dst++ = s_PalRGB[srow[col]];
  }
  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  SDL_UpdateTexture(s_Texture, &r, s_Stage, w * 4);
  SdlPresentCurrent();
}

/* Black the whole screen (original: FillRect(window, BLACK_BRUSH) in SetPalette) and present. */
void SdlBackendClearBlack(void)
{
  if ( !s_Texture )
    return;
  if ( s_TexWidth * s_TexHeight > s_StagePixels )
  {
    if ( s_Stage )
      free(s_Stage);
    s_Stage = (unsigned int *)malloc((size_t)s_TexWidth * s_TexHeight * 4);
    s_StagePixels = s_Stage ? s_TexWidth * s_TexHeight : 0;
    if ( !s_Stage )
      return;
  }
  {
    int i;
    for ( i = 0; i < s_TexWidth * s_TexHeight; ++i )
      s_Stage[i] = 0xFF000000;
  }
  SDL_UpdateTexture(s_Texture, 0, s_Stage, s_TexWidth * 4);
  SdlPresentCurrent();
}

/* Debug grid overlay (SetPalette a2 != 0 branch; g_DisplayMode[0] is never written, so dead
   in practice - kept as renderer lines instead of the original GDI pen grid). */
void SdlBackendDrawGrid(unsigned int colorRef)
{
  int i;

  if ( !s_Texture || !s_Renderer )
    return;
  SdlPresentCurrent();                          /* draw over the current content, like GDI did */
  SDL_SetRenderDrawColor(s_Renderer,
      (unsigned)(colorRef & 0xFF),
      (unsigned)((colorRef >> 8) & 0xFF),
      (unsigned)((colorRef >> 16) & 0xFF), 255);
  for ( i = 0; i < s_TexWidth; i += 8 )
    SDL_RenderDrawLine(s_Renderer, i, 0, i, s_TexHeight - 1);
  for ( i = 0; i < s_TexHeight; i += 8 )
    SDL_RenderDrawLine(s_Renderer, 0, i, s_TexWidth - 1, i);
  SDL_RenderPresent(s_Renderer);
}

/* Palette LUT (replaces SetDIBColorTable): entries are what the DIB color table got, i.e.
   already system-palette-overridden by SetPalette when a1 is set. */
void SdlBackendUpdatePalette(const PALETTEENTRY *entries, int count)
{
  int i;

  if ( count > 256 )
    count = 256;
  for ( i = 0; i < count; ++i )
    s_PalRGB[i] = 0xFF000000
        | ((unsigned int)entries[i].peRed << 16)
        | ((unsigned int)entries[i].peGreen << 8)
        | (unsigned int)entries[i].peBlue;
}

/* Message pump for the game loops: SDL_PollEvent pumps the Win32 queue (SDL internally does
   PeekMessage/TranslateMessage/DispatchMessage, so our wndproc subclass sees every message).
   Returns 1 when SDL_QUIT was received (original: WM_QUIT, Msg.message == 18). */
int SdlBackendPumpEvents(void)
{
  SDL_Event ev;
  int quit = 0;

  while ( SDL_PollEvent(&ev) )
  {
    if ( ev.type == SDL_QUIT )
      quit = 1;
  }
  SdlWavePump();
  return quit;
}

/* ---- SDL wave device (replaces waveOutOpen) ---- */

int SdlWave_Open(void)
{
  SDL_AudioSpec want;
  SDL_AudioSpec have;

  if ( s_WaveDev )
    return 1;
  if ( !(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) )
  {
    if ( SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 )
    {
      DBG_LOG("SdlWave_Open: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
      return 0;
    }
  }
  SDL_zero(want);
  want.freq = 11025;                            /* identical to the original WAVEFORMATEX */
  want.format = AUDIO_U8;
  want.channels = 1;
  want.samples = 1024;
  want.callback = 0;                            /* push mode: audio.c drains it via SdlWavePump */
  s_WaveDev = SDL_OpenAudioDevice(0, 0, &want, &have, 0); /* no format changes allowed */
  if ( !s_WaveDev )
  {
    DBG_LOG("SdlWave_Open: SDL_OpenAudioDevice failed: %s", SDL_GetError());
    return 0;
  }
  if ( have.freq != 11025 || have.format != AUDIO_U8 || have.channels != 1 )
  {
    DBG_LOG("SdlWave_Open: device gave %d Hz / 0x%X / %dch, refusing", have.freq, have.format, have.channels);
    SDL_CloseAudioDevice(s_WaveDev);
    s_WaveDev = 0;
    return 0;
  }
  s_WaveVolume = 255;                           /* waveOut devices started at full volume */
  SDL_PauseAudioDevice(s_WaveDev, 0);
  return 1;
}

void SdlWave_Close(void)
{
  if ( s_WaveDev )
  {
    SDL_CloseAudioDevice(s_WaveDev);
    s_WaveDev = 0;
  }
}

/* Queue one buffer (SDL_QueueAudio copies it, so the WAVEHDR/pcm buffer is reusable at once). */
int SdlWave_Queue(const void *data, unsigned int len)
{
  const unsigned char *src = (const unsigned char *)data;
  static unsigned char *s_Scaled;
  static unsigned int s_ScaledCap;
  unsigned int i;

  if ( !s_WaveDev || !data )
    return 0;
  if ( s_WaveVolume != 255 )
  {
    if ( len > s_ScaledCap )
    {
      free(s_Scaled);
      s_Scaled = (unsigned char *)malloc(len);
      s_ScaledCap = s_Scaled ? len : 0;
      if ( !s_Scaled )
        return 0;
    }
    for ( i = 0; i < len; ++i )
    {
      int v = 128 + (((int)src[i] - 128) * s_WaveVolume) / 255;
      s_Scaled[i] = (unsigned char)v;
    }
    src = s_Scaled;
  }
  return SDL_QueueAudio(s_WaveDev, src, len) == 0;
}

void SdlWave_Clear(void)
{
  if ( s_WaveDev )
    SDL_ClearQueuedAudio(s_WaveDev);
}

void SdlWave_Pause(int pause)
{
  if ( s_WaveDev )
    SDL_PauseAudioDevice(s_WaveDev, pause ? 1 : 0);
}

unsigned int SdlWave_Queued(void)
{
  return s_WaveDev ? SDL_GetQueuedAudioSize(s_WaveDev) : 0;
}

int SdlWave_IsOpen(void)
{
  return s_WaveDev != 0;
}

void SdlWave_SetVolume(int vol)
{
  if ( vol < 0 )
    vol = 0;
  if ( vol > 255 )
    vol = 255;
  s_WaveVolume = vol;
}

int SdlWave_GetVolume(void)
{
  return s_WaveVolume;
}

#pragma pack(pop)
