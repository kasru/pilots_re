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
typedef UINT           *LPUINT;

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
typedef size_t          SIZE_T;             /* winnt.h: host.c casts arena sizes through it */
typedef DWORD          *LPCOLORREF;
typedef DWORD           COLORREF;
/* windef.h shape: an unprototyped function pointer - only passed around here
   (PFN_UTREGISTER's last argument, GetProcAddress's result) */
typedef INT_PTR        (*FARPROC)(void);

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
/* Guarded: on x86 GCC's ia32intrin.h (pulled in by SDL/ffmpeg headers) already
   has `#define _rotl(a,b) __rold((a),(b))` - redefining it as a function would
   be a compile error, and the macro is semantically the same rotate. */
#ifndef _rotl
static __inline unsigned int _rotl(unsigned int v, int c)
{
    unsigned int n = (unsigned int)c & 31u;
    return n ? ((v << n) | (v >> (32u - n))) : v;
}
#endif
#ifndef _rotr
static __inline unsigned int _rotr(unsigned int v, int c)
{
    unsigned int n = (unsigned int)c & 31u;
    return n ? ((v >> n) | (v << (32u - n))) : v;
}
#endif

#define _stricmp  strcasecmp
#define _strnicmp strncasecmp
#define stricmp   strcasecmp
#define strnicmp  strncasecmp

extern char *strupr(char *s);
#define _strupr  strupr          /* host.c asks for the underscore name */

/*
 * setjmp on newlib/aarch64 (the Nintendo Switch). glibc exports _setjmp - the
 * variant that leaves the signal mask alone - and that is what
 * pilots_globals.h's `#define setjmp(env) _setjmp(g_ShimJumpBuf)` asks for.
 * newlib has no _setjmp at all: libc.a exports setjmp/longjmp only, and
 * <setjmp.h> declares no prototype for it either. Route the name to the
 * setjmp that does exist; it saves the signal mask as well, which on the
 * Switch is nothing worth preserving.
 *
 * The chain terminates instead of recursing. While `setjmp` is being expanded
 * it is blue-painted, so the `setjmp(g_ShimJumpBuf)` that _setjmp produces is
 * the real library call - the very same paint the longjmp macro beside it in
 * pilots_globals.h already relies on ("not self-referential: macros are
 * blue-painted").
 */
#ifdef __SWITCH__
#include <setjmp.h>
#define _setjmp(env) setjmp(env)
#endif

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
#define MM_MCINOTIFY    0x03B9  /* mmsyscom.h: MCI completion notify (video.c uses 0x3B9 literally) */

#define HTCLIENT 1
#define PM_REMOVE 1

/* messages port-main's host.c uses and the ones sdl_backend synthesizes */
#define WM_PARENTNOTIFY 0x0121
#define WM_NCHITTEST    0x0084
#define WM_USER         0x0400

/* window styles / ShowWindow commands (host.c: WS_CHILD for the MCI open,
   SW_SHOWNORMAL to reveal the main window before the intro) */
#define WS_CHILD        0x40000000u
#define WS_VISIBLE      0x10000000u
#define WS_POPUP        0x80000000u
#define WS_CAPTION      0x00C00000u
#define WS_SYSMENU      0x00080000u
#define WS_THICKFRAME   0x00040000u
#define WS_MINIMIZEBOX  0x00020000u
#define WS_MAXIMIZEBOX  0x00010000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_OVERLAPPED   0x00000000u
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | \
                             WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define SW_HIDE         0
#define SW_SHOWNORMAL   1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOW         5
#define SW_MINIMIZE     6
#define SW_RESTORE      9

/* MessageBox / dialog results and the standard arrow cursor */
#define MB_OK           0x00000000u
#define MB_ICONERROR    0x00000010u
#define MB_ICONHAND     0x00000010u
#define MB_ICONWARNING  0x00000030u
#define MB_ICONINFORMATION 0x00000040u
#define IDOK            1
#define IDCANCEL        2
#define IDC_ARROW       MAKEINTRESOURCEA(32512)

/* GetLogicalDrives/GetDriveTypeA values (host.c searches for AVI by drive type) */
#define DRIVE_UNKNOWN   0
#define DRIVE_NO_ROOT_DIR 1
#define DRIVE_REMOVABLE 2
#define DRIVE_FIXED     3
#define DRIVE_REMOTE    4
#define DRIVE_CDROM     5
#define DRIVE_RAMDISK   6

/* pack/unpack helpers (host.c reads coordinates out of lParam/wParam) */
#define LOWORD(l)  ((WORD)(((DWORD_PTR)(l)) & 0xFFFFu))
#define HIWORD(l)  ((WORD)((((DWORD_PTR)(l)) >> 16) & 0xFFFFu))
#define MAKELONG(a, b) ((LONG)(((WORD)(a)) | ((DWORD)((WORD)(b))) << 16))

