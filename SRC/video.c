#include "pilots_fwd.h"

#pragma pack(push, 2)

/* ---- video module ---- */

BOOL __stdcall InitializeVideoPlayer(int Offset)
{
  video_repeat_count = 1;
  frame_y_offset = 0;
  frame_x_offset = 0;
  music_data_size = 0;
  sound_data_size = 0;
  palette_size = 0;
  frame_data_size = 0;
  compressed_buffer_size = 0;
  target_frame_delay = 0;
  video_file_offset = 0;
  music_data_buffer = 0;
  sound_data_buffer = 0;
  palette_buffer = 0;
  frame_data_buffer = 0;
  compressed_buffer = 0;
  music_playing_flag = 0;
  sound_playing_flag = 0;
  frame_width = dword_4161FC;
  frame_height = dword_416200;
  ResetGameState();
  return SeekFile(0, Offset);
}

char CleanupVideoResources()
{
  if ( sound_playing_flag )
    ClearInputBuffer();
  ResetGameState();
  SafeFreeMemoryBlock((void *)compressed_buffer);
  SafeFreeMemoryBlock((void *)frame_data_buffer);
  SafeFreeMemoryBlock((void *)palette_buffer);
  SafeFreeMemoryBlock((void *)sound_data_buffer);
  SafeFreeMemoryBlock((void *)music_data_buffer);
  RestoreGraphicsAfterVideo();
  return 1;
}

int HandleVideoError()
{
  DBG_ENTER("HandleVideoError");
  CleanupVideoResources();
  SetErrorMessage("Movie playback error."); return 0;
}

int __stdcall ReadAndDecompressVideoData(int a1, void *DstBuf)
{
  if ( !(_DWORD)chunk_data_qword )
    return 1;
  if ( (_DWORD)chunk_data_qword == HIDWORD(chunk_data_qword) )
    return ReadFileData(chunk_data_qword, DstBuf);
  ReadFileData(chunk_data_qword, (void *)compressed_buffer);
  LZSSDecompress(chunk_data_qword, DstBuf, (char *)(INT_PTR)compressed_buffer);
  return 1;
}

char ParseVideoHeader()
{
  char result; // al

  switch ( video_chunk_type )
  {
    case 0:
      if ( chunk_data_qword != 0x5D2D3D53563D2D5BLL )
        goto LABEL_20;
      return 3;
    case 1:
      video_repeat_count = 1;
      video_file_offset = 0;
      if ( has_low_dword )
        video_repeat_count = chunk_data_qword;
      if ( has_high_dword )
        target_frame_delay = HIDWORD(chunk_data_qword);
      goto LABEL_29;
    case 2:
      if ( has_low_dword )
      {
        compressed_buffer_size = chunk_data_qword;
        compressed_buffer = (INT_PTR)AllocFromMemoryPool(chunk_data_qword);
        if ( !compressed_buffer )
          goto LABEL_20;
      }
      goto LABEL_29;
    case 3:
      if ( has_low_dword )
      {
        frame_data_size = chunk_data_qword;
        frame_data_buffer = (INT_PTR)AllocFromMemoryPool(chunk_data_qword);
        if ( !frame_data_buffer )
          goto LABEL_20;
      }
      if ( has_high_dword )
      {
        palette_size = HIDWORD(chunk_data_qword);
        palette_buffer = (INT_PTR)AllocFromMemoryPool(HIDWORD(chunk_data_qword));
        if ( !palette_buffer )
          goto LABEL_20;
      }
      goto LABEL_29;
    case 4:
      if ( !has_low_dword
        || (sound_data_size = chunk_data_qword, (sound_data_buffer = (INT_PTR)AllocFromMemoryPool(chunk_data_qword)) != 0) )
      {
        if ( !has_high_dword )
          goto LABEL_29;
        music_data_size = HIDWORD(chunk_data_qword);
        music_data_buffer = (INT_PTR)AllocFromMemoryPool(HIDWORD(chunk_data_qword));
        if ( music_data_buffer )
          goto LABEL_29;
      }
LABEL_20:
      result = HandleVideoError();
      break;
    case 5:
      if ( has_low_dword )
        frame_x_offset = chunk_data_qword;
      if ( has_high_dword )
        frame_x_offset = HIDWORD(chunk_data_qword);
      goto LABEL_29;
    case 6:
      if ( has_low_dword )
        frame_width = chunk_data_qword;
      if ( has_high_dword )
        frame_height = HIDWORD(chunk_data_qword);
      goto LABEL_29;
    default:
LABEL_29:
      result = 1;
      break;
  }
  return result;
}

