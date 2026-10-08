/*
 * win32_shim.h - minimal Win32/mmsystem/io/vfw compatibility layer for the Linux build.
 *
 * Non-Windows only. On Windows the real SDK headers are used and this file is
 * never reached. The fake headers in SRC/winshim/ (windows.h, mmsystem.h, io.h,
 * vfw.h, intrin.h, crtdbg.h) just include this file; SRC/winshim is added to the
 * include path ONLY by the Linux CMake build.
 *
 * Scope (kept intentionally narrow, per the "minimal #ifdef" porting rule):
 *   - types/macros the decompiled code references (HWND, DWORD, WM_*, packed
 *     structs like STARTUPINFOA with the exact Windows x86 layout where the
 *     game memcpys fixed sizes);
 *   - CRT names (_open/_read/_stricmp/strupr/_rotl...);
 *   - kernel32/user32/gdi32/winmm functions the game calls, implemented over
 *     SDL2/POSIX in win32_shim.c;
 *   - a tiny posted-message queue + dispatch (PostMessageA/PeekMessageA/
 *     DispatchMessageA/PostQuitMessage), because on Linux there is no Win32
 *     message queue: sdl_backend.c synthesizes WM_* messages from SDL events
 *     and posts them here, the game's own PeekMessageA loops drain it.
 *
 * The 32-bit (-m32) requirement is a hard constraint: game_globals_struct.h
 * stores pointers in 32-bit fields byte-for-byte like the original EXE.
 */
#ifndef PILOTS_WIN32_SHIM_H
#define PILOTS_WIN32_SHIM_H

#ifndef _WIN32

#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <pthread.h>
#include <strings.h>   /* strcasecmp/strncasecmp for the _stricmp macros below */

/* ---- MSVC keywords ---- */
#define __cdecl
/*
 * __stdcall -> GCC's i386 stdcall attribute: the callee cleans its own stack
 * via `ret N`, exactly like on Win32. This is NOT cosmetic - script.c's
 * CallNativeWithVMStack measures the bytes the callee cleaned to learn how
 * many VM-stack entries the native consumed (g_NativeArgsConsumed). With
 * plain cdecl that measurement would always be 0 and the VM stack would never
 * pop. Same reason CALLBACK/WINAPI/PASCAL map to stdcall: wndproc/timer
 * callback types in this shim must match the game's __stdcall declarations.
 */
#define __stdcall       __attribute__((stdcall))
#define __fastcall      __attribute__((fastcall))
#define __inline        inline
#define __declspec(x)
#ifndef CALLBACK
#define CALLBACK        __stdcall
#endif
#ifndef WINAPI
#define WINAPI          __stdcall
#endif
#define APIENTRY        __stdcall
#define PASCAL          __stdcall

/* MSVC CRT intrinsic (resource.c:64) */
#define _ReturnAddress() __builtin_return_address(0)

#define __int8   char
#define __int16  short
#define __int32  int
#define __int64  long long

/* ---- basic Win32 types (Win32 x86 sizes; -m32 keeps int/long/pointer at 4) ---- */
typedef void            VOID;
typedef int             BOOL;          /* Win BOOL is 32-bit */
typedef unsigned char   BYTE;
typedef unsigned short  WORD;
typedef unsigned int    DWORD;
typedef unsigned int    UINT;
typedef int             INT;
typedef int             LONG;          /* Win32 LONG = 32-bit */
typedef unsigned long   ULONG;
typedef signed short    SHORT;
typedef char            CHAR;
typedef unsigned short  WCHAR;
typedef float           FLOAT;

typedef intptr_t        LONG_PTR;
typedef uintptr_t       ULONG_PTR;
typedef intptr_t        INT_PTR;
typedef uintptr_t       UINT_PTR;
typedef uintptr_t       DWORD_PTR;
typedef uintptr_t       WPARAM;
typedef intptr_t        LPARAM;
typedef intptr_t        LRESULT;

