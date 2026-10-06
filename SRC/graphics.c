#include "pilots_fwd.h"

#pragma pack(push, 2)

/* ---- graphics module ---- */

static int s_VideoTargetsDone;                  /* SDL renderer+texture created once per run */

int GetCurrentScreenMode()
{
  return g_pGraphicsFunc(0, 516, 0);
}

int GetMaxScreenMode()
{
  return g_pGraphicsFunc(0, 522, 0);
}

char InitDisplay()
{
  DBG_ENTER("InitDisplay");
  DBG_LOG("InitDisplay: screenRes=%dx%d client=%dx%d pConfigEx=0x%X",
    g_VideoConfig.screenResX, g_VideoConfig.screenResY,
    g_VideoConfig.clientWidth, g_VideoConfig.clientHeight,
    (unsigned)g_VideoConfig.pConfigEx);
  if ( g_VideoConfig.screenResX >= g_VideoConfig.clientWidth && g_VideoConfig.screenResY >= g_VideoConfig.clientHeight )
  {
    ProcessMessages();
    if ( !PlayIntroVideo(
            g_VideoConfig.pConfigEx->videoWindowX,
            g_VideoConfig.pConfigEx->videoWindowY,
            g_VideoConfig.pConfigEx->videoWindowWidth,
            g_VideoConfig.pConfigEx->videoWindowHeight,
            0) )
    {
      ShowWindow(g_MainWindow, g_VideoConfig.nShowCmd);
      UpdateWindow(g_MainWindow);
      SendMessageA(g_MainWindow, 0x30Fu, 0, 0);
    }
    return 1;
  }
  else
  {
    SetErrorMessage("Game REQUIRES larger screen.");
    return 0;
  }
}

BOOL __stdcall InitializeVideoSystem(void *a1, struct VideoConfigEx *a2, struct VideoDataBuffer *a3)
{
  DBG_ENTER("InitializeVideoSystem");
  int v5; // eax
  void *pLockedMem; // eax

  g_VideoConfig.pDataBuffer = a3;
  g_VideoConfig.pConfigEx = a2;
  DBG_LOG("InitVidSys: pConfigEx=0x%X (&dword_4161F0=0x%X)",
    (unsigned)g_VideoConfig.pConfigEx, (unsigned)&dword_4161F0);
  g_VideoConfig.pInputConfig = a1;
  if ( !g_MainWindow )
    CreateMainWindow(a2);
  if ( !g_Reserved )
  {
    /* Plain bottom-up 8-bit backbuffer (replaces CreateDIBSection). Same row layout the DIB
       had: stride rounded up to 4 bytes, rows stored last-line-first; only the present step
       (SdlBackendPresentRect) flips rows, like the BitBlt source did. */
    int bbStride = (bmi.bmiHeader.biWidth + 3) & ~3;
    int bbHeight = bmi.bmiHeader.biHeight < 0 ? -bmi.bmiHeader.biHeight : bmi.bmiHeader.biHeight;
    g_Reserved = (int)calloc(1, (size_t)bbStride * bbHeight);
    DBG_LOG("InitVidSys: backbuffer %dx%d stride=%d -> 0x%X", bbStride, bbHeight, bbStride, g_Reserved);
  }
  if ( !s_VideoTargetsDone )
  {
    if ( SdlBackendCreateVideoTargets(g_VideoConfig.clientWidth, g_VideoConfig.clientHeight) )
      s_VideoTargetsDone = 1;
    else
      DBG_LOG("InitVidSys: SdlBackendCreateVideoTargets failed");
  }
  v5 = g_Reserved;
  a3->buffer = (void *)g_Reserved;
  if ( v5 )
    a3->field_0 -= a3->size;
  a3->field_0 -= 80000;
  DBG_LOG("InitVidSys: field_0=%d size=%d g_Reserved=0x%X", a3->field_0, a3->size, g_Reserved);
  if ( !g_VideoConfig.pLockedMem )
  {
    g_VideoConfig.hGlobalMem = GlobalAlloc(2u, a3->field_0);
    g_VideoConfig.pLockedMem = GlobalLock(g_VideoConfig.hGlobalMem);
    DBG_LOG("InitVidSys: hGlobalMem=0x%X pLockedMem=0x%X", (unsigned int)g_VideoConfig.hGlobalMem, (unsigned int)g_VideoConfig.pLockedMem);
  }
  pLockedMem = g_VideoConfig.pLockedMem;
  a3->field_4 = (int)g_VideoConfig.pLockedMem;
  return pLockedMem != 0;
}

char __stdcall SetFrameRate(int frameRate)
{
  g_VideoConfig.frameDelayMs = 0;
  if ( (_BYTE)frameRate )
    g_VideoConfig.frameDelayMs = 1000 / (unsigned __int8)frameRate;
  return 1;
}