char ProcessAnimationFrame()
{
  char *v0; // ebx
  int sprite_x; // esi
  int sprite_y; // edi
  unsigned __int8 *v3; // ebx
  int sprite_width; // ebp
  unsigned __int8 *v5; // edx
  unsigned __int8 *v6; // ebx
  unsigned __int8 *v7; // edx
  unsigned __int8 *v8; // edx
  unsigned __int8 *v9; // ebx
  unsigned __int8 *v10; // edx
  unsigned __int8 *v11; // ebx
  unsigned __int8 *v12; // edx
  unsigned __int8 *v13; // edx
  unsigned __int8 input_result; // al
  unsigned int frame_duration; // ecx
  unsigned __int8 input_wait_result; // al
  char sprite_flags; // [esp+10h] [ebp-Ch]
  int current_time; // [esp+14h] [ebp-8h] BYREF
  int sprite_height; // [esp+18h] [ebp-4h]

  if ( ReadAndDecompressVideoData(frame_data_size, (void *)frame_data_buffer) )
  {
    v0 = (char *)(INT_PTR)frame_data_buffer;
    current_time = 1;
    if ( video_chunk_type == 1 )
    {
      video_paused_flag = 0;
      ResumeWavePlayback();
      ClearBackbuffer(has_high_dword);
      if ( (unsigned int)video_repeat_count > 1 )
        video_file_offset = FileTell();
      current_time = 0;
      frame_ready_flag = 0;
    }
    else if ( video_chunk_type == 2 )
    {
      video_paused_flag = 1;
      if ( --video_repeat_count )
        SeekFile(0, video_file_offset);
    }
    if ( (_DWORD)chunk_data_qword )
    {
      do
      {
        sprite_x = frame_x_offset;
        sprite_y = frame_y_offset;
        sprite_flags = *v0;
        v3 = (unsigned __int8 *)(v0 + 1);
        if ( (sprite_flags & 0x40) != 0 )
        {
          sprite_width = frame_width;
          sprite_height = frame_height;
        }
        else
        {
          v5 = v3;
          v6 = v3 + 1;
          sprite_x = *v5 + frame_x_offset;
          if ( (sprite_flags & 4) != 0 )
          {
            v7 = v6++;
            sprite_x += *v7 << 8;
          }
          v8 = v6;
          v9 = v6 + 1;
          sprite_y = *v8 + frame_y_offset;
          if ( (sprite_flags & 8) != 0 )
          {
            v10 = v9++;
            sprite_y += *v10 << 8;
          }
          sprite_width = *v9;
          v11 = v9 + 1;
          if ( (sprite_flags & 0x10) != 0 )
          {
            v12 = v11++;
            sprite_width += *v12 << 8;
          }
          sprite_height = *v11;
          v3 = v11 + 1;
          if ( (sprite_flags & 0x20) != 0 )
          {
            v13 = v3++;
            sprite_height += *v13 << 8;
          }
        }
        v0 = (char *)(v3 + (int)(INT_PTR)RLEBlitAdditive_Dispatch(sprite_height, sprite_width, sprite_y, sprite_x, sprite_flags & 3, v3));
        if ( frame_ready_flag )
          BlitDirtyRegion(0, sprite_height + sprite_y, sprite_width + sprite_x, sprite_y, sprite_x);
      }
      while ( sprite_flags >= 0 );
    }
    if ( frame_ready_flag )
      ClearDirtyRegionsAndInit(1);
    input_result = PumpWindowMessages(&current_time);
    if ( input_result != 130 )
    {
      if ( input_result == 131 )
        return 3;
      if ( video_chunk_type == 1 )
      {
        BlitDirtyRegion(
          g_DisplayMode[0],
          frame_height + frame_y_offset,
          frame_width + frame_x_offset,
          frame_y_offset,
          frame_x_offset);
        PumpWindowMessages(&last_frame_time);
        frame_counter = 0;
        skip_frame_counter = 0;
        frame_ready_flag = 1;
LABEL_45:
        FlushGDI();
        ++frame_counter;
        return 1;
      }
      if ( !target_frame_delay )
      {
        frame_ready_flag = 1;
        goto LABEL_45;
      }
      frame_duration = (current_time - last_frame_time) / (unsigned int)frame_counter;
      if ( frame_duration > target_frame_delay )
      {
        if ( sound_data_size )
        {
          if ( ++skip_frame_counter != 8 )
          {
            frame_ready_flag = 0;
            goto LABEL_45;
          }
        }
      }
      else if ( frame_duration < target_frame_delay )
      {
        do
        {
          input_wait_result = PumpWindowMessages(&current_time);
          if ( input_wait_result == 130 )
            return 2;
          if ( input_wait_result == 131 )
            return 3;
        }
        while ( (current_time - last_frame_time) / (unsigned int)frame_counter < target_frame_delay );
      }
      skip_frame_counter = 0;
      frame_ready_flag = 1;
      goto LABEL_45;
    }
    return 2;
  }
  return 0;
}

BOOL SetVideoPalette()
{
  return (unsigned __int8)ReadAndDecompressVideoData(palette_size, (void *)palette_buffer)
      && SetPalette(0, g_DisplayMode[0], 256, (BYTE *)(INT_PTR)palette_buffer);
}

int PlayVideoSoundEffect()
{
  int result; // eax

  if ( video_chunk_type )
    PauseWavePlayback();
  if ( (unsigned __int8)ReadAndDecompressVideoData(sound_data_size, (void *)sound_data_buffer) )
    result = PlaySoundEffect(-127, 0, -1, (int *)(INT_PTR)sound_data_buffer);
  else
    result = 0;
  sound_playing_flag = result;
  return result;
}

char PlayVideoMusic()
{
  char result; // al

  if ( music_playing_flag && (!video_paused_flag || video_chunk_type) )
  {
    ResetGameState();
    music_playing_flag = 0;
  }
  if ( !(_DWORD)chunk_data_qword )
    return 1;
  if ( music_playing_flag )
    return SeekFile(1, chunk_data_qword);
  if ( (unsigned __int8)ReadAndDecompressVideoData(music_data_size, (void *)music_data_buffer) )
    result = PlayMusicTrack((INT_PTR)music_data_buffer);
  else
    result = 0;
  music_playing_flag = result;
  return result;
}

