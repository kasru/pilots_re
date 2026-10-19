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
static unsigned int *s_Shadow;                  /* the frame as the game last drew it, ARGB; the texture
                                                   is only ever given this, once per shown frame */
static int s_ShadowPixels;                      /* capacity in pixels */
static int s_ShadowW;                           /* what it is sized for: always the texture's size */
static int s_ShadowH;

/* Present coalescing, in two parts. Both were read off measurements, not reasoned out;
   the numbers that forced them are below and at the defer site.

   TIME. Never two presents closer than PRESENT_MIN_INTERVAL_MS.
   STREAM. While rect blits arrive less than PRESENT_STREAM_GAP_MS apart, the game is
   sweeping - a scene transition is ~1200 16x16 tiles in a tight loop - and a present in the
   middle of that shows one more tile than the last one did, so inside a sweep present only
   every PRESENT_BURST_RECTS rects. That still shows the checkerboard's own 16 passes. When
   blits stop arriving that fast the update is interactive (a cursor, a dialog) and is shown
   at once; the message pumps also flush whatever is pending.

   The stream part exists because the time gate alone does nothing where a present blocks
   for a whole frame. One transition on the console, with the time gate only:

       1200 rect blits ... 19584 ms wall; convert 3 ms, upload 1147 ms, present 18447 ms
       in 1177 presents

   - the game could lay down tiles only as fast as we stopped it for a vsync. The same burst
   on x86 measured 480 ms / 1200 presents (0.4 ms each) and 5 ms once the stream gate was in.
   The upload term was a separate finding: SDL's GLES2 backend re-uploads a streaming texture
   WHOLE when it is unlocked, so 1200 rect uploads were 1.4 GB of bus traffic. Hence
   s_Shadow: rects go into a local frame and the texture gets one upload per shown frame. */
#define PRESENT_MIN_INTERVAL_MS 1               /* burst presents no faster than ~1000/s */
#define PRESENT_STREAM_GAP_MS   4               /* blits closer than this = a sweep is in progress */
#define PRESENT_BURST_RECTS     96              /* within a sweep, show at most one in this many tiles */
static Uint32 s_LastPresentTick;                /* SDL_GetTicks of the last real present */
static Uint32 s_LastRectTick;                   /* SDL_GetTicks of the last rect blit */
static unsigned s_RectsSincePresent;            /* rect blits laid down since the last shown frame */
static int s_PresentPending;                    /* the shadow holds pixels the texture has not been given */
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
static int s_StretchPic;                         /* '-' (port feature): fill the screen, aspect not kept */
static int s_PicX;                               /* picture origin, window client px */
static int s_PicY;
static int s_PicW;                               /* picture size, window client px */
static int s_PicH;

/* ---- busy indicator (port feature) ----
   A scene loads inside the game's own loop: it reads for seconds on end and draws nothing while
   it does, so on screen that looks exactly like a hang - which is how it was reported. The port
   cannot wrap that loop (it is the game's), but every read the game makes goes through the shim,
   so _read beats SdlBackendLoadBeat: reading while the screen has been still for a second is the
   only evidence there is. The quiet screen alone is not the trigger - a resting screen is quiet
   too and simply does not read. The spinner is drawn by SdlDrawGameFrame over the last frame the
   game presented, and the beat itself forces the present, because during a load nothing else
   would ever redraw. */
#define SDL_LOAD_QUIET_MS 1000u                  /* a screen this still, with reads, is a load */
#define SDL_LOAD_SPIN_MS  120u                   /* the spinner's own frame rate */
#define SDL_LOAD_ACTIVE_MS 800u                  /* reads older than this are no longer evidence of a load */
static unsigned s_RectBlitsTotal;                /* every rect the game blitted, debug or not: kept for the burst statistics */
static Uint32 s_LastGameBlit;                    /* the last rect the GAME blitted - the only clock a load is measured on */
static Uint32 s_LoadStart;                       /* 0 = no load noticed */
static Uint32 s_LoadSpinTick;                    /* the last spinner frame */
static Uint32 s_LoadBeatTick;                    /* the last read the game made */
static unsigned long s_IdleBytes;                /* read since that last blit */
static int s_LoadPhase;                          /* rotates the marks */

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

#ifdef __SWITCH__
/* Where the left stick has left the cursor, in the game's own client px. -1 until the
   first pump, which parks it in the middle of the picture (see SdlPollSwitchControls). */
static int s_StickX = -1;
static int s_StickY;
static int s_RLedgesLogged;                    /* the "no binding" edge lines, capped so one button cannot flood the log */
static unsigned int s_LastMovePost;            /* throttle for the WM_MOUSEMOVE stream - see MOVE_MIN_INTERVAL_MS */
static int s_MovePending;                      /* the stick moved since the last post, not yet sent */
#endif
/* R down on the console: the interactive-object overlay, the same picture Ctrl gives.
   Outside __SWITCH__ nothing ever sets it and it costs one byte - SdlUpdateHotspots reads it
   unconditionally, so the overlay has one "held" expression for every platform. */
static int s_PadOverlayHeld;

static int s_FingerLogged;                       /* the first few touches are logged, then it is quiet */

static void SdlUpdateLayout(void);               /* where the picture goes */
static void SdlApplyFullscreen(void);            /* SDL side of s_Fullscreen (no-op pre-window) */
static int SdlMapMousePoint(int *x, int *y);     /* client px -> game px; 0 = outside the picture */
static int SdlPictureTransformed(void);
static int SdlFloorDiv(int a, int b);
static void SdlCollectHotspots(void);            /* fill s_HotspotX/Y from the render stack */
static void SdlLoopProfileTick(void);            /* what the loop did in the last two seconds */
static void SdlQuitWatch(void);                  /* the engine's own stop signals, sampled every pump */
static void SdlQuitNoteClick(int x, int y);      /* "the player had just clicked here" for that watch */
#ifdef _DEBUG
static void SdlMixDumpFlush(void);               /* empties the mixer's output recorder, from a pump */
#endif
static void SdlDrawHotspots(void);               /* white markers over the copied frame */
static void SdlUpdateHotspots(void);             /* sample Ctrl once per pump */
static void SdlDrawSpinner(void);                /* the busy marks, over the copied frame */
static void SdlCursorKeepAlive(void);            /* the arrow the engine only redraws when it moves */
/* Needed on both sides of the _WIN32 split: SdlUpdateLayout has to force a frame on every
   platform now that presents are coalesced, and the !/_WIN32 event paths each had their own
   prototype of these two. */
static void SdlPresentCurrent(void);             /* defined below; re-presented on expose */
static void SdlFlushPresent(void);               /* flush a coalesced rect update */
#ifdef __SWITCH__
static void SdlPollSwitchControls(void);         /* the pad, once per pump - and see WinShim_PumpSDLToQueue:
                                                    while a movie plays the game pumps through PeekMessageA
                                                    only, so a button read here would never be heard */
#endif

/* client coordinates packed the Win32 way: LOWORD = x, HIWORD = y (both platform halves) */
static LPARAM SdlPackPoint(int x, int y)
{
  return (LPARAM)(((unsigned int)(y & 0xFFFF) << 16) | (unsigned int)(x & 0xFFFF));
}

/* The dispatch flag is functional, not a statistic: SdlBackendPresentRect returns early while
   the port is handing the game its queued messages (the pump's SdlFlushPresent shows those
   rects), and both dispatch loops set it - so it exists in release builds too. */
static int s_InMsgDispatch;                     /* set while the port hands the game its queued messages:
                                                   attributes a rect to the message path */

#ifdef _DEBUG
static unsigned s_RectBlits;                    /* PILOTS_DEBUG_DISSOLVE statistics */
static unsigned s_RectBlitsMsg;                 /* ... of those, blitted while the port was dispatching
                                                   a message: the cursor's own path through
                                                   UpdateCursorPosition -> RenderDirtyRegions. The
                                                   split is what says whether a screen with a moving
                                                   pointer still animates (frame rects) or only draws
                                                   the pointer (message rects). */
static unsigned s_FramePolls;                   /* GetCursorPos polls from main.c: one per game frame */
static unsigned s_Presents;
static unsigned int s_PresentUs;                /* time spent inside SdlPresentCurrent, us */

/* ---- what one scene transition actually costs ----
 *
 * The checkerboard between two screens is not one big blit: the game's dissolve is a
 * grid of small rects, each of which arrives here as its own SdlBackendPresentRect
 * (PILOTS_DEBUG_DISSOLVE's own note counts ~1280 of them for one sweep). Whether the
 * time then goes into the palette conversion, into uploading the rect, or into the
 * present is the whole optimisation question, and the three have nothing in common as
 * fixes - so all three are timed, per rect, and summed for a burst.
 *
 * A burst is closed by silence: BURST_GAP_MS without a single rect blit. That is how a
 * transition ends in reality - the game blits its tiles, then stops to do something else.
 * This is deliberately NOT behind an environment variable: the console has no shell to
 * set one in, and the same line comes out of an x86, an x64 and a Switch run doing the
 * same thing, which is what makes it the number to compare between them.
 *
 * Only bursts of real size are logged (a resting screen repaints a few rects), and only
 * a limited number of them, so a long session stays readable. Debug builds only: in
 * release every DBG_* is gone, and so is this.
 */
#define BURST_GAP_MS     120                      /* this long without a rect blit = the burst is over */
#define BURST_MIN_RECTS  30                       /* below this it is ordinary repaint, not a transition */
#define BURST_MAX_LOGS   40                       /* keep a long session's log readable */

static unsigned s_BurstRects;                    /* 0 = no burst open */
static unsigned s_BurstPixels;                   /* sum of w*h over the burst's rects */
static unsigned s_BurstPresents;
static unsigned int s_BurstConvertUs;            /* palette LUT into the shadow: the per-rect cost */
static unsigned int s_BurstPresentUs;            /* shown frames, upload included: the whole-frame cost */
static unsigned s_BurstFirstRect;                /* width<<16 | height of the first rect: the tile */
static Uint32 s_BurstStartTick;
static Uint32 s_BurstLastTick;
static unsigned s_BurstLines;

/* Everything below is a difference of this, so its wrap (every ~71 min) is invisible. */
static unsigned int SdlNowUs(void)
{
  return (unsigned int)(SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency());
}

static void SdlBurstLog(void)
{
  if ( s_BurstRects >= BURST_MIN_RECTS && s_BurstLines < BURST_MAX_LOGS )
  {
    ++s_BurstLines;
    DBG_LOG("SdlPresentBurst #%u: %u rect blits, %u Kpx converted, %u ms wall; into the shadow %u us; shown by %u presents in %u us; first rect %dx%d",
            s_BurstLines, s_BurstRects, s_BurstPixels / 1024,
            (unsigned)(s_BurstLastTick - s_BurstStartTick),
            s_BurstConvertUs, s_BurstPresents, s_BurstPresentUs,
            (int)(s_BurstFirstRect >> 16), (int)(s_BurstFirstRect & 0xFFFF));
  }
  s_BurstRects = 0;                              /* closed: the next rect opens a new one */
}

static void SdlBurstNote(Uint32 now, int w, int h, unsigned int convertUs)
{
  if ( !s_BurstRects || (Sint32)(now - s_BurstLastTick) > BURST_GAP_MS )
  {
    SdlBurstLog();                               /* whatever was open before is finished business */
    s_BurstPixels = 0;
    s_BurstPresents = 0;
    s_BurstConvertUs = 0;
    s_BurstPresentUs = 0;
    s_BurstFirstRect = ((unsigned)w << 16) | (unsigned)h;
    s_BurstStartTick = now;
  }
  ++s_BurstRects;
  s_BurstPixels += (unsigned)(w * h);
  s_BurstConvertUs += convertUs;
  s_BurstLastTick = now;
}

/* Called from the pumps: a burst that ended (the game stopped blitting) is reported here,
   so a transition's line appears without waiting for the next transition to start. */
static void SdlBurstReportIfOver(void)
{
  Uint32 now = SDL_GetTicks();

  if ( s_BurstRects && (Sint32)(now - s_BurstLastTick) >= BURST_GAP_MS )
    SdlBurstLog();
}
#endif

static SDL_AudioDeviceID s_WaveDev;             /* 11025 Hz / AUDIO_U8 / mono queue-audio device */
static SDL_AudioDeviceID s_MidiDev;             /* 44100 Hz / AUDIO_S16SYS / stereo queue-audio device */
static int s_WaveOpen;                          /* the game's idea that a wave device exists:
                                                   in shared mode there is no s_WaveDev to ask */
static int s_WaveVolume = 255;                  /* 0..255, applied at queue time (u8 PCM centered at 128) */

/* The shared mixer's data, up here with the two devices it replaces because
   SdlBackendShutdown (well above) has to close it; the mixer's functions live with the
   audio sections further down, where the reasons for each choice - and how the mode is
   turned on per platform - are written out. */

#define SDL_MIX_RATE   44100                    /* the synth's own rate: no resampling of the
                                                   louder, longer source, only of the blips */
#define SDL_MIX_RATIO  4                        /* SDL_MIX_RATE / the game's 11025 */
#define SDL_MIX_SAMPLES 1024
#define SDL_MIX_RING   32768u                   /* frames per ring, a power of two: 0.74 s */
#define SDL_MIX_MASK   (SDL_MIX_RING - 1)

typedef struct
{
  short *pcm;                                   /* stereo interleaved, SDL_MIX_RING frames */
  volatile unsigned int w;                      /* monotonic frame counters; wrapping at 2^32
                                                   is fine because only their difference is
                                                   ever read, and that fits in an int */
  volatile unsigned int r;
} MixRing;