/* GDI color helper (data.c initialises transition grid colors with RGB()) */
#define GetRValue(c) ((BYTE)((c) & 0xFFu))
#define GetGValue(c) ((BYTE)(((c) >> 8) & 0xFFu))
#define GetBValue(c) ((BYTE)(((c) >> 16) & 0xFFu))
#ifndef RGB
#define RGB(r, g, b) ((DWORD)(((BYTE)(r)) | ((DWORD)((BYTE)(g))) << 8 | \
                              ((DWORD)((BYTE)(b))) << 16))
#endif

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

/* real mmeapi.h/mmsyscom.h values (win32_shim.c's waveOut/midiOut shims) */
#define MMSYSERR_NOERROR 0
#define MMSYSERR_ERROR   1               /* generic failure; real mmsystem.h value */
#define WAVE_MAPPER      ((UINT)-1)
#define MIDI_MAPPER      ((UINT)-1)
#define CALLBACK_NULL    0x00000000u
#define CALLBACK_WINDOW  0x00010000u

typedef HANDLE HWAVEOUT;
typedef HANDLE HMIDIOUT;
typedef HANDLE HWAVEIN;

/* real mciapi.h values (MCIERR_BASE == 256) */
#define MCIERR_FILE_NOT_FOUND     275  /* MCIERR_BASE + 19 */
#define MCIERR_MISSING_PARAMETER  273  /* MCIERR_BASE + 17 */

/* real mciapi.h/Digitalv.h command values (host.c sends these through the real
   structs; mci_ffmpeg.c answers them on Linux) */
#define MCI_OPEN                  0x0803
#define MCI_CLOSE                 0x0804
#define MCI_PLAY                  0x0806
#define MCI_STOP                  0x0808
#define MCI_STATUS                0x0814
#define MCI_WINDOW                0x0841
#define MCI_NOTIFY                0x00000001u
#define MCI_WAIT                  0x00000002u
#define MCI_OPEN_ELEMENT          0x00000200u
#define MCI_STATUS_ITEM           0x00000100u
#define MCI_NOTIFY_SUCCESSFUL     0x0001
#define MCI_ANIM_OPEN_WS          0x00010000u
#define MCI_ANIM_OPEN_PARENT      0x00020000u
#define MCI_DGV_STATUS_HWND       0x00004001u

/* MCI parameter blocks: field order of the real mciapi.h structs (the shim
   implementations and mci_ffmpeg.c read them through these same types, so the
   layout is consistent everywhere it is used) */
typedef struct tagMCI_GENERIC_PARMS {
    DWORD_PTR dwCallback;
} MCI_GENERIC_PARMS, *PMCI_GENERIC_PARMS;

typedef struct tagMCI_OPEN_PARMSA {
    DWORD_PTR  dwCallback;
    MCIDEVICEID wDeviceID;
    LPCSTR     lpstrDeviceType;
    LPCSTR     lpstrElementName;
    LPCSTR     lpstrAlias;
} MCI_OPEN_PARMSA, *PMCI_OPEN_PARMSA;
typedef MCI_OPEN_PARMSA MCI_OPEN_PARMS;

typedef struct tagMCI_ANIM_OPEN_PARMSA {
    DWORD_PTR  dwCallback;
    MCIDEVICEID wDeviceID;
    LPCSTR     lpstrDeviceType;
    LPCSTR     lpstrElementName;
    LPCSTR     lpstrAlias;
    DWORD      dwStyle;
    HWND       hWndParent;
} MCI_ANIM_OPEN_PARMSA, *PMCI_ANIM_OPEN_PARMSA;
typedef MCI_ANIM_OPEN_PARMSA MCI_ANIM_OPEN_PARMS;

typedef struct tagMCI_PLAY_PARMS {
    DWORD_PTR dwCallback;
    DWORD     dwFrom;
    DWORD     dwTo;
} MCI_PLAY_PARMS, *PMCI_PLAY_PARMS;

typedef struct tagMCI_STATUS_PARMS {
    DWORD_PTR dwCallback;
    DWORD_PTR dwReturn;
    DWORD     dwItem;
    DWORD     dwTrack;
} MCI_STATUS_PARMS, *PMCI_STATUS_PARMS;

typedef struct tagMCI_WINDOW_PARMSA {
    DWORD_PTR dwCallback;
    HWND      hWnd;
    UINT      nCmdShow;
    DWORD_PTR dwExplicitWindow;
} MCI_WINDOW_PARMSA, *PMCI_WINDOW_PARMSA;
typedef MCI_WINDOW_PARMSA MCI_WINDOW_PARMS;