char __stdcall ProcessVideoCommands(int Offset)
{
  char command_result; // [esp+4h] [ebp-4h]

  if ( InitializeVideoPlayer(Offset) )
  {
    while ( 1 )
    {
      command_result = 0;
      if ( ReadFileData(0xCu, &video_command_type) )
      {
        switch ( video_command_type )
        {
          case 0:
            goto LABEL_4;
          case 1:
            command_result = ProcessAnimationFrame();
            break;
          case 2:
            command_result = SetVideoPalette();
            break;
          case 3:
            command_result = PlayVideoSoundEffect();
            break;
          case 4:
            command_result = PlayVideoMusic();
            break;
          default:
            break;
        }
      }
LABEL_9:
      if ( command_result == 2 )
        break;
      if ( !command_result )
        return HandleVideoError();
      if ( command_result == 3 )
        return CleanupVideoResources();
    }
    while ( ReadFileData(0xCu, &video_command_type) )
    {
      if ( video_command_type )
      {
        if ( !SeekFile(1, chunk_data_qword) )
          return HandleVideoError();
      }
      else if ( !video_chunk_type || video_chunk_type == 1 )
      {
LABEL_4:
        command_result = ParseVideoHeader();
        goto LABEL_9;
      }
    }
  }
  return HandleVideoError();
}

int __stdcall ExecuteVideoPlayback(int videoIndex)
{
  int commandResult; // eax

  if ( !video_enabled_flag )
    return 1;
  ++*((_BYTE*)&video_recursion_counter + 1);
  commandResult = ProcessVideoCommands(*(_DWORD *)(INT_PTR)(dword_416338 + 4 * videoIndex));
  --*((_BYTE*)&video_recursion_counter + 1);
  return commandResult;
}

/* 64-bit Windows MCI workaround.
   mciSendCommandA(MCI_OPEN) faults INSIDE winmm.dll instead of returning an
   MCIERROR - Windows Error Reporting names WINMM.dll as the faulting module,
   offset 0x176fa - and kills the process. Measured on this OS, call by call:
   only that ONE call is broken. mciSendCommandA on an already-open device
   answers normally (MCI_STATUS/MCI_PLAY/MCI_CLOSE all come back), and the
   STRING interface opens the very same device with no trouble at all, because
   it builds the parameter block winmm wants internally. So the device is opened
   with mciSendStringA("open <file> type avivideo alias <n>") and everything
   else keeps going through mciSendCommandA, exactly as the original does.
   A 32-bit build never takes this path - there MCI_OPEN works - and neither
   does Linux, where MCI is mci_ffmpeg.c, so it is compiled only for _WIN64. */
#if defined(_WIN64)

static volatile int s_MciOpenViaString;               /* command-interface MCI_OPEN is broken here */
static volatile int s_MciFaulted;                     /* the last guarded call faulted */

static MCIERROR MciSendCommandGuarded(MCIDEVICEID idDevice, UINT uMsg, DWORD dwFlags, DWORD_PTR dwParam)
{
  MCIERROR mcierr; // eax

  s_MciFaulted = 0;
  __try
  {
    mcierr = mciSendCommandA(idDevice, uMsg, dwFlags, dwParam);
  }
  __except ( EXCEPTION_EXECUTE_HANDLER )
  {
    DBG_LOG("MciSendCommandGuarded: uMsg=0x%X faulted inside winmm", uMsg);
    s_MciFaulted = 1;
    mcierr = (MCIERROR)-1;
  }
  return mcierr;
}

static MCIDEVICEID OpenMCIByString(const char *FileName)
{
  char cmd[320];
  MCIERROR err;
  MCIDEVICEID id;

  wsprintfA(cmd, "open \"%s\" type avivideo alias pilotvid", FileName);
  err = mciSendStringA(cmd, 0, 0, 0);
  id = err ? 0 : mciGetDeviceIDA("pilotvid");
  DBG_LOG("OpenMCIByString: err=%lu id=%lu", (unsigned long)err, (unsigned long)id);
  return id;
}

#define MCI_SEND(id, msg, flags, param)  MciSendCommandGuarded(id, msg, flags, param)

#else

#define MCI_SEND(id, msg, flags, param)  mciSendCommandA(id, msg, flags, param)

#endif

char __stdcall CheckMCIError(MCIERROR mcierr)
{
  CHAR pszText[256]; // [esp+0h] [ebp-100h] BYREF

  if ( !mcierr )
    return 1;
  mciGetErrorStringA(mcierr, pszText, 0x100u);
  DBG_LOG("CheckMCIError: err=%d text='%s'", mcierr, pszText);
  ShowErrorDialog(pszText);
  return 0;
}

int __stdcall SendMCICommand(DWORD_PTR dwParam2, DWORD_PTR dwParam1, UINT uMsg)
{
  MCIERROR v3; // eax

  if ( !g_MCIDeviceID )
    return 0;
  v3 = MCI_SEND((MCIDEVICEID)g_MCIDeviceID, uMsg, (DWORD)dwParam1, dwParam2);
#if defined(_WIN64)
  if ( v3 == (MCIERROR)-1 )
    return 0;                                         // winmm faulted: skip, no error dialog
#endif
  return CheckMCIError(v3);
}

DWORD_PTR __stdcall SendMCICommandSimple(DWORD_PTR result, DWORD_PTR dwParam1, UINT uMsg)
{
  DWORD_PTR dwParam2; // [esp+0h] [ebp-4h] BYREF

  dwParam2 = result;
  if ( g_MCIDeviceID )
    return SendMCICommand((DWORD_PTR)&dwParam2, dwParam1, uMsg);
  return result;
}

