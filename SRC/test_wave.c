#include "pilots_fwd.h"

/* Scratch harness: verifies the SDL wave path end-to-end without the game.
   Not part of the build bats (like test_cast.c). Build:
     cl /TC /W3 /Od /D_DEBUG /DWIN32 /MTd /I..\third_party\SDL2\include <all game .c> test_wave.c
     link /SUBSYSTEM:CONSOLE ... SDL2.lib
   It wires the real InitGraphics -> InitDirectDraw_Win95 -> InitWaveOut chain, queues a
   square-wave sound through PlaySoundEffect and pumps SdlWavePump the way the game loops
   do; the debug log must show "first buffer queued" + "first completion reported". */

int main(void)
{
  _BYTE maxScreen = 0;
  _BYTE curScreen = 0;
  static _BYTE soundData[4 + 8192];
  unsigned int maxQueued = 0;
  unsigned int cur;
  int i;

  DbgLogInit();
  DBG_ENTER("test_wave main");

  SDL_SetMainReady();
  InitGraphics(&maxScreen, &curScreen, 0, 4);
  if ( !SdlWave_IsOpen() )
  {
    printf("FAIL: wave device not open\n");
    return 1;
  }
  printf("wave device open, wave volume=%d\n", SdlWave_GetVolume());

  *(_DWORD *)soundData = 8192;                  // sound header: data length (multiple of 4)
  for ( i = 0; i < 8192; ++i )                  // ~460 Hz square wave, u8 centered at 128
    soundData[4 + i] = (((i / 12) & 1) ? 0xE0 : 0x40);

  PlaySoundEffect(1, 0, -1, (int *)soundData);  // game entry point (prefills two buffers)

  for ( i = 0; i < 900; ++i )                   // ~9 s of pumping, like WinMain/PumpWindowMessages:
                                                // the mixer reports per 5 s of audio, so a shorter
                                                // pump proves the queue drained and still logs nothing
  {
    SdlWavePump();
    cur = SdlWave_Queued();
    if ( cur > maxQueued )
      maxQueued = cur;
    SDL_Delay(10);
  }
  printf("max queued=%u final queued=%u (device is consuming: %s)\n",
         maxQueued, SdlWave_Queued(),
         SdlWave_Queued() < maxQueued ? "yes" : "NO");
  printf("expect in log: WriteWaveBuffer first queued + SdlWavePump first completion\n");

  CleanupWaveOut();
  SDL_Quit();
  return 0;
}