BOOL __stdcall SetPalette(char a1, char a2, int cEntries, BYTE *a4)
{
  PALETTEENTRY *palPalEntry; // esi
  int m; // edx
  BYTE *v10; // eax
  struct { WORD palVersion; WORD palNumEntries; PALETTEENTRY palPalEntry[256]; } plpal;
  PALETTEENTRY *v16_ptr; /* alias for &plpal.palPalEntry[246] */

  palPalEntry = plpal.palPalEntry;
  v16_ptr = &plpal.palPalEntry[246];
  if ( g_VideoConfig.clientWidth && g_VideoConfig.clientHeight )
  {
    if ( a2 )
      SdlBackendDrawGrid(*(_DWORD *)&aCreatedibsecti[4 * a2 + 15]);// color table read kept; renderer grid instead of GDI pen lines
    else
      SdlBackendClearBlack();                   // original: FillRect(window, BLACK_BRUSH) - black the screen
  }
  plpal.palVersion = 768;
  plpal.palNumEntries = cEntries;
  if ( cEntries > 0 )
  {
    for ( m = 0; m < cEntries; ++m )
    {
      palPalEntry->peRed = *a4;
      palPalEntry->peGreen = a4[1];
      v10 = a4 + 2;
      a4 += 3;
      palPalEntry->peBlue = *v10;
      palPalEntry->peFlags = 4;
      ++palPalEntry;
    }
  }
  if ( a1 )
  {
    GetSystemPaletteEntries(g_VideoConfig.hDC, 0, 0xAu, plpal.palPalEntry);
    GetSystemPaletteEntries(g_VideoConfig.hDC, 0xF6u, 0xAu, v16_ptr);
  }
  SdlBackendUpdatePalette(plpal.palPalEntry, cEntries);// palette LUT for 8-bit present (replaces SetDIBColorTable)
  if ( g_VideoConfig.hPalette )
    DeleteObject((HGDIOBJ)g_VideoConfig.hPalette);
  g_VideoConfig.hPalette = (int)CreatePalette((LOGPALETTE *)&plpal);
  if ( g_MainWindow )
    SendMessageA(g_MainWindow, 0x30Fu, 0, 0);
  return g_VideoConfig.hPalette != 0;
}

int __stdcall BlitRectToScreen(int bmp_height, int cy, int cx, int y, int x, void *lpBits)
{                                               // Convert the rect through the palette LUT, upload and present
  SdlBackendPresentRect(lpBits, bmp_height, g_BackbufferStride, x, y, cx, cy);
// (replaces BitBlt from the DIB / StretchDIBits with srcY = bmp_height - y - cy; both were 1:1, no scaling)
  return 1;
}

char __stdcall BlitScreenWithDissolve(
        char use_dissolve,
        int bmp_height,
        int unused,
        int rect_height,
        int rect_width,
        int rect_y,
        int rect_x,
        void *lpBits)
{                                               // Early out if window has zero dimensions
int tileX;
// esi
int i;
// ebx
int passIndex;
// [esp+10h] [ebp-Ch]
int *pPatternY;
// [esp+14h] [ebp-8h]
int *pPatternX;
// [esp+18h] [ebp-4h]

  if ( g_VideoConfig.clientWidth && g_VideoConfig.clientHeight )
  {                                             // Dissolve mode: iterate 16 passes with different tile offsets
    if ( use_dissolve )
    {
      passIndex = 0;                            // 16 dissolve passes, each using a different (x,y) offset from pattern tables
      pPatternY = (int *)&g_DissolvePatternY;
      pPatternX = (int *)&g_DissolvePatternX;
      do
      {                                         // Iterate X across screen in 64-pixel steps starting at pattern offset
        for ( tileX = *pPatternX; tileX < g_VideoConfig.clientWidth; tileX += 64 )
        {                                       // Iterate Y across screen in 64-pixel steps starting at pattern offset
          for ( i = *pPatternY; i < g_VideoConfig.clientHeight; i += 64 )
          {
            BlitRectToScreen(bmp_height, 16, 16, i, tileX, lpBits);// Blit one 16x16 tile to screen
            // GdiFlush removed: BlitRectToScreen presents immediately (dissolve animation)
          }
        }
        ++pPatternY;
        ++pPatternX;
        ++passIndex;
      }
      while ( passIndex < 16 );
    }
    else
    {
BlitRectToScreen(bmp_height, rect_height, rect_width, rect_y, rect_x, lpBits);
// Non-dissolve: single rectangle blit
    }
  }
  return 1;
}

BOOL FlushGDI(void)
{
  // GdiFlush removed: every BlitRectToScreen presents immediately, nothing to flush
  return 1;
}

char __stdcall PumpWindowMessages(DWORD *timestamp)
{
  char result; // al

  if ( !*timestamp )
    g_VideoConfig.eventCode = 14;
  if ( SdlBackendPumpEvents() )                 // SDL_PollEvent drains + dispatches; SDL_QUIT == WM_QUIT (18)
  {
    g_VideoConfig.pErrorMessage = 1;
    return -125;
  }
  *timestamp = SDL_GetTicks();                  // was: timeGetTime()
  result = g_VideoConfig.eventCode;
  g_VideoConfig.eventCode = 14;
  return result;
}

#pragma pack(pop)