int PlayMCIVideo()
{
  DWORD_PTR dwParam2[3]; // [esp+0h] [ebp-Ch] BYREF

  dwParam2[0] = (unsigned __int16)g_MainWindow;
#if defined(_WIN64)
  /* MCI_PLAY_PARMS.dwCallback is where mciavi32 posts MM_MCINOTIFY (VideoWndProc
     case 0x3B9), and that message is the ONLY thing that clears g_VideoPlaying
     and so ends PlayIntroVideo's wait loop. The 16-bit truncation above assumes
     an HWND fits in a word - true on Win9x, where mciavi32 also fell back to the
     AVI window's parent, so x86 keeps working with it. On 64-bit Windows handles
     are far wider than 16 bits (0x802F0 truncated to 0x2F0), the truncated value
     names no window, MM_MCINOTIFY never arrives and the game hangs on the last
     frame forever. Pass the whole handle instead - dwCallback is DWORD_PTR on
     both arches, so this is lossless, and x86 is left byte-identical. */
  dwParam2[0] = (DWORD_PTR)g_MainWindow;
#endif
  return SendMCICommand((DWORD_PTR)dwParam2, 1u, 0x806u);
}

LRESULT __stdcall VideoWndProc(HWND hWnd, UINT Msg, HWND wParam, _DWORD *lParam)
{
  DWORD_PTR v4; // eax
  int v6; // eax
  int v7; // eax
  int v8; // eax
  int v9; // eax
  HCURSOR CursorA; // eax
  int v11; // [esp-8h] [ebp-48h]
  struct tagPAINTSTRUCT Paint; // [esp+0h] [ebp-40h] BYREF

  v4 = Msg;
  switch ( Msg )
  {
    case 0x47u:
      InvalidateRect(hWnd, 0, 0);
      return DefWindowProcA(hWnd, Msg, (WPARAM)wParam, (LPARAM)lParam);
    case 0x24u:
      v6 = g_VideoConfig.borderWidth + g_VideoConfig.initialClientWidth;
      lParam[8] = g_VideoConfig.borderWidth + g_VideoConfig.initialClientWidth;
      lParam[2] = v6;
      v7 = g_VideoConfig.titleBarHeight + g_VideoConfig.initialClientHeight;
      lParam[9] = g_VideoConfig.titleBarHeight + g_VideoConfig.initialClientHeight;
      lParam[3] = v7;
      return 0;
    case 5u:
      g_VideoConfig.clientWidth = (unsigned __int16)lParam;
      g_VideoConfig.clientHeight = HIWORD(lParam);
      return 0;
    case 0x210u:
      v4 = (unsigned __int16)wParam;
      if ( (unsigned __int16)wParam != 513 && (unsigned __int16)wParam != 516 )
        return 0;
      goto LABEL_21;
    case 0x201u:
    case 0x204u:
LABEL_21:
      DBG_LOG("VideoWndProc: skip request (Msg=0x%X wParam=0x%X)", Msg, (unsigned)(INT_PTR)wParam);
      SendMCICommandSimple(v4, 0, 0x808u);
      return 0;
    case 0x102u:
      v4 = (DWORD_PTR)wParam;
      if ( wParam != (HWND)32 && wParam != (HWND)13 )
        return 0;
      goto LABEL_21;
    case 0x20u:
      if ( (_WORD)lParam == 1 )
      {
        CursorA = LoadCursorA(0, (LPCSTR)0x7F00);
        SetCursor(CursorA);
        return 0;
      }
      return DefWindowProcA(hWnd, Msg, (WPARAM)wParam, (LPARAM)lParam);
    case 0x311u:
      if ( wParam == hWnd )
        return 0;
      goto LABEL_40;
    case 0x30Fu:
LABEL_40:
      DBG_LOG("VideoWndProc LABEL_40: Msg=0x%X wParam=0x%X hPalette=0x%X flag=%d",
        Msg, (unsigned)(INT_PTR)wParam, (unsigned)(INT_PTR)g_VideoConfig.hPalette, g_VideoFlags[2]);
      if ( !g_VideoConfig.hPalette )
        return 0;
      if ( Msg == 783 || wParam != (HWND)g_VideoWindowHandle )
        g_VideoFlags[2] = 0;
      SendMessageA((HWND)g_VideoWindowHandle, Msg, (WPARAM)wParam, (LPARAM)lParam);
      DBG_LOG("VideoWndProc LABEL_40: after forward flag=%d", g_VideoFlags[2]);
      if ( g_VideoFlags[3] || Msg == 785 )
        g_VideoFlags[2] = 1;
      SelectPalette(g_VideoConfig.hDC, (HPALETTE)(INT_PTR)g_VideoConfig.hPalette, Msg == 785);
      if ( !RealizePalette(g_VideoConfig.hDC) )
        return 0;
      InvalidateRect(hWnd, 0, 0);
      return 1;
    case 0xFu:
      BeginPaint(hWnd, &Paint);
      DBG_LOG("VideoWndProc: WM_PAINT g_VideoFlags[2]=%d rcPaint=(%d,%d)-(%d,%d)",
        g_VideoFlags[2], Paint.rcPaint.left, Paint.rcPaint.top,
        Paint.rcPaint.right, Paint.rcPaint.bottom);
      if ( g_VideoFlags[2] )
      {
        if ( !g_VideoFlag3 )
        {
          g_VideoFlag3 = 1;
          v8 = g_VideoWindowHeight;
          if ( !g_VideoWindowHeight )
            v8 = 240;
          v11 = v8;
          v9 = g_VideoWindowWidth;
          if ( !g_VideoWindowWidth )
            v9 = 320;
          MoveWindow((HWND)g_VideoWindowHandle, g_VideoWindowX, g_VideoWindowY, v9, v11, 1);
        }
        BlitDirtyRegion(
          g_VideoFlags[0],
          Paint.rcPaint.bottom,
          Paint.rcPaint.right,
          Paint.rcPaint.top,
          Paint.rcPaint.left);
        g_VideoFlags[0] = 0;
      }
      EndPaint(hWnd, &Paint);
      return 0;
    case 0x3B9u:
      g_VideoPlaying = 0;
      return 0;
    case 0x10u:
      g_VideoExitRequested = 1;
      goto LABEL_21;
    default:
      return DefWindowProcA(hWnd, Msg, (WPARAM)wParam, (LPARAM)lParam);
  }
}