typedef void           *LPVOID;
typedef const void     *LPCVOID;
typedef BYTE           *LPBYTE;
typedef DWORD          *LPDWORD;
typedef CHAR           *LPSTR;
typedef const CHAR     *LPCSTR;
typedef WCHAR          *LPWSTR;
typedef const WCHAR    *LPCWSTR;
typedef int            *LPBOOL;

typedef void           *HANDLE;
typedef HANDLE          HINSTANCE;
typedef HANDLE          HMODULE;
typedef HANDLE          HWND;
typedef HANDLE          HDC;
typedef HANDLE          HICON;
typedef HANDLE          HCURSOR;
typedef HANDLE          HMENU;
typedef HANDLE          HGDIOBJ;
typedef HANDLE          HBRUSH;
typedef HANDLE          HBITMAP;
typedef HANDLE          HFONT;
typedef HANDLE          HPEN;
typedef HANDLE          HRGN;
typedef HANDLE          HPALETTE;
typedef HANDLE          HGLOBAL;
typedef HANDLE          HKEY;
typedef int             HFILE;
typedef DWORD          *LPCOLORREF;

typedef LRESULT (CALLBACK *WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef WNDPROC         LPWNDPROC;

#define CONST const
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void*)0)
#endif

#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE 0xFFFFFFFFu

/* calling this a "resource" handle keeps LoadCursorA(0, MAKEINTRESOURCE(...)) working */
typedef WORD ATOM;
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(WORD)(i))

/* ---- idiom helpers used by defs.h / game code ---- */
static __inline unsigned int _rotl(unsigned int v, int c)
{
    unsigned int n = (unsigned int)c & 31u;
    return n ? ((v << n) | (v >> (32u - n))) : v;
}
static __inline unsigned int _rotr(unsigned int v, int c)
{
    unsigned int n = (unsigned int)c & 31u;
    return n ? ((v >> n) | (v << (32u - n))) : v;
}

#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#define stricmp   strcasecmp
#define strnicmp  strncasecmp

extern char *strupr(char *s);

/* ---- winuser.h message / input constants (from the Windows SDK) ---- */
#define WM_DESTROY      0x0002
#define WM_SIZE         0x0005
#define WM_ACTIVATE     0x0006
#define WM_SETFOCUS     0x0007
#define WM_KILLFOCUS    0x0008
#define WM_CLOSE        0x0010
#define WM_QUIT         0x0012
#define WM_SETCURSOR    0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_PAINT        0x000F
#define WM_KEYDOWN      0x0100
#define WM_KEYUP        0x0101
#define WM_CHAR         0x0102
#define WM_KEYLAST      0x0109
#define WM_MOUSEMOVE    0x0200
#define WM_LBUTTONDOWN  0x0201
#define WM_LBUTTONUP    0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN  0x0204
#define WM_RBUTTONUP    0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MOUSEACTIVATE 0x0021
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_QUERYNEWPALETTE 0x030F
#define WM_PALETTECHANGED  0x0311
#define WM_SETTEXT      0x000C
#define MM_WOM_DONE     0x03BD
#define MM_MCINOTIFY    0x03CB

#define HTCLIENT 1
#define PM_REMOVE 1

/* virtual keys */
#define VK_BACK    0x08
#define VK_TAB     0x09
#define VK_RETURN  0x0D
#define VK_SHIFT   0x10
#define VK_CONTROL 0x11
#define VK_MENU    0x12
#define VK_PAUSE   0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE  0x1B
#define VK_SPACE   0x20
#define VK_PRIOR   0x21
#define VK_NEXT    0x22
#define VK_END     0x23
#define VK_HOME    0x24
#define VK_LEFT    0x25
#define VK_UP      0x26
#define VK_RIGHT   0x27
#define VK_DOWN    0x28
#define VK_SNAPSHOT 0x2C
#define VK_INSERT  0x2D
#define VK_DELETE  0x2E
#define VK_LWIN    0x5B
#define VK_F1      0x70
#define VK_NUMLOCK 0x90
#define VK_SCROLL  0x91

#define VK_NUMPAD0 0x60
#define VK_MULTIPLY 0x6A
#define VK_ADD     0x6B
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE  0x6F

