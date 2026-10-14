#include "pilots_fwd.h"
#ifdef _WIN32
#include <SDL_syswm.h>
#endif
#include "adlmidi.h"

#pragma pack(push, 2)

/* ---- SDL2 backend ----
   Replaces the original GDI presentation path (CreateDIBSection + BitBlt/StretchDIBits +
   SetDIBColorTable) with an SDL texture + renderer. Two platform halves for message
   delivery to the game's MainWndProc/VideoWndProc:
     - Windows: subclasses SDL's window proc so the game keeps receiving the raw Win32
       messages (VK codes, WM_CHAR, WM_PAINT update rects) exactly as the original did;
     - Linux:   there is no Win32 queue - SdlDispatchSdlEvent translates SDL events into
       WM_* messages and posts them through win32_shim's PostMessageA; PeekMessageA/
       DispatchMessageA (the game's own loops + SdlBackendPumpEvents) drain that queue.
   Also wraps the SDL queue-audio devices; audio.c keeps the original 8 x 2048 byte WAVEHDR
   protocol on top of the wave one (SdlWavePump stands in for the MM_WOM_DONE callback).
   MIDI music renders through libADLMIDI (third_party/libADLMIDI, OPL3 emulation) into a
   second queue-audio device - only the winmm 10 ms event timer stays from the midiOut path.
   AVI playback (video.c) still uses MCI. */

static SDL_Window *s_SdlWindow;
static SDL_Renderer *s_Renderer;
static SDL_Texture *s_Texture;
static int s_TexWidth;                          /* texture (backing store) size in pixels */
static int s_TexHeight;
static SDL_Texture *s_VideoTex;                 /* MCI movie overlay (SdlBackendVideoFrame) */
static int s_VideoTexW;
static int s_VideoTexH;
#ifdef _WIN32
static WNDPROC s_OrigWndProc;                   /* SDL's original window proc, before our subclass */
#endif

static unsigned int s_PalRGB[256];              /* palette LUT: 8-bit index -> 0xAARRGGBB */
static unsigned int *s_Stage;                   /* staging buffer for rect conversion */
static int s_StagePixels;

/* Present coalescing. A dissolve sweeps ~1200 16x16 tiles and used to pay a full
   clear+copy+RenderPresent for every one of them - on X11 (llvmpipe GL in the
   container) that measured 5.7 s for a single scene transition. The
   palette->texture upload is cheap (~46 us a tile) and always happens right
   away; the present is deferred while less than PRESENT_MIN_INTERVAL_MS passed
   since the last one, and every message pump flushes the tail
   (SdlBackendPumpEvents entry+after dispatch, WinShim_PumpSDLToQueue entry).
   The original animated the dissolve tile by tile (GdiFlush inside the loop,
   commit ad1b246) - the checkerboard still steps here, just much faster:
   measured 7 presents / 86 ms on X11 instead of 1200 / 5663 ms, and 4 / 2.5 ms
   on the headless dummy driver instead of 1200 / 467 ms.
   Windows keeps the original present-per-blit (#ifndef _WIN32 at the defer site). */
#define PRESENT_MIN_INTERVAL_MS 1               /* burst presents no faster than ~1000/s */
static Uint32 s_LastPresentTick;                /* SDL_GetTicks of the last real present */
static int s_PresentPending;                    /* texture updated since, not yet shown */
static int s_GameClientW;                       /* the game's pinned logical client size */
static int s_GameClientH;                       /* - see SdlHandleResize: g_VideoConfig.clientWidth/Height
                                                      is zeroed by the game's own WM_SIZE on minimize
                                                      (SIZE_MINIMIZED -> 0,0) and cannot be the refill
                                                      source on restore */
static unsigned s_EvMotion;                     /* SDL mouse events seen (diagnostics) */
static unsigned s_EvEnter;                      /* SDL window ENTER events seen */

/* ---- borderless fullscreen (port feature: -fullscreen / Alt+Enter) ----
   Windowed - the original behavior - the picture IS the window: every transform here is
   the identity and nothing in the game sees a difference.
   Fullscreen (SDL_WINDOW_FULLSCREEN_DESKTOP) the window covers the desktop, so the game
   picture is scaled to fit it (aspect preserved) and centered: s_PicX/Y/W/H is where it
   lands in window client pixels, and mouse coordinates are mapped through it, so the game
   keeps working in its own 640x480 space - the letterbox around the picture behaves
   exactly like "the mouse left the window" in the original (no message is forwarded).
   Native movie mode: Windows plays a movie in a real mciavi32 child window which draws
   1:1 and cannot be scaled (video.c), so while one plays the picture keeps its natural
   size and only the centering offset applies - background and child stay aligned. */
static int s_Fullscreen;                         /* SDL_WINDOW_FULLSCREEN_DESKTOP requested */
static int s_NativeMovie;                        /* keep the picture at its natural size (Windows movie) */
static int s_PicX;                               /* picture origin, window client px */
static int s_PicY;
static int s_PicW;                               /* picture size, window client px */
static int s_PicH;

/* ---- debug overlay: interactive-object markers (port feature: hold Ctrl) ----
   Marks every object the engine's own click hit test would accept, so "where can I click
   here" is visible on any scene instead of guessed. It is a read-only walk over the live
   render stack, taken from the engine's own selection pass - the game is never asked to
   do anything differently, so the 32-bit byte-identical baseline is untouched.
   Ctrl was picked because it is free on both OSes: the game reads Shift as a key modifier
   (main.c: GetAsyncKeyState(16) adds 8 to keyIndex) and Alt+Enter is already the fullscreen
   toggle, plus a real window manager may swallow Alt before the game sees it. GetAsyncKeyState
   itself is the one keyboard call that answers the same way on both platforms - a real
   user32 on Windows, a SDL_GetModState shim on Linux (win32_shim.c) - which matters because
   on Windows the wndproc subclass hands WM_KEYDOWN straight to the game and SDL never
   updates its own modifier state there. */
#define HOTSPOT_MAX 128
static int s_HotspotHeld;                        /* Ctrl down at the last pump */
static int s_HotspotPrev;                        /* ... and at the pump before that */
static int s_HotspotCount;                       /* markers queued for the next draw */
static int s_HotspotX[HOTSPOT_MAX];              /* game client px, screen space */
static int s_HotspotY[HOTSPOT_MAX];

static void SdlUpdateLayout(void);               /* where the picture goes */
static void SdlApplyFullscreen(void);            /* SDL side of s_Fullscreen (no-op pre-window) */
static int SdlMapMousePoint(int *x, int *y);     /* client px -> game px; 0 = outside the picture */
static int SdlPictureTransformed(void);
static int SdlFloorDiv(int a, int b);
static void SdlCollectHotspots(void);            /* fill s_HotspotX/Y from the render stack */
static void SdlDrawHotspots(void);               /* white markers over the copied frame */
static void SdlUpdateHotspots(void);             /* sample Ctrl once per pump */

/* client coordinates packed the Win32 way: LOWORD = x, HIWORD = y (both platform halves) */
static LPARAM SdlPackPoint(int x, int y)
{
  return (LPARAM)(((unsigned int)(y & 0xFFFF) << 16) | (unsigned int)(x & 0xFFFF));
}

#ifdef _DEBUG
static unsigned s_RectBlits;                    /* PILOTS_DEBUG_DISSOLVE statistics */
static unsigned s_Presents;
static unsigned int s_PresentUs;                /* time spent inside SdlPresentCurrent, us */
#endif

static SDL_AudioDeviceID s_WaveDev;             /* 11025 Hz / AUDIO_U8 / mono queue-audio device */
static int s_WaveVolume = 255;                  /* 0..255, applied at queue time (u8 PCM centered at 128) */

#ifdef _WIN32
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
    case WM_MOUSEACTIVATE:
    case MM_MCINOTIFY:                          /* MCI movie callbacks */
    case MM_WOM_DONE:                           /* legacy waveOut callback (dead with SDL, kept faithful) */
    case WM_CLOSE:                              /* game destroys the window -> WM_DESTROY -> SDL_QUIT */
      return MainWndProc(hWnd, uMsg, wParam, lParam);

    case WM_WINDOWPOSCHANGED:                   /* VideoWndProc repaint path; WM_SIZE/WM_MOVE are re-dispatched from it */
      {
        /* The game first (it repaints from this message), then SDL's own proc. SDL has to see
           this one: it is the ONLY message from which SDL 2.30 learns a window's new size - its
           WM_SIZE handler sends just RESTORED/MINIMIZED/MAXIMIZED, while the RESIZED event that
           reaches the renderer (SDL_OnWindowResized -> WINDOWEVENT_SIZE_CHANGED ->
           D3D11_WindowEvent -> IDXGISwapChain_ResizeBuffers) is generated from
           WM_WINDOWPOSCHANGED alone. Skipping it leaves the swap chain at the size the window
           was CREATED with, while SDL_GetRendererOutputSize answers with the live client size -
           so the picture is drawn into a 640x480 backbuffer that DXGI stretches over the whole
           1920x1080 client: magnified 2.4x horizontally / 1.8x vertically and cut at the
           backbuffer's edge, which is what "-fullscreen" used to show. */
        LRESULT gameResult = MainWndProc(hWnd, uMsg, wParam, lParam);
        CallWindowProc(s_OrigWndProc, hWnd, uMsg, wParam, lParam);
        return gameResult;
      }

    case WM_LBUTTONDOWN:                        /* mouse: window client px -> game px (identity while windowed) */
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MOUSEMOVE:
      {
        int cx = (int)(short)LOWORD(lParam);
        int cy = (int)(short)HIWORD(lParam);
        int mx = cx;
        int my = cy;
        if ( !SdlMapMousePoint(&mx, &my) )
          return 0;                             /* letterbox: as if the mouse had left the window */
#ifdef _DEBUG
        if ( (uMsg == WM_LBUTTONDOWN || uMsg == WM_RBUTTONDOWN) && (mx != cx || my != cy) )
          DBG_LOG("SdlGameWndProc: 0x%X client (%d,%d) -> game (%d,%d)",
                  uMsg, cx, cy, mx, my);        /* only interesting while the picture is transformed */
#endif
        return MainWndProc(hWnd, uMsg, wParam, SdlPackPoint(mx, my));
      }

    case WM_SYSKEYDOWN:                         /* Alt+Enter: borderless fullscreen toggle (port feature) */
      if ( wParam == VK_RETURN )
      {
        SdlBackendToggleFullscreen();
        return 0;                               /* swallowed: the game must not see the Enter */
      }
      return CallWindowProc(s_OrigWndProc, hWnd, uMsg, wParam, lParam);

    case WM_SYSCHAR:                            /* TranslateMessage turns the swallowed Alt+Enter into this */
      if ( wParam == 13 )
        return 0;
      return CallWindowProc(s_OrigWndProc, hWnd, uMsg, wParam, lParam);

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
      /* fullscreen: nothing may pin the window to the game size, or the resize SDL is about
         to make (and every later one) gets clamped back to it */
      if ( !s_Fullscreen )
        MainWndProc(hWnd, uMsg, wParam, lParam);
      return CallWindowProc(s_OrigWndProc, hWnd, uMsg, wParam, lParam);

    case WM_SIZE:                               /* game records client size, SDL tracks it too */