char __stdcall FindDriveByType(char drive_type, _BYTE *drive_type_map, _BYTE *out_drive_path)
{
  int driveIndex; // ebx
  _BYTE *ptr; // ecx

  driveIndex = 0;
  ptr = drive_type_map;
  do
  {                                             // Check if this drive matches the requested type
    if ( *ptr == drive_type )
    {
      drive_type_map[driveIndex] = 0;           // Null-terminate the map at this position (marks it as consumed)
      *out_drive_path = driveIndex + 65;        // Build drive path string: letter = 'A' + index, then ':', then null
      out_drive_path[1] = 58;
      out_drive_path[2] = 0;
      return 1;
    }
    ++ptr;
    ++driveIndex;
  }
  while ( driveIndex < 32 );
  return 0;
}

BOOL __stdcall FindVideoOnRemovableDrives(_BYTE *a1, _BYTE *a2)
{
  return FindDriveByType(3, a1, a2)
      || FindDriveByType(5, a1, a2)
      || FindDriveByType(4, a1, a2)
      || FindDriveByType(6, a1, a2);
}

char __stdcall SearchVideoFileOnDrives(char *Str, char *Destination, char *lpFileName)
{
  char v3; // cl
  _BYTE *v4; // ebx
  char *v5; // eax
  size_t v6; // ebx
  char *v7; // eax
  char *v8; // eax
  _BYTE v10[32]; // [esp+10h] [ebp-28h] BYREF
  DWORD LogicalDrives; // [esp+30h] [ebp-8h]
  size_t i; // [esp+34h] [ebp-4h]

  LogicalDrives = GetLogicalDrives();
  for ( i = 0; i < 0x20; ++i )
  {
    v3 = i;
    v4 = &v10[i];
    v10[i] = 0;
    if ( ((1 << v3) & LogicalDrives) != 0 )
    {
      RootPathName[0] = i + 65;
      *v4 = GetDriveTypeA(RootPathName);
    }
  }
  v5 = strcpy(lpFileName, g_ExecutablePath);
  *strrchr(v5, 92) = 0;
  v6 = strlen(lpFileName);
  i = strlen(Str);
  do
  {
    if ( v6 + i < 0x48 )
    {
      v7 = strcat(lpFileName, (const char *)&asc_410892);
      v8 = strcat(v7, Str);
      strcpy(Destination, v8);
      strcat(lpFileName, aAvi);
      strcat(Destination, aBmp);
      DBG_LOG("SearchVideoFileOnDrives: try '%s'", lpFileName);
      if ( GetFileAttributesA(lpFileName) != -1 )
        return 1;
      v6 = 2;
    }
  }
  while ( FindVideoOnRemovableDrives(v10, lpFileName) );
  return 0;
}

BOOL __stdcall CheckVideoFileExists(char *Str, char *a2, char *lpFileName)
{
  char *v3; // eax
  char *v4; // eax
  char *v5; // eax
  char *v6; // eax
  char Destination[64]; // [esp+0h] [ebp-40h] BYREF

  v3 = strcpy(Destination, g_ExecutablePath);
  strrchr(v3, 92)[1] = 0;
  v4 = strrchr(Str, 92);
  strcat(Destination, v4 + 1);
  v5 = strcpy(lpFileName, Destination);
  strcat(v5, aAvi);
  v6 = strcpy(a2, Destination);
  strcat(v6, aBmp);
  DBG_LOG("CheckVideoFileExists: try '%s'", lpFileName);
  return GetFileAttributesA(lpFileName) != -1;
}