/* ---- wingdi.h ---- */
#pragma pack(push, 2)
typedef struct tagPALETTEENTRY { BYTE peRed; BYTE peGreen; BYTE peBlue; BYTE peFlags; } PALETTEENTRY, *LPPALETTEENTRY;
typedef struct tagLOGPALETTE {
    WORD palVersion;
    WORD palNumEntries;
    PALETTEENTRY palPalEntry[1];
} LOGPALETTE, *LPLOGPALETTE;
/* wingdi.h palette entry flags (host.c Host_SetPalette) */
#define PC_RESERVED  0x01
#define PC_EXPLICIT  0x02
#define PC_NOCOLLAPSE 0x04

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

/* wingdi.h values/structs the game's BMP path uses (host.c Host_LoadAviBackdrop) */
#define BI_RGB 0

#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
    WORD  bfType;
    DWORD bfSize;
    WORD  bfReserved1;
    WORD  bfReserved2;
    DWORD bfOffBits;
} BITMAPFILEHEADER, *LPBITMAPFILEHEADER;
#pragma pack(pop)

typedef struct tagRGBQUAD { BYTE rgbBlue; BYTE rgbGreen; BYTE rgbRed; BYTE rgbReserved; } RGBQUAD;

typedef struct tagBITMAPINFO {
    BITMAPINFOHEADER bmiHeader;
    RGBQUAD bmiColors[1];
} BITMAPINFO, *LPBITMAPINFO;
#pragma pack(pop)

/* GetDeviceCaps indices are passed as raw numbers by the game (video.c:38 == RASTERCAPS) */
#define RASTERCAPS 38
#define RC_PALETTE 0x1000   /* GetDeviceCaps(RASTERCAPS) bit: a palettized device */

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
LONG_PTR SetWindowLongPtrA(HWND, int, LONG_PTR);
LRESULT CallWindowProcA(WNDPROC, HWND, UINT, WPARAM, LPARAM);
int  GetCursorPos(LPPOINT);
int  SetCursorPos(int, int);
int  ScreenToClient(HWND, LPPOINT);
int  ClientToScreen(HWND, LPPOINT);
SHORT GetAsyncKeyState(int);
SHORT GetKeyState(int);
int  WinHelpA(HWND, LPCSTR, UINT, ULONG_PTR);
/* winhelp.h commands host.c uses (HELP_QUIT is the shutdown path) */
#define HELP_CONTENTS     0x0003
#define HELP_PARTIALKEY   0x0005
#define HELP_QUIT         0x0002
int  wsprintfA(LPSTR, LPCSTR, ...);
int  wvsprintfA(LPSTR, LPCSTR, va_list);

/* kernel32 */
DWORD GetModuleFileNameA(HINSTANCE, LPSTR, DWORD);
DWORD GetFullPathNameA(LPCSTR, DWORD, LPSTR, LPSTR *);
DWORD GetTempPathA(DWORD, LPSTR);
int   SetCurrentDirectoryA(LPCSTR);
DWORD GetVersion(void);
void  Sleep(DWORD);
/* winbase.h GlobalAlloc flags (host.c: GMEM_MOVEABLE for the arena, audio.c:
   GMEM_FIXED for the GMW exchange buffer) */