#ifdef _DEBUG
      DBG_LOG("SdlGameWndProc: WM_SIZE wp=0x%X lp=%dx%d fs=%d", (unsigned)wParam,
              (int)(unsigned short)LOWORD(lParam), (int)(unsigned short)HIWORD(lParam), s_Fullscreen);
#endif
      /* fullscreen: the window size is the desktop's - the game keeps its own logical size,
         or clientWidth/Height becomes the clip bound past the fixed backbuffer */
      if ( s_Fullscreen )
        MainWndProc(hWnd, uMsg, wParam, SdlPackPoint(s_GameClientW, s_GameClientH));
      else
        MainWndProc(hWnd, uMsg, wParam, lParam);
      SdlUpdateLayout();                        /* the window is the fullscreen size now */
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

#else /* !_WIN32: synthesize Win32 messages from SDL events ---------------- */

static void SdlPresentCurrent(void);            /* defined below; re-presented on expose */
static void SdlFlushPresent(void);              /* flush a coalesced rect update (defined below) */

/* SDL scancode -> Win32 virtual key. Letter/numpad VKs follow the physical (US) key
   position - the same MapVirtualKey-based values the original game matched against. */
static unsigned int SdlScancodeToVK(SDL_Scancode sc)
{
  if ( sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z )
    return (unsigned int)('A' + (sc - SDL_SCANCODE_A));
  if ( sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9 )
    return (unsigned int)('1' + (sc - SDL_SCANCODE_1));
  if ( sc == SDL_SCANCODE_0 )
    return '0';
  if ( sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F24 )
    return (unsigned int)(VK_F1 + (sc - SDL_SCANCODE_F1));
  if ( sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9 )
    return (unsigned int)(0x61 + (sc - SDL_SCANCODE_KP_1));   /* VK_NUMPAD1..VK_NUMPAD9 */
  switch ( sc )
  {
    case SDL_SCANCODE_KP_0:      return 0x60;   /* VK_NUMPAD0 */
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6A;
    case SDL_SCANCODE_KP_PLUS:   return 0x6B;
    case SDL_SCANCODE_KP_MINUS:  return 0x6D;
    case SDL_SCANCODE_KP_DECIMAL: return 0x6E;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6F;
    case SDL_SCANCODE_RETURN:    return VK_RETURN;
    case SDL_SCANCODE_ESCAPE:    return VK_ESCAPE;
    case SDL_SCANCODE_BACKSPACE: return VK_BACK;
    case SDL_SCANCODE_TAB:       return VK_TAB;
    case SDL_SCANCODE_SPACE:     return VK_SPACE;
    case SDL_SCANCODE_PAGEUP:    return VK_PRIOR;
    case SDL_SCANCODE_PAGEDOWN:  return VK_NEXT;
    case SDL_SCANCODE_END:       return VK_END;
    case SDL_SCANCODE_HOME:      return VK_HOME;
    case SDL_SCANCODE_LEFT:      return VK_LEFT;
    case SDL_SCANCODE_UP:        return VK_UP;
    case SDL_SCANCODE_RIGHT:     return VK_RIGHT;
    case SDL_SCANCODE_DOWN:      return VK_DOWN;
    case SDL_SCANCODE_PRINTSCREEN: return VK_SNAPSHOT;
    case SDL_SCANCODE_INSERT:    return VK_INSERT;
    case SDL_SCANCODE_DELETE:    return VK_DELETE;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT:    return VK_SHIFT;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL:     return VK_CONTROL;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT:      return VK_MENU;
    case SDL_SCANCODE_PAUSE:     return VK_PAUSE;
    case SDL_SCANCODE_CAPSLOCK:  return VK_CAPITAL;
    case SDL_SCANCODE_NUMLOCKCLEAR: return VK_NUMLOCK;
    case SDL_SCANCODE_SCROLLLOCK: return VK_SCROLL;
    case SDL_SCANCODE_MINUS:     return 0xBD;   /* VK_OEM_MINUS */
    case SDL_SCANCODE_EQUALS:    return 0xBB;   /* VK_OEM_PLUS */
    case SDL_SCANCODE_LEFTBRACKET:  return 0xDB;
    case SDL_SCANCODE_RIGHTBRACKET: return 0xDD;
    case SDL_SCANCODE_SEMICOLON: return 0xBA;
    case SDL_SCANCODE_APOSTROPHE: return 0xDE;
    case SDL_SCANCODE_GRAVE:     return 0xC0;
    case SDL_SCANCODE_BACKSLASH: return 0xDC;
    case SDL_SCANCODE_COMMA:     return 0xBC;
    case SDL_SCANCODE_PERIOD:    return 0xBE;
    case SDL_SCANCODE_SLASH:     return 0xBF;
    default:                     return 0;
  }
}

/* SDL_TEXTINPUT is UTF-8; the ANSI window of the original delivered cp1251 bytes in
   WM_CHAR (Russian layout: U+0410..U+044F -> 0xC0..0xFF, Ё/ё -> 0xA8/0xB8) */
static char SdlTextToCp1251(const char *s)
{
  unsigned int cp = (unsigned char)s[0];

  if ( cp >= 0x80 )
  {
    if ( (cp & 0xE0u) == 0xC0u && (s[1] & 0xC0u) == 0x80u )
      cp = ((cp & 0x1Fu) << 6) | ((unsigned char)s[1] & 0x3Fu);
    else
      return '?';
  }
  if ( cp < 0x80 )
    return (char)cp;
  if ( cp == 0x0401 )
    return (char)0xA8;
  if ( cp == 0x0451 )
    return (char)0xB8;
  if ( cp >= 0x0410 && cp <= 0x044F )
    return (char)(0xC0 + (cp - 0x0410));
  return '?';
}

static void SdlPostChar(HWND hwnd, char ch)
{
  PostMessageA(hwnd, WM_CHAR, (WPARAM)(unsigned char)ch, 0);
}

#ifdef _DEBUG
/* SDL window event ids by name, for the SDL_WINDOWEVENT dispatch log */
static const char *SdlWindowEventName(unsigned char e)
{
  switch ( e )
  {
    case SDL_WINDOWEVENT_SHOWN:          return "SHOWN";
    case SDL_WINDOWEVENT_HIDDEN:         return "HIDDEN";
    case SDL_WINDOWEVENT_EXPOSED:        return "EXPOSED";
    case SDL_WINDOWEVENT_MOVED:          return "MOVED";
    case SDL_WINDOWEVENT_RESIZED:        return "RESIZED";
    case SDL_WINDOWEVENT_SIZE_CHANGED:   return "SIZE_CHANGED";
    case SDL_WINDOWEVENT_MINIMIZED:      return "MINIMIZED";
    case SDL_WINDOWEVENT_MAXIMIZED:      return "MAXIMIZED";
    case SDL_WINDOWEVENT_RESTORED:       return "RESTORED";
    case SDL_WINDOWEVENT_ENTER:          return "ENTER";
    case SDL_WINDOWEVENT_LEAVE:          return "LEAVE";
    case SDL_WINDOWEVENT_FOCUS_GAINED:   return "FOCUS_GAINED";
    case SDL_WINDOWEVENT_FOCUS_LOST:     return "FOCUS_LOST";
    case SDL_WINDOWEVENT_CLOSE:          return "CLOSE";
    default:                             return "?";
  }
}
#endif

/* One resize as the game sees it. The original window never changed size:
   WM_GETMINMAXINFO (MainWndProc/VideoWndProc case 0x24) pinned max track size and
   max size to the initial client rect, so every WM_SIZE carried exactly that size.
   The SDL window may still be resized behind our back (size hints are only hints -
   fullscreen ignores them), and a foreign size forwarded into
   g_VideoConfig.clientWidth/Height corrupts g_MouseX/g_MouseY - the RenderDirtyRegions
   clip bound; with a clip past the fixed backbuffer the cursor repaint
   (CopyRectToBackbuffer) walks off the buffer (SIGSEGV on maximize, before the
   sizes were decoupled). So the game only ever sees its own logical client size;
   the repaint (WM_WINDOWPOSCHANGED + InvalidateRect) still follows the real window.
   The logical size comes from s_GameClientW/H, NOT from g_VideoConfig: minimize
   makes the game store 0x0 (its WM_SIZE writes lParam unconditionally), so a refill
   sourced from that same field pins client=0x0 forever after restore - clip bound 0
   -> no dirty region ever renders (frozen picture, cursor gone; maximize/restore
   with a WM that unmaps on the way reproduces it). */
static void SdlHandleResize(HWND hwnd, int w, int h)
{
  DBG_LOG("SdlHandleResize: window %dx%d, game keeps %dx%d", w, h,
          s_GameClientW, s_GameClientH);
  SdlUpdateLayout();                           /* fullscreen: where the picture goes now */
  PostMessageA(hwnd, WM_WINDOWPOSCHANGED, 0, 0);
  PostMessageA(hwnd, WM_SIZE, 0, SdlPackPoint(s_GameClientW, s_GameClientH));
  InvalidateRect(hwnd, 0, 0);
}