int __stdcall LoadVideoBitmap(char *FileName)
{
  DBG_LOG("LoadVideoBitmap: FileName='%s'", FileName);
  int result; // eax
  int v2; // esi
  UINT v3; // ebx
  UINT v4; // ebx
  BYTE *v5; // ebx
  BYTE *v6; // edx
#pragma pack(push, 1)
  struct { __int16 bfType; int bfSize; __int16 bfReserved1; __int16 bfReserved2; int bfOffBits; } _bmfh;
  struct { _BYTE biSize_raw[4]; int biWidth; int biHeight; __int16 biPlanes; __int16 biBitCount;
    int biCompression; int biSizeImage; int biXPelsPerMeter; int biYPelsPerMeter;
    UINT biClrUsed; int biClrImportant; } _bmih;
#pragma pack(pop)
#define DstBuf _bmfh.bfType
#define v8 _bmfh.bfSize
#define v9 _bmfh.bfReserved1
#define v10 _bmfh.bfReserved2
#define v11 _bmfh.bfOffBits
#define v12 _bmih.biSize_raw
#define v13 _bmih.biWidth
#define v14 _bmih.biHeight
#define v15 _bmih.biPlanes
#define v16 _bmih.biBitCount
#define v17 _bmih.biCompression
  UINT cEntries; /* = _bmih.biClrUsed, but accessed separately */
  _DWORD v19[256]; // [esp+40h] [ebp-700h] BYREF
  BYTE v20[768]; // [esp+440h] [ebp-300h] BYREF

  result = _open(FileName, 0x8000);
  v2 = result;
  if ( result != -1 )
  {
    _read(result, &DstBuf, 0xEu);
    _read(v2, v12, 0x28u);
    cEntries = _bmih.biClrUsed;
    if ( !cEntries )
      cEntries = 256;
    v3 = 4 * cEntries;
    _read(v2, v19, 4 * cEntries);
    v4 = v3 + 54;
    DBG_LOG("LoadVideoBitmap: bfType=%d bfSize=%d bfOffBits=%d",
      DstBuf, v8, v11);
    DBG_LOG("LoadVideoBitmap: biWidth=%d biHeight=%d biPlanes=%d biBitCount=%d biCompression=%d",
      v13, v14, v15, v16, v17);
    DBG_LOG("LoadVideoBitmap: cEntries=%u pDataBuffer->size=%d configW=%d configH=%d",
      cEntries, g_VideoConfig.pDataBuffer->size,
      g_VideoConfig.pConfigEx->width, g_VideoConfig.pConfigEx->height);
    DBG_LOG("LoadVideoBitmap: expected bfSize=%d (v4=%u + dataSize=%d) actual v8=%d",
      v4 + g_VideoConfig.pDataBuffer->size, v4, g_VideoConfig.pDataBuffer->size, v8);
    if ( DstBuf == 19778
      && v4 + g_VideoConfig.pDataBuffer->size == v8
      && !v9
      && !v10
      && v11 == v4
      && v15 == 1
      && v16 == 8
      && !v17
      && g_VideoConfig.pConfigEx->width == v13
      && g_VideoConfig.pConfigEx->height == v14 )
    {
      if ( cEntries )
      {
        v5 = (BYTE *)v19;
        v6 = &v20[2];
        do
        {
          *(v6 - 2) = v5[2];
          *(v6 - 1) = v5[1];
          *v6 = *v5;
          v5 += 4;
          v6 += 3;
        }
        while ( &v19[cEntries] > (_DWORD *)v5 );
      }
      SetPalette(1, 0, cEntries, v20);
      _read(v2, g_VideoConfig.pDataBuffer->buffer, g_VideoConfig.pDataBuffer->size);
    }
    else
    {
      ShowErrorDialog("Invalid movie background bitmap.");
    }
    return _close(v2);
  }
  return result;
}
#undef DstBuf
#undef v8
#undef v9
#undef v10
#undef v11
#undef v12
#undef v13
#undef v14
#undef v15
#undef v16
#undef v17

/* A click ON the movie never reached the game window.
   The skip path is VideoWndProc's own: 0x201/0x204 (button down, or a space/enter
   WM_CHAR) -> LABEL_21 -> MCI_STOP -> mciavi32 posts MM_MCINOTIFY (0x3B9) ->
   VideoWndProc clears g_VideoPlaying and PlayIntroVideo's wait loop ends. But
   mciavi32 draws into a real AVIWnd32 window that sits over the movie as a WS_CHILD
   of the game window, so a click there is swallowed by that child and never arrives
   - and it sends no WM_PARENTNOTIFY either, so VideoWndProc's 0x210 branch cannot
   catch it (verified: the case never fires even with a probe). A click NEXT TO the
   movie always worked, nothing swallowed it.
   Subclass that child and relay the three skip messages to the game window by hand;
   everything else still goes to mciavi32's own window proc through CallWindowProcA.
   Windows only: on Linux MCI is mci_ffmpeg.c, there is no child window at all -
   SDL posts every click straight at g_MainWindow, so both positions already worked. */
#if defined(_WIN32)

static HWND s_MciChildSubclassed;                     /* mciavi32 keeps one AVIWnd32; subclass it once */
static WNDPROC s_MciChildOldProc;

static LRESULT __stdcall MciChildForwardProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
  if ( Msg == 0x201u || Msg == 0x204u || Msg == 0x102u )
  {
    RECT childRect; // [esp+0h] [ebp-10h] BYREF

    if ( GetClientRect(hWnd, &childRect) )
      DBG_LOG("MciChildForwardProc: Msg=0x%X child rect=(%d,%d)-(%d,%d)",
        Msg, childRect.left, childRect.top, childRect.right, childRect.bottom);
    else
      DBG_LOG("MciChildForwardProc: Msg=0x%X", Msg);
    if ( g_MainWindow )
      SendMessageA((HWND)g_MainWindow, Msg, wParam, lParam);
    return 0;                                          // the child must not act on it a second time
  }
  return CallWindowProcA(s_MciChildOldProc, hWnd, Msg, wParam, lParam);
}

static void MciChildForwardClicks(HWND hWndVideo)
{
  if ( !hWndVideo || hWndVideo == (HWND)g_MainWindow )
    return;
  if ( s_MciChildSubclassed == hWndVideo )
    return;                                            // AVIWnd32 is reused between videos
  s_MciChildOldProc = (WNDPROC)SetWindowLongPtrA(hWndVideo, GWLP_WNDPROC, (LONG_PTR)MciChildForwardProc);
  if ( !s_MciChildOldProc )
  {
    DBG_LOG("MciChildForwardClicks: SetWindowLongPtrA failed on 0x%X", (unsigned)(INT_PTR)hWndVideo);
    return;
  }
  s_MciChildSubclassed = hWndVideo;
  DBG_LOG("MciChildForwardClicks: subclassed 0x%X, skip clicks relay to 0x%X",
    (unsigned)(INT_PTR)hWndVideo, (unsigned)g_MainWindow);
}

#else

#define MciChildForwardClicks(hWndVideo)  ((void)0)

#endif