#define GMEM_FIXED      0x0000
#define GMEM_MOVEABLE   0x0002
#define GMEM_ZEROINIT   0x0040
#define GPTR            (GMEM_FIXED | GMEM_ZEROINIT)
HGLOBAL GlobalAlloc(UINT, size_t);
HGLOBAL GlobalFree(HGLOBAL);
HMODULE LoadLibraryA(LPCSTR);
void   *GetProcAddress(HMODULE, LPCSTR);
int    FreeLibrary(HMODULE);
int    CreateProcessA(LPCSTR, LPSTR, void *, void *, int, DWORD, void *, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
DWORD  WaitForSingleObject(HANDLE, DWORD);
/* winbase.h values for the two calls above */
#define NORMAL_PRIORITY_CLASS 0x00000080u
#define WAIT_FAILED          0xFFFFFFFFu
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
HPALETTE CreatePalette(const LOGPALETTE *);   /* wingdi.h: the argument is const */
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
MCIERROR mciSendStringA(LPCSTR, LPSTR, UINT, HWND);
MCIDEVICEID mciGetDeviceIDA(LPCSTR);
int      mciGetErrorStringA(MCIERROR, LPSTR, UINT);

/* waveOut over the SDL audio device (win32_shim.c drives SdlWave_* in
   sdl_backend.c; MM_WOM_DONE is posted to the CALLBACK_WINDOW window when a
   header's bytes have drained, so wave.c's WHDR_DONE polling works as on winmm) */
MMRESULT waveOutOpen(HWAVEOUT *, UINT, const WAVEFORMATEX *, DWORD_PTR, DWORD_PTR, DWORD);
MMRESULT waveOutClose(HWAVEOUT);
MMRESULT waveOutGetID(HWAVEOUT, LPUINT);
MMRESULT waveOutPrepareHeader(HWAVEOUT, LPWAVEHDR, UINT);
MMRESULT waveOutUnprepareHeader(HWAVEOUT, LPWAVEHDR, UINT);
MMRESULT waveOutWrite(HWAVEOUT, LPWAVEHDR, UINT);
MMRESULT waveOutReset(HWAVEOUT);
MMRESULT waveOutPause(HWAVEOUT);
MMRESULT waveOutRestart(HWAVEOUT);
MMRESULT waveOutGetVolume(HWAVEOUT, LPDWORD);
MMRESULT waveOutSetVolume(HWAVEOUT, DWORD);

/* midiOut: silent stubs for now (the libADLMIDI synth is not ported yet) */
MMRESULT midiOutOpen(HMIDIOUT *, UINT, DWORD_PTR, DWORD_PTR, DWORD);
MMRESULT midiOutClose(HMIDIOUT);
MMRESULT midiOutGetID(HMIDIOUT, LPUINT);
MMRESULT midiOutShortMsg(HMIDIOUT, DWORD);
MMRESULT midiOutReset(HMIDIOUT);
MMRESULT midiOutSetVolume(HMIDIOUT, DWORD);
MMRESULT midiOutGetVolume(HMIDIOUT, LPDWORD);

/* winmm sound */
int sndPlaySoundA(LPCSTR, UINT);

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
/* sys/stat.h permission bits under the _-prefixed names host.c uses */
#define _S_IREAD    0x0100
#define _S_IWRITE   0x0080
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
#define _itoa  itoa           /* text.c asks for the underscore name */

/* extra kernel32 bits used by video.c / fileio.c / graphics.c */
DWORD GetFileAttributesA(LPCSTR lpFileName);
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
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
int  WinShim_MsgQueueDepth(void);    /* posted messages waiting to be dispatched */
unsigned WinShim_MsgDispatched(void);/* messages handed to the game since the start */
HWND WinShimMainWindow(void);        /* the fake main-window handle SdlBackendCreateWindow returns */
void ShimPathToUnix(const char *src, char *dst, size_t cap); /* '\\'->'/' path copy (win32_shim.c) */

/* ---- sdl_backend.c helpers win32_shim.c / mci_ffmpeg.c call ---- */
void SdlBackendShowWindow(int show);      /* ShowWindow on the SDL window */
int  SdlBackendIsMinimized(void);         /* IsIconic */
DWORD SdlBackendGetWinStyle(void);        /* styles AdjustWindowRectEx derives metrics from */
void SdlBackendGetClientSize(int *w, int *h); /* window client size in px */
void SdlBackendSetCursorVisible(int visible);
void SdlBackendClientToGame(int *x, int *y); /* window client px -> game px (identity windowed) */
void SdlBackendGameToClient(int *x, int *y); /* game px -> window client px */
void SdlBackendNoteFramePoll(void);       /* main.c polled the cursor: the profile's frame counter */
void SdlBackendLoadBeat(unsigned int bytes); /* the shim's _read saw bytes; a still screen = loading */
void SdlBackendVideoFrame(const void *bgra, int srcw, int srch,
                          int x, int y, int w, int h); /* one MCI movie frame (mci_ffmpeg.c) */

/* ---- SDL wave device behind the waveOut shims (sdl_backend.c) ---- */
int  SdlWave_Open(void);
void SdlWave_Close(void);
int  SdlWave_Queue(const void *data, unsigned int len);
void SdlWave_Clear(void);
void SdlWave_Pause(int pause);
unsigned int SdlWave_Queued(void);
void WinShim_WavePump(void);          /* retire drained WAVEHDRs (win32_shim.c, from the pumps) */

/* ---- libADLMIDI synth behind the midiOut shims (sdl_backend.c): the music
       through the same seam the wave already uses - wave.c keeps its winmm
       protocol and the device under midiOut* is this. Non-Windows only:
       on Windows midiOut* is the real winmm. ---- */
int  SdlMidi_Open(void);
void SdlMidi_Close(void);
void SdlMidi_Reset(void);
void SdlMidi_ShortMsg(unsigned int packed); /* midiOutShortMsg word: status | data1<<8 | data2<<16 */
void SdlMidi_Pump(void);               /* render PCM on the timer tick (ShimTimerProc) */

/* ---- MCI video playback (mci_ffmpeg.c) ---- */
void WinShim_VideoPump(void);        /* decode+present+MM_MCINOTIFY; from PeekMessageA's SDL pump */
void WinShim_MciNotifyMove(HWND hWnd, int X, int Y, int nWidth, int nHeight); /* MCI child rect */

/* case-insensitive Win path -> existing Unix path (win32_shim.c) */
void WinShim_ResolvePath(const char *win_path, char *out, size_t cap);

#endif /* !_WIN32 */
#endif /* PILOTS_WIN32_SHIM_H */