/* ---- windef.h structs ---- */
typedef struct tagRECT { LONG left; LONG top; LONG right; LONG bottom; } RECT, *LPRECT;
typedef struct tagPOINT { LONG x; LONG y; } POINT, *LPPOINT;
typedef struct tagSIZE { LONG cx; LONG cy; } SIZE;

typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *LPMSG;

typedef struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;

typedef struct tagMINMAXINFO {
    POINT ptReserved;
    POINT ptMaxSize;
    POINT ptMaxPosition;
    POINT ptMinTrackSize;
    POINT ptMaxTrackSize;
} MINMAXINFO, *LPMINMAXINFO;

/* exact Windows x86 layout: the game asserts StartupInfo.cb = 68 and memcpys
   sizeof(STARTUPINFOA) from a template global */
#pragma pack(push, 4)
typedef struct _STARTUPINFOA {
    DWORD  cb;
    LPSTR  lpReserved;
    LPSTR  lpDesktop;
    LPSTR  lpTitle;
    DWORD  dwX;
    DWORD  dwY;
    DWORD  dwXSize;
    DWORD  dwYSize;
    DWORD  dwXCountChars;
    DWORD  dwYCountChars;
    DWORD  dwFillAttribute;
    DWORD  dwFlags;
    WORD   wShowWindow;
    WORD   cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput;
    HANDLE hStdOutput;
    HANDLE hStdError;
} STARTUPINFOA, *LPSTARTUPINFOA;

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD  dwProcessId;
    DWORD  dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;
#pragma pack(pop)

/* ---- winbase.h ---- */
#define GENERIC_READ  0x80000000u
#define GENERIC_WRITE 0x40000000u
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define CREATE_NEW    1
#define FILE_ATTRIBUTE_NORMAL 0x80

typedef struct _OFSTRUCT {
    BYTE cBytes;
    BYTE fFixedDisk;
    WORD nErrCode;
    WORD Reserved1;
    WORD Reserved2;
    CHAR szPathName[128];
} OFSTRUCT, *LPOFSTRUCT;

/* critical section: Windows ones are recursive -> recursive pthread mutex */
typedef struct _CRITICAL_SECTION {
    pthread_mutex_t mutex;
    int inited;
} CRITICAL_SECTION, *LPCRITICAL_SECTION;

#define CW_USEDEFAULT ((int)0x80000000)
#define GWLP_WNDPROC (-4)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)

/* GetSystemMetrics indices the game uses */
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXFIXEDFRAME 32
#define SM_CYFIXEDFRAME 33
#define SM_CXSIZEFRAME 32
#define SM_CYSIZEFRAME 33

/* ---- mmsystem.h (winmm) ---- */
typedef UINT MMRESULT;
typedef DWORD MCIERROR;
typedef UINT MCIDEVICEID;

struct timecaps_tag { UINT wPeriodMin; UINT wPeriodMax; };
typedef struct timecaps_tag TIMECAPS, *LPTIMECAPS;

typedef void (CALLBACK *LPTIMECALLBACK)(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);
#define TIME_ONESHOT   0
#define TIME_PERIODIC  1
#define TIMERR_NOERROR 0

#pragma pack(push, 2)
typedef struct wavehdr_tag {
    LPSTR  lpData;
    DWORD  dwBufferLength;
    DWORD  dwBytesRecorded;
    DWORD_PTR dwUser;
    DWORD  dwFlags;
    DWORD  dwLoops;
    struct wavehdr_tag *lpNext;
    DWORD_PTR reserved;
} WAVEHDR, *LPWAVEHDR;

typedef struct tWAVEFORMATEX {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
    WORD  wBitsPerSample;
    WORD  cbSize;
} WAVEFORMATEX, *LPWAVEFORMATEX;

typedef struct tWAVEFORMAT {
    WORD  wFormatTag;
    WORD  nChannels;
    DWORD nSamplesPerSec;
    DWORD nAvgBytesPerSec;
    WORD  nBlockAlign;
} WAVEFORMAT, *LPWAVEFORMAT;
#pragma pack(pop)