char OpenMCIVideoDevice()
{
  char result; // al
  MCIERROR v1; // eax
  CHAR FileName[72]; // [esp+0h] [ebp-C4h] BYREF
  char Destination[72]; // [esp+48h] [ebp-7Ch] BYREF
  DWORD_PTR dwParam2[7]; // [esp+90h] [ebp-34h] BYREF
#pragma pack(push, 1)
  struct { DWORD_PTR dwCallback; HWND hWnd; int nCmdShow; } _mciWndParms;
#pragma pack(pop)
#define v5 _mciWndParms.dwCallback
#define hWnd _mciWndParms.hWnd
#define v7 _mciWndParms.nCmdShow

  result = (char)g_VideoConfig.pConfigEx;
  DBG_LOG("OpenMCIVideoDevice: pConfigEx->field_24=0x%08X HIBYTE=%d",
    g_VideoConfig.pConfigEx->field_24, HIBYTE(g_VideoConfig.pConfigEx->field_24));
  g_VideoFlags[0] = BYTE2(g_VideoConfig.pConfigEx->field_24);
  DBG_LOG("OpenMCIVideoDevice: HIBYTE(field_24)=%d g_VideoFilePath='%s'",
    HIBYTE(g_VideoConfig.pConfigEx->field_24), g_VideoFilePath);
  if ( HIBYTE(g_VideoConfig.pConfigEx->field_24) == 2
    && (CheckVideoFileExists(g_VideoFilePath, Destination, FileName)
     || (result = SearchVideoFileOnDrives(g_VideoFilePath, Destination, FileName)) != 0) )
  {
    DBG_LOG("OpenMCIVideoDevice: FileName='%s' Dest='%s'", FileName, Destination);
    dwParam2[3] = (DWORD_PTR)FileName;
    dwParam2[5] = 0x40000000;
    dwParam2[6] = (DWORD_PTR)g_MainWindow;
#if defined(_WIN64)
    if ( s_MciOpenViaString )                        // known-bad call: go straight to the string api
    {
      dwParam2[1] = OpenMCIByString(FileName);
      v1 = 0;                                        // a zero device id below still skips the video
    }
    else
#endif
    {
      v1 = MCI_SEND(0, 0x803u, 0x30200u, (DWORD_PTR)dwParam2);
      DBG_LOG("OpenMCIVideoDevice: mciSendCommandA returned %d, dwParam2[1]=%u", v1, (unsigned)dwParam2[1]);
#if defined(_WIN64)
      if ( s_MciFaulted )
      {                                              // command-interface MCI_OPEN died here; open by
        s_MciOpenViaString = 1;                      // string instead, and remember it for good
        dwParam2[1] = OpenMCIByString(FileName);
        v1 = 0;
      }
#endif
    }
    result = CheckMCIError(v1);
    DBG_LOG("OpenMCIVideoDevice: CheckMCIError returned %d", (int)result);
    if ( result )
    {
      g_MCIDeviceID = dwParam2[1];
      DBG_LOG("OpenMCIVideoDevice: g_MCIDeviceID set to %d", g_MCIDeviceID);
#if defined(_WIN64)
      // x86: the dwParam2[5]/[6] stores above are NOT past-the-end garbage - with a 4-byte DWORD_PTR
      // they land exactly on _mciWndParms.dwCallback/.hWnd (adjacent stack slots), and that hWnd=g_MainWindow
      // is what makes mciavi32 create its AVIWnd32 window as a CHILD of the game window (video renders
      // inside the application window, as in the original game).  On x64 DWORD_PTR is 8 bytes, so those
      // stores fall past MCI_OPEN_PARMS, hWnd stays uninitialized and mciavi32 falls back to a separate
      // top-level window.  Behaviour change for the 64-bit port only: restore the values the x86 layout
      // delivered implicitly.
      v5 = 0x40000000;
      hWnd = (HWND)g_MainWindow;
#endif
      v7 = 16385;
      DBG_LOG("OpenMCIVideoDevice: &v5=%p &hWnd=%p sizeof(DWORD_PTR)=%u",
        (void *)&v5, (void *)&hWnd, (unsigned)sizeof(DWORD_PTR));
      SendMCICommand((DWORD_PTR)&v5, 0x100u, 0x814u);
      DBG_LOG("OpenMCIVideoDevice: MCI_WINDOW returned hWnd=0x%X", (unsigned)(INT_PTR)hWnd);
#if defined(_WIN64)
      // The x64 mciavi32 ignores every parent hint: a probe over MCI_OWN_WINDOW + hWnd, + dwCallback,
      // MCI_ANIM_WINDOW_HWND and both together all produce the same captioned top-level AVIWnd32
      // (style 0x06CE0000, WS_CHILD never set), while on x86 the very same call ends up with a
      // WS_CHILD of the game window.  Reparent it here so the video renders inside the application
      // window like the original game - and since a child window carries neither a caption nor a
      // taskbar entry, the separate-window symptom goes away with it.
      if ( hWnd && g_MainWindow )
      {
        LONG style = GetWindowLongA(hWnd, GWL_STYLE);
        style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
        style |= WS_CHILD | WS_CLIPSIBLINGS;
        SetWindowLongA(hWnd, GWL_STYLE, style);
        SetParent(hWnd, (HWND)g_MainWindow);
        DBG_LOG("OpenMCIVideoDevice: reparented 0x%X under game window 0x%X",
          (unsigned)(INT_PTR)hWnd, (unsigned)g_MainWindow);
      }
#endif
      g_VideoWindowHandle = (INT_PTR)hWnd;
      MciChildForwardClicks(hWnd);                      // a click on the movie must skip it too
      DBG_LOG("OpenMCIVideoDevice: MoveWindow(0x%X, %d, %d, 0, 0)",
        (unsigned)(INT_PTR)hWnd, g_VideoWindowX, g_VideoWindowY);
      MoveWindow(hWnd, g_VideoWindowX, g_VideoWindowY, 0, 0, 0);
      return LoadVideoBitmap(Destination);
    }
  }
  return result;
}
#undef v5
#undef hWnd
#undef v7