static void SdlDispatchSdlEvent(const SDL_Event *ev)
{
  HWND hwnd = g_MainWindow;               /* 0 before CreateMainWindow: input events drop */

  switch ( ev->type )
  {
    case SDL_QUIT:                        /* signal / last window gone: the normal close path */
      if ( hwnd )
        PostMessageA(hwnd, WM_CLOSE, 0, 0);
      break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
      {
        unsigned int vk;
        if ( !hwnd )
          break;
        if ( ev->type == SDL_KEYDOWN
          && (ev->key.keysym.scancode == SDL_SCANCODE_RETURN
           || ev->key.keysym.scancode == SDL_SCANCODE_KP_ENTER)
          && (ev->key.keysym.mod & (KMOD_LALT | KMOD_RALT)) )
        {
          SdlBackendToggleFullscreen();         /* Alt+Enter: the game never sees the Enter */
          break;
        }
        vk = SdlScancodeToVK(ev->key.keysym.scancode);
        if ( !vk )
          break;
        PostMessageA(hwnd, ev->type == SDL_KEYDOWN ? WM_KEYDOWN : WM_KEYUP, vk, 0);
        /* Keys that never produce SDL_TEXTINPUT: emit the WM_CHAR that TranslateMessage
           would have generated on Windows. Autorepeat keydowns repeat the char too. */
        if ( ev->type == SDL_KEYDOWN )
          switch ( vk )
          {
            case VK_RETURN: SdlPostChar(hwnd, '\r'); break;
            case VK_ESCAPE: SdlPostChar(hwnd, 27);   break;
            case VK_TAB:    SdlPostChar(hwnd, '\t'); break;
            case VK_BACK:   SdlPostChar(hwnd, 8);    break;
            default: break;
          }
        break;
      }

    case SDL_TEXTINPUT:
      if ( hwnd )
        SdlPostChar(hwnd, SdlTextToCp1251(ev->text.text));
      break;

    case SDL_MOUSEMOTION:
      ++s_EvMotion;
      if ( !hwnd )
        break;
      {
        int mx = ev->motion.x;
        int my = ev->motion.y;
        if ( !SdlMapMousePoint(&mx, &my) )
          break;                            /* letterbox: as if the mouse had left the window */
        PostMessageA(hwnd, WM_SETCURSOR, (WPARAM)hwnd, HTCLIENT); /* LOWORD(lParam) == HTCLIENT */
        PostMessageA(hwnd, WM_MOUSEMOVE, 0, SdlPackPoint(mx, my));
      }
      break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      {
        UINT msg;
        if ( !hwnd )
          break;
        if ( ev->button.button == SDL_BUTTON_LEFT )
          msg = (ev->type == SDL_MOUSEBUTTONDOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        else if ( ev->button.button == SDL_BUTTON_RIGHT )
          msg = (ev->type == SDL_MOUSEBUTTONDOWN) ? WM_RBUTTONDOWN : WM_RBUTTONUP;
        else
          break;                          /* middle button: unhandled by the game, skip */
        {
          int bx = ev->button.x;
          int by = ev->button.y;
          int cx = bx;
          int cy = by;
          if ( !SdlMapMousePoint(&bx, &by) )
            break;                        /* click on the letterbox: nothing to hit, drop it */
#ifdef _DEBUG
          if ( ev->type == SDL_MOUSEBUTTONDOWN && (bx != cx || by != cy) )
            DBG_LOG("SdlDispatchSdlEvent: 0x%X client (%d,%d) -> game (%d,%d)",
                    msg, cx, cy, bx, by); /* only interesting while the picture is transformed */
#endif
          PostMessageA(hwnd, WM_SETCURSOR, (WPARAM)hwnd, HTCLIENT);
          PostMessageA(hwnd, msg, 0, SdlPackPoint(bx, by));
        }
        break;
      }

    case SDL_WINDOWEVENT:
      if ( !hwnd )
        break;
#ifdef _DEBUG
      DBG_LOG("SDL_WINDOWEVENT: %s (%d,%d)", SdlWindowEventName(ev->window.event),
              ev->window.data1, ev->window.data2);
#endif
      switch ( ev->window.event )
      {
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED:
          SdlHandleResize(hwnd, ev->window.data1, ev->window.data2);
          break;
        case SDL_WINDOWEVENT_EXPOSED:
          SdlPresentCurrent();            /* SDL keeps no backbuffer: re-present now, ... */
          InvalidateRect(hwnd, 0, 0);     /* ...let the game mark its dirty regions too */
          break;
        case SDL_WINDOWEVENT_MINIMIZED:   /* Win32 SIZE_MINIMIZED sends client 0,0 */
          DBG_LOG("MINIMIZED: post WM_SIZE 0,0 - the game client becomes 0x0");
          PostMessageA(hwnd, WM_SIZE, 0, 0);
          break;
        case SDL_WINDOWEVENT_RESTORED:         /* like SdlHandleResize: the game gets its logical size back, not the physical one */
          DBG_LOG("RESTORED: post WM_SIZE %dx%d", s_GameClientW, s_GameClientH);
          PostMessageA(hwnd, WM_SIZE, 0, SdlPackPoint(s_GameClientW, s_GameClientH));
          InvalidateRect(hwnd, 0, 0);
          break;
        case SDL_WINDOWEVENT_ENTER:
          ++s_EvEnter;
          PostMessageA(hwnd, WM_SETCURSOR, (WPARAM)hwnd, HTCLIENT);
          break;
        case SDL_WINDOWEVENT_CLOSE:
          PostMessageA(hwnd, WM_CLOSE, 0, 0);
          break;
        default:
          break;
      }
      break;

    default:
      break;
  }
}

/* SDL event drain -> the win32_shim message queue (called from PeekMessageA when empty) */
int WinShim_PumpSDLToQueue(void)
{
  SDL_Event ev;

  SdlFlushPresent();                            /* show rect updates coalesced since the last pump */
  while ( SDL_PollEvent(&ev) )
    SdlDispatchSdlEvent(&ev);
#ifdef _DEBUG
  static int s_ResizeSimDone;                  /* fires once per run */
  /* Headless resize simulation (containers have no window manager to maximize the
     window with): PILOTS_DEBUG_RESIZE=1 reproduces a WM maximize once the game
     texture exists and the window is up - physical SDL resize, the WM_SIZE path,
     and a cursor sitting over the enlarged window (the coordinate overflow that
     took the real maximize down). Fires once per run. */
  if ( !s_ResizeSimDone && s_Texture && g_MainWindow
       && s_SdlWindow && (SDL_GetWindowFlags(s_SdlWindow) & SDL_WINDOW_SHOWN) )
  {
    char *sim = getenv("PILOTS_DEBUG_RESIZE");
    s_ResizeSimDone = 1;
    if ( sim && *sim && *sim != '0' )
    {
      int w = g_VideoConfig.clientWidth * 2, h = g_VideoConfig.clientHeight * 2;
      SDL_Event mev;
      DBG_LOG("WinShim_PumpSDLToQueue: PILOTS_DEBUG_RESIZE simulates maximize to %dx%d", w, h);
      SDL_SetWindowSize(s_SdlWindow, w, h);        /* physical window, as the WM would */
      SdlHandleResize(g_MainWindow, w, h);         /* WM_SIZE path (events add a duplicate when the driver reports it) */
      memset(&mev, 0, sizeof mev);                 /* cursor over the enlarged window, as SDL would deliver it */
      mev.type = SDL_MOUSEMOTION;
      mev.motion.x = w - 8;
      mev.motion.y = h - 8;
      SdlDispatchSdlEvent(&mev);
    }
  }
  static int s_DissolveSimDone;                  /* fires once per run */
  /* Dissolve benchmark: PILOTS_DEBUG_DISSOLVE=1 replays one real scene transition
     (BlitScreenWithDissolve with the dissolve flag: 16 passes x 16x16 tiles on a
     64px grid, i.e. ~1280 BlitRectToScreen -> SdlBackendPresentRect calls) and logs
     how many of them reached the screen, how many actual presents fired and how
     long the whole sweep took - the slow-transition diagnosis. */
  if ( !s_DissolveSimDone && s_Texture && g_MainWindow
       && s_SdlWindow && (SDL_GetWindowFlags(s_SdlWindow) & SDL_WINDOW_SHOWN)
       && g_BackbufferPixels )
  {
    char *sim = getenv("PILOTS_DEBUG_DISSOLVE");
    s_DissolveSimDone = 1;
    if ( sim && *sim && *sim != '0' )
    {
      Uint64 t0, t1;
      unsigned rects0 = s_RectBlits, pres0 = s_Presents, us0 = s_PresentUs;
      DBG_LOG("WinShim_PumpSDLToQueue: PILOTS_DEBUG_DISSOLVE replays one scene transition");
      t0 = SDL_GetPerformanceCounter();
      BlitScreenWithDissolve(1, g_BackbufferHeight, g_BackbufferStride,
                             g_VideoConfig.clientHeight, g_VideoConfig.clientWidth,
                             0, 0, g_BackbufferPixels);
      SdlFlushPresent();                        /* coalesced tail: show it, count and time it too */
      t1 = SDL_GetPerformanceCounter();
      DBG_LOG("PILOTS_DEBUG_DISSOLVE: %u rect blits, %u presents, %u us in present, total %u us",
              s_RectBlits - rects0, s_Presents - pres0, s_PresentUs - us0,
              (unsigned)((t1 - t0) * 1000000u / SDL_GetPerformanceFrequency()));
    }
  }
#endif
  WinShim_VideoPump();                         /* MCI movie: decode/present/notify (mci_ffmpeg.c) */
  return 0;
}

/* Headless test hook: SIGUSR1 makes SdlBackendPumpEvents report "quit", so the game
   leaves its main loop through the normal WinMain LABEL_19 cleanup path - lets the
   full shutdown sequence be exercised without a window manager (scripts/diag_hang.sh pokes
   it from outside the container). */
static volatile sig_atomic_t s_Usr1Quit;
static void SdlSigUsr1Handler(int sig)
{
  (void)sig;
  s_Usr1Quit = 1;
}

#endif /* _WIN32 */

int SdlBackendStartup(void)
{
#ifndef _WIN32
  s_Usr1Quit = 0;
  signal(SIGUSR1, SdlSigUsr1Handler);
#endif
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

/* ---- borderless fullscreen (-fullscreen / Alt+Enter); see the statics at the top ---- */
#define SDL_FS_MAX_PIN 16384                     /* stands in for the window's max-size pin while fullscreen */

/* Signed division toward -inf: a mouse point left/above the picture must map to a negative
   game coordinate, not to 0 - the main loop tests it against the client rect. */
static int SdlFloorDiv(int a, int b)
{
  int q = a / b;
  if ( (a % b) != 0 && (a < 0) != (b < 0) )
    --q;
  return q;
}

static int SdlPictureTransformed(void)
{
  return s_PicW > 0 && s_PicH > 0
      && (s_PicX != 0 || s_PicY != 0 || s_PicW != s_GameClientW || s_PicH != s_GameClientH);
}

/* Where the picture goes. Windowed it is the window itself (identity - the original
   behavior). Fullscreen: a letterboxed box of the window, aspect preserved, centered; while
   a Windows movie plays, the picture's natural size centered instead - mciavi32 draws its
   child 1:1 and cannot follow a scaled background. */
static void SdlUpdateLayout(void)
{
  int w = s_GameClientW;
  int h = s_GameClientH;
  int ww, wh;
  int picW, picH;
  int newX, newY;
  int changed;

  if ( w <= 0 || h <= 0 )
    return;
  if ( !s_Fullscreen || !s_SdlWindow )
  {
    s_PicX = 0;
    s_PicY = 0;
    s_PicW = w;
    s_PicH = h;
    return;
  }
  /* The client rect, not SDL_GetWindowSize: on Windows SDL only learns the new size on
     WM_WINDOWPOSCHANGED, which arrives AFTER WM_SIZE - the layout is recomputed inside the
     WM_SIZE handler, where SDL still has the old size (and at startup the window is still
     hidden when -fullscreen is applied, so no size message has arrived at all). The client
     rect is also the space the mouse coordinates come in. */
  ww = wh = 0;
#ifdef _WIN32
  {
    RECT rc;
    if ( g_MainWindow && GetClientRect((HWND)g_MainWindow, &rc) )
    {
      ww = rc.right;
      wh = rc.bottom;
    }
  }
#endif
  if ( ww <= 0 || wh <= 0 )
    SDL_GetWindowSize(s_SdlWindow, &ww, &wh);
  if ( ww <= 0 || wh <= 0 )
    return;                                     /* minimized: keep the last layout */
  if ( s_NativeMovie )
  {
    picW = w;
    picH = h;
  }
  else if ( (long long)ww * h <= (long long)wh * w )
  {
    picW = ww;                                  /* window is the wider one: fit the width */
    picH = (int)((long long)ww * h / w);
    if ( picH < 1 )
      picH = 1;
  }
  else
  {
    picH = wh;                                  /* window is the taller one: fit the height */
    picW = (int)((long long)wh * w / h);
    if ( picW < 1 )
      picW = 1;
  }
  newX = (ww - picW) / 2;
  newY = (wh - picH) / 2;
  changed = (newX != s_PicX || newY != s_PicY || picW != s_PicW || picH != s_PicH);
  s_PicX = newX;
  s_PicY = newY;
  s_PicW = picW;
  s_PicH = picH;
  if ( changed )
  {
    /* The renderer output is the space SDL_RenderCopy actually draws in - on Windows it is
       NOT guaranteed to equal the client rect (DPI scaling, a backbuffer SDL has not resized
       yet). When it is smaller than ww/wh the copy gets clipped and the window shows only the
       top-left corner of the scaled picture, so log it next to the rect it must match. */
    int outW = 0, outH = 0;
    if ( s_Renderer && SDL_GetRendererOutputSize(s_Renderer, &outW, &outH) != 0 )
      outW = outH = 0;
    DBG_LOG("SdlUpdateLayout: window %dx%d -> picture %dx%d at (%d,%d)%s (renderer output %dx%d)",
            ww, wh, s_PicW, s_PicH, s_PicX, s_PicY,
            s_NativeMovie ? " (native movie)" : "", outW, outH);
#ifdef _WIN32
    VideoRealignMciChild();                     /* the movie child follows the picture rect */
#endif
  }
}

/* window client px -> game client px (identity while the picture is the window) */
void SdlBackendClientToGame(int *x, int *y)
{
  if ( !SdlPictureTransformed() )
    return;
  *x = SdlFloorDiv((*x - s_PicX) * s_GameClientW, s_PicW);
  *y = SdlFloorDiv((*y - s_PicY) * s_GameClientH, s_PicH);
}

/* game client px -> window client px (position of the MCI movie child on Windows) */
void SdlBackendGameToClient(int *x, int *y)
{
  if ( !SdlPictureTransformed() )
    return;
  *x = s_PicX + (int)((long long)*x * s_PicW / s_GameClientW);
  *y = s_PicY + (int)((long long)*y * s_PicH / s_GameClientH);
}

/* Mouse point for the game; returns 0 when it lands outside the picture, so the letterbox is
   dropped instead of sent - the game then behaves exactly as when the mouse left its window
   in the original (no message at all, input flag off in the main loop). */
static int SdlMapMousePoint(int *x, int *y)
{
  if ( !SdlPictureTransformed() )
    return 1;
  SdlBackendClientToGame(x, y);
  return *x >= 0 && *x < s_GameClientW && *y >= 0 && *y < s_GameClientH;
}

/* SDL side of s_Fullscreen. Idempotent, and a no-op while the window does not exist yet:
   WinMain records -fullscreen early, SdlBackendCreateVideoTargets applies it after
   CreateMainWindow's SetWindowPos (which would undo an earlier call). */
static void SdlApplyFullscreen(void)
{
  int want = s_Fullscreen ? 1 : 0;
  int isFs;

  if ( !s_SdlWindow )
    return;
  isFs = (SDL_GetWindowFlags(s_SdlWindow) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
  if ( want != isFs )
  {
    if ( want )
    {
      /* Both size pins have to go first: SDL's own max size (set when the window was created)
         and the game's WM_GETMINMAXINFO pin would clamp the fullscreen resize back to the game. */
      SDL_SetWindowMaximumSize(s_SdlWindow, SDL_FS_MAX_PIN, SDL_FS_MAX_PIN);
#ifndef _WIN32
      /* On X11 SDL's desktop fullscreen only ASKS the window manager
         (_NET_WM_STATE_FULLSCREEN ClientMessage) and then bumps its own idea of the window
         to the display bounds - it never resizes the window itself. With a WM that is
         enough; without one (bare Xvfb, which is every harness in tests/linux) the X window
         stays 640x480 at (320,120) while SDL reports 1280x720, so every picture rect and
         every mouse mapping is computed for a window that does not exist - proven against
         X by a standalone SDL probe with no game code in it. So push the geometry ourselves,
         and while the window is still windowed: SDL ignores SetWindowSize once the
         fullscreen flag is up. Borderless fullscreen IS "a display-sized window at (0,0)",
         with or without a WM; setfs below then only adds the hint and the flag. */
      {
        SDL_DisplayMode dm;
        int di = SDL_GetWindowDisplayIndex(s_SdlWindow);
        if ( di < 0 )
          di = 0;
        if ( SDL_GetDesktopDisplayMode(di, &dm) == 0 && dm.w > 0 && dm.h > 0 )
        {
          SDL_SetWindowPosition(s_SdlWindow, 0, 0);     /* SDL centers windows without a WM */
          SDL_SetWindowSize(s_SdlWindow, dm.w, dm.h);
          DBG_LOG("SdlApplyFullscreen: forced X geometry %dx%d at (0,0)", dm.w, dm.h);
        }
      }
#endif
      if ( SDL_SetWindowFullscreen(s_SdlWindow, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0 )
        DBG_LOG("SdlApplyFullscreen: SDL_SetWindowFullscreen(DESKTOP) failed: %s", SDL_GetError());
    }
    else
    {
      SDL_SetWindowFullscreen(s_SdlWindow, 0);
#ifndef _WIN32
      /* Mirror of the enter path: SDL restored the flag, but the windowed size it remembers
         is the full-screen one we forced - put the game's own window back. */
      if ( s_GameClientW > 0 && s_GameClientH > 0 )
      {
        SDL_SetWindowSize(s_SdlWindow, s_GameClientW, s_GameClientH);
        SDL_SetWindowPosition(s_SdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
      }
#endif
      if ( s_GameClientW > 0 && s_GameClientH > 0 )
        SDL_SetWindowMaximumSize(s_SdlWindow, s_GameClientW, s_GameClientH);
    }
    SdlUpdateLayout();
    DBG_LOG("SdlApplyFullscreen: %s -> picture %dx%d at (%d,%d)",
            want ? "on" : "off", s_PicW, s_PicH, s_PicX, s_PicY);
    if ( g_MainWindow )
      InvalidateRect((HWND)g_MainWindow, 0, 1); /* whole-window repaint at the new geometry */
  }
  SdlUpdateLayout();
}

void SdlBackendSetFullscreen(int on)
{
  int was = s_Fullscreen ? 1 : 0;

  on = on ? 1 : 0;
  if ( on == was && s_SdlWindow )
    return;
  s_Fullscreen = on;
  SdlApplyFullscreen();
}

int SdlBackendToggleFullscreen(void)
{
  SdlBackendSetFullscreen(!s_Fullscreen);
  return s_Fullscreen;
}

/* Windows movie: the mciavi32 child window draws 1:1 and cannot scale with the picture, so
   the picture drops to its natural size for the duration and only keeps the centering offset. */
void SdlBackendSetNativeMovie(int on)
{
  on = on ? 1 : 0;
  if ( on == (s_NativeMovie != 0) )
    return;
  s_NativeMovie = on;
  SdlUpdateLayout();
  DBG_LOG("SdlBackendSetNativeMovie: %d -> picture %dx%d at (%d,%d)",
          on, s_PicW, s_PicH, s_PicX, s_PicY);
}

/* -fullscreen / /fullscreen on the command line (port option, case-insensitive).
   Windows passes WinMain's lpCmdLine, win32_shim joins argv[1..] into the same string. */
/* ---- command line: -fullscreen and -datapath <dir> ----
   SdlBackendParseCmdLine is called from WinMain (main.c) before the game opens
   anything, and that is exactly what makes -datapath work. Everything the engine
   looks up is CWD-relative - g_ExecutablePath stays ".\PILOTS.EXE", the scripts
   ask for ".\movies\intro.AVI", the saves come from script data - so changing
   the working directory here moves the whole lookup at once and the game code
   does not have to know about it. Without the switch nothing is touched, which
   is the original behaviour: resources are found next to wherever the process
   was started.
   Two things deliberately stay with the exe rather than follow -datapath: the
   debug log and the .hlp, both derived from GetModuleFileNameA. */

/* Next token of the command line. A '"'-quoted token may contain spaces and the
   quotes are not part of it. Returns 0 once the line is exhausted, 1 when a token
   was read - *len can then be 0 for an empty "" token, which is why the caller
   must not treat 0 as "found nothing". */
static int SdlNextToken(const char **pp, const char **ptok, int *len)
{
  const char *p = *pp;
  const char *tok;

  while ( *p == ' ' || *p == '\t' )
    ++p;
  if ( !*p )
    return 0;
  if ( *p == '"' )
  {
    ++p;
    tok = p;
    while ( *p && *p != '"' )
      ++p;
    *len = (int)(p - tok);
    if ( *p == '"' )
      ++p;
  }
  else
  {
    tok = p;
    while ( *p && *p != ' ' && *p != '\t' )
      ++p;
    *len = (int)(p - tok);
  }
  *ptok = tok;
  *pp = p;
  return 1;
}

/* The whole token is "-name" (or "/name"), case-insensitively, and nothing else. */
static int SdlArgIs(const char *tok, int len, const char *name)
{
  int i;

  for ( i = 0; name[i]; ++i )
  {
    char a = (i + 1 < len) ? tok[i + 1] : 0;       /* +1 skips the leading - or / */
    if ( a >= 'A' && a <= 'Z' )
      a += 32;
    if ( a != name[i] )
      return 0;
  }
  return len == i + 1;
}

static void SdlUseDataPath(const char *path, int len)
{
  char dir[MAX_PATH];

  if ( len >= (int)sizeof(dir) )
    len = (int)sizeof(dir) - 1;
  memcpy(dir, path, len);
  dir[len] = '\0';
  if ( !SetCurrentDirectoryA(dir) )                /* real one on Windows, shim over chdir on Linux */
    DBG_LOG("SdlBackendParseCmdLine: -datapath '%s': cannot use it, keeping the current directory", dir);
  else
    DBG_LOG("SdlBackendParseCmdLine: -datapath '%s': game resources are looked up here", dir);
}

int SdlBackendParseCmdLine(const char *cmdLine)
{
  const char *p = cmdLine;
  const char *tok;
  int len;
  int found = 0;

  if ( !p )
    return 0;
  while ( SdlNextToken(&p, &tok, &len) )
  {
    if ( len <= 1 || (tok[0] != '-' && tok[0] != '/') )
      continue;
    if ( SdlArgIs(tok, len, "fullscreen") )
      found = 1;
    else if ( SdlArgIs(tok, len, "datapath") )
    {
      const char *val;
      int vlen;

      if ( SdlNextToken(&p, &val, &vlen) && vlen > 0 )
        SdlUseDataPath(val, vlen);
      else
        DBG_LOG("SdlBackendParseCmdLine: -datapath needs a directory, ignored");
    }
  }
  if ( found )
  {
    DBG_LOG("SdlBackendParseCmdLine: -fullscreen -> borderless desktop fullscreen, Alt+Enter toggles");
    SdlBackendSetFullscreen(1);
  }
  return found;
}

void SdlBackendShutdown(void)
{
  if ( s_VideoTex )
  {
    SDL_DestroyTexture(s_VideoTex);
    s_VideoTex = 0;
    s_VideoTexW = s_VideoTexH = 0;
  }
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
#ifdef _WIN32
  s_OrigWndProc = 0;
#endif
  if ( s_Stage )
  {
    free(s_Stage);
    s_Stage = 0;
    s_StagePixels = 0;
  }
  SdlMidi_Close();                       /* normally already done by ShutdownMultimedia; idempotent */
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
  HWND hwnd;
  int px = x == (int)CW_USEDEFAULT ? SDL_WINDOWPOS_UNDEFINED : x;
  int py = y == (int)CW_USEDEFAULT ? SDL_WINDOWPOS_UNDEFINED : y;

#ifndef _WIN32
  /* -fullscreen from the command line: the window is created hidden, and SDL defers an
     explicit SetWindowPosition while the window is hidden - with the fullscreen flag already
     up it never replays it, so X is left with the creation position (SDL centres an
     UNDEFINED one: 320,120 for 640x480 on a 1280x720 screen) and the "fullscreen" window
     then hangs half off-screen. Verified against X directly. Nothing moves the window
     between creation and map, so asking X for (0,0) right here is what sticks; on Windows
     SDL's own fullscreen path positions the window and the proven path stays untouched. */
  if ( s_Fullscreen )
    px = py = 0;
#endif

  if ( !s_SdlWindow )
  {
    if ( resizable )
      flags |= SDL_WINDOW_RESIZABLE;
    s_SdlWindow = SDL_CreateWindow(
        title ? title : "",
        px, py,
        clientW, clientH, flags);
    if ( !s_SdlWindow )
    {
      DBG_LOG("SdlBackendCreateWindow: SDL_CreateWindow failed: %s", SDL_GetError());
      return 0;
    }
  }
#ifdef _WIN32
  {
    SDL_SysWMinfo wmi;
    SDL_VERSION(&wmi.version);
    if ( !SDL_GetWindowWMInfo(s_SdlWindow, &wmi) || wmi.subsystem != SDL_SYSWM_WINDOWS )
    {
      DBG_LOG("SdlBackendCreateWindow: SDL_GetWindowWMInfo failed: %s", SDL_GetError());
      return 0;
    }
    hwnd = wmi.info.win.window;
    s_OrigWndProc = (WNDPROC)SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)SdlGameWndProc);
  }
#else
  /* No native HWND: the game gets win32_shim's fake handle; SdlDispatchSdlEvent posts
     WM_* messages at g_MainWindow and DispatchMessageA routes them to MainWndProc. */
  hwnd = WinShimMainWindow();
  SDL_StartTextInput();                    /* SDL_TEXTINPUT -> WM_CHAR (Unicode ANSI parity) */
#endif
  /* The original pinned the window with WM_GETMINMAXINFO (MainWndProc case 0x24:
     max track/max size = border + initial client) - it never grew or maximized.
     Mirror that with an SDL max-size hint so the WM cannot resize it either; even
     a WM that ignores the hint (fullscreen) cannot hurt - SdlHandleResize keeps the
     foreign size away from the game. */
  SDL_SetWindowMaximumSize(s_SdlWindow, clientW, clientH);
  if ( resizable )                              /* SDL's WM_GETMINMAXINFO handler preserves this */
    SDL_SetWindowMinimumSize(s_SdlWindow, clientW, clientH);
  /* Remember the logical size the window was created with: it is the game's
     constant WM_SIZE payload for the whole session (the original window never
     resized), and the only restore source that survives the game zeroing
     g_VideoConfig.clientWidth/Height on minimize. */
  s_GameClientW = clientW;
  s_GameClientH = clientH;
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
  SdlApplyFullscreen();                          /* -fullscreen recorded before the window existed */
  if ( !s_Renderer )
  {
    /* No vsync: the present cadence is ours - coalesced bursts on Linux
       (PRESENT_MIN_INTERVAL_MS), one per blit on Windows */
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
    s_PresentPending = 0;                       /* pending uploads died with the old texture */
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

/* Where the interactive objects are. Replicates the engine's own selection pass
   (ProcessInputAndRender, render.c:402-438, plus CheckObjectHitTest, render.c:1706) but
   keeps every candidate instead of the single winner it resolves to - the whole point of
   the overlay is to show all of them at once:
     - walk the live render stack downward, Start down to just past End (the engine's range);
     - keep the composites, (flags & 3) == 3: their posX/posY are CHILD INDEXES, not pixels;
     - keep the ones the sprite channel calls 2 (hover/click UI) or 3 (focus objects);
     - the pixel position is on the child, the size is the first two u16 of the sprite
       header its first DWORD points at; both are world space, so the camera offset comes
       off to land in screen space.
   Read-only and side-effect free: unlike the real pass nothing is written into animLink,
   so the game cannot tell this ever ran. */
static void SdlCollectHotspots(void)
{
  RenderObject *obj;

  s_HotspotCount = 0;
  if ( !g_RenderObjectsStart || !g_RenderObjectsEnd || !g_SpriteSlotArray )
    return;                                     /* nothing loaded yet */
  if ( g_RenderObjectsStart <= g_RenderObjectsEnd )
    return;
  for ( obj = g_RenderObjectsStart; obj > g_RenderObjectsEnd; --obj )
  {
    RenderObject *child;
    const unsigned short *hdr;
    int type, x, y, w, h;

    if ( (obj->flags & 3) != 3 )
      continue;
    type = LOBYTE(g_SpriteSlotArray[obj->slotIndex].baseData);
    if ( type != 2 && type != 3 )
      continue;
    if ( obj->posY == 0xFFFF )
      continue;                                 /* composite without a child */
    child = &g_RenderObjectsStart[-obj->posY];
    if ( child > g_RenderObjectsStart || child < g_RenderObjectsEnd )
      continue;                                 /* index outside the allocated stack */
    if ( !child->spriteDataPtr )
      continue;
    /* 32-bit pointer slot in game data - the read CheckObjectHitTest does (render.c:1720).
       On LP64 it only survives because the pool is kept below 2 GB, see docs/lp64.md; the
       same expression is used deliberately instead of "fixing" the truncation, so the
       overlay can never disagree with the hit test it is meant to mirror. */
    hdr = (const unsigned short *)(INT_PTR)*(unsigned int *)child->spriteDataPtr;
    w = hdr[0];
    h = hdr[1];
    if ( w <= 0 || h <= 0 || w > 4096 || h > 4096 )
      continue;                                 /* not a sprite header: skip rather than mark junk */
    x = (int)(__int16)child->posX - g_CameraOffsetX;
    y = (int)(__int16)child->posY - g_CameraOffsetY;
    if ( x >= s_GameClientW || y >= s_GameClientH || x + w <= 0 || y + h <= 0 )
      continue;                                 /* fully off-screen: nothing to mark */
    if ( s_HotspotCount >= HOTSPOT_MAX )
      break;
    s_HotspotX[s_HotspotCount] = x + w / 2;     /* centre: lands inside whatever is clickable */
    s_HotspotY[s_HotspotCount] = y + h / 2;
    ++s_HotspotCount;
  }
}

/* White markers over the copied frame, in WINDOW client px: a game-space radius would be
   scaled differently along X and Y in fullscreen and come out an ellipse, while one radius
   in window space stays a circle and still grows with the picture. SDL2 has no circle
   primitive, so the disc is filled scanline by scanline like SdlBackendDrawGrid does with
   lines. Nothing here reaches the 8-bit backbuffer, the palette or the dirty regions -
   the renderer is cleared and recopied on every present, so the markers vanish by simply
   not being drawn again (see SdlUpdateHotspots for the forced present that makes that
   visible on a static screen). */
static void SdlDrawHotspots(void)
{
  int r, r2, i;

  if ( !s_HotspotCount )
    return;
  /* ~3 game px, so the marker scales with the picture; clamped because the extremes are
     either invisible or a blob. One radius for X and Y keeps it round. */
  r = s_GameClientH > 0 ? (3 * s_PicH) / s_GameClientH : 3;
  if ( r < 2 )
    r = 2;
  if ( r > 16 )
    r = 16;
  r2 = r * r;
  SDL_SetRenderDrawColor(s_Renderer, 255, 255, 255, 255);
  for ( i = 0; i < s_HotspotCount; ++i )
  {
    int cx = s_HotspotX[i], cy = s_HotspotY[i];
    int dy;
    SdlBackendGameToClient(&cx, &cy);
    for ( dy = -r; dy <= r; ++dy )
    {
      int half = 0;
      while ( (half + 1) * (half + 1) + dy * dy <= r2 )
        ++half;
      SDL_RenderDrawLine(s_Renderer, cx - half, cy + dy, cx + half, cy + dy);
    }
  }
}

/* Show the current texture. Windowed it lands 1:1 at the top-left - mirrors the original
   BitBlt which never scaled - fullscreen it fills the letterboxed picture rect instead
   (see the fullscreen block). Outside the texture the renderer is cleared to black, like
   the window background. */
static void SdlDrawGameFrame(void)              /* clear + game texture; no present */
{
  SDL_Rect dst;

  if ( !s_Texture || !s_Renderer )
    return;
  SdlUpdateLayout();                            /* layout follows the window, not events */
  /* The renderer's viewport is only re-applied by SDL from its own window events - and on
     Windows SDL learns a window's new size ONLY from WM_WINDOWPOSCHANGED, which we routed to
     MainWndProc alone (see SdlGameWndProc). Then the viewport keeps the 640x480 the window
     was created with while SDL_GetRendererOutputSize answers with the live client size, and
     SDL_RenderClear and every SDL_RenderCopy are clipped to that stale box - which is exactly
     "only the top-left part of the picture is drawn" in fullscreen. The message now goes to
     SDL too, which is what resizes the D3D swap chain; forcing the viewport to the whole
     output every frame keeps the copy matching the layout even if some other path misses an
     event. */
  SDL_RenderSetViewport(s_Renderer, NULL);
#ifdef _DEBUG
  /* Draw-time truth: the layout above is computed from the client rect (the mouse space),
     while SDL_RenderCopy works in the renderer's own space - output size, viewport, scale,
     logical size. Any of those going stale is invisible to the layout lines and to the
     viewport line, and it did crop the picture once (swap chain stuck at the creation size
     while output said 1536x864). So log all of them on every change, windowed and
     fullscreen alike - without scale/logical the numbers alone never add up. */
  {
    static int s_LgOw, s_LgOh, s_LgPx, s_LgPy, s_LgPw, s_LgPh, s_LgIw, s_LgIh;
    int ow = 0, oh = 0, iw = 0, ih = 0, ww = 0, wh = 0;
    int lgw = 0, lgh = 0;
    int scx1000 = 1000, scy1000 = 1000;
    float scx = 1.0f, scy = 1.0f;
    SDL_Rect vp;
    SDL_GetRendererOutputSize(s_Renderer, &ow, &oh);
    SDL_GetWindowSizeInPixels(s_SdlWindow, &iw, &ih);
    SDL_GetWindowSize(s_SdlWindow, &ww, &wh);
    SDL_RenderGetViewport(s_Renderer, &vp);
    SDL_RenderGetScale(s_Renderer, &scx, &scy);
    SDL_RenderGetLogicalSize(s_Renderer, &lgw, &lgh);
    scx1000 = (int)(scx * 1000.0f + 0.5f);
    scy1000 = (int)(scy * 1000.0f + 0.5f);
    if ( ow != s_LgOw || oh != s_LgOh || vp.x != s_LgPx || vp.y != s_LgPy
      || vp.w != s_LgPw || vp.h != s_LgPh || iw != s_LgIw || ih != s_LgIh )
    {
      s_LgOw = ow; s_LgOh = oh;
      s_LgPx = vp.x; s_LgPy = vp.y; s_LgPw = vp.w; s_LgPh = vp.h;
      s_LgIw = iw; s_LgIh = ih;
      DBG_LOG("SdlDrawGameFrame: window %dx%d pixels %dx%d output %dx%d "
              "viewport (%d,%d %dx%d) scale %d.%03dx%d.%03d logical %dx%d "
              "picture %dx%d at (%d,%d) tex %dx%d",
              ww, wh, iw, ih, ow, oh, vp.x, vp.y, vp.w, vp.h,
              scx1000 / 1000, scx1000 % 1000, scy1000 / 1000, scy1000 % 1000,
              lgw, lgh, s_PicW, s_PicH, s_PicX, s_PicY, s_TexWidth, s_TexHeight);
    }
  }
#endif
  SDL_SetRenderDrawColor(s_Renderer, 0, 0, 0, 255);
  SDL_RenderClear(s_Renderer);
  dst.x = s_PicX;
  dst.y = s_PicY;
  dst.w = s_PicW > 0 ? s_PicW : s_TexWidth;
  dst.h = s_PicH > 0 ? s_PicH : s_TexHeight;
  SDL_RenderCopy(s_Renderer, s_Texture, 0, &dst);
  if ( s_HotspotHeld && s_HotspotCount )         /* hold Ctrl: mark the interactive objects */
    SdlDrawHotspots();
}

static void SdlPresentCurrent(void)
{
#ifdef _DEBUG
  Uint64 perf0 = SDL_GetPerformanceCounter();
#endif
  if ( !s_Texture || !s_Renderer )
    return;
  SdlDrawGameFrame();
  SDL_RenderPresent(s_Renderer);
  s_PresentPending = 0;                         /* the texture as it stands is on screen now */
  s_LastPresentTick = SDL_GetTicks();
#ifdef _DEBUG
  ++s_Presents;
  s_PresentUs += (unsigned)((SDL_GetPerformanceCounter() - perf0)
                            * 1000000u / SDL_GetPerformanceFrequency());
#endif
}

/* Show rect updates deferred by the burst coalescing in SdlBackendPresentRect (a
   no-op when nothing is pending; on Windows nothing ever is - presents are
   immediate there). Called from the message pumps so a half-drawn frame never
   waits for the next blit. */
static void SdlFlushPresent(void)
{
  if ( s_PresentPending )
    SdlPresentCurrent();
}

/* Sample Ctrl and refresh the marker list, once per pump. The forced present on the state
   change is the part that actually makes the feature usable: Windows presents per blit and
   SdlFlushPresent only presents when something was blitted, so on a resting screen nothing
   would ever redraw and the markers would either never appear or never go away. Both edges
   need it - down to show them, up to erase them. */
static void SdlUpdateHotspots(void)
{
  int held = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  int prevCount = s_HotspotCount;
  int toggled = held != s_HotspotPrev;

  s_HotspotHeld = held;
  if ( held )
    SdlCollectHotspots();
  else
    s_HotspotCount = 0;
  if ( toggled || s_HotspotCount != prevCount )
    DBG_LOG("SdlUpdateHotspots: Ctrl %s, %d interactive object(s)",
            held ? "down" : "up", s_HotspotCount);
  s_HotspotPrev = held;
  if ( toggled && s_Texture && s_Renderer )
    SdlPresentCurrent();
}

/* One frame of MCI playback (mci_ffmpeg.c): the game frame the same way
   SdlPresentCurrent shows it, with the movie stretched into the MCI child
   window's client rect (x/y/w/h, w or h <= 0 = natural stream size), then
   present. Re-presenting the game texture every call also repairs the screen
   right after the game does its own present (WM_PAINT during playback). */
void SdlBackendVideoFrame(const void *bgra, int srcw, int srch, int x, int y, int w, int h)
{
  SDL_Rect dst;

  if ( !s_Renderer || !bgra || srcw <= 0 || srch <= 0 )
    return;
  if ( w <= 0 )
    w = srcw;
  if ( h <= 0 )
    h = srch;
  if ( !s_VideoTex || s_VideoTexW != srcw || s_VideoTexH != srch )
  {
    if ( s_VideoTex )
      SDL_DestroyTexture(s_VideoTex);
    s_VideoTex = SDL_CreateTexture(s_Renderer, SDL_PIXELFORMAT_ARGB8888,
                                   SDL_TEXTUREACCESS_STREAMING, srcw, srch);
    s_VideoTexW = srcw;
    s_VideoTexH = srch;
    if ( !s_VideoTex )
    {
      s_VideoTexW = s_VideoTexH = 0;
      DBG_LOG("SdlBackendVideoFrame: SDL_CreateTexture failed: %s", SDL_GetError());
      return;
    }
  }
  SDL_UpdateTexture(s_VideoTex, NULL, bgra, srcw * 4);
  SdlDrawGameFrame();
  {                                                 /* child rect: game px -> window px (identity windowed) */
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
  s_PresentPending = 0;                         /* the movie frame showed the game texture too */
  s_LastPresentTick = SDL_GetTicks();
}

/* Convert an 8-bit bottom-up rect through the palette LUT, upload it and present.
   Mirrors the original BitBlt/StretchDIBits(srcY = bmp_height - y - cy) row flip.
   On non-Windows the upload always happens right away but the present is
   coalesced (PRESENT_MIN_INTERVAL_MS, flushed at the message pumps) - see the
   note by the define. */
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
#ifdef _DEBUG
  ++s_RectBlits;
#endif

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
#ifndef _WIN32
  /* burst coalescing: a present here costs ~4.7 ms on X11 vs 46 us of upload;
     defer while more blits are about to follow, the pumps flush the tail */
  s_PresentPending = 1;
  if ( (SDL_GetTicks() - s_LastPresentTick) < PRESENT_MIN_INTERVAL_MS )
    return;
#endif
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
  {
    int x0 = i, y0 = 0, x1 = i, y1 = s_TexHeight - 1;
    SdlBackendGameToClient(&x0, &y0);
    SdlBackendGameToClient(&x1, &y1);
    SDL_RenderDrawLine(s_Renderer, x0, y0, x1, y1);
  }
  for ( i = 0; i < s_TexHeight; i += 8 )
  {
    int x0 = 0, y0 = i, x1 = s_TexWidth - 1, y1 = i;
    SdlBackendGameToClient(&x0, &y0);
    SdlBackendGameToClient(&x1, &y1);
    SDL_RenderDrawLine(s_Renderer, x0, y0, x1, y1);
  }
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

/* Message pump for the game loops. Returns 1 when it is time to leave the loop
   (original: WM_QUIT, Msg.message == 18). */
int SdlBackendPumpEvents(void)
{
  SdlUpdateHotspots();                          /* hold Ctrl to mark interactive objects */
#ifdef _WIN32
  /* SDL_PollEvent pumps the Win32 queue (SDL internally does PeekMessage/
     TranslateMessage/DispatchMessage, so our wndproc subclass sees every message);
     SDL_QUIT == WM_QUIT here. */
  SDL_Event ev;
  int quit = 0;

  SdlFlushPresent();                          /* pending burst present (no-op on Windows) */
  while ( SDL_PollEvent(&ev) )
  {
    if ( ev.type == SDL_QUIT )
      quit = 1;
  }
  SdlWavePump();
  SdlFlushPresent();                          /* also after WM_PAINT dispatched this round */
  return quit;
#else
  /* Drain the win32_shim queue: PeekMessageA pumps SDL events into it when empty
     (WinShim_PumpSDLToQueue -> SdlDispatchSdlEvent -> PostMessageA), each message
     goes through the game's own MainWndProc like a real DispatchMessage loop. */
  MSG m;
  int quit = 0;

  SdlFlushPresent();                          /* show rect updates coalesced since the last pump */

#ifdef _DEBUG
  /* PILOTS_DEBUG_HB=1: 1 s heartbeat - is the pump alive, what does the game think
     its client size / clip bounds are, does SDL still deliver rendering activity.
     Catches the maximize/restore freeze (client 0x0, minimized flag, presents gone). */
  {
    static int s_HbEnable = -1;
    if ( s_HbEnable < 0 )
    {
      char *hb = getenv("PILOTS_DEBUG_HB");
      s_HbEnable = (hb && *hb && *hb != '0');
    }
    if ( s_HbEnable )
    {
      static Uint32 s_HbNext;
      Uint32 now = SDL_GetTicks();
      if ( !s_HbNext || (Sint32)(now - s_HbNext) >= 0 )
      {
        int mx = 0, my = 0, ww = 0, wh = 0;
        float gx = 0, gy = 0;
        Uint32 fl = s_SdlWindow ? SDL_GetWindowFlags(s_SdlWindow) : 0;

        SDL_GetMouseState(&mx, &my);
        SDL_GetGlobalMouseState(&gx, &gy);       /* server query: sees the pointer even with no events */
        if ( s_SdlWindow )
          SDL_GetWindowSize(s_SdlWindow, &ww, &wh);
        s_HbNext = now + 1000;
        DBG_LOG("HB: presents=%u blits=%u client=%dx%d tex=%dx%d win=%dx%d flags=0x%X mouse=%d,%d gmouse=%.0f,%.0f mot=%u enter=%u pending=%d",
                s_Presents, s_RectBlits,
                g_VideoConfig.clientWidth, g_VideoConfig.clientHeight,
                s_TexWidth, s_TexHeight, ww, wh, (unsigned)fl, mx, my, gx, gy,
                s_EvMotion, s_EvEnter, s_PresentPending);
      }
    }
  }
#endif

  if ( s_Usr1Quit )                               /* SIGUSR1 test hook: graceful quit */
    return 1;

  while ( PeekMessageA(&m, 0, 0, 0, PM_REMOVE) )
  {
    if ( m.message == WM_QUIT )
    {
      quit = 1;
      break;
    }
    TranslateMessage(&m);
    DispatchMessageA(&m);
  }
  SdlWavePump();
  SdlFlushPresent();                          /* also after WM_PAINT dispatched this round */
  return quit || WinShim_QuitPosted();
#endif
}

/* ---- small helpers for win32_shim.c (GetDC metrics, ShowWindow, IsIconic, styles) ---- */

void SdlBackendGetClientSize(int *w, int *h)
{
  int W = 0, H = 0;
  if ( s_SdlWindow )
    SDL_GetWindowSize(s_SdlWindow, &W, &H);
  if ( w ) *w = W;
  if ( h ) *h = H;
}

int SdlBackendIsMinimized(void)
{
  return s_SdlWindow && (SDL_GetWindowFlags(s_SdlWindow) & SDL_WINDOW_MINIMIZED) != 0;
}

void SdlBackendShowWindow(int show)
{
  if ( !s_SdlWindow )
    return;
  if ( show )
  {
    SDL_ShowWindow(s_SdlWindow);
    SDL_RaiseWindow(s_SdlWindow);
  }
  else
    SDL_HideWindow(s_SdlWindow);
}

/* The styles CreateMainWindow computed but the SDL window never received natively;
   AdjustWindowRectEx (win32_shim) derives border/titlebar metrics from these. */
DWORD SdlBackendGetWinStyle(void)
{
  if ( s_SdlWindow && (SDL_GetWindowFlags(s_SdlWindow) & SDL_WINDOW_RESIZABLE) )
    return 0x02CF0000u;                    /* WS_CLIPCHILDREN|WS_CAPTION|WS_THICKFRAME|... */
  return 0x020A0000u;                      /* original fixed-window style from main.c */
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

  static unsigned int s_WaveQueueCalls;           /* diagnostics: throttles the per-buffer PCM log */
  unsigned int nonzero;

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
  /* Diagnostics (debug builds only, throttled): is the game handing us audible PCM, and does
     the device keep draining it? U8 silence is 0x80 - a paused device or an all-silent sound
     slot looks identical in the TriggerSoundSlot log, this line tells them apart. */
  if ( s_WaveQueueCalls < 8 || (s_WaveQueueCalls & 63) == 0 )
  {
    nonzero = 0;
    for ( i = 0; i < len; ++i )
    {
      if ( src[i] != 0x80 )
        ++nonzero;
    }
    DBG_LOG("SdlWave_Queue: #%u len=%u nonzero=%u stillQueued=%u vol=%d",
            s_WaveQueueCalls, len, nonzero, (int)SdlWave_Queued(), s_WaveVolume);
  }
  ++s_WaveQueueCalls;
  return SDL_QueueAudio(s_WaveDev, src, len) == 0;
}

void SdlWave_Clear(void)
{
  static int s_WaveFirstClearLogged;              /* one-shot: means headers ran out (device stopped draining) */

  if ( s_WaveDev )
  {
    if ( !s_WaveFirstClearLogged )
    {
      s_WaveFirstClearLogged = 1;
      DBG_LOG("SdlWave_Clear: queued audio dropped (waveOutReset path - device fell behind)");
    }
    SDL_ClearQueuedAudio(s_WaveDev);
  }
}

void SdlWave_Pause(int pause)
{
  static int s_WavePaused = -1;                   /* log the transition only, not every call */

  if ( s_WaveDev )
  {
    if ( s_WavePaused != (pause ? 1 : 0) )
    {
      s_WavePaused = pause ? 1 : 0;
      DBG_LOG("SdlWave_Pause: %d", s_WavePaused);
    }
    SDL_PauseAudioDevice(s_WaveDev, pause ? 1 : 0);
  }
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

/* ---- MIDI music: libADLMIDI (OPL3 emulation) replacing midiOut ----
   audio.c keeps the game's own event-stream parser (ProcessMidiEvents) and its 10 ms winmm
   timer; each timer tick pushes the parsed midiOutShortMsg-shaped word here via
   SdlMidi_ShortMsg and pulls rendered PCM via SdlMidi_Pump into a second SDL queue-audio
   device (44100 Hz S16 stereo). The synth state is touched from the winmm timer thread and
   from the game thread (volume/start/stop/cleanup), so every entry point takes s_MidiCS.
   The critical section is initialized once and never deleted: a timer callback may still be
   in flight while the game tears multimedia down - the original had the same benign race
   with a closed midiOut handle, but a deleted CS would be undefined behavior. */

#define SDL_MIDI_RATE          44100
#define SDL_MIDI_BANK          68  /* "WOPL (4op bank by James Alan Nguyen and Wohlstand)", a GM set;
                                      0 = AIL "The Fat Man" (library default); see adl_getBankNames() */
#define SDL_MIDI_CHUNK_SAMPLES 1024 /* adl_generate counts samples, one frame = two: 512 frames ~ 11.6 ms */
#define SDL_MIDI_QUEUE_TARGET  (128 * 1024) /* ~0.7 s of PCM headroom over game-thread stalls */

static struct ADL_MIDIPlayer *s_Synth;
static SDL_AudioDeviceID s_MidiDev;      /* 44100 Hz / AUDIO_S16SYS / stereo queue-audio device */
static CRITICAL_SECTION s_MidiCS;
static int s_MidiCSReady;
static int s_MidiVolume = 255;           /* 0..255, applied at queue time (like the wave volume) */
static int s_MidiFeed;                   /* 1 between StartMidiPlayback and ResetMidi: render every tick */
static short s_MidiChunk[SDL_MIDI_CHUNK_SAMPLES];
static unsigned int s_MidiMsgCount;      /* diagnostics: messages seen since open/reset */
static int s_MidiFirstMsgLogged;
static int s_MidiFirstAudioLogged;

static void MidiLock(void)
{
  if ( s_MidiCSReady )
    EnterCriticalSection(&s_MidiCS);
}

static void MidiUnlock(void)
{
  if ( s_MidiCSReady )
    LeaveCriticalSection(&s_MidiCS);
}

int SdlMidi_Open(void)
{
  SDL_AudioSpec want;
  SDL_AudioSpec have;

  if ( s_Synth )
    return 1;
  if ( !s_MidiCSReady )
  {
    InitializeCriticalSection(&s_MidiCS);
    s_MidiCSReady = 1;
  }
  if ( !(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) )
  {
    if ( SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 )
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
  want.callback = 0;                     /* push mode: SdlMidi_Pump renders into the queue */
  s_MidiDev = SDL_OpenAudioDevice(0, 0, &want, &have, 0); /* no format changes allowed */
  if ( !s_MidiDev )
  {
    DBG_LOG("SdlMidi_Open: SDL_OpenAudioDevice failed: %s", SDL_GetError());
    return 0;
  }
  if ( have.freq != SDL_MIDI_RATE || have.format != AUDIO_S16SYS || have.channels != 2 )
  {
    DBG_LOG("SdlMidi_Open: device gave %d Hz / 0x%X / %dch, refusing", have.freq, have.format, have.channels);
    SDL_CloseAudioDevice(s_MidiDev);
    s_MidiDev = 0;
    return 0;
  }
  s_Synth = adl_init(SDL_MIDI_RATE);
  if ( !s_Synth )
  {
    DBG_LOG("SdlMidi_Open: adl_init failed: %s", adl_errorString());
    SDL_CloseAudioDevice(s_MidiDev);
    s_MidiDev = 0;
    return 0;
  }
  if ( adl_switchEmulator(s_Synth, ADLMIDI_EMU_NUKED) < 0 )
    DBG_LOG("SdlMidi_Open: Nuked emulator unavailable (%s), keeping %s",
            adl_errorString(), adl_chipEmulatorName(s_Synth));
  if ( adl_setBank(s_Synth, SDL_MIDI_BANK) < 0 )
    DBG_LOG("SdlMidi_Open: bank %d unavailable (%s), keeping the default one",
            SDL_MIDI_BANK, adl_errorString());
  s_MidiVolume = 255;                    /* midiOut devices started at full volume */
  s_MidiFeed = 0;
  s_MidiMsgCount = 0;
  s_MidiFirstMsgLogged = 0;
  s_MidiFirstAudioLogged = 0;
  SDL_PauseAudioDevice(s_MidiDev, 0);
  DBG_LOG("SdlMidi_Open: libADLMIDI ready: emu=%s bank=%d rate=%d",
          adl_chipEmulatorName(s_Synth), SDL_MIDI_BANK, SDL_MIDI_RATE);
  return 1;
}

void SdlMidi_Close(void)
{
  MidiLock();
  if ( s_Synth )
  {
    adl_close(s_Synth);
    s_Synth = 0;
  }
  s_MidiFeed = 0;
  if ( s_MidiDev )
  {
    SDL_CloseAudioDevice(s_MidiDev);
    s_MidiDev = 0;
  }
  MidiUnlock();
}

/* Arm PCM feeding; called by StartMidiPlayback once its timer is running. */
void SdlMidi_Start(void)
{
  MidiLock();
  if ( s_Synth )
    s_MidiFeed = 1;
  MidiUnlock();
}

/* midiOutReset equivalent: cut all sounding notes, drop already-rendered PCM, and stop
   feeding (the caller - ResetMidi - has already killed the timer). */
void SdlMidi_Reset(void)
{
  MidiLock();
  s_MidiFeed = 0;
  if ( s_Synth )
    adl_panic(s_Synth);
  if ( s_MidiDev )
    SDL_ClearQueuedAudio(s_MidiDev);
  if ( s_MidiMsgCount )
  {
    /* A timing-dependent diagnostic, NOT an invariant: this prints only when the
       10 ms MidiTimerCallback happened to run at least once since the previous
       reset, so between a back-to-back Stop/Start pair x86 may log nothing where
       x64 catches one tick (or the other way round). Measured under the SAME
       click script, both arches issue the identical reset sequence and the
       identical message counts per playback period - only the tick that lands
       inside a short stop/start gap differs. Do not treat it as an arch check. */
    DBG_LOG("SdlMidi_Reset: %u messages processed", s_MidiMsgCount);
    s_MidiMsgCount = 0;
  }
  MidiUnlock();
}

/* The midiOutShortMsg-packed word from ProcessMidiEvents: status | data1<<8 | data2<<16. */
void SdlMidi_ShortMsg(unsigned int packed)
{
  unsigned int status = packed & 0xFF;
  unsigned int data1 = (packed >> 8) & 0xFF;
  unsigned int data2 = (packed >> 16) & 0xFF;
  unsigned int channel = status & 0x0F;

  MidiLock();
  if ( s_Synth )
  {
    ++s_MidiMsgCount;
    if ( !s_MidiFirstMsgLogged )
    {
      s_MidiFirstMsgLogged = 1;
      DBG_LOG("SdlMidi_ShortMsg: first message 0x%02X", status);
    }
    switch ( status & 0xF0 )
    {
      case 0x80:                         /* note off (the release velocity in data2 is dropped) */
        adl_rt_noteOff(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
        break;
      case 0x90:                         /* note on; velocity 0 == note off, as in the original synth */
        if ( data2 )
          adl_rt_noteOn(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1, (ADL_UInt8)data2);
        else
          adl_rt_noteOff(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
        break;
      case 0xA0:                         /* polyphonic aftertouch */
        adl_rt_noteAfterTouch(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1, (ADL_UInt8)data2);
        break;
      case 0xB0:                         /* control change */
        adl_rt_controllerChange(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1, (ADL_UInt8)data2);
        break;
      case 0xC0:                         /* program change (2-byte message) */
        adl_rt_patchChange(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
        break;
      case 0xD0:                         /* channel aftertouch (2-byte message) */
        adl_rt_channelAfterTouch(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data1);
        break;
      case 0xE0:                         /* pitch bend: data1 = LSB, data2 = MSB */
        adl_rt_pitchBendML(s_Synth, (ADL_UInt8)channel, (ADL_UInt8)data2, (ADL_UInt8)data1);
        break;
      default:                           /* system messages never reach here (ProcessMidiEvents skips them) */
        break;
    }
  }
  MidiUnlock();
}

void SdlMidi_SetVolume(int vol)
{
  if ( vol < 0 )
    vol = 0;
  if ( vol > 255 )
    vol = 255;
  MidiLock();
  s_MidiVolume = vol;
  if ( !vol && s_Synth )
  {
    adl_panic(s_Synth);                  /* original also did midiOutReset when muted */
    if ( s_MidiDev )
      SDL_ClearQueuedAudio(s_MidiDev);   /* drop PCM rendered at the old volume */
  }
  MidiUnlock();
}

int SdlMidi_GetVolume(void)
{
  return s_MidiVolume;
}

/* Render PCM into the music queue. Called from MidiTimerCallback every 10 ms while the
   stream is armed: that thread keeps feeding even when the game thread is busy loading,
   and the queue depth absorbs the timer's jitter. The loop tops the queue up to the target
   (self-regulating: full queue == skip, drained queue == refill at <=16 chunks per tick). */
void SdlMidi_Pump(void)
{
  unsigned int queued;
  int guard = 0;
  int generated;

  MidiLock();
  if ( !s_Synth || !s_MidiDev || !s_MidiFeed )
  {
    MidiUnlock();
    return;
  }
  queued = SDL_GetQueuedAudioSize(s_MidiDev);
  while ( queued < SDL_MIDI_QUEUE_TARGET && guard < 16 )
  {
    int i;
    generated = adl_generate(s_Synth, SDL_MIDI_CHUNK_SAMPLES, s_MidiChunk);
    if ( generated <= 0 )
      break;
    if ( s_MidiVolume != 255 )
    {
      for ( i = 0; i < generated; ++i )
        s_MidiChunk[i] = (short)(((int)s_MidiChunk[i] * s_MidiVolume) / 255);
    }
    if ( !s_MidiFirstAudioLogged )
    {
      /* Also timing-dependent, not an arch check: the count of nonzero samples in
         the FIRST rendered chunk depends on how many MIDI events had reached the
         synth by then, which in turn depends on when the 10 ms timer first fired.
         The same arch produces 937 or 941 across runs; an idle 10 s start gives
         937 on BOTH arches. The arch-invariant values are in SdlMidi_Open
         (emu / bank / rate) and in the first message's status byte. */
      int nonzero = 0;
      for ( i = 0; i < generated; ++i )
      {
        if ( s_MidiChunk[i] != 0 )
          ++nonzero;
      }
      if ( nonzero )
      {
        s_MidiFirstAudioLogged = 1;
        DBG_LOG("SdlMidi_Pump: first audible PCM (%d of %d samples nonzero)", nonzero, generated);
      }
    }
    if ( SDL_QueueAudio(s_MidiDev, s_MidiChunk, (unsigned int)generated * sizeof(short)) != 0 )
      break;
    queued += (unsigned int)generated * sizeof(short);
    ++guard;
  }
  MidiUnlock();
}

#pragma pack(pop)