#define WAVE_FORMAT_PCM 1
#define WHDR_DONE    0x00000001
#define WHDR_PREPARED 0x00000002
#define WHDR_BEGINLOOP 0x00000004
#define WHDR_ENDLOOP 0x00000008
#define WHDR_INQUEUE 0x00000010

#define MCIERR_CANNOT_FIND_DRIVER 275
#define MCIERR_MISSING_PARAMETER  312

/* ---- wingdi.h ---- */
#pragma pack(push, 2)
typedef struct tagPALETTEENTRY { BYTE peRed; BYTE peGreen; BYTE peBlue; BYTE peFlags; } PALETTEENTRY, *LPPALETTEENTRY;
typedef struct tagLOGPALETTE {
    WORD palVersion;
    WORD palNumEntries;
    PALETTEENTRY palPalEntry[1];
} LOGPALETTE, *LPLOGPALETTE;

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *LPBITMAPINFOHEADER;

typedef struct tagRGBQUAD { BYTE rgbBlue; BYTE rgbGreen; BYTE rgbRed; BYTE rgbReserved; } RGBQUAD;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO, *LPBITMAPINFO;
#pragma pack(pop)

/* GetDeviceCaps indices are passed as raw numbers by the game (video.c:38 == RASTERCAPS) */

/* ---- winuser / gdi / kernel32 / winmm function protos ---- */
#ifdef __cplusplus
extern "C" {
#endif

/* user32 */
int  MessageBoxA(HWND, LPCSTR, LPCSTR, UINT);
int  MessageBeep(UINT);
int  GetSystemMetrics(int);
HDC  GetDC(HWND);
int  ReleaseDC(HWND, HDC);
int  UpdateWindow(HWND);
int  ShowWindow(HWND, int);
int  InvalidateRect(HWND, const RECT *, int);
int  GetUpdateRect(HWND, LPRECT, int);
HDC  BeginPaint(HWND, LPPAINTSTRUCT);
int  EndPaint(HWND, const PAINTSTRUCT *);
LRESULT DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
int  PostMessageA(HWND, UINT, WPARAM, LPARAM);
LRESULT SendMessageA(HWND, UINT, WPARAM, LPARAM);
void PostQuitMessage(int);
int  PeekMessageA(LPMSG, HWND, UINT, UINT, UINT);
int  GetMessageA(LPMSG, HWND, UINT, UINT);
int  TranslateMessage(const MSG *);
LRESULT DispatchMessageA(const MSG *);
int  DestroyWindow(HWND);
int  MoveWindow(HWND, int, int, int, int, int);
int  SetWindowPos(HWND, HWND, int, int, int, int, UINT);
LONG GetWindowLongA(HWND, int);
int  AdjustWindowRectEx(LPRECT, DWORD, int, DWORD);
int  IsIconic(HWND);
HCURSOR LoadCursorA(HINSTANCE, LPCSTR);
HCURSOR SetCursor(HCURSOR);
int  GetCursorPos(LPPOINT);
int  SetCursorPos(int, int);
int  ScreenToClient(HWND, LPPOINT);
int  ClientToScreen(HWND, LPPOINT);
SHORT GetAsyncKeyState(int);
SHORT GetKeyState(int);
int  WinHelpA(HWND, LPCSTR, UINT, ULONG_PTR);
int  wsprintfA(LPSTR, LPCSTR, ...);
int  wvsprintfA(LPSTR, LPCSTR, va_list);

/* kernel32 */
DWORD GetModuleFileNameA(HINSTANCE, LPSTR, DWORD);
DWORD GetFullPathNameA(LPCSTR, DWORD, LPSTR, LPSTR *);
DWORD GetTempPathA(DWORD, LPSTR);
DWORD GetVersion(void);
void  Sleep(DWORD);
HGLOBAL GlobalAlloc(UINT, size_t);
HGLOBAL GlobalFree(HGLOBAL);
HMODULE LoadLibraryA(LPCSTR);
void   *GetProcAddress(HMODULE, LPCSTR);
int    FreeLibrary(HMODULE);
int    CreateProcessA(LPCSTR, LPSTR, void *, void *, int, DWORD, void *, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DWORD  WaitForSingleObject(HANDLE, DWORD);
int    CloseHandle(HANDLE);
HANDLE CreateFileA(LPCSTR, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
int    WriteFile(HANDLE, const void *, DWORD, LPDWORD, void *);
int    ReadFile(HANDLE, void *, DWORD, LPDWORD, void *);
int    FlushFileBuffers(HANDLE);
int    lstrlenA(LPCSTR);
void   OutputDebugStringA(LPCSTR);
int    WideCharToMultiByte(UINT, DWORD, const WCHAR *, int, char *, int, const char *, int *);
void   InitializeCriticalSection(LPCRITICAL_SECTION);
void   EnterCriticalSection(LPCRITICAL_SECTION);
void   LeaveCriticalSection(LPCRITICAL_SECTION);
void   DeleteCriticalSection(LPCRITICAL_SECTION);

/* gdi32 */
HPALETTE CreatePalette(LPLOGPALETTE);
int      DeleteObject(HGDIOBJ);
int      DeleteDC(HDC);
HPALETTE SelectPalette(HDC, HPALETTE, int);
UINT     RealizePalette(HDC);
UINT     GetSystemPaletteEntries(HDC, UINT, UINT, PALETTEENTRY *);
int      GetDeviceCaps(HDC, int);

/* winmm */
DWORD  timeGetTime(void);
MMRESULT timeSetEvent(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
MMRESULT timeKillEvent(MMRESULT);
MMRESULT timeBeginPeriod(UINT);
MMRESULT timeEndPeriod(UINT);
MMRESULT timeGetDevCaps(LPTIMECAPS, UINT);
MMRESULT mciSendCommandA(MCIDEVICEID, UINT, DWORD, DWORD_PTR);
int      mciGetErrorStringA(MCIERROR, LPSTR, UINT);

/* msvcrt/io.h names */
int    _open(const char *, int, ...);
int    _close(int);
int    _read(int, void *, unsigned int);
int    _write(int, const void *, unsigned int);
long   _lseek(int, long, int);

/* MSVC _O_* flag values (ucrt fcntl.h) - mapped to POSIX inside _open() */
#define _O_RDONLY   0x0000
#define _O_WRONLY   0x0001
#define _O_RDWR     0x0002
#define _O_APPEND   0x0008
#define _O_CREAT    0x0100
#define _O_TRUNC    0x0200
#define _O_EXCL     0x0400
#define _O_TEXT     0x4000
#define _O_BINARY   0x8000
#define _O_NOINHERIT 0x0080
#define _O_TEMPORARY 0x0040

/* msvcrt: _chsize (fileio.c TruncateFile) and remove() with Win path semantics */
int    _chsize(int fd, long size);
int    WinShim_remove(const char *path);
#define remove(p) WinShim_remove(p)

/* msvcrt has itoa, glibc does not (text.c:395 formats a native result) */
char  *itoa(int value, char *buffer, int radix);

/* extra kernel32 bits used by video.c / fileio.c / graphics.c */
DWORD GetFileAttributesA(LPCSTR lpFileName);
DWORD GetLogicalDrives(void);
UINT  GetDriveTypeA(LPCSTR lpRootPathName);
HGLOBAL GlobalLock(HGLOBAL hMem);
int     GlobalUnlock(HGLOBAL hMem);

#define FILE_SHARE_READ  0x00000001
#define FILE_SHARE_WRITE 0x00000002

#ifdef __cplusplus
}
#endif

/* ---- internal queue plumbing used by sdl_backend.c (not part of the Win32 API) ---- */
int  WinShim_PumpSDLToQueue(void);   /* drain SDL events -> synthesize+post WM_* (sdl_backend.c) */
int  WinShim_QuitPosted(void);       /* PostQuitMessage seen? */
HWND WinShimMainWindow(void);        /* the fake main-window handle SdlBackendCreateWindow returns */

#endif /* !_WIN32 */
#endif /* PILOTS_WIN32_SHIM_H */