BOOL ProcessMessages()
{
  BOOL hasMessage; // eax
  struct tagMSG Msg; // [esp+0h] [ebp-1Ch] BYREF

  for ( hasMessage = PeekMessageA(&Msg, 0, 0, 0, 1u); hasMessage; hasMessage = PeekMessageA(&Msg, 0, 0, 0, 1u) )
  {
    if ( Msg.message != 256
      && Msg.message != 513
      && Msg.message != 516
      && Msg.message != 514
      && Msg.message != 517
      && Msg.message != 512 )
    {
      TranslateMessage(&Msg);
      DispatchMessageA(&Msg);
    }
  }
  SdlWavePump();                                // refill PCM while messages drain (was done by MM_WOM_DONE)
  return hasMessage;
}

char __stdcall PlayIntroVideo(int windowHeight, int windowWidth, int windowY, int windowX, char *videoPath)
{
  DBG_ENTER("PlayIntroVideo");
  int isWin3x; // al
  char *pathPtr1; // eax
  char *pathPtr2; // eax
  char *pathPtr3; // eax
  DWORD_PTR v9; // eax
  const char *videoFileName; // [esp-4h] [ebp-30h]
  signed int winVersion; // [esp+4h] [ebp-28h]
  struct tagMSG msg; // [esp+8h] [ebp-24h] BYREF
  struct tagPOINT cursorPos; // [esp+24h] [ebp-8h] BYREF

  winVersion = GetVersion();
  isWin3x = winVersion < 0 && (unsigned __int8)winVersion < 4u;
  g_VideoFlags[3] = isWin3x;
  g_VideoFlags[1] = 1;
  g_MCIDeviceID = 0;
  g_VideoExitRequested = 0;
  g_VideoPlaying = 0;
  g_VideoFlag3 = 0;
  g_VideoFlags[2] = 0;
  g_VideoWindowX = windowX;
  g_VideoWindowY = windowY;
  g_VideoWindowWidth = windowWidth;
  g_VideoWindowHeight = windowHeight;
  pathPtr1 = videoPath;
  if ( !videoPath )
    pathPtr1 = "intro";
  videoFileName = pathPtr1;
  { const char *_vidBase = (const char *)&g_VideoConfig.pConfigEx[1];
    DBG_LOG("PlayIntroVideo: vidBase='%s' videoFileName='%s'",
      _vidBase ? _vidBase : "(null)", videoFileName ? videoFileName : "(null)");
  }
  pathPtr2 = strcpy(g_VideoFilePath, (const char *)&g_VideoConfig.pConfigEx[1]);
  pathPtr3 = strcat(pathPtr2, (const char *)&asc_410892);
  strcat(pathPtr3, videoFileName);
  DBG_LOG("PlayIntroVideo: g_VideoFilePath='%s'", g_VideoFilePath);
  DBG_LOG("PlayIntroVideo: windowX=%d windowY=%d windowW=%d windowH=%d",
    g_VideoWindowX, g_VideoWindowY, g_VideoWindowWidth, g_VideoWindowHeight);
  OpenMCIVideoDevice();
  DBG_LOG("PlayIntroVideo: g_MCIDeviceID=%d", g_MCIDeviceID);
  if ( g_MCIDeviceID )
  {
    v9 = PlayMCIVideo();
    if ( (_BYTE)v9 )
    {
      if ( videoPath )
        InvalidateRect((HWND)g_MainWindow, 0, 0);
      else
        ShowWindow((HWND)g_MainWindow, 1);
      if ( !g_VideoFlags[2] )
      {
        if ( (GetDeviceCaps(g_VideoConfig.hDC, 38) & 0x100) != 0 )
        {
          DBG_LOG("PlayIntroVideo: palette device -> post 0x30F (hPalette=0x%X cap38=0x%X)",
            (unsigned)(INT_PTR)g_VideoConfig.hPalette, (unsigned)GetDeviceCaps(g_VideoConfig.hDC, 38));
          PostMessageA((HWND)g_MainWindow, 0x30Fu, 0, 0);
        }
        else
        {
          DBG_LOG("PlayIntroVideo: no palette device -> arm flag directly");
          g_VideoFlags[2] = 1;
        }
      }
      g_VideoPlaying = 1;
      do
      {                                             // Was a blocking GetMessage loop: with SDL there is no MM_WOM_DONE
        SdlWavePump();                              // message to wake it, so poll + pump PCM + short sleep instead
        if ( PeekMessageA(&msg, 0, 0, 0, 1u) )
        {
          TranslateMessage(&msg);
          v9 = DispatchMessageA(&msg);
        }
        else
        {
          Sleep(5);
        }
      }
      while ( g_VideoPlaying );                     // ONLY MM_MCINOTIFY (VideoWndProc 0x3B9) ends this - see PlayMCIVideo's dwCallback
      DBG_LOG("PlayIntroVideo: wait loop ended, closing the device");   // a click on the movie lands here within a frame
    }
    SendMCICommandSimple(v9, 0, 0x804u);
    DestroyWindow((HWND)g_VideoWindowHandle);
    g_VideoWindowHandle = 0;
    g_VideoFlags[1] = 0;
    if ( g_VideoExitRequested )
    {
      PostMessageA((HWND)g_MainWindow, 0x10u, 0, 0);
    }
    else
    {
      RestoreGraphicsAfterVideo();
      ProcessMessages();
      GetCursorPos(&cursorPos);
      SetCursorPos(cursorPos.x, cursorPos.y);
    }
    return 1;
  }
  else
  {
    DBG_LOG("PlayIntroVideo: no MCI device, returning 0");
    g_VideoFlags[1] = 0;
    return 0;
  }
}

#pragma pack(pop)