static int s_SharedAudio;                       /* 1 = one device, a ring per source, mix in the callback */
static SDL_AudioDeviceID s_MixDev;              /* that one device, when they are shared */
static MixRing s_WaveRing;
static MixRing s_MidiRing;
static MixRing s_MovieRing;                     /* a movie's PCM, see SdlMix_MovieOpen */
static volatile unsigned int s_MovieRate;       /* the movie's own sample rate, 0 = no movie audio */
static unsigned int s_MovieSrcFrames;           /* source samples handed over since the movie started */
static volatile unsigned int s_WaveFramesQueued;
static volatile unsigned int s_WaveFramesEaten; /* both monotonic: their difference / ratio is
                                                   the game's pending byte count */
static volatile int s_WaveHeld;                 /* wave-only pause: pausing the shared device
                                                   would stop the music with it, so the mixer
                                                   just stops reading the wave ring instead */

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
    case WM_KEYDOWN:                            /* VK codes from game data match wParam natively */
      if ( wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT )
      {
        /* '-' (port feature): the picture fills the screen instead of keeping the game's aspect.
           Swallowed: the game has no binding for this key, and the toggle is the port's business.
           Alt+Enter is handled in WM_SYSKEYDOWN below, the same way. */
        SdlBackendToggleStretch();
        return 0;
      }
      return MainWndProc(hWnd, uMsg, wParam, lParam);

    case WM_PAINT:                              /* dirty rect -> AddDirtyRegion + DefWindowProc (validated) */
    case WM_QUERYNEWPALETTE:
    case WM_PALETTECHANGED:
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
  HWND hwnd = (HWND)g_MainWindow;          /* 0 before CreateMainWindow: input events drop */

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
        if ( ev->type == SDL_KEYDOWN
          && (ev->key.keysym.scancode == SDL_SCANCODE_MINUS
           || ev->key.keysym.scancode == SDL_SCANCODE_KP_MINUS) )
        {
          SdlBackendToggleStretch();            /* '-' (port feature), the same key as on Windows:
                                                   the game never sees it either */
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

    case SDL_FINGERDOWN:
    case SDL_FINGERUP:
    case SDL_FINGERMOTION:
      {
        /* The touchscreen. SDL's Switch build reads it on its own (SDL_switchtouch.o),
           and reports a finger as a position normalised to the window, so the first step
           is to put it back into client pixels - from there this is the mouse path, with
           the same picture-rect mapping, the same letterbox rejection and the same
           messages. A finger is the LEFT button: a touch is a click, not a hover. SDL's
           own touch-to-mouse synthesis is turned off in SdlBackendStartup, or every touch
           here would arrive twice. */
        int mx, my, ww = s_GameClientW, wh = s_GameClientH;
        if ( !hwnd )
          break;
        if ( s_SdlWindow )
          SDL_GetWindowSize(s_SdlWindow, &ww, &wh);
        mx = (int)(ev->tfinger.x * ww);
        my = (int)(ev->tfinger.y * wh);
        if ( !SdlMapMousePoint(&mx, &my) )
          break;                          /* touched the letterbox: nothing to hit */
#ifdef _DEBUG
        if ( s_FingerLogged < 6 )
        {
          s_FingerLogged++;
          DBG_LOG("touch: %d window %dx%d client (%d,%d) -> game (%d,%d)",
                  ev->type, ww, wh, (int)(ev->tfinger.x * ww), (int)(ev->tfinger.y * wh), mx, my);
        }
#endif
        PostMessageA(hwnd, WM_SETCURSOR, (WPARAM)hwnd, HTCLIENT);
        PostMessageA(hwnd, WM_MOUSEMOVE, 0, SdlPackPoint(mx, my));
#ifdef __SWITCH__
        s_StickX = mx;                    /* the stick carries on from where the finger left it */
        s_StickY = my;
#endif
        if ( ev->type == SDL_FINGERDOWN )
          PostMessageA(hwnd, WM_LBUTTONDOWN, 0, SdlPackPoint(mx, my));
        else if ( ev->type == SDL_FINGERUP )
          PostMessageA(hwnd, WM_LBUTTONUP, 0, SdlPackPoint(mx, my));
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

#ifdef __SWITCH__
  /* The pad here too, not only in SdlBackendPumpEvents, because this is the pump a movie
     uses: video.c waits with `while (g_VideoPlaying) { PeekMessageA; Sleep(5) }` and
     PeekMessageA ends up here, never in SdlBackendPumpEvents. Reading the pad only there
     makes the stick and both buttons deaf exactly while the intro plays - and A is what is
     meant to skip it (VideoWndProc's own 0x201 case, with MainWndProc delegating every
     message to it while g_VideoFlags[1] is up). Touch already arrives through
     SdlDispatchSdlEvent below. */
  SdlPollSwitchControls();
#endif
  SdlCursorKeepAlive();                         /* a movie pumps here and nowhere else, so the arrow would
                                                   fade for its whole duration without this */
  SdlLoopProfileTick();
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

/* The log need not wait for the SD card. OutputDebugStringA (win32_shim.c) writes
   every DBG_* line to stderr as well as into the log file, and on the Switch stderr
   can be pointed at the machine that launched the app - the actual work is in
   switch_libnx.c, which is its own translation unit for the reason spelled out
   there (libnx and the generated global aliases cannot be parsed in the other
   order). Started from the hbmenu list, the call does nothing.
   Why it matters here: after a crash the log file on the card is complete and
   Atmosphere writes its crash report next to it, so nothing is lost - but only
   this channel shows the log while the app is still up, which is the difference
   between "it died somewhere in the resources" and seeing the number that was
   about to kill it. */
int SdlBackendStartup(void)
{
#ifdef __SWITCH__
  SdlSwitchAttachDebugChannel();
  SdlSwitchReportEnvironment();
#endif
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
#ifdef __SWITCH__
  /* SDL synthesises a left click out of every finger unless told not to
     (SDL_HINT_TOUCH_MOUSE_EVENTS is on by default). This port does that conversion
     itself, in the SDL_FINGER cases of SdlDispatchSdlEvent, where the touch can be put
     through the picture-rect mapping like any other point - so SDL's own version is
     switched off here, or one touch would reach the game as two clicks. */
  SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
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
/* Redraw immediately after the picture's rect moved. Only the renderer applies that rect, and
   only on its next frame - and with presents coalesced nothing promises such a frame: the
   game may have blitted nothing at all since. This is the same need SdlUpdateHotspots
   documents for the Ctrl markers, and the reason a fullscreen toggle could otherwise leave
   the previous scaling on screen until the game next drew something. */
static void SdlShowLayoutChange(void)
{
  if ( s_Texture && s_Renderer )
    SdlPresentCurrent();
}

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
    /* Windowed: the picture IS the client. Leaving fullscreen lands here, so this path has
       to notice a change too, not just record the identity rect. */
    changed = (s_PicX != 0 || s_PicY != 0 || s_PicW != w || s_PicH != h);
    s_PicX = 0;
    s_PicY = 0;
    s_PicW = w;
    s_PicH = h;
    if ( changed )
      SdlShowLayoutChange();
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
  else if ( s_StretchPic )
  {
    picW = ww;                                  /* '-' (port feature): the whole screen, the game's
                                                   aspect deliberately not kept - that is the point
                                                   of the key, and clicks still map back through
                                                   this rect, so nothing else has to know */
    picH = wh;
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
    DBG_LOG("SdlUpdateLayout: window %dx%d -> picture %dx%d at (%d,%d)%s (renderer output %dx%d)%s",
            ww, wh, s_PicW, s_PicH, s_PicX, s_PicY,
            s_NativeMovie ? " (native movie)" : "", outW, outH,
            s_StretchPic ? " [- stretched]" : "");
#ifdef _WIN32
    VideoRealignMciChild();                     /* the movie child follows the picture rect */
#endif
    SdlShowLayoutChange();
  }
}

/* window client px -> game client px (identity while the picture is the window) */
void SdlBackendClientToGame(int *x, int *y)
{
  if ( !SdlPictureTransformed() )
    return;
  *x = SdlFloorDiv((*x - s_PicX) * s_GameClientW, s_PicW);
  *y = SdlFloorDiv((*y - s_PicY) * s_GameClientH, s_PicH);
  /* A result outside the picture reads as inputState=0 in main.c ("erase the cursor, do not
     repaint it") - the letterbox drops are by design, and a cursor-vanishing hunt starts here. */
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

#ifdef __SWITCH__
/* The console's stick and buttons, turned into the mouse messages the game already speaks:
 * SdlDispatchSdlEvent does the same thing for a real mouse, so the game cannot tell the two
 * apart - same WM_SETCURSOR first, same packed game coordinates after it.
 *
 * The stream is THROTTLED, and the rate is the whole story. Every posted WM_MOUSEMOVE makes the
 * wndproc run UpdateCursorPosition -> RenderDirtyRegions SYNCHRONOUSLY, and the stick used to
 * post one every pump it was held - 343-650 messages/s measured. At that rate the dirty-region
 * pass alone starved the frame loop's scene work: the walking character froze under a moving
 * cursor while the loop itself stayed healthy (frame age 89 ms), because each pass rebuilds and
 * re-blits the render-object list over its snapshot restore. MOVE_MIN_INTERVAL_MS bounds the
 * stream to a real mouse's rate; the cursor then repaints at that rate and the scene keeps
 * animating.
 *
 * (Writing g_VideoConfig.cursorX/Y straight from the pad - no messages at all - was tried and is
 * wrong, and it is worth keeping the reason: the frame-end HandleGameEvent repaints the cursor
 * object only while g_InputState[1] is set, and the ONLY writer of that bit that does not depend
 * on main.c's own pointer-inside-the-picture test is UpdateCursorPosition, the per-message path
 * such a change removes. Without the stream the engine's arrow stops being painted at all - the
 * level-select map showed none, and in the scenes it appeared only where the frame-end happened
 * to move it and vanished again. The stream is the game's own cursor path; throttling it is
 * enough.)
 *
 *  - Left stick MOVES the cursor, it does not position it. The stick's own range is
 *    +-0x7FFF and the picture is s_GameClientW px wide, so an absolute mapping would
 *    jump the cursor to wherever the stick happened to be resting; and the cursor is
 *    clamped, so it can never end up outside the picture the way a real mouse can.
 *  - A is the left button, B the right one. Both are posted at the current cursor
 *    position, and both are posted twice, down and up, because the game's click is a
 *    press and the release is what ends it.
 *  - R held marks the interactive objects: the same overlay Ctrl shows (SdlUpdateHotspots
 *    samples it as one more "held" source). Nothing walks or clicks a list on a button
 *    press any more: the old walk-and-click "turn the pilots over" scheme was removed after
 *    the console measured it clicking the screen's EXIT button ((607,450) on the level map,
 *    (552,448) on the startup screen, where the second click quit the game through the
 *    script's own loop-stop byte).
 *  - X and Y are read but not mapped to anything yet: they are in the log and in the
 *    button mask, so mapping one later is one line here.
 */
#define STICK_DEADZONE 3000        /* of 0x7FFF: a stick at rest drifts by a few hundred */
#define STICK_SPEED_PX_PER_SEC 640 /* px per SECOND at full deflection: crosses the picture in one second.
                                      The speed used to be per PUMP, and the pump is anything but a
                                      clock: measured on the console, 50-60 pumps/s at rest, on the
                                      level map an average of 17350/s (peak 143924/s - its busy loop),
                                      in a busy level 47337/s peak. One pixel per pump at the map's
                                      rate crosses the picture in 37 ms, so any touch of the stick
                                      past the 8% quantization edge pinned the cursor to the screen
                                      border (clipped sprite - reads as "the cursor vanished") and
                                      the 50 ms throttle then posted that border position as a jump:
                                      the measured "jerkily, with jumps, ending at the screen edges".
                                      Integrating over milliseconds makes the cursor's speed a
                                      property of the STICK alone - the pump rate cannot touch it. */
#define MOVE_MIN_INTERVAL_MS 50    /* the throttle that keeps the scene animating while the cursor moves;
                                      see the block comment above. 50 ms is a real mouse's report rate. */

static void SdlPostClick(UINT downMsg, UINT upMsg, int down)
{
  if ( !down )
  {
    PostMessageA(g_MainWindow, upMsg, 0, SdlPackPoint(s_StickX, s_StickY));
    return;
  }
  PostMessageA(g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
  PostMessageA(g_MainWindow, WM_MOUSEMOVE, 0, SdlPackPoint(s_StickX, s_StickY));
  PostMessageA(g_MainWindow, downMsg, 0, SdlPackPoint(s_StickX, s_StickY));
}

/* The dump of what the engine would click, kept because that list is what turns "the second
 * one from the left" into coordinates - it printed (31,450), (95,448), (223,448), (607,450)
 * on the character screen, and (607,450) turned out to be the button that LEAVES the screen,
 * which is what the removed walk eventually clicked (and, one screen later, quit the game).
 * R held shows the same list as the Ctrl overlay discs; the dump prints on the R press so
 * the log keeps the numbers next to the picture. */
static void SdlDumpHotspots(int which)
{
#ifdef _DEBUG
  int i;

  /* Collected here, not read from the overlay's copy: the R press edge lands in the same pump
     that sets s_PadOverlayHeld, one pump before SdlUpdateHotspots has seen it - so the overlay's
     list would still be the previous one, which on a fresh press is empty. */
  SdlCollectHotspots();
  DBG_LOG("SdlSwitchControls: %s - %d object(s) the engine would click on, in the order the engine walks them:",
          which ? "R" : "L", s_HotspotCount);
  for ( i = 0; i < s_HotspotCount; ++i )
    DBG_LOG("SdlSwitchControls:   #%d at (%d,%d)", i, s_HotspotX[i], s_HotspotY[i]);
#endif
}

static void SdlPollSwitchControls(void)
{
  static unsigned int s_PrevButtons;
  static unsigned int s_LastStickPoll;        /* ms of the previous poll - the stick integrates over TIME */
  static long long s_StickAccX, s_StickAccY;  /* sub-pixel remainder: a slight deflection must creep, not stall
                                                 at zero until the integer division finally pays out a pixel */
  int sx, sy;
  unsigned int buttons;
  int dx, dy, nx, ny;
  unsigned int now;
  int dt;
  long long ax, ay;

  if ( !g_MainWindow )
    return;
  if ( s_StickX < 0 )
  {
    s_StickX = s_GameClientW / 2;               /* the middle: the one point certain to be inside */
    s_StickY = s_GameClientH / 2;
  }

  SdlSwitchPollPad(&sx, &sy, &buttons);

  if ( sx > -STICK_DEADZONE && sx < STICK_DEADZONE )
    sx = 0;                                     /* a cursor that creeps on its own is worse than a dead edge */
  if ( sy > -STICK_DEADZONE && sy < STICK_DEADZONE )
    sy = 0;
  now = SDL_GetTicks();
  dt = (int)(now - s_LastStickPoll);
  s_LastStickPoll = now;
  if ( dt < 0 || dt > 100 )
    dt = 100;                                   /* the first poll and a stall alike: never fling the cursor */
  ax = s_StickAccX + (long long)sx * STICK_SPEED_PX_PER_SEC * dt;
  ay = s_StickAccY - (long long)sy * STICK_SPEED_PX_PER_SEC * dt; /* the stick points up, the client y grows down */
  dx = (int)(ax / (1000 * 0x7FFF));
  dy = (int)(ay / (1000 * 0x7FFF));
  s_StickAccX = ax - (long long)dx * (1000 * 0x7FFF);
  s_StickAccY = ay - (long long)dy * (1000 * 0x7FFF);

  nx = s_StickX + dx;
  ny = s_StickY + dy;
  if ( nx < 0 )
    nx = 0;
  if ( ny < 0 )
    ny = 0;
  if ( nx > s_GameClientW - 1 )
    nx = s_GameClientW - 1;
  if ( ny > s_GameClientH - 1 )
    ny = s_GameClientH - 1;
  if ( nx != s_StickX || ny != s_StickY )
  {
    s_StickX = nx;
    s_StickY = ny;
    s_MovePending = 1;                           /* posted below, once the throttle allows */
  }
  /* Throttled WM_MOUSEMOVE: the wndproc repaints the cursor SYNCHRONOUSLY per message (see the
     block comment), so the stream's rate is both the cursor's repaint rate and a tax on the
     scene's frame budget. Posting the CURRENT position - never a backlog - means one message is
     enough however many pumps were skipped; the pending flag flushes the final position within
     one interval of the stick letting go, so the cursor never stops short of where it was aimed. */
  if ( s_MovePending && SDL_GetTicks() - s_LastMovePost >= MOVE_MIN_INTERVAL_MS )
  {
    s_MovePending = 0;
    s_LastMovePost = SDL_GetTicks();
    PostMessageA(g_MainWindow, WM_SETCURSOR, (WPARAM)g_MainWindow, HTCLIENT);
    PostMessageA(g_MainWindow, WM_MOUSEMOVE, 0, SdlPackPoint(s_StickX, s_StickY));
  }

  /* Every edge the pad can produce is now a line, because "the game closed itself" has to be
     readable next to what the player did: A/B say where the pointer stood (the click lands on
     the cursor's coordinates, not on the button's), X/Y are the two bits with no binding at
     all - if the player is pressing them, that has to be visible instead of looking like a
     dead button. */
  if ( (buttons & SWITCH_PAD_A) != (s_PrevButtons & SWITCH_PAD_A) )
  {
    DBG_LOG("SdlSwitchControls: A %s -> left click at (%d,%d)",
            (buttons & SWITCH_PAD_A) ? "down" : "up", s_StickX, s_StickY);
    if ( buttons & SWITCH_PAD_A )
      SdlQuitNoteClick(s_StickX, s_StickY);      /* the press: kept for the exit report */
    SdlPostClick(WM_LBUTTONDOWN, WM_LBUTTONUP, (buttons & SWITCH_PAD_A) != 0);
  }
  if ( (buttons & SWITCH_PAD_B) != (s_PrevButtons & SWITCH_PAD_B) )
  {
    DBG_LOG("SdlSwitchControls: B %s -> right click at (%d,%d)",
            (buttons & SWITCH_PAD_B) ? "down" : "up", s_StickX, s_StickY);
    SdlPostClick(WM_RBUTTONDOWN, WM_RBUTTONUP, (buttons & SWITCH_PAD_B) != 0);
  }
  if ( (buttons & (SWITCH_PAD_X | SWITCH_PAD_Y)) != (s_PrevButtons & (SWITCH_PAD_X | SWITCH_PAD_Y)) )
    DBG_LOG("SdlSwitchControls: X/Y edge, prev=0x%X now=0x%X (no binding on the console, ignored)",
            s_PrevButtons & (SWITCH_PAD_X | SWITCH_PAD_Y),
            buttons & (SWITCH_PAD_X | SWITCH_PAD_Y));

  /* R is a HELD state, not an edge: held, it asks for the interactive-object overlay - the
     same picture Ctrl gives (SdlUpdateHotspots samples s_PadOverlayHeld as a second "held"
     source next to GetAsyncKeyState). The sample lands one pump late, because
     SdlUpdateHotspots runs before the pad poll in SdlBackendPumpEvents; a pump is a
     millisecond, so the marker discs simply follow the button by that much. On the press
     edge the clickable list is dumped, because the overlay shows WHERE the objects are and
     the log is what names them in numbers. */
  s_PadOverlayHeld = (buttons & SWITCH_PAD_R) != 0;
  if ( (buttons & SWITCH_PAD_R) && !(s_PrevButtons & SWITCH_PAD_R) )
    SdlDumpHotspots(1);
  /* This button binds nothing, but its press edge is still said, capped: a button that is
     silently dead is a bug report waiting to happen. */
  if ( (buttons & SWITCH_PAD_L) && !(s_PrevButtons & SWITCH_PAD_L) && s_RLedgesLogged < 8 )
  {
    ++s_RLedgesLogged;
    DBG_LOG("SdlSwitchControls: L pressed - no binding (the overlay is on R now); %d edge line(s) of %d",
            s_RLedgesLogged, 8);
  }

  /* "-" (the Joy-Con minus): the picture's own stretch toggle, port-side like Alt+Enter -
     the game has no binding for it and is not told about it. Press edge only, so holding it
     does not flicker between the two modes. From a window the first press goes fullscreen and
     stretched, because "over the whole screen" needs the screen. */
  if ( (buttons & SWITCH_PAD_MINUS) && !(s_PrevButtons & SWITCH_PAD_MINUS) )
  {
    DBG_LOG("SdlSwitchControls: '-' (minus) pressed -> the port's picture toggle");
    SdlBackendToggleStretch();
  }

  s_PrevButtons = buttons;
}
#endif /* __SWITCH__ */

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

/* '-' (port feature): switch between the game's own picture (letterboxed, aspect kept) and the
   same picture stretched over the whole screen. Asking for the second half needs fullscreen, so
   the key implies it: from a window it goes fullscreen and stretched, and from then on it only
   flips the stretching. Alt+Enter stays the way back to the window, and a windowed picture stays
   the identity it always was. */
int SdlBackendToggleStretch(void)
{
  s_StretchPic = !s_StretchPic;
  if ( s_StretchPic && !s_Fullscreen )
    SdlBackendSetFullscreen(1);                  /* it applies the layout, and sees the stretch */
  else
    SdlUpdateLayout();
  DBG_LOG("SdlBackendToggleStretch: %s -> picture %dx%d at (%d,%d)",
          s_StretchPic ? "stretched over the screen" : "the game's own aspect, letterboxed",
          s_PicW, s_PicH, s_PicX, s_PicY);
  return s_StretchPic;
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

#ifdef __SWITCH__
/* The homebrew menu passes no command line, so -datapath cannot be used there
   and the engine would look for .\PILOTS.EXE at whatever the CWD happens to be
   (the SD root). Fall back to the usual homebrew location - but only when the
   game data is really in it: whoever prefers the data elsewhere keeps exactly
   the previous behaviour, and if nothing is there the CWD is left untouched,
   so this can only ever add a case, never remove one. Goes through the same
   SetCurrentDirectoryA as -datapath, so the sdmc: prefix and the
   case-insensitive walk stay in one place. */
static void SdlSwitchDefaultDataPath(void)
{
  char probe[MAX_PATH];

  strcpy(probe, "sdmc:/switch/pilots");
  strcat(probe, "/PILOTS.EXE");
  if ( GetFileAttributesA(probe) == (DWORD)-1 )
    return;
  if ( SetCurrentDirectoryA("sdmc:/switch/pilots") )
    DBG_LOG("SdlBackendParseCmdLine: no -datapath on the Switch, using sdmc:/switch/pilots");
}
#endif /* __SWITCH__ */

int SdlBackendParseCmdLine(const char *cmdLine)
{
  const char *p = cmdLine;
  const char *tok;
  int len;
  int found = 0;
  int dataPathGiven = 0;

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
      {
        dataPathGiven = 1;
        SdlUseDataPath(val, vlen);
      }
      else
        DBG_LOG("SdlBackendParseCmdLine: -datapath needs a directory, ignored");
    }
  }
#ifdef __SWITCH__
  if ( !dataPathGiven )
    SdlSwitchDefaultDataPath();
#else
  (void)dataPathGiven;
#endif
  if ( found )
  {
    DBG_LOG("SdlBackendParseCmdLine: -fullscreen -> borderless desktop fullscreen, Alt+Enter toggles");
    SdlBackendSetFullscreen(1);
  }
  return found;
}

void SdlBackendShutdown(void)
{
  /* Two lines that bracket the teardown: the last thing in the log decides whose crash an
     ugly exit was - ours, or the loader's (see the note under SDL_Quit below). */
  DBG_LOG("SdlBackendShutdown: enter");
  /* If WinHelpA(HELP_QUIT) already reported, this is a no-op; if it never came (WM_QUIT, or
     pErrorMessage == 1 - both jump over CleanupHelpAndWindow), the last pump's snapshot is
     what is left of the reason, so it still gets one line. */
  SdlQuitReportExit("SdlBackendShutdown - the loop left some other way (WM_QUIT / pErrorMessage == 1)");
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
  if ( s_Shadow )
  {
    free(s_Shadow);                             /* the frame the port kept for itself; the texture it
                                                   was uploaded to is already gone */
    s_Shadow = 0;
    s_ShadowPixels = 0;
    s_ShadowW = s_ShadowH = 0;
  }
  SdlMidi_Close();                       /* normally already done by ShutdownMultimedia; idempotent */
  if ( s_WaveDev )
  {
    SDL_CloseAudioDevice(s_WaveDev);
    s_WaveDev = 0;
  }
  if ( s_MixDev )
  {
    /* Both sources have said goodbye above, so the one device they shared can go too -
       and the rings with it, in that order: closing the device is what stops the callback
       that reads them. */
    SDL_CloseAudioDevice(s_MixDev);
    s_MixDev = 0;
    free(s_WaveRing.pcm);
    s_WaveRing.pcm = 0;
    free(s_MidiRing.pcm);
    s_MidiRing.pcm = 0;
    free(s_MovieRing.pcm);                        /* a movie that was never closed still ends here */
    s_MovieRing.pcm = 0;
  }
  SDL_Quit();
#ifdef __SWITCH__
  /* Last thing, and only because the console's loader makes it one. The pool is mapped a
     second time, at a low address (SdlSwitchMapLowMemory), and that alias keeps
     references to pages hbloader owns and takes back when the app returns to it. Leaving
     it in place is what made hbloader break at exit - a fatal inside the loader reads as
     "the game crashed on exit", and its report had no PILOT_RV offset anywhere in it. */
  SdlSwitchUnmapLowMemory();
#endif
  DBG_LOG("SdlBackendShutdown: everything torn down, handing control back to the loader");
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
#ifdef __SWITCH__
  /* SDL_StartTextInput() is what turns SDL_TEXTINPUT into the WM_CHAR the game reads, so
     Linux needs it. The console must not ask for it: its backend implements it by opening
     the on-screen keyboard (SDL_switchswkb.o: SWITCH_StartTextInput -> swkbdInlineAppear),
     which spends a few seconds on the swkbd handshake before the keyboard appears at all -
     a stall at startup and an on-screen keyboard over a game that has no use for either.
     Keys still reach the game as SDL_KEYDOWN -> WM_KEYDOWN; only WM_CHAR would be missing,
     and on this console there is no physical keyboard to type with. Opening the console's
     keyboard deliberately - the game does ask for text in the original - is a separate
     port feature, not something to do as a side effect of creating a window. */
  DBG_LOG("SdlBackendCreateWindow: text input left off on purpose - on this console SDL_StartTextInput opens the on-screen keyboard");
#else
  SDL_StartTextInput();                    /* SDL_TEXTINPUT -> WM_CHAR (Unicode ANSI parity) */
#endif
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

  /* The frame the game paints, kept locally at exactly the texture's size:
     SdlBackendPresentRect converts its rects into this, and it is the only thing ever
     handed to SDL_UpdateTexture. One whole upload per shown frame beats one upload per
     tile, because SDL's GLES2 backend re-uploads a streaming texture in full on unlock
     (see the measurement by s_Shadow). */
  if ( s_ShadowPixels < clientW * clientH )
  {
    free(s_Shadow);
    s_Shadow = (unsigned int *)malloc((size_t)clientW * clientH * sizeof(unsigned int));
    s_ShadowPixels = s_Shadow ? clientW * clientH : 0;
  }
  if ( !s_Shadow )
  {
    DBG_LOG("SdlBackendCreateVideoTargets: cannot allocate a %dx%d shadow frame, refusing the video targets",
            clientW, clientH);
    SDL_DestroyTexture(s_Texture);
    s_Texture = 0;
    s_TexWidth = s_TexHeight = 0;
    return 0;
  }
  s_ShadowW = clientW;
  s_ShadowH = clientH;
  {
    int i;
    for ( i = 0; i < clientW * clientH; ++i )
      s_Shadow[i] = 0xFF000000u;              /* black, as SetPalette's FillRect would leave it: a new
                                                 texture has nothing the game drew, so show black rather
                                                 than whatever the allocator handed back */
  }
  s_PresentPending = 1;
  s_RectsSincePresent = 0;
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
  /* ~4.5 game px (1.5x the old 3), so the marker scales with the picture; clamped because
     the extremes are either invisible or a blob. One radius for X and Y keeps it round.
     The half pixel rounds up: 3 becomes 5, because rounding 4.5 down would grow the marker
     by 1.33 instead of the asked-for 1.5. */
  r = s_GameClientH > 0 ? (9 * s_PicH + s_GameClientH) / (2 * s_GameClientH) : 5;
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
  if ( s_LoadStart && (SDL_GetTicks() - s_LoadBeatTick) < SDL_LOAD_ACTIVE_MS )
    SdlDrawSpinner();                            /* the game is loading: say so over the still frame,
                                                    and only while the reads say it really is */
}

/* Hand the shadow frame to the texture, once, for everything the game blitted since the
   last shown frame. Called where a frame is about to be composed (SdlPresentCurrent, and
   the movie frame, which draws the game texture under the movie). */
static void SdlUploadShadow(void)
{
  if ( !s_PresentPending )
    return;
  if ( !s_Texture || !s_Shadow )
  {
    s_PresentPending = 0;
    return;
  }
  SDL_UpdateTexture(s_Texture, NULL, s_Shadow, s_ShadowW * 4);
  s_PresentPending = 0;
  s_RectsSincePresent = 0;
}

static void SdlPresentCurrent(void)
{
#ifdef _DEBUG
  Uint64 perf0 = SDL_GetPerformanceCounter();
#endif
  if ( !s_Texture || !s_Renderer )
    return;
  SdlUploadShadow();
  SdlDrawGameFrame();
  SDL_RenderPresent(s_Renderer);
  s_PresentPending = 0;                         /* the texture as it stands is on screen now */
  s_LastPresentTick = SDL_GetTicks();           /* present throttling: counts every frame shown */
#ifdef _DEBUG
  ++s_Presents;
  {
    unsigned int us = (unsigned)((SDL_GetPerformanceCounter() - perf0)
                                 * 1000000ull / SDL_GetPerformanceFrequency());
    s_PresentUs += us;
    if ( s_BurstRects )
    {
      ++s_BurstPresents;                           /* the count matters as much as the time: 1200 vs 30 is the whole point */
      s_BurstPresentUs += us;                      /* only presents inside a burst belong to it */
    }
  }
#endif
}

/* A rotating ring of marks with a fading tail - the shape a busy indicator has everywhere. Drawn
   procedurally rather than from the game's icon on purpose: the .ico is carved out of PILOTS.EXE
   (it is game data, and only the Windows build has it next to the exe), and an indicator that
   explains a hang must not itself depend on an optional file. Sizes come from the picture, so it
   is as legible on the console's 1280x720 as in a 640x480 window, and it sits inside the picture
   rect rather than in the letterbox, where nobody would look. */
static void SdlDrawSpinner(void)
{
  static const short dx[8] = { 8, 6, 0, -6, -8, -6, 0, 6 };   /* a unit octagon: radius / 8 */
  static const short dy[8] = { 0, -6, -8, -6, 0, 6, 8, 6 };
  int r = s_PicH / 22;
  int cx = s_PicX + s_PicW / 2;
  int cy = s_PicY + s_PicH - 2 * r - 6;
  float size;
  SDL_Rect bar;
  SDL_FRect dot;
  int i;

  if ( r < 8 )
    r = 8;
  size = (float)(r / 2 < 2 ? 2 : r / 2);
  /* A dark strip behind the marks: the game's frames are bright and dark in the same place, and
     white marks need a background of their own to be read on both. */
  bar.x = cx - r * 3;
  bar.y = cy - r - 4;
  bar.w = r * 6;
  bar.h = r * 2 + 8;
  SDL_SetRenderDrawBlendMode(s_Renderer, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(s_Renderer, 0, 0, 0, 130);
  SDL_RenderFillRect(s_Renderer, &bar);
  for ( i = 0; i < 8; ++i )
  {
    int age = (s_LoadPhase - i) & 7;             /* the mark just passed by the head of the ring */
    int alpha = 245 - age * 30;
    float d = (float)(r * 8 - size) / 16.0f;     /* the ring's radius, minus half a mark */

    dot.x = (float)(cx + dx[i] * r / 8 - d);
    dot.y = (float)(cy + dy[i] * r / 8 - d);
    dot.w = size;
    dot.h = size;
    SDL_SetRenderDrawColor(s_Renderer, 255, 255, 255, (Uint8)(alpha < 45 ? 45 : alpha));
    SDL_RenderFillRectF(s_Renderer, &dot);
  }
  SDL_SetRenderDrawColor(s_Renderer, 255, 255, 255, 255);
  SDL_SetRenderDrawBlendMode(s_Renderer, SDL_BLENDMODE_NONE);   /* as SdlDrawGameFrame left it */
}

/* The shim's read path calls this after every successful read. Most reads are ordinary - the game
   reads while it draws, and then the screen is not still, so nothing here happens. What this is
   listening for is a second of silence on screen *with* bytes moving: that is the shape of a load
   the player cannot tell from a hang. The clock it measures on is the last rect the GAME blitted
   (s_LastGameBlit), deliberately not the last present: a layout change or the spinner's own repaint
   shows a frame too, and either one restarting the clock stopped the indicator after a single step
   and emptied its byte count - which is how a 40 s font load came out of the first version as
   "40 s and 0 KB read" with a frozen mark. Once noticed, the spinner is repainted on its own rhythm,
   because during a load no one else would ever present - that is what makes the screen look dead. */
void SdlBackendLoadBeat(unsigned int bytes)
{
  static Uint32 s_QuietMs;                        /* PILOTS_LOAD_QUIET_MS, 0 = not read yet */
  Uint32 still;
  Uint32 now = SDL_GetTicks();

  if ( !s_Texture || !s_Renderer )
    return;
  if ( !s_QuietMs )
  {
    char *e = getenv("PILOTS_LOAD_QUIET_MS");
    unsigned long v = e ? strtoul(e, 0, 10) : 0;
    s_QuietMs = (v >= 50u && v <= 60000u) ? (Uint32)v : SDL_LOAD_QUIET_MS;
    DBG_LOG("SdlLoad: a screen still for %u ms while the game reads counts as loading", s_QuietMs);
  }
  s_LoadBeatTick = now;
  if ( !s_LastGameBlit )
    return;                                       /* nothing on screen yet: nothing to overlay */
  s_IdleBytes += bytes;                           /* counted from the last blit, not from the moment
                                                     the threshold is crossed: the point of the number
                                                     is what the wait cost */
  still = now - s_LastGameBlit;
  if ( still < s_QuietMs )
    return;                                       /* the game drew recently: ordinary reading */
  if ( !s_LoadStart )
  {
    s_LoadStart = s_LastGameBlit;                 /* the stillness began with that frame */
    DBG_LOG("SdlLoad: nothing drawn for %u ms while the game read %lu bytes - showing a spinner",
            (unsigned)still, s_IdleBytes);
  }
  if ( (now - s_LoadSpinTick) >= SDL_LOAD_SPIN_MS )
  {
    s_LoadSpinTick = now;
    ++s_LoadPhase;
    SdlPresentCurrent();                          /* SdlDrawGameFrame draws the marks themselves */
  }
}

/* The game blitted, so it draws again and whatever it was waiting for has arrived: the indicator
   goes away with the frame that replaces it, and what it stood through gets said once. When the
   reads had already stopped, the mark was not drawn any more (SdlDrawGameFrame checks the freshness
   of s_LoadBeatTick) - spinning while nothing is being read would be a lie, and a hang is a
   different thing from a load. */
void SdlBackendFrameShown(void)
{
  Uint32 now = SDL_GetTicks();

  if ( !s_LoadStart )
    return;
  if ( (now - s_LoadBeatTick) >= SDL_LOAD_ACTIVE_MS )
    DBG_LOG("SdlLoad: the screen stood still %u s with %lu KB read, the reads stopped %u ms before the picture moved again",
            (unsigned)((now - s_LoadStart) / 1000u), s_IdleBytes / 1024ul,
            (unsigned)(now - s_LoadBeatTick));
  else
    DBG_LOG("SdlLoad: the screen stood still %u s with %lu KB read, and the game is drawing again",
            (unsigned)((now - s_LoadStart) / 1000u), s_IdleBytes / 1024ul);
  s_LoadStart = 0;
}

/* Show rect updates deferred by the burst coalescing in SdlBackendPresentRect (a no-op
   when nothing is pending). Called from the message pumps so a half-drawn frame never
   waits for the next blit - on Windows as of the measurement at the defer site, where it
   used to be an unconditional no-op because every blit presented on the spot. */
static void SdlFlushPresent(void)
{
  if ( s_PresentPending )
    SdlPresentCurrent();
#ifdef _DEBUG
  /* Every message pump calls this, which makes it exactly the place to notice that the
     game has stopped blitting: that is a finished burst, i.e. an ended transition. */
  SdlBurstReportIfOver();
#endif
}

/* Sample Ctrl and refresh the marker list, once per pump. The forced present on the state
   change is the part that actually makes the feature usable: Windows presents per blit and
   SdlFlushPresent only presents when something was blitted, so on a resting screen nothing
   would ever redraw and the markers would either never appear or never go away. Both edges
   need it - down to show them, up to erase them. */
static void SdlUpdateHotspots(void)
{
  int ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  int held = ctrl || s_PadOverlayHeld;          /* the console's R is the second held source */
  int prevCount = s_HotspotCount;
  int toggled = held != s_HotspotPrev;
  static const char *s_Src = "Ctrl";            /* named on the way down, kept for the way up */

  if ( held )
    s_Src = ctrl ? "Ctrl" : "pad R";
  s_HotspotHeld = held;
  if ( held )
    SdlCollectHotspots();
  else
    s_HotspotCount = 0;
  if ( toggled || s_HotspotCount != prevCount )
    DBG_LOG("SdlUpdateHotspots: %s %s, %d interactive object(s)",
            s_Src, held ? "down" : "up", s_HotspotCount);
  s_HotspotPrev = held;
  if ( toggled && s_Texture && s_Renderer )
    SdlPresentCurrent();
}

/* ---- what the loop is doing (diagnostic) ----
 *
 * Because "the animation stops while I move the pointer" has several very different causes and they
 * look identical from the sofa: the game may be busy replaying a trail of pointer positions, the
 * loop may be drowning in presents, or the queue may be growing because the game simply cannot keep
 * up. One line every two seconds separates them: pump rate (how often the loop comes round), the
 * rate at which the game takes messages out of the queue, the present rate and the time spent inside
 * them, the rect blit rate, and the queue depth right now. Numbers on the sofa, not guesses.
 */
#ifdef _DEBUG
/* Two of the profile's numbers exist only where the port owns the message queue. On Windows that
   queue is user32's and win32_shim.c is not part of the build at all, so asking it would be a link
   error against symbols that legitimately do not exist there; on that platform the queue belongs to
   somebody else and the honest answer is "not measurable". */
static unsigned SdlProfMsgDispatched(void)
{
#ifdef _WIN32
  return 0;
#else
  return WinShim_MsgDispatched();
#endif
}

static int SdlProfQueueDepth(void)
{
#ifdef _WIN32
  return -1;
#else
  return WinShim_MsgQueueDepth();
#endif
}
#endif

/* ---- why the engine ended its frame loop (read-only watch) ----
 *
 * main.c's frame loop has exactly two ways out: SdlBackendPumpEvents() reporting WM_QUIT
 * (goto LABEL_19, straight to CleanupGameResources) and ProcessGameFrame() == 0 (break,
 * which runs CleanupHelpAndWindow -> WinHelpA(HELP_QUIT) first). The log already tells
 * those two apart by whether the WinHelpA line is there - what it cannot say is WHICH of
 * ProcessGameFrame's exit conditions fired, and "the game closed itself" is precisely the
 * report this port has to answer with a reason instead of a guess. The candidates are all
 * engine-side: UpdateFrameRate() reading a non-zero loop-stop byte out of the script's own
 * data (gameloop.c's OPCODE_SET_FRAMERATE hands g_GameLoopDataPtr the script's buffer, so
 * byte 0 of it is the script's), ProcessSpecialEvent/ProcessNormalEvent, HandleGameEvent,
 * RenderDirtyRegions - plus a g_VideoConfig.pErrorMessage the engine set, whose path shows
 * a message without logging a single line.
 *
 * The port is never inside the frame, so it cannot see the return value itself. What it can
 * see is the state: a copy taken on every pump, printed when WinHelpA(HELP_QUIT) comes
 * through - that moment is after the break and BEFORE CleanupGameResources has touched any
 * of these globals, so what gets printed is the state the decision was made on (a second
 * copy at SdlBackendShutdown covers the paths that skip WinHelpA, in which case the last
 * pump's snapshot is what is left). Every one of these variables is only ever READ here;
 * the port writes none of them. The loop-stop byte additionally gets its own line the moment
 * it turns non-zero, because that is the one candidate a pump is guaranteed to see before
 * the final frame: frame N sets the byte, frame N+1's UpdateFrameRate reads it, and main.c
 * pumps in between.
 *
 * The last A click is kept alongside on purpose: a quit that can be read next to "the player
 * had just clicked at (x,y)" answers a different - and much more answerable - question than
 * a quit on its own, which is how an accidental press on an in-game exit is distinguished
 * from a script stopping itself out of the blue. */
static Uint32 s_SnapTick;
static int s_SnapStop;
static int s_SnapErrFlag;
static const char *s_SnapErrMsg;
static long long s_SnapSpecial;
static int s_SnapRecursion;
static int s_SnapEvent, s_SnapKey, s_SnapChar;
static int s_SnapCursorX, s_SnapCursorY;
static Uint32 s_SnapClickTick;
static int s_SnapClickX, s_SnapClickY;
static int s_StopReported;                      /* the loop-stop byte has had its line */
static int s_ErrReported;                       /* ... same for pErrorMessage */
static int s_ExitReported;                      /* one exit report per run, not one per caller */

static void SdlQuitNoteClick(int x, int y)
{
  s_SnapClickX = x;
  s_SnapClickY = y;
  s_SnapClickTick = SDL_GetTicks();
}

static void SdlQuitWatch(void)
{
#ifdef _DEBUG
  s_SnapTick = SDL_GetTicks();
  s_SnapStop = g_GameLoopDataPtr
      ? (int)*(const unsigned char *)&g_GameLoopDataPtr->flags_or_id : -1;   /* -1: no script buffer yet */
  s_SnapErrFlag = g_ErrorFlag;
  s_SnapErrMsg = g_VideoConfig.pErrorMessage;
  s_SnapSpecial = (long long)g_SpecialMode;
  s_SnapRecursion = video_recursion_counter;
  s_SnapEvent = g_VideoConfig.eventCode;
  s_SnapKey = g_VideoConfig.keyIndex;
  s_SnapChar = g_VideoConfig.charCode;
  s_SnapCursorX = g_VideoConfig.cursorX;
  s_SnapCursorY = g_VideoConfig.cursorY;
  if ( s_SnapStop > 0 && !s_StopReported )
  {
    s_StopReported = 1;
    DBG_LOG("SdlQuitWatch: the script's loop-stop byte went to %d -> UpdateFrameRate() returns 0 and main.c breaks out of the frame loop",
            s_SnapStop);
  }
  if ( s_SnapErrMsg && !s_ErrReported )
  {
    unsigned long long e = (unsigned long long)(UINT_PTR)s_SnapErrMsg;

    s_ErrReported = 1;
    /* no %p and no %ll here either: DbgPrintf goes through wvsprintfA on Windows (the same
       subset switch_libnx.c names), so an address is two 32-bit halves */
    DBG_LOG("SdlQuitWatch: the engine set pErrorMessage=0x%08X%08X%s -> main.c's error path (the value 1 means 'quit without showing anything')",
            (unsigned int)(e >> 32), (unsigned int)e,
            s_SnapErrMsg == (const char *)(INT_PTR)1 ? " [1]" : "");
  }
#endif
}

/* Printed from WinHelpA(HELP_QUIT) - the first thing that runs after main.c's break - and,
   if that never comes, from SdlBackendShutdown. Read-only, and once per run. */
void SdlQuitReportExit(const char *where)
{
#ifdef _DEBUG
  Uint32 now = SDL_GetTicks();
  /* DbgPrintf is wvsprintfA on Windows: no %p, no %ll (the subset switch_libnx.c names),
     so both 64-bit values go out as two 32-bit halves. */
  unsigned long long msg = (unsigned long long)(UINT_PTR)s_SnapErrMsg;
  unsigned long long spec = (unsigned long long)s_SnapSpecial;

  if ( s_ExitReported )
    return;
  s_ExitReported = 1;
  DBG_LOG("SdlQuitReport: %s: the frame loop ended; state as of the last pump %u ms ago: loopStop=%d errorFlag=%d pErrorMessage=0x%08X%08X specialMode=0x%08X%08X recursion=%d",
          where, (unsigned)(now - s_SnapTick), s_SnapStop, s_SnapErrFlag,
          (unsigned int)(msg >> 32), (unsigned int)msg,
          (unsigned int)(spec >> 32), (unsigned int)spec, s_SnapRecursion);
  DBG_LOG("SdlQuitReport: inputs at that pump: eventCode=%d keyIndex=%d charCode=%d cursor=(%d,%d); last A click at (%d,%d), %s",
          s_SnapEvent, s_SnapKey, s_SnapChar, s_SnapCursorX, s_SnapCursorY,
          s_SnapClickX, s_SnapClickY,
          s_SnapClickTick ? "yes" : "none in this run");
  if ( s_SnapClickTick )
    DBG_LOG("SdlQuitReport: the last A click was %u ms before this report",
            (unsigned)(now - s_SnapClickTick));
#else
  (void)where;
#endif
}

static void SdlLoopProfileTick(void)
{
#ifdef _DEBUG
  SdlMixDumpFlush();                            /* a full recorder is emptied here, never from the callback */
  SdlQuitWatch();                               /* and the engine's own stop signals, while we are at it */
  static Uint32 s_Tick;
  static unsigned s_Pumps;
  static unsigned s_Lines;
  static unsigned s_Presents0;
  static unsigned s_Us0;
  static unsigned s_Msgs0;
  static unsigned s_Rects0;
  static unsigned s_RectsMsg0;
  static unsigned s_Frames0;
  unsigned pres, us, msgs, rects, rectsMsg, frames, span;
  Uint32 now;

  now = SDL_GetTicks();
  if ( !s_Tick )
  {
    s_Tick = now;
    s_Presents0 = s_Presents;
    s_Us0 = s_PresentUs;
    s_Msgs0 = SdlProfMsgDispatched();
    s_Rects0 = s_RectBlits;
    s_RectsMsg0 = s_RectBlitsMsg;
    s_Frames0 = s_FramePolls;
    return;
  }
  ++s_Pumps;
  span = now - s_Tick;
  if ( span < 2000 || s_Lines >= 120 )
    return;
  pres = s_Presents - s_Presents0;
  us = s_PresentUs - s_Us0;
  msgs = SdlProfMsgDispatched() - s_Msgs0;
  rects = s_RectBlits - s_Rects0;
  rectsMsg = s_RectBlitsMsg - s_RectsMsg0;
  frames = s_FramePolls - s_Frames0;
  ++s_Lines;
  /* The two fields added for "the animation stops while the pointer moves": rects split into the
     message path (the cursor, rendered inside the dispatch) and the frame path (the scene, from
     ProcessGameFrame) - a moving pointer over an animating screen has BOTH, over a dead one only
     the first - and "frame N ms ago (delay M)": main.c breaks out of its pump spin when
     frameDelayMs + g_LastFrameTime < now, so a healthy loop keeps N at about M, and a starved one
     lets it grow without bound while pumps keep coming. frames/s is the same question asked from
     the other end: main.c polls GetCursorPos once per frame, and the shim counts those polls. */
  DBG_LOG("SdlLoopProfile: %u ms - pumps %u (%u/s), messages taken %u (%u/s), presents %u (%u/s), %u us in present (avg %u us), rects %u/s (msg %u/s frame %u/s), frames %u/s, frame %u ms ago (delay %u), queue now %d",
          span, s_Pumps, s_Pumps * 1000u / span, msgs, msgs * 1000u / span,
          pres, pres * 1000u / span, us, pres ? us / pres : 0,
          rects * 1000u / span, rectsMsg * 1000u / span,
          (rects - rectsMsg) * 1000u / span, frames * 1000u / span,
          (unsigned)(now - g_LastFrameTime), (unsigned)g_VideoConfig.frameDelayMs,
          SdlProfQueueDepth());
  s_Tick = now;
  s_Pumps = 0;
  s_Presents0 = s_Presents;
  s_Us0 = s_PresentUs;
  s_Msgs0 = SdlProfMsgDispatched();
  s_Rects0 = s_RectBlits;
  s_RectsMsg0 = s_RectBlitsMsg;               /* these two too: the first version rebased only the
                                                 old counters after a printed line, so "msg" and
                                                 "frames" accumulated from the first tick all
                                                 session - monotonic growth, and the frame column
                                                 underflowed into billions once msg passed rects */
  s_Frames0 = s_FramePolls;
#endif
}

/* ---- the cursor that fades out while it is not moved (port feature) ----
 *
 * The engine repaints its pointer only in reaction to a mouse message: render.c's HandleGameEvent
 * adds the cursor object to the dirty regions only when its position CHANGED. On Windows and X11 the
 * consequence is invisible, because a real system cursor is composed over the window and never needs
 * repainting; on the console SDL has no cursor at all - the Switch video driver implements no
 * ShowCursor, there is no such symbol in the shipped libSDL2.a - so there the arrow is only ever
 * what the game last painted, and anything the scene paints over that spot takes it away until the
 * player moves. That is the reported symptom, and it is a Switch-only symptom.
 *
 * What the port does is make the engine call itself: AddObjectToDirtyRegions(&g_CursorObject), the
 * same call render.c makes when the pointer moves, so the arrow comes back through the engine's own
 * dirty-region pass, in the same frame the pump is in, with no position changing and therefore no
 * background that has to be restored. It took three tries to land on that, and every one of them was
 * measured rather than judged:
 *
 *   1. the same call, made too early: on the first console run no mouse message had arrived yet, the
 *      object was parked at (0,0), and the repair drew the arrow into a corner nobody looks at -
 *      which read as "this object is not the arrow". It is: the log has it at (320,240) after the
 *      first touch.
 *   2. posting a one-pixel WM_MOUSEMOVE away and back. This brings the arrow back, but a move also
 *      makes the engine restore what it thinks was under the old position, and over an animated
 *      scene that restore is not faithful - done every frame, it is exactly the flicker the player
 *      then saw ("the cursor blinks faster than before"). A move costs more than a redraw.
 *   3. deciding WHEN to repair by counting rects blitted since the arrow was last seen: a repaired
 *      arrow is itself a blit, so at threshold 16 with a 500 ms re-arm the repair fed itself into a
 *      metronome - the log showed an exact half-second rhythm with 30-34 rects, which is an arrow,
 *      not a scene. That was the first blinking cursor.
 *
 * The condition is therefore measured on the picture: a 65x65 square of the port's own shadow frame
 * (the frame the game itself last drew, the memory the texture is uploaded from) centred on
 * g_VideoConfig.cursorX/Y, compared frame to frame. A resting scene changes nowhere, so nothing is
 * asked for and the feature costs nothing; an animated one changes under the arrow every frame, so
 * the arrow is asked back every frame and every shown frame contains it. The snapshot is deliberately
 * NOT refreshed on a mismatch but one pump later, which is the frame the repair produced: refreshing
 * it from the erased frame is the self-feeding trap all over again.
 */
#define CURSOR_BOX      32                        /* half of the watched square, in the game's px - an arrow fits in it */
#define CURSOR_BOX_PX   (2 * CURSOR_BOX + 1)
#define CURSOR_DIFF_MIN 8                         /* pixels that must differ before it counts as painted over */

static unsigned int s_CursorSnap[CURSOR_BOX_PX * CURSOR_BOX_PX];

static void SdlCursorKeepAlive(void)
{
  static int s_X = -1;                           /* the pointer the snapshot was taken around */
  static int s_Y = -1;
  static int s_WantSnap;
  int cx, cy, x0, y0, w, h, row, col, diff = 0;

  if ( !s_Shadow || s_ShadowW != s_TexWidth || s_ShadowH != s_TexHeight )
    return;
  /* The pointer as the GAME sees it (main.c writes these from WM_MOUSEMOVE), not as the port last
     sent it: a message swallowed on the way - outside the picture in the letterbox, for instance -
     leaves the game standing somewhere else, and it is the game's point that gets overpainted. */
  cx = (int)(short)g_VideoConfig.cursorX;
  cy = (int)(short)g_VideoConfig.cursorY;
  if ( cx <= 0 && cy <= 0 )
    return;                                      /* the game has not seen the pointer yet */
  x0 = cx - CURSOR_BOX;
  y0 = cy - CURSOR_BOX;
  if ( x0 < 0 )
    x0 = 0;
  if ( y0 < 0 )
    y0 = 0;
  w = CURSOR_BOX_PX;
  h = CURSOR_BOX_PX;
  if ( x0 + w > s_ShadowW )
    w = s_ShadowW - x0;
  if ( y0 + h > s_ShadowH )
    h = s_ShadowH - y0;
  if ( w <= 0 || h <= 0 )
    return;

  if ( s_WantSnap || cx != s_X || cy != s_Y )
  {
    /* This is the frame the game drew after the pointer moved, or after the last repair: it has the
       arrow in it, so it is the one to compare the next ones against. */
    s_X = cx;
    s_Y = cy;
    s_WantSnap = 0;
    for ( row = 0; row < h; ++row )
      for ( col = 0; col < w; ++col )
        s_CursorSnap[(size_t)row * CURSOR_BOX_PX + col] =
            s_Shadow[(size_t)(y0 + row) * s_ShadowW + x0 + col];
    return;
  }

  for ( row = 0; row < h; ++row )
    for ( col = 0; col < w; ++col )
      diff += (s_CursorSnap[(size_t)row * CURSOR_BOX_PX + col] !=
               s_Shadow[(size_t)(y0 + row) * s_ShadowW + x0 + col]);
  if ( diff < CURSOR_DIFF_MIN )
  {
    if ( diff > 0 )
    {
      for ( row = 0; row < h; ++row )             /* a few pixels: re-anchor rather than speak up */
        for ( col = 0; col < w; ++col )
          s_CursorSnap[(size_t)row * CURSOR_BOX_PX + col] =
              s_Shadow[(size_t)(y0 + row) * s_ShadowW + x0 + col];
    }
    return;                                      /* nothing was painted over the pointer: leave it be */
  }

#ifdef __SWITCH__
  /* The engine's own mechanism, called the way the engine calls it: render.c's HandleGameEvent
     hands the cursor object to the dirty regions when its position changes, and the dirty-region
     pass that follows the scene's own painting is what puts the arrow on top. This is the first
     version of that idea with a pointer that actually exists: when it was tried before, the object
     was parked at (0,0) because no mouse message had ever arrived, so the repair drew the arrow
     into the corner and looked dead. The alternative - a one-pixel WM_MOUSEMOVE - does get the
     arrow back, but it also makes the engine restore the background under the old position, and
     with an animated scene that restore is not faithful, which at this rate is exactly what the
     player sees blinking. Nothing moves, so nothing has to be restored. */
  AddObjectToDirtyRegions(&g_CursorObject);
#endif
  s_WantSnap = 1;
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
  SdlUploadShadow();                            /* the game frame under the movie may have moved since the
                                                   last shown frame, and it lives in the shadow now */
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

/* Lay an 8-bit bottom-up rect into the shadow frame, converting it through the palette LUT,
   and decide whether the frame it has just become part of should be shown. Mirrors the
   original BitBlt/StretchDIBits(srcY = bmp_height - y - cy) row flip; the showing decision is
   the two-part coalescing gate, written out with its defines. */
void SdlBackendPresentRect(const void *bits, int bmp_height, int stride, int x, int y, int w, int h)
{
  const unsigned char *src = (const unsigned char *)bits;
  unsigned int *dst;
  Uint32 now;
  int inSweep;
  int row;
#ifdef _DEBUG
  unsigned int t0;                               /* checkpoint for the burst timing below */
  unsigned int convertUs;
#endif

  if ( !s_Texture || !bits || stride <= 0 || bmp_height <= 0 )
    return;
  if ( s_LoadStart )
    SdlBackendFrameShown();                      /* the game blits: whatever it waited for has arrived */
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
  if ( s_InMsgDispatch )
    ++s_RectBlitsMsg;                           /* the cursor redraw of UpdateCursorPosition */
#endif

  /* Convert straight into the shadow frame. There is no staging buffer any more: the rect
     is converted where it will live until the next shown frame takes the whole frame to the
     texture, so a per-rect bounce only cost a copy and a malloc that outlived every rect. */
  if ( s_ShadowW != s_TexWidth || s_ShadowH != s_TexHeight )
    return;                                    /* cannot happen while both are set together in
                                                  SdlBackendCreateVideoTargets, but the writes below
                                                  are indexed by the shadow's own row width */
  dst = s_Shadow + (size_t)y * s_ShadowW + x;
#ifdef _DEBUG
  t0 = SdlNowUs();
#endif
  for ( row = 0; row < h; ++row )
  {
    /* bottom-up source: screen row y+row lives at buffer row bmp_height-1-(y+row) */
    const unsigned char *srow = src + stride * (bmp_height - 1 - (y + row)) + x;
    unsigned int *drow = dst + (size_t)row * s_ShadowW;
    int col;
    for ( col = 0; col < w; ++col )
      drow[col] = s_PalRGB[srow[col]];
  }
#ifdef _DEBUG
  convertUs = SdlNowUs() - t0;
#endif
  now = SDL_GetTicks();
  /* The game has drawn. Both load clocks restart here and nowhere else (see SdlBackendLoadBeat),
     and the rect total is what the cursor keep-alive reads to know that something has been painted
     over the frame since the arrow was last drawn. */
  s_LastGameBlit = now;
  s_IdleBytes = 0;
  ++s_RectBlitsTotal;
  /* Was the previous rect blit moments ago? Then the game is mid-sweep, and one tile more
     on screen is not a frame worth stopping for. */
  inSweep = (Sint32)(now - s_LastRectTick) < PRESENT_STREAM_GAP_MS;
  s_LastRectTick = now;
  s_PresentPending = 1;
  ++s_RectsSincePresent;
#ifdef _DEBUG
  /* Sampled before the present decision: whether this rect ends up showing now or later is a
     separate question from what it cost to get here. There is no upload to time here any
     more - the whole-frame upload happens inside SdlPresentCurrent and is counted with the
     present time there. */
  SdlBurstNote(now, w, h, convertUs);
#endif
  if ( s_InMsgDispatch )
    return;                                    /* the message path only blits, it does not present:
                                                  its show is the pump's SdlFlushPresent after the
                                                  dispatch. Presenting here blocks the dispatch loop
                                                  on vsync per cursor move - measured on the console
                                                  while the stick moved: 100 msgs/s, each cursor
                                                  erase+repaint tripping the gates below and waiting
                                                  ~10 ms in SDL_RenderPresent, so the engine's frame
                                                  ran 1-2/s and aged 1388 ms (the scene's animation
                                                  stopped); the frame path, whose blits are not in a
                                                  dispatch, keeps presenting on the spot as before. */
  if ( (Sint32)(now - s_LastPresentTick) < PRESENT_MIN_INTERVAL_MS )
    return;                                    /* time gate */
  if ( inSweep && s_RectsSincePresent < PRESENT_BURST_RECTS )
    return;                                    /* stream gate: see the note by the defines */
  SdlPresentCurrent();
}

/* Black the whole screen (original: FillRect(window, BLACK_BRUSH) in SetPalette) and present. */
void SdlBackendClearBlack(void)
{
  int i;

  if ( !s_Texture || !s_Shadow || s_ShadowW != s_TexWidth || s_ShadowH != s_TexHeight )
    return;
  for ( i = 0; i < s_ShadowW * s_ShadowH; ++i )
    s_Shadow[i] = 0xFF000000u;
  s_PresentPending = 1;                         /* SdlUploadShadow inside SdlPresentCurrent carries it over */
  s_RectsSincePresent = 0;
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
  SdlCursorKeepAlive();                         /* the engine's own arrow, kept painted while it is still */
  SdlLoopProfileTick();                         /* and what the loop cost while it did all of this */
#ifdef __SWITCH__
  SdlPollSwitchControls();                      /* left stick + A/B -> the mouse messages the game already speaks */
#endif
#ifdef _WIN32
  /* SDL_PollEvent pumps the Win32 queue (SDL internally does PeekMessage/
     TranslateMessage/DispatchMessage, so our wndproc subclass sees every message);
     SDL_QUIT == WM_QUIT here. */
  SDL_Event ev;
  int quit = 0;

  SdlFlushPresent();                          /* pending burst present, if the game blitted */
  s_InMsgDispatch = 1;                        /* rects from here on are the message path's (the cursor) */
  while ( SDL_PollEvent(&ev) )
  {
    if ( ev.type == SDL_QUIT )
      quit = 1;
  }
  s_InMsgDispatch = 0;
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

  s_InMsgDispatch = 1;                        /* the dispatch below is where the cursor gets its blits:
                                                 UpdateCursorPosition renders inside DispatchMessageA, and
                                                 the split (msg rects vs frame rects) is what says whether
                                                 the scene keeps animating while the pointer moves */
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
  s_InMsgDispatch = 0;
  SdlWavePump();
  SdlFlushPresent();                          /* also after WM_PAINT dispatched this round */
  return quit || WinShim_QuitPosted();
#endif
}

/* main.c samples the pointer once per frame (GetCursorPos + ScreenToClient, when the input config
   carries the mouse bit) - and on the platforms with no user32 the port's shim IS the only
   GetCursorPos there is, so that call is the frame counter the profile needs: when the loop starves,
   the polls stop with it. Windows has the real user32 GetCursorPos and never comes through here, so
   the profile reads frames 0 there - the honest answer for a queue the port does not own. */
void SdlBackendNoteFramePoll(void)
{
#ifdef _DEBUG
  ++s_FramePolls;                               /* the profile's frame counter; release has no reader */
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

/* ---- ONE DEVICE CARRYING BOTH SOURCES, for platforms whose SDL allows one ----
 *
 * Normally the port runs two SDL devices: the game's waveOut side gets 11025 Hz / mono /
 * U8 (the original WAVEFORMATEX) and the MIDI side gets 44100 Hz / stereo / S16 (what the
 * synth renders), each a push queue. That works wherever a process may open the default
 * device twice - Windows and Linux. The console's SDL has one output per process and
 * refuses the second open outright, which is measured, not assumed:
 *      SdlMidi_Open: SDL_OpenAudioDevice failed: Audio device already open
 * and the audible consequence is the report from the sofa: sound effects play, music does
 * not, because only the source that opened first ever reaches the speaker.
 *
 * Shared mode is therefore: one device at the synth's format, and two rings the audio
 * callback sums. What is deliberate here:
 *  - the synth keeps rendering on the game thread (SdlMidi_Pump, driven by the game's own
 *    10 ms timer) exactly as before; the callback only copies and adds. Putting an OPL3
 *    emulator inside the audio callback would put its cost on a deadline.
 *  - effects are resampled at queue time - 11025 -> 44100 by repeating each sample four
 *    times, mono -> stereo, U8 -> S16 through the same volume curve the two-device path
 *    uses - because that is where the volume was already applied, and for the game's short
 *    blips this is no worse than whatever the desktop driver does with them.
 *  - the wave side still reports pending bytes in the GAME's units, because SdlWavePump in
 *    audio.c counts its buffers in those bytes: the units of SdlWave_Queued are part of the
 *    completion protocol with the game, not a detail of the device.
 *  - Windows and Linux never enter this mode, so the path proven by audio.ps1 and x86check
 *    is byte-for-byte the one that used to run.
 *
 * SDL_MIX_* and the rings themselves are declared up with the two device handles they
 * replace, because SdlBackendShutdown has to take the shared one down.
 */
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
  ring->r = ring->w;                           /* drop what is queued, keep the counters running */
}

/* Producer: the game thread. Consumer: SDL's audio callback. One of each, and the reader
   only ever looks at frames the writer finished writing before bumping w, so volatile is
   the whole synchronisation this needs. */
static unsigned int MixRingWrite(MixRing *ring, const short *frames, unsigned int n)
{
  unsigned int room = MixRingFree(ring);
  unsigned int i;
  unsigned int pos;

  if ( n > room )
    n = room;
  pos = ring->w & SDL_MIX_MASK;
  for ( i = 0; i < n; ++i )
  {
    ring->pcm[pos * 2] = frames[i * 2];
    ring->pcm[pos * 2 + 1] = frames[i * 2 + 1];
    pos = (pos + 1) & SDL_MIX_MASK;
  }
  ring->w += n;
  return n;
}

/* Where rendered music can go: a device of its own, or the mixer's ring. Two shapes of the
   same thing, so the three places that need to know ask one question instead of testing
   both every time. */
static int MidiSinkReady(void)
{
  return s_MidiDev || (s_SharedAudio && s_MidiRing.pcm != 0);
}

static void MidiSinkClear(void)
{
  if ( s_MidiDev )
    SDL_ClearQueuedAudio(s_MidiDev);
  else if ( s_SharedAudio && s_MidiRing.pcm )
    MixRing_Reset(&s_MidiRing);
}

static int MidiSinkWrite(const short *frames, unsigned int n)
{
  if ( s_MidiDev )
    return SDL_QueueAudio(s_MidiDev, frames, n * 2 * sizeof(short)) == 0;
  return MixRingWrite(&s_MidiRing, frames, n) == n;
}

static unsigned int MidiSinkBacklog(void)
{
  if ( s_MidiDev )
    return SDL_GetQueuedAudioSize(s_MidiDev);
  return (s_MidiRing.w - s_MidiRing.r) * 4;    /* bytes, the same units SDL's queue reports */
}

/* ---- what the device is actually handed (diagnostic) ----
 *
 * Every number the mixer already reports is a summary of the same buffer the device receives, so
 * when those numbers say "audible" and the player says "silence", the log has nothing left to give:
 * a peak of 8224 and a peak of an identical-looking DC offset print the same line. What the log
 * cannot say is the *shape*. So the port keeps the bytes themselves: the first MIX_DUMP_SECONDS of
 * its own output are recorded verbatim and written next to the exe as a WAV, which can then be
 * measured offline (RMS over short windows, zero runs, spectrum) without a second pair of ears.
 *
 * Capture is armed by the first window that carries an effect rather than starting at open: a dump
 * of the title music proves nothing about the effects, and the console gives no way to say "start
 * now" - so it records the moment the contested signal first appears, which is what the question is
 * about. It is a debug-build feature; on the console it is always on because there is neither a
 * command line nor an environment there, and elsewhere it is PILOTS_AUDIO_DUMP=1.
 */
#ifdef _DEBUG
#define MIX_DUMP_SECONDS 6

static short *s_MixDump;                        /* the mixer's output, verbatim */
static unsigned int s_MixDumpCap;               /* frames the buffer holds */
static unsigned int s_MixDumpFrames;            /* frames captured so far */
static int s_MixDumpStarted;                    /* the wave gate above has opened */
static int s_MixDumpPending;                    /* full: the file is written from the pump, never from here */
static int s_MixDumpDone;                       /* written: the recorder is closed for good */
static char s_MixDumpPath[MAX_PATH];

static int SdlMixDumpWanted(void)
{
#ifdef __SWITCH__
  return 1;
#else
  char *e = getenv("PILOTS_AUDIO_DUMP");
  return (e && *e && *e != '0') ? 1 : 0;
#endif
}

static void SdlMixDumpPrepare(void)
{
  char base[MAX_PATH];
  int n;
  int i;

  s_MixDumpPath[0] = 0;
  if ( !SdlMixDumpWanted() )
    return;
  s_MixDumpCap = SDL_MIX_RATE * MIX_DUMP_SECONDS;
  s_MixDump = (short *)malloc(s_MixDumpCap * 2 * sizeof(short));
  if ( !s_MixDump )
    return;
  /* The same directory the debug log goes to: the exe's own, so the dump is where the person
     pulling it over FTP already looks. */
  n = (int)GetModuleFileNameA(0, base, sizeof base - 1);
  if ( n <= 0 || n >= sizeof base - 1 )
    return;
  base[n] = 0;
  for ( i = n - 1; i >= 0; --i )
  {
    if ( base[i] == '\\' || base[i] == '/' )
    {
      base[i + 1] = 0;
      break;
    }
  }
  if ( i < 0 )
    base[0] = 0;                                /* no directory in the name at all: the Switch has
                                                   no /proc/self/exe, so GetModuleFileNameA answers
                                                   the bare 'PILOT_RV.EXE' and the first dump was
                                                   written as PILOT_RV.EXEpilots_mix.wav - a name
                                                   --pull-dump never looks for. An empty base is the
                                                   CWD, which is exactly where the engine's own
                                                   bare-name files (PILOT_RV_debug.log) already
                                                   land on that platform. */
  /* The string keeps Windows' separators: on the shimmed platforms CreateFileA goes through the
     same path resolution as everything else, which accepts them - proven by the first version,
     whose log line read '\home\...\build64\pilots_mix.wav' while the file landed next to the
     executable. What must not be done is adding a separator of the port's own on top of that. */
  sprintf(s_MixDumpPath, "%spilots_mix.wav", base);
}

static void SdlMixDumpCapture(const short *out, unsigned int frames, unsigned int waveFrames)
{
  unsigned int n;

  if ( !s_MixDump || s_MixDumpPending || s_MixDumpDone || !s_MixDumpPath[0] )
    return;
  if ( !s_MixDumpStarted )
  {
    if ( !waveFrames )
      return;
    s_MixDumpStarted = 1;
    DBG_LOG("SdlMix_Dump: recording %d s of the mixer's output, from the first effect, into '%s'",
            MIX_DUMP_SECONDS, s_MixDumpPath);
  }
  n = frames;
  if ( s_MixDumpFrames + n > s_MixDumpCap )
    n = s_MixDumpCap - s_MixDumpFrames;
  memcpy(s_MixDump + s_MixDumpFrames * 2, out, n * 2 * sizeof(short));
  s_MixDumpFrames += n;
  if ( s_MixDumpFrames >= s_MixDumpCap )
    s_MixDumpPending = 1;
}

/* Four little-endian words and two little-endian halves - the WAV header is written by hand because
   the port must not depend on anything but what the shim already implements on three platforms. */
static void SdlPut32(char *p, unsigned int v)
{
  p[0] = (char)(v & 0xFF);
  p[1] = (char)((v >> 8) & 0xFF);
  p[2] = (char)((v >> 16) & 0xFF);
  p[3] = (char)((v >> 24) & 0xFF);
}

static void SdlPut16(char *p, unsigned int v)
{
  p[0] = (char)(v & 0xFF);
  p[1] = (char)((v >> 8) & 0xFF);
}

static void SdlMixDumpFlush(void)
{
  char hdr[44];
  HANDLE f;
  DWORD written;
  unsigned int data = s_MixDumpFrames * 2 * sizeof(short);

  if ( !s_MixDumpPending || s_MixDumpDone )
    return;
  s_MixDumpDone = 1;                             /* before anything else: a second attempt would truncate what the first wrote */
  s_MixDumpPending = 0;
  SdlPut32(hdr, 0x46464952u);                    /* 'RIFF' */
  SdlPut32(hdr + 4, 36 + data);
  SdlPut32(hdr + 8, 0x45564157u);                /* 'WAVE' */
  SdlPut32(hdr + 12, 0x20746d66u);               /* 'fmt ' */
  SdlPut32(hdr + 16, 16);
  SdlPut16(hdr + 20, 1);                         /* PCM */
  SdlPut16(hdr + 22, 2);                         /* the mixer's own two channels */
  SdlPut32(hdr + 24, SDL_MIX_RATE);
  SdlPut32(hdr + 28, SDL_MIX_RATE * 4);
  SdlPut16(hdr + 32, 4);
  SdlPut16(hdr + 34, 16);
  SdlPut32(hdr + 36, 0x61746164u);               /* 'data' */
  SdlPut32(hdr + 40, data);

  f = CreateFileA(s_MixDumpPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS,
                  FILE_ATTRIBUTE_NORMAL, NULL);
  if ( f == INVALID_HANDLE_VALUE )
  {
    DBG_LOG("SdlMix_Dump: cannot write '%s'", s_MixDumpPath);
    free(s_MixDump);
    s_MixDump = 0;
    return;
  }
  WriteFile(f, hdr, sizeof hdr, &written, NULL);
  WriteFile(f, s_MixDump, data, &written, NULL);
  CloseHandle(f);
  DBG_LOG("SdlMix_Dump: %u frames of the mixer's output written to '%s'", s_MixDumpFrames, s_MixDumpPath);
  free(s_MixDump);
  s_MixDump = 0;
}
#endif

static void SdlAudioMix(void *userdata, Uint8 *stream, int len)
{
  short *out = (short *)stream;
  unsigned int frames = (unsigned int)len / 4; /* two channels, two bytes */
  unsigned int i;
#ifdef _DEBUG
  /* The mixer's own accounting: which source actually reaches the callback, and how loud it
     is there. This exists because "the music plays but the effects do not" has two very
     different causes - nothing arriving in the ring, or something arriving inaudibly small -
     and a console run cannot be asked. Peaks are absolute values of the mixed-in samples. */
  unsigned int waveFrames = 0;
  unsigned int midiFrames = 0;
  unsigned int movieFrames = 0;
  int wavePeak = 0;
  int midiPeak = 0;
  int moviePeak = 0;
  int outPeak = 0;                              /* what the device is being handed, see below */
  static unsigned int s_MixFrames;
  static unsigned int s_MixSumWave;            /* accumulated since the last report line */
  static unsigned int s_MixSumMidi;
  static unsigned int s_MixSumMovie;
  static int s_MixSumWavePeak;
  static int s_MixSumMidiPeak;
  static int s_MixSumMoviePeak;
  static int s_MixSumOutPeak;
  static unsigned s_MixLines;
  static int s_MixFirstPeak;                   /* the first burst, measured over its frames */
  static unsigned int s_MixFirstCount;
  static int s_MixFirstDone;
  int v;
#endif

  for ( i = 0; i < frames; ++i )
  {
    int left = 0;
    int right = 0;

    if ( s_WaveRing.pcm && !s_WaveHeld && s_WaveRing.w != s_WaveRing.r )
    {
      unsigned int p = (s_WaveRing.r & SDL_MIX_MASK) * 2;
      left = s_WaveRing.pcm[p];
      right = s_WaveRing.pcm[p + 1];
      s_WaveRing.r += 1;
      s_WaveFramesEaten += 1;
#ifdef _DEBUG
      ++waveFrames;
      v = left < 0 ? -left : left;
      if ( v > wavePeak )
        wavePeak = v;
      v = right < 0 ? -right : right;
      if ( v > wavePeak )
        wavePeak = v;
      /* The first burst the mixer is given, measured rather than sampled: the very first
         frame of a sound is usually its silent lead-in, so one value says nothing (it said
         "L=0 R=0" while the data was fine). The peak over the burst does say. */
      if ( !s_MixFirstDone )
      {
        if ( v > s_MixFirstPeak )
          s_MixFirstPeak = v;
        if ( ++s_MixFirstCount >= 4096 )       /* a whole 2048-byte queue in this case */
        {
          s_MixFirstDone = 1;
          DBG_LOG("SdlAudioMix: the mixer heard the first effect burst: peak %d over %u frames",
                  s_MixFirstPeak, s_MixFirstCount);
        }
      }
#endif
    }
    if ( s_MidiRing.pcm && s_MidiRing.w != s_MidiRing.r )
    {
      unsigned int p = (s_MidiRing.r & SDL_MIX_MASK) * 2;
      left += s_MidiRing.pcm[p];
      right += s_MidiRing.pcm[p + 1];
      s_MidiRing.r += 1;
#ifdef _DEBUG
      ++midiFrames;
      v = s_MidiRing.pcm[p] < 0 ? -s_MidiRing.pcm[p] : s_MidiRing.pcm[p];
      if ( v > midiPeak )
        midiPeak = v;
#endif
    }
    if ( s_MovieRing.pcm && s_MovieRing.w != s_MovieRing.r )
    {
      unsigned int p = (s_MovieRing.r & SDL_MIX_MASK) * 2;
      left += s_MovieRing.pcm[p];
      right += s_MovieRing.pcm[p + 1];
      s_MovieRing.r += 1;
#ifdef _DEBUG
      ++movieFrames;
      v = s_MovieRing.pcm[p] < 0 ? -s_MovieRing.pcm[p] : s_MovieRing.pcm[p];
      if ( v > moviePeak )
        moviePeak = v;
#endif
    }
    if ( left > 32767 )
      left = 32767;
    else if ( left < -32768 )
      left = -32768;
    if ( right > 32767 )
      right = 32767;
    else if ( right < -32768 )
      right = -32768;
    out[i * 2] = (short)left;
    out[i * 2 + 1] = (short)right;
#ifdef _DEBUG
    /* And what leaves the port. The per-source peaks above can all be honest while the stream the
       device is handed is silent - a source summed into a variable that is never written out looks
       exactly like a source nobody hears - so the output is measured where it is written, not where
       it is read. */
    {
      int o = left < 0 ? -left : left;
      int p = right < 0 ? -right : right;
      if ( p > o )
        o = p;
      if ( o > outPeak )
        outPeak = o;
    }
#endif
  }

#ifdef _DEBUG
  SdlMixDumpCapture(out, frames, waveFrames);   /* these very bytes, kept to be measured offline */
  s_MixSumWave += waveFrames;
  s_MixSumMidi += midiFrames;
  s_MixSumMovie += movieFrames;
  if ( wavePeak > s_MixSumWavePeak )
    s_MixSumWavePeak = wavePeak;
  if ( midiPeak > s_MixSumMidiPeak )
    s_MixSumMidiPeak = midiPeak;
  if ( moviePeak > s_MixSumMoviePeak )
    s_MixSumMoviePeak = moviePeak;
  if ( outPeak > s_MixSumOutPeak )
    s_MixSumOutPeak = outPeak;
  s_MixFrames += frames;
  if ( s_MixFrames >= SDL_MIX_RATE * 5u )      /* one line per 5 s of audio */
  {
    /* Only windows that actually carried a movie or an effect are written, and the cap counts
       lines printed, not time passed. The first version capped at 20 lines of everything, which
       ran the budget out on music alone: the effects on the console - the thing being looked
       for - started long after, and every line that existed said "wave 0". */
    if ( s_MixLines < 40 && (s_MixSumWave || s_MixSumMovie) )
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

/* Whether the sources have to share one device. On the console this is not a choice (the
   platform's SDL has one output per process and refuses the second open). Elsewhere the default
   follows what else is available: Windows has mciavi32, which plays a movie's sound by itself,
   so there the two-device path stays; on Linux and the Switch a movie's sound only exists if
   the mixer exists, so sharing is the default there. PILOTS_SHARE_AUDIO=0 or =1 forces either
   way, which is how the shared path gets measured where a speaker can be replaced by a number:
   the mixer probe logs the peak of what each source contributes. */
static int SdlShareAudioWanted(void)
{
#ifdef __SWITCH__
  return 1;
#else
  static int s_Share = -1;
  if ( s_Share < 0 )
  {
    char *e = getenv("PILOTS_SHARE_AUDIO");
#ifdef _WIN32
    s_Share = 0;
#else
    s_Share = 1;
#endif
    if ( e && *e )
      s_Share = (*e != '0');
    DBG_LOG("SdlShareAudioWanted: one device for all sources = %d (platform default, PILOTS_SHARE_AUDIO='%s')",
            s_Share, e ? e : "");
  }
  return s_Share;
#endif
}

/* The first source that wants audio opens the single device; the other just uses it. Both
   formats are the synth's, so the game's PCM is the one that gets converted. */
static int SdlMix_Open(const char *who)
{
  SDL_AudioSpec want;
  SDL_AudioSpec have;

  if ( s_MixDev )
    return 1;
  if ( !(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) )
  {
    if ( SDL_InitSubSystem(SDL_INIT_AUDIO) != 0 )
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
  s_MixDev = SDL_OpenAudioDevice(0, 0, &want, &have, SDL_AUDIO_ALLOW_SAMPLES_CHANGE);
  if ( !s_MixDev )
  {
    DBG_LOG("SdlMix_Open: %s could not open the shared device: %s", who, SDL_GetError());
    s_MixDev = 0;
    return 0;
  }
  if ( have.freq != SDL_MIX_RATE || have.format != AUDIO_S16SYS || have.channels != 2 )
  {
    DBG_LOG("SdlMix_Open: shared device gave %d Hz / format 0x%X / %d ch, this mixer only sums S16 stereo",
            have.freq, (unsigned int)have.format, have.channels);
    SDL_CloseAudioDevice(s_MixDev);
    s_MixDev = 0;
    return 0;
  }
  if ( !s_WaveRing.pcm && !MixRing_New(&s_WaveRing) )
    return 0;
  if ( !s_MidiRing.pcm && !MixRing_New(&s_MidiRing) )
    return 0;
  s_SharedAudio = 1;
  SDL_PauseAudioDevice(s_MixDev, 0);
  /* The driver's name and the device's index travel with the success line because a peak measured
     inside the callback proves the mixer works, not that the bytes reach a speaker: a second device
     on a platform with one output, or a fallback driver, is exactly a callback that runs and a
     silence that follows. The name is what tells the two apart from a log. */
  DBG_LOG("SdlMix_Open: one device for both sources (%s asked first), %d Hz / %d ch / S16, %u frames per ring; driver '%s', device #%u, playing=%d",
          who, have.freq, have.channels, SDL_MIX_RING,
          SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "(none)",
          (unsigned int)s_MixDev, (int)SDL_GetAudioDeviceStatus(s_MixDev));
#ifdef _DEBUG
  SdlMixDumpPrepare();                          /* the recorder only: it starts at the first effect */
#endif
  return 1;
}

/* A movie's PCM, the mixer's third source. The game's own AVI reader (ProcessVideoCommands) has
   no sound at all, and on these platforms there is no mciavi32 to play the WAVE stream that sits
   next to the video one in the same file, so mci_ffmpeg.c reads those chunks itself, paces them
   against the frames it has shown, and hands the bytes here. A ring of its own rather than a
   second use of the wave one because stopping a movie - the skip path - has to drop what it had
   queued without touching the music or a sound effect already in progress. */
int SdlMix_MovieOpen(unsigned int rate)
{
  if ( !rate || !SdlShareAudioWanted() || !SdlMix_Open("movie") )
  {
    DBG_LOG("SdlMix_MovieOpen: %u Hz refused - the movie's sound only exists through the mixer", rate);
    return 0;
  }
  if ( !s_MovieRing.pcm && !MixRing_New(&s_MovieRing) )
    return 0;
  MixRing_Reset(&s_MovieRing);
  s_MovieRate = rate;
  s_MovieSrcFrames = 0;
  DBG_LOG("SdlMix_MovieOpen: movie PCM at %u Hz into the mixer at %d Hz", rate, SDL_MIX_RATE);
  return 1;
}

/* Rate conversion by exact boundaries: output frame o plays source sample floor(o * rate /
   SDL_MIX_RATE), so each source sample is copied as many times as there are output frames
   starting inside it. Against the mixer's 44100 the game's 22050 and 11025 give exactly 2 and 4
   copies; another ratio degrades to nearest-sample repetition, which is what the wave path does
   at its fixed ratio anyway. Returns the number of source bytes taken - fewer than asked for
   when the ring is full, and the caller keeps the rest for the next pump. */
unsigned int SdlMix_MovieWrite(const unsigned char *src, unsigned int len)
{
  unsigned int room = MixRingFree(&s_MovieRing);
  unsigned int pos = s_MovieRing.w & SDL_MIX_MASK;
  unsigned int emitted = 0;
  unsigned int i;

  if ( !s_MovieRing.pcm || !s_MovieRate )
    return 0;
  for ( i = 0; i < len; ++i )
  {
    unsigned int from = (unsigned int)(((unsigned long long)s_MovieSrcFrames * SDL_MIX_RATE) / s_MovieRate);
    unsigned int to = (unsigned int)(((unsigned long long)(s_MovieSrcFrames + 1) * SDL_MIX_RATE) / s_MovieRate);
    unsigned int n = to - from;
    short s = (short)(((int)src[i] - 128) * 257);
    unsigned int k;

    if ( n > room )
      break;
    for ( k = 0; k < n; ++k )
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
  if ( s_MovieRing.pcm )
    MixRing_Reset(&s_MovieRing);
  s_MovieRate = 0;
  s_MovieSrcFrames = 0;
}

/* The game's PCM in, mix frames appended: one U8 sample becomes SDL_MIX_RATIO identical
   stereo frames, with the volume applied in the game's own U8 curve first so the shared
   path and the two-device path answer a volume change identically. Returns the number of
   game bytes taken; a full ring drops the tail, which is what the two-device path's
   "device fell behind" case looks like from the game's side. */
static unsigned int SdlWaveAppendShared(const unsigned char *src, unsigned int len)
{
  unsigned int samples = MixRingFree(&s_WaveRing) / SDL_MIX_RATIO;
  unsigned int pos;
  unsigned int i;

  if ( len > samples )
    len = samples;
  pos = s_WaveRing.w & SDL_MIX_MASK;
  for ( i = 0; i < len; ++i )
  {
    int v = src[i];                             /* the sample itself: the first version of this line
                                                   started from 128 and only ever changed it by the
                                                   volume, which made every output 0 - the console's
                                                   "music plays, effects do not", peak 0 in the mixer
                                                   probe, which is how it was caught */
    short s;
    unsigned int k;

    if ( s_WaveVolume != 255 )
      v = 128 + ((v - 128) * s_WaveVolume) / 255;
    s = (short)((v - 128) * 257);              /* the same expansion SDL does for U8 -> S16 */
    for ( k = 0; k < SDL_MIX_RATIO; ++k )
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

/* ---- SDL wave device (replaces waveOutOpen) ---- */

int SdlWave_Open(void)
{
  SDL_AudioSpec want;
  SDL_AudioSpec have;

  if ( s_WaveDev )
    return 1;
  if ( SdlShareAudioWanted() )
  {
    /* No device of our own here: on this platform there is not a second one to open, and
       the shared device's format is not the game's, which is exactly why the format check
       below is skipped on this path - SdlWaveAppendShared is the converter. */
    if ( !SdlMix_Open("wave") )
      return 0;
    s_WaveVolume = 255;                         /* as below: a fresh waveOut starts at full volume */
    s_WaveOpen = 1;
    return 1;
  }
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
  s_WaveOpen = 1;
  SDL_PauseAudioDevice(s_WaveDev, 0);
  return 1;
}

void SdlWave_Close(void)
{
  /* Logged on a real close only. This function is otherwise silent, and
     docs/sound-disappears.md names that as a masking hole: a close nobody can see looks exactly
     like a device that stopped draining, and the two need different fixes. */
  if ( s_WaveOpen )
    DBG_LOG("SdlWave_Close: wave device closed (dev=%d shared=%d)",
            s_WaveDev ? 1 : 0, s_SharedAudio);
  s_WaveOpen = 0;
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

  if ( !s_WaveOpen || !data )
  {
#ifdef _DEBUG
    /* This return sits BEFORE the per-buffer log and used to be the one completely silent exit
       in the wave path - and a silent failure here is a permanent chain death: WriteWaveBuffer
       leaves the header free, MixAudioBuffers' state1 stays set, and no completion will ever
       come. Say it, capped. */
    static unsigned int s_RefusedLines;
    if ( s_RefusedLines < 20 )
    {
      ++s_RefusedLines;
      DBG_LOG("SdlWave_Queue: refused before the per-buffer log (open=%d data set=%d) - line %u of 20",
              s_WaveOpen, data != 0, s_RefusedLines);
    }
#endif
    return 0;
  }
  if ( s_WaveVolume != 255 && !s_SharedAudio )
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
  /* Diagnostics (debug builds, EVERY buffer - the throttle that showed only #0..#7 and every
     64th was removed on purpose, per docs/sound-disappears.md: the late triggers of the session
     where the effects died were exactly calls #8 and #9 and the log could not see them. The
     rate is one call per 2048 played bytes - 5.4/s while a sound is active, nothing during
     silence - so "every" is cheap and the stillQueued column is what names a frozen device.
     U8 silence is 0x80: a paused device or an all-silent slot looks identical in the
     TriggerSoundSlot log, this line tells them apart. */
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
  if ( s_SharedAudio )
  {
    unsigned int taken = SdlWaveAppendShared(src, len);

    if ( taken != len )
    {
      /* Same situation the two-device path reports from SdlWave_Clear: the consumer is not
         keeping up, so the tail of this buffer has nowhere to go. Every drop, capped: one drop
         is a hiccup, a drop per buffer while the ring stays full IS the failure - and the old
         once-only log could not tell the two apart. */
      static unsigned int s_WaveDropLines;
      if ( s_WaveDropLines < 40 )
      {
        ++s_WaveDropLines;
        DBG_LOG("SdlWave_Queue: shared ring full, %d of %d game bytes dropped (drop line %u of 40)",
                (int)(len - taken), (int)len, s_WaveDropLines);
      }
    }
    return taken == len;
  }
  return SDL_QueueAudio(s_WaveDev, src, len) == 0;
}

void SdlWave_Clear(void)
{
  static int s_WaveFirstClearLogged;              /* one-shot: means headers ran out (device stopped draining) */

  if ( s_SharedAudio )
  {
    if ( !s_WaveFirstClearLogged )
    {
      s_WaveFirstClearLogged = 1;
      DBG_LOG("SdlWave_Clear: queued audio dropped (waveOutReset path - device fell behind)");
    }
    MixRing_Reset(&s_WaveRing);
    s_WaveFramesQueued = s_WaveFramesEaten;       /* pending becomes zero, both counters keep running */
    return;
  }
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

  if ( !s_WaveOpen )
    return;
  if ( s_WavePaused != (pause ? 1 : 0) )
  {
    s_WavePaused = pause ? 1 : 0;
    DBG_LOG("SdlWave_Pause: %d", s_WavePaused);
  }
  if ( s_SharedAudio )
  {
    /* Held at the mixer rather than at the device: this device also carries the music, and
       the game asks for the effects to stop, not the soundtrack. Nothing is lost while held
       - the mixer stops reading the ring, so the pending count (the game's completion
       protocol) stands still until it is let go. */
    s_WaveHeld = pause ? 1 : 0;
    return;
  }
  SDL_PauseAudioDevice(s_WaveDev, pause ? 1 : 0);
}

unsigned int SdlWave_Queued(void)
{
  if ( s_SharedAudio )
    /* Game units (bytes of 11025/mono/U8), because SdlWavePump counts its WAVEHDRs in them:
       four mix frames per game byte, and the difference of two running counters, which is
       the only thing that survives them wrapping. */
    return (s_WaveFramesQueued - s_WaveFramesEaten) / SDL_MIX_RATIO;
  return s_WaveDev ? SDL_GetQueuedAudioSize(s_WaveDev) : 0;
}

/* Frames sitting in the shared wave ring right now - the device side of the protocol against
   SdlWave_Queued's accounting. A watchdog that sees SdlWave_Queued() > 0 while this reads 0 has
   found the desync docs/sound-disappears.md was left hunting: the counters would promise bytes
   the ring has already given away, so SdlWavePump stops reporting completions, the WAVEHDRs
   stay INQUEUE for ever, and every later effect dies in PlaySoundEffect's early return. Debug
   builds only: the single caller is that watchdog. */
unsigned int SdlWave_RingDepth(void)
{
#ifdef _DEBUG
  return (s_SharedAudio && s_WaveRing.pcm) ? (unsigned int)(s_WaveRing.w - s_WaveRing.r) : 0;
#else
  return 0;
#endif
}

int SdlWave_IsOpen(void)
{
  return s_WaveOpen != 0;
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
/* s_MidiDev itself is declared up with s_WaveDev: the shared-mode mixer needs it there. */
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
  if ( SdlShareAudioWanted() )
  {
    /* No device of our own: the console has one output, and the mixer's format IS this one,
       so the synth's PCM needs no conversion - only a ring to be summed from. Whoever asks
       first opens the device; the other finds it ready. */
    if ( !SdlMix_Open("midi") )
      return 0;
  }
  else
  {
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
  }
  s_Synth = adl_init(SDL_MIDI_RATE);
  if ( !s_Synth )
  {
    DBG_LOG("SdlMidi_Open: adl_init failed: %s", adl_errorString());
    if ( s_MidiDev )
    {
      SDL_CloseAudioDevice(s_MidiDev);
      s_MidiDev = 0;
    }
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
  if ( s_MidiDev )
    SDL_PauseAudioDevice(s_MidiDev, 0);
  DBG_LOG("SdlMidi_Open: libADLMIDI ready: emu=%s bank=%d rate=%d%s",
          adl_chipEmulatorName(s_Synth), SDL_MIDI_BANK, SDL_MIDI_RATE,
          s_SharedAudio ? ", sharing the wave device" : "");
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
  if ( s_SharedAudio && s_MidiRing.pcm )
    MixRing_Reset(&s_MidiRing);                /* the device itself is shared: SdlBackendShutdown closes it */
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
  if ( MidiSinkReady() )
    MidiSinkClear();
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
    MidiSinkClear();                     /* drop PCM rendered at the old volume */
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
  if ( !s_Synth || !MidiSinkReady() || !s_MidiFeed )
  {
    MidiUnlock();
    return;
  }
  queued = MidiSinkBacklog();
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
    if ( !MidiSinkWrite(s_MidiChunk, (unsigned int)generated / 2) )
      break;
    queued += (unsigned int)generated * sizeof(short);
    ++guard;
  }
  MidiUnlock();
}

#pragma pack(pop)
