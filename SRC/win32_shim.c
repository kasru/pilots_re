/*
 * win32_shim.c - Linux implementation of the Win32/mmsystem/CRT surface declared in
 * win32_shim.h. On Windows this translation unit is empty (#ifndef _WIN32, except a
 * Win64-only setjmp side buffer at the bottom) and the
 * real SDK provides everything; the .bat builds still list the file for symmetry.
 *
 * Design notes:
 *  - paths: the game builds "\"-separated paths everywhere (g_ExecutablePath is
 *    literally ".\\PILOTS.EXE"); everything below translates '\' -> '/', everything
 *    else keeps Win semantics so GetFullPathNameA's strrchr('\\') logic in
 *    fileio.c/video.c keeps working byte-for-byte.
 *  - case: ONE rule for every file the game opens - the lookup is case-insensitive,
 *    because that is what the original ran on (Win9x ISO9660/FAT) and the engine
 *    derives the names from its own data section, not from what is on disk. The
 *    data asks for ".\\movies\\intro.AVI" (vidBase + lowercase name from the script,
 *    + aAvi/aAvi-style uppercase extension) while the CD ships "MOVIES\\INTRO.AVI";
 *    on Windows that mismatch is invisible, on a case-sensitive FS it is fatal. So
 *    WinShim_ResolvePath()/ShimOpenWin() walk the path component by component
 *    against the real directory and every entry point funnels through them:
 *      _open()      -> ShimOpenWin()          fileio.c data + save files, video.c BMP
 *      CreateFileA()-> ShimOpenWin()
 *      GetFileAttributesA()                   video.c AVI/BMP existence probes
 *      remove()     -> WinShim_ResolvePath()  save deletion
 *      mci_ffmpeg.c -> WinShim_ResolvePath()  avformat_open_input
 *    Adding a new file open? Route it through the same two functions, or the
 *    original resources stop working the moment they are copied verbatim.
 *  - messages: there is no Win32 queue here. sdl_backend.c translates SDL events and
 *    posts WM_* messages via PostMessageA(); PeekMessageA() drains the queue (pumping
 *    SDL when empty) and DispatchMessageA() calls the game's Host_MainWndProc directly -
 *    the same call sequence the original had, just without a real message loop.
 *  - MCI video playback lives in mci_ffmpeg.c (ffmpeg-backed mciSendCommandA).
 */
#include "gm.h"

#ifndef _WIN32

/* SDL for the timer/mouse-state/mod-state answers below and for the audio
   device the waveOut shims drive. SDL_MAIN_HANDLED: main() in this file is the
   real entry point, SDL must not rename it to SDL_main. */
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <unistd.h>  /* getcwd() for the open-path log line below */
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef remove
#undef remove          /* use the real glibc remove() inside WinShim_remove */
#endif
/* <stdio.h> was included while the remove macro was active, so its declaration
   got renamed to WinShim_remove - redeclare glibc's remove for this TU. */
extern int remove(const char *path);

#include <dirent.h>   /* case-insensitive path walk below (Win9x semantics) */
#ifndef __SWITCH__
#include <sys/mman.h> /* low memory pool below (GlobalAlloc), see the note there */
#endif

/* Side setjmp buffer: pilots_globals.h routes setjmp/longjmp here (the
   generated 64-byte g_JumpBuffer field is too small for glibc's jmp_buf). */
jmp_buf g_ShimJumpBuf;

/* ===================================================================== */
/* paths                                                                 */
/* ===================================================================== */

void ShimPathToUnix(const char *src, char *dst, size_t cap)
{
  size_t i;
  for ( i = 0; i + 1 < cap && src[i]; ++i )
    dst[i] = (src[i] == '\\') ? '/' : src[i];
  dst[i] = '\0';
}

/* Win32 file lookup is case-insensitive (the original data comes from a Win9x
   CD: the game asks for ".\movies\intro.AVI" whatever the real case is) while
   a Linux FS is not. Build a case-resolved copy of the path component by
   component; falls back to appending the raw component when nothing matches.
   Returns the resolved path in out (not guaranteed to exist - callers stat it). */
static int ShimResolveUnixCI(const char *win_path, char *out, size_t cap)
{
  char norm[MAX_PATH * 2];
  char *seg;
  size_t rlen = 0;

  ShimPathToUnix(win_path, norm, sizeof norm);
  out[0] = '\0';
  seg = norm;
  if ( *seg == '/' )
  {
    if ( cap < 2 )
      return 0;
    out[rlen++] = '/';
    out[rlen] = '\0';
    ++seg;
  }
  while ( *seg )
  {
    char *slash = strchr(seg, '/');
    size_t seglen = slash ? (size_t)(slash - seg) : strlen(seg);
    char saved = seg[seglen];
    int matched = 0;
    DIR *d;
    struct dirent *e;

    seg[seglen] = '\0';
    d = opendir(rlen ? out : ".");
    if ( d )
    {
      while ( (e = readdir(d)) != NULL )
      {
        if ( strcasecmp(e->d_name, seg) == 0 )
        {
          size_t el = strlen(e->d_name);
          if ( rlen + el + 2 >= cap )
            break;
          memcpy(out + rlen, e->d_name, el);
          rlen += el;
          out[rlen] = '\0';
          matched = 1;
          break;
        }
      }
      closedir(d);
    }
    if ( !matched )
    {
      if ( rlen + seglen + 2 >= cap )
      {
        seg[seglen] = saved;
        return 0;
      }
      memcpy(out + rlen, seg, seglen);
      rlen += seglen;
      out[rlen] = '\0';
    }
    seg[seglen] = saved;
    if ( !slash )
      break;
    if ( rlen + 2 >= cap )
      return 0;
    out[rlen++] = '/';
    out[rlen] = '\0';
    seg = slash + 1;
  }
  return 1;
}

/* Resolve a Win path to an existing Unix path (exact case first, then the
   case-insensitive walk); plain '\'->'/' conversion when nothing exists (a
   create-target). Exported for mci_ffmpeg.c (avformat wants a real path). */
void WinShim_ResolvePath(const char *win_path, char *out, size_t cap)
{
  char ci[MAX_PATH * 2];
  struct stat st;

  ShimPathToUnix(win_path, out, cap);
  if ( stat(out, &st) == 0 )
    return;
  if ( ShimResolveUnixCI(win_path, ci, sizeof ci) && stat(ci, &st) == 0 )
  {
    size_t i;
    for ( i = 0; i + 1 < cap && ci[i]; ++i )
      out[i] = ci[i];
    out[i] = '\0';
  }
}

/* open() with the same case-insensitive lookup; missing files keep the plain
   path so create flags (O_CREAT/O_EXCL) behave like Win9x on the raw name. */
static int ShimOpenWin(const char *win_path, int of, int mode)
{
  char up[MAX_PATH * 2];
  char ci[MAX_PATH * 2];
  struct stat st;
  int fd;

  ShimPathToUnix(win_path, up, sizeof up);
  if ( stat(up, &st) == 0 )
    fd = open(up, of, mode);
  else if ( ShimResolveUnixCI(win_path, ci, sizeof ci) && stat(ci, &st) == 0 )
    fd = open(ci, of, mode);
  else
    fd = open(up, of, mode);
  /* Где лежит реально открытый файл — та же диагностика, что v0.1 вела в
     Game_OpenFile: именно по этой строке tests/linux/datapath.sh видит, что
     файл данных ушёл за -datapath, а не остался в exe-каталоге. Относительный
     запрос (".\PILOTS.EXE" после -datapath) для лога дополняется до CWD —
     но не через realpath: тот развернул бы симлинк и показал путь, которым
     движок НЕ спрашивал файл (в логе обязан стоять его собственный запрос). */
#ifdef _DEBUG
  {
    char abs[MAX_PATH * 2];
    char cwd[MAX_PATH];

    WinShim_ResolvePath(win_path, abs, sizeof abs);
    if (abs[0] != '/' && getcwd(cwd, sizeof cwd) != NULL)
    {
      const char *rel = (abs[0] == '.' && abs[1] == '/') ? abs + 2 : abs;
      size_t cl = strlen(cwd);
      if (cl + 1 + strlen(rel) + 1 <= sizeof abs)
      {
        cwd[cl] = '/';
        strcpy(cwd + cl + 1, rel);
        strcpy(abs, cwd);
      }
    }
    DBG_LOG("ShimOpenWin: open '%s' -> fullPath='%s' fd=%d", win_path, abs, fd);
  }
#endif
  return fd;
}

/* in-place '/' -> '\' (GetModuleFileNameA result for debug_log path handling) */
static void ShimPathToWin(char *s)
{
  for ( ; *s; ++s)
    if ( *s == '/' )
      *s = '\\';
}

/* SetCurrentDirectoryA - used by the -datapath command line switch to point the
   whole game at another resource directory. Resolved the same way every file
   open is (case-insensitive like Win9x), so -datapath ./PILOTS on a
   case-sensitive filesystem still lands on ./pilots. */
int SetCurrentDirectoryA(LPCSTR lpPathName)
{
  char unixPath[MAX_PATH * 2];

  if ( !lpPathName || !*lpPathName )
    return 0;
  WinShim_ResolvePath(lpPathName, unixPath, sizeof unixPath);
  return chdir(unixPath) == 0;
}

DWORD GetFullPathNameA(LPCSTR lpFileName, DWORD nBufferLength, LPSTR lpBuffer, LPSTR *lpFilePart)
{
  char unixPath[MAX_PATH * 2];
  char absPath[MAX_PATH * 2];
  char norm[MAX_PATH * 2];
  char *o;
  const char *p;
  size_t len;

  if ( !lpFileName || !lpBuffer || !nBufferLength )
    return 0;
  ShimPathToUnix(lpFileName, unixPath, sizeof unixPath);
  if ( unixPath[0] != '/' )
  {
    char cwd[MAX_PATH];
    if ( !getcwd(cwd, sizeof cwd) )
      return 0;
    snprintf(absPath, sizeof absPath, "%s/%s", cwd, unixPath);
  }
  else
  {
    snprintf(absPath, sizeof absPath, "%s", unixPath);
  }

  /* normalize: drop "." components, resolve ".." */
  p = absPath;
  o = norm;
  if ( *p == '/' )
  {
    *o++ = '/';
    while ( *p == '/' )
      ++p;
  }
  while ( *p )
  {
    const char *seg = p;
    size_t segLen;
    while ( *p && *p != '/' )
      ++p;
    segLen = (size_t)(p - seg);
    while ( *p == '/' )
      ++p;
    if ( segLen == 1 && seg[0] == '.' )
      continue;
    if ( segLen == 2 && seg[0] == '.' && seg[1] == '.' )
    {
      /* pop the last component (stay at root for leading '/') */
      if ( o > norm + (norm[0] == '/' ? 1 : 0) )
      {
        --o;
        while ( o > norm && o[-1] != '/' )
          --o;
        if ( o > norm && o[-1] == '/' && norm[0] != '/' )
          --o;
      }
      continue;
    }
    if ( o > norm && o[-1] != '/' )
      *o++ = '/';
    if ( o + segLen >= norm + sizeof norm )
      return 0;
    memcpy(o, seg, segLen);
    o += segLen;
  }
  *o = '\0';
  if ( !norm[0] )
  {
    norm[0] = '/';
    norm[1] = '\0';
  }

  ShimPathToWin(norm);
  len = strlen(norm);
  if ( len + 1 > nBufferLength )
    return (DWORD)(len + 1);         /* Windows: required size incl. NUL, nothing written */
  memcpy(lpBuffer, norm, len + 1);
  if ( lpFilePart )
  {
    char *slash = strrchr(lpBuffer, '\\');
    *lpFilePart = slash ? slash + 1 : lpBuffer;
  }
  return (DWORD)len;
}

DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer)
{
  const char *tmp = getenv("TMPDIR");
  char buf[MAX_PATH];
  size_t len;

  if ( !tmp || !tmp[0] )
    tmp = "/tmp";
  /* no trailing slash: BuildAlternateSavePath formats "%s\\%s" itself */
  snprintf(buf, sizeof buf, "%s", tmp);
  len = strlen(buf);
  if ( !lpBuffer || nBufferLength == 0 )
    return (DWORD)len;
  if ( len + 1 > nBufferLength )
    return (DWORD)(len + 1);
  memcpy(lpBuffer, buf, len + 1);
  return (DWORD)len;
}

DWORD GetModuleFileNameA(HINSTANCE hModule, LPSTR lpFilename, DWORD nSize)
{
  char exePath[MAX_PATH];
  ssize_t n;
  size_t len;

  (void)hModule;
  if ( !lpFilename || !nSize )
    return 0;
  n = readlink("/proc/self/exe", exePath, sizeof exePath - 1);
  if ( n <= 0 )
  {
    /* extremely unlikely; keep something sane so DbgLogInit still works */
    snprintf(lpFilename, nSize, "PILOT_RV.EXE");
    return (DWORD)strlen(lpFilename);
  }
  exePath[n] = '\0';
  ShimPathToWin(exePath);
  /* Windows module paths always carry ".EXE" and the game relies on it:
     CreateHelpFilePath does *strrchr(path, '.') = 0 unconditionally (main.c:37)
     and DbgLogInit strips the extension before appending "_debug.log". The
     Linux binary has no extension, so append a virtual one. */
  len = strlen(exePath);
  if ( len + 4 < sizeof exePath )
  {
    memcpy(exePath + len, ".EXE", 5);
    len += 4;
  }
  if ( len >= nSize )
  {
    memcpy(lpFilename, exePath, nSize - 1);
    lpFilename[nSize - 1] = '\0';
    return nSize;
  }
  memcpy(lpFilename, exePath, len + 1);
  return (DWORD)len;
}

DWORD GetVersion(void)
{
  /* Positive value with low byte >= 4: audio.c takes the InitDirectDraw_Win95 (SDL)
     branch and video.c's isWin3x stays 0. Was: real Windows GetVersion(). */
  return 0x0000040Au;
}

void Sleep(DWORD ms)
{
  SDL_Delay(ms);
}

/* ===================================================================== */
/* CRT bits the game uses directly                                       */
/* ===================================================================== */

char *strupr(char *s)
{
  char *p;
  for ( p = s; p && *p; ++p )
    *p = (char)toupper((unsigned char)*p);
  return s;
}

int _open(const char *path, int flags, ...)
{
  int of = 0;
  int mode = 0666;

  if ( flags & _O_CREAT )
  {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, int) & 0777;
    va_end(ap);
  }
  /* MSVC _O_* low bits match POSIX O_RDONLY/O_WRONLY/O_RDWR */
  of = flags & 3;
  if ( flags & _O_CREAT )  of |= O_CREAT;
  if ( flags & _O_EXCL )   of |= O_EXCL;
  if ( flags & _O_TRUNC )  of |= O_TRUNC;
  if ( flags & _O_APPEND ) of |= O_APPEND;
  return ShimOpenWin(path, of, mode);      /* case-insensitive lookup like Win9x */
}

int _close(int fd)
{
  return close(fd);
}

int _read(int fd, void *buf, unsigned int count)
{
  int n = (int)read(fd, buf, count);

  /* A loading screen the player can see has to be driven from here: a scene loads inside the
     game's own loop, where nothing the port owns is called except these reads, and the screen
     stands still the whole time (see SdlBackendLoadBeat). */
  if ( n > 0 )
    SdlBackendLoadBeat((unsigned int)n);
  return n;
}

int _write(int fd, const void *buf, unsigned int count)
{
  return (int)write(fd, buf, count);
}

long _lseek(int fd, long ofs, int whence)
{
  return (long)lseek(fd, ofs, whence);
}

int _chsize(int fd, long size)
{
  return ftruncate(fd, size);
}

/* Delete with the very same case-insensitive lookup every other entry point
   uses: a save written under one case has to be removable under the other
   (BuildSaveFilePath strupr()s the base path, the file on disk may not be). */
int WinShim_remove(const char *path)
{
  char up[MAX_PATH * 2];
  WinShim_ResolvePath(path, up, sizeof up);
  return remove(up);
}

/* msvcrt-compatible itoa: digits reversed into a temp, '-' only for radix 10,
   non-10 radices treat value as unsigned (so -1 @16 -> "ffffffff"). */
char *itoa(int value, char *buffer, int radix)
{
  static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
  char tmp[34];                 /* 32 bits + sign + NUL */
  unsigned int u;
  int i = 0, j = 0, neg = 0;

  if ( radix < 2 || radix > 36 )
  {
    buffer[0] = '\0';
    return buffer;
  }
  if ( value < 0 && radix == 10 )
  {
    neg = 1;
    u = (unsigned int)(-(value + 1)) + 1u;  /* INT_MIN safe */
  }
  else
  {
    u = (unsigned int)value;
  }
  do
  {
    tmp[i++] = digits[u % (unsigned)radix];
    u /= (unsigned)radix;
  } while ( u );
  if ( neg )
    tmp[i++] = '-';
  while ( i > 0 )
    buffer[j++] = tmp[--i];
  buffer[j] = '\0';
  return buffer;
}

int wsprintfA(LPSTR out, LPCSTR fmt, ...)
{
  va_list ap;
  int n;

  /* Windows wsprintfA is a 1024-char printf; same overflow profile as the original */
  va_start(ap, fmt);
  n = vsnprintf(out, 1024, fmt, ap);
  va_end(ap);
  return n < 0 ? 0 : n;
}

int wvsprintfA(LPSTR out, LPCSTR fmt, va_list ap)
{
  int n = vsnprintf(out, 1024, fmt, ap);
  return n < 0 ? 0 : n;
}

int lstrlenA(LPCSTR s)
{
  return s ? (int)strlen(s) : 0;
}

int WideCharToMultiByte(UINT codePage, DWORD flags, const WCHAR *src, int srcLen,
                        char *dst, int dstLen, const char *defaultChar, int *usedDefault)
{
  unsigned int u;
  char c;

  (void)codePage; (void)flags; (void)defaultChar;
  if ( usedDefault )
    *usedDefault = 0;
  if ( !src || srcLen < 1 || !dst || dstLen < 1 )
    return 0;
  u = src[0];
  c = '?';
  if ( u < 0x80 )
    c = (char)u;
  else if ( u == 0x0401 )                 /* Ё */
    c = (char)0xA8;
  else if ( u == 0x0451 )                 /* ё */
    c = (char)0xB8;
  else if ( u >= 0x0410 && u <= 0x044F )  /* А..я -> cp1251 0xC0..0xFF */
    c = (char)(0xC0 + (u - 0x0410));
  dst[0] = c;
  return 1;
}

/* ===================================================================== */
/* file attrs / drives (video.c AVI search)                              */
/* ===================================================================== */

DWORD GetFileAttributesA(LPCSTR lpFileName)
{
  char up[MAX_PATH * 2];
  struct stat st;

  if ( !lpFileName )
    return 0xFFFFFFFFu;
  WinShim_ResolvePath(lpFileName, up, sizeof up); /* case-insensitive like Win9x */
  if ( stat(up, &st) != 0 )
    return 0xFFFFFFFFu;
  return 0x80;                             /* FILE_ATTRIBUTE_NORMAL */
}

DWORD GetLogicalDrives(void)
{
  return 0;                                /* no drive letters; the loop in
                                              SearchVideoFileOnDrives exits cleanly */
}

UINT GetDriveTypeA(LPCSTR lpRootPathName)
{
  (void)lpRootPathName;
  return 3;                                /* DRIVE_FIXED */
}

/* ===================================================================== */
/* process / library stubs                                               */
/* ===================================================================== */

int CreateProcessA(LPCSTR lpApplicationName, LPSTR lpCommandLine, void *procAttrs,
                   void *threadAttrs, int bInheritHandles, DWORD dwCreationFlags,
                   void *env, LPCSTR lpCurrentDirectory, LPSTARTUPINFOA lpStartupInfo,
                   LPPROCESS_INFORMATION lpProcessInformation)
{
  (void)lpApplicationName; (void)procAttrs; (void)threadAttrs;
  (void)bInheritHandles; (void)dwCreationFlags; (void)env;
  (void)lpCurrentDirectory; (void)lpStartupInfo; (void)lpProcessInformation;
  DBG_LOG("CreateProcessA: stubbed (cmd='%s')", lpCommandLine ? lpCommandLine : "");
  return 0;                                /* launch fails; game shows its own error path */
}

DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds)
{
  (void)hHandle; (void)dwMilliseconds;
  return 0;
}

HMODULE LoadLibraryA(LPCSTR lpLibFileName)
{
  (void)lpLibFileName;
  return 0;                                /* GMW.DLL path is dead (GetVersion forces SDL) */
}

void *GetProcAddress(HMODULE hModule, LPCSTR lpProcName)
{
  (void)hModule; (void)lpProcName;
  return 0;
}

int FreeLibrary(HMODULE hModule)
{
  (void)hModule;
  return 0;
}

/* ===================================================================== */
/* memory                                                                */
/* ===================================================================== */

/* The game hands pointers that live in its memory pool through 32-bit fields and
   `int` parameters everywhere: (int)g_TemplateDataBuffer is what every opcode
   receives as its payload pointer, dword_4115C8 keeps the opcode buffer, and the
   opcode handlers read their data back out with (INT_PTR) on a 32-bit int. On x86
   that round trip is lossless because the entire heap sits below 4 GB. On LP64 a
   plain malloc block lands at 0x5555.../0x7fff..., the truncated low half gets
   sign-extended into a pointer again and the very first opcode that touches its
   data dies on 0xFFFFFFFF........ - which is what the 64-bit build did at
   gameloop.c:338 the moment it hit opcode 0x92.

   So every GlobalAlloc block goes into the first 2 GB instead: the truncation the
   decompiled code performs stays lossless and sign-extends back to the same
   address, which is byte-identical on x86 and avoids rewriting ~240 (INT_PTR)
   call sites - the memory pool is a GlobalAlloc block (InitializeVideoSystem).

   MAP_32BIT is Linux/x86 only; elsewhere the second path asks for a low
   address and rejects anything the kernel put higher. The Switch is a third
   case: newlib has no mmap at all, and the image whose .bss it could take the
   pool from is itself mapped above 2 GB by the loader - see the __SWITCH__
   branch of Shim_AllocLow, where that was measured on hardware and what it
   leaves the port to find out.

   munmap needs the mapping length and GlobalFree is given no size, so each block
   carries it in a 16-byte header (keeps the payload 16-byte aligned, like malloc). */

#define SHIM_LOWMEM_HDR 16u

#ifdef __SWITCH__
/*
 * No <sys/mman.h> on newlib, so neither mmap path below exists here. What the Switch
 * does instead has been measured at every step, because the first two guesses were
 * both wrong:
 *
 *  - THE IMAGE IS NOT LOW. hbloader maps the .nro high: &dword_4161F0 came back as
 *    0x78c3fa888 in one run and the code as 0000000A64056000 in another (it
 *    randomises). So the pool cannot come out of .bss and still satisfy the round
 *    trip.
 *  - NEITHER IS THE HEAP. The heap is a slice of hbloader's own (its forwarder calls
 *    svcSetHeapSize, and the NRO is handed the result as libnx's "heap override",
 *    runtime/init.c: envHasHeapOverride), and the run says where it landed: "the
 *    malloc heap sits at 0000000CDD4DC000 + 3127 MB". malloc was tried here, it
 *    answered, and the answer is that line - so it is not asked again.
 *
 * What is left is to ask for a mapping of one's own, and the console does allow it:
 * SdlSwitchMapLowMemory (switch_libnx.c) turns a block into a transfer-memory object
 * and maps it at an address the caller names - which is how libnx maps its own IPC
 * buffers, so it is not an exotic route. The pool is built in that alias, so the
 * pointers the game stores in an int name the right bytes. The .bss pool stays as the
 * fallback: if the kernel ever refuses, the game still runs and the log says out loud
 * that the pointers are no good, which beats refusing to start.
 *
 * SIZE: InitVidSys asks for poolSizeBytes - frameDataSize - 80000 and the shipped
 * resource has poolSizeBytes = 10485760, i.e. 10098560 bytes (logged as "InitVidSys:
 * field_0=10098560 size=307200"), plus audio.c's 0xF000. 16 MB covers both. Note the
 * bump reserves bytes + HDR: GlobalAlloc writes its 16-byte header *before* the
 * payload it returns, so a block that only reserved `bytes` would have the next
 * block's header land on its last 16 bytes.
 */
#define SHIM_LOWMEM_POOL (16u * 1024u * 1024u)

static unsigned char s_SwitchLowMemPool[SHIM_LOWMEM_POOL] __attribute__((aligned(16)));
static unsigned char *s_LowPool;              /* the low alias, or .bss if there is no alias */
static size_t s_LowPoolSize;
static size_t s_LowPoolUsed;
static int s_LowMemTooHighReported;

static int Shim_RoundTripOk(void *p)
{
  return (INT_PTR)p == (INT_PTR)(int)(INT_PTR)p;
}

static void *Shim_AllocLow(size_t bytes)
{
  /* Rounded to 16 like malloc: the bump hand-out is not the first block only, and the header
     promise at SHIM_LOWMEM_HDR ("keeps the payload 16-byte aligned") has to hold for the
     NEXT payload as well - GlobalAlloc stores a size_t right at the start of every block. */
  size_t total = (bytes + SHIM_LOWMEM_HDR + 15u) & ~(size_t)15u;
  unsigned char *p;

  if ( !s_LowPool )
  {
    void *low = SdlSwitchMapLowMemory(SHIM_LOWMEM_POOL);

    if ( low && Shim_RoundTripOk(low) )
      s_LowPool = (unsigned char *)low;
    else
      s_LowPool = s_SwitchLowMemPool;
    s_LowPoolSize = SHIM_LOWMEM_POOL;
    DBG_LOG("Shim_AllocLow: building the pool in %s",
            s_LowPool == s_SwitchLowMemPool ? "the image's .bss" : "a mapping below 2 GB");
  }

  if ( total > s_LowPoolSize - s_LowPoolUsed )
  {
    DBG_LOG("Shim_AllocLow: %d bytes will not fit in the pool, %d of %d used",
            (int)bytes, (int)s_LowPoolUsed, (int)s_LowPoolSize);
    return 0;
  }
  p = s_LowPool + s_LowPoolUsed;
  s_LowPoolUsed += total;
  if ( !Shim_RoundTripOk(p) && !s_LowMemTooHighReported )
  {
    /* Once per run: it is a property of where the memory is, not of this block, and
       repeating it per block hides everything else. */
    s_LowMemTooHighReported = 1;
    DBG_LOG("Shim_AllocLow: this pool is NOT below 2 GB - a pointer the game keeps in an int will not survive it");
  }
  DBG_LOG("Shim_AllocLow: %d bytes -> 0x%08X%08X (%s 2 GB)", (int)bytes,
          (unsigned int)((unsigned long long)(UINT_PTR)p >> 32),
          (unsigned int)(unsigned long long)(UINT_PTR)p,
          Shim_RoundTripOk(p) ? "BELOW" : "ABOVE");
  return p;
}
#else
static void *Shim_AllocLow(size_t bytes)
{
  size_t total = bytes + SHIM_LOWMEM_HDR;
  void *p;

#ifdef MAP_32BIT
  p = mmap(0, total, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
  if ( p != MAP_FAILED )
  {
    /* The flag is meaningful on x86_64 only: the i386 kernel ignores it and
       answers with its usual high mapping (measured: 0xEC25D010 for a 10 MB
       request) - so the height is checked on this path too, not just trusted. */
    if ( (UINT_PTR)p <= 0x7FFFFFFFu )
      return p;
    munmap(p, total);
  }
#endif
  /* No MAP_32BIT: hint at a low address (not MAP_FIXED - if something already
     owns it the kernel may pick another spot, and the check rejects that).
     Measured on i386: the hint is honored exactly (0x10000000 back). */
  p = mmap((void *)(UINT_PTR)0x10000000, total, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if ( p == MAP_FAILED )
    return 0;
  if ( (UINT_PTR)p > 0x7FFFFFFFu )
  {
    munmap(p, total);                     /* too high: the int round trip would not survive it */
    return 0;
  }
  return p;
}
#endif /* !__SWITCH__ */

HGLOBAL GlobalAlloc(UINT uFlags, size_t dwBytes)
{
  unsigned char *p;

  (void)uFlags;
  if ( !dwBytes )
    dwBytes = 1;
  p = (unsigned char *)Shim_AllocLow(dwBytes);
  if ( !p )
    return 0;
  *(size_t *)p = dwBytes + SHIM_LOWMEM_HDR;
  return p + SHIM_LOWMEM_HDR;
}

HGLOBAL GlobalFree(HGLOBAL hMem)
{
#ifdef __SWITCH__
  (void)hMem;          /* bump pool, low alias or .bss: neither has anything to give back */
  return 0;            /* and both call sites are shutdown paths */
#else
  unsigned char *p;

  if ( !hMem )
    return 0;
  p = (unsigned char *)hMem - SHIM_LOWMEM_HDR;
  munmap(p, *(size_t *)p);
  return 0;
#endif
}

HGLOBAL GlobalLock(HGLOBAL hMem)
{
  return hMem;                             /* low block: handle == payload (payload keeps 16-byte alignment) */
}

int GlobalUnlock(HGLOBAL hMem)
{
  (void)hMem;
  return 1;
}

/* ===================================================================== */
/* critical sections: recursive pthread mutex (Win CRITICAL_SECTION is    */
/* recursive too)                                                        */
/* ===================================================================== */

static void ShimInitCS(LPCRITICAL_SECTION cs)
{
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&cs->mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  cs->inited = 1;
}

void InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
  ShimInitCS(cs);
}

void EnterCriticalSection(LPCRITICAL_SECTION cs)
{
  if ( !cs->inited )
    ShimInitCS(cs);
  pthread_mutex_lock(&cs->mutex);
}

void LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
  if ( cs->inited )
    pthread_mutex_unlock(&cs->mutex);
}

void DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
  if ( cs->inited )
  {
    pthread_mutex_destroy(&cs->mutex);
    cs->inited = 0;
  }
}

/* ===================================================================== */
/* CreateFileA family - only the debug log uses file handles             */
/* ===================================================================== */

#define SHIM_NUM_HANDLES 32
static int s_HandleFds[SHIM_NUM_HANDLES];  /* 0 = free; handle value = index + 1 */

static int ShimHandleToFd(HANDLE h)
{
  intptr_t idx = (intptr_t)h - 1;
  if ( idx < 0 || idx >= SHIM_NUM_HANDLES || !s_HandleFds[idx] )
    return -1;
  return s_HandleFds[idx];
}

HANDLE CreateFileA(LPCSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   void *lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile)
{
  int of = 0;
  int fd;
  int i;

  (void)dwShareMode; (void)lpSecurityAttributes; (void)dwFlagsAndAttributes;
  (void)hTemplateFile;
  if ( !lpFileName )
    return INVALID_HANDLE_VALUE;
  if ( dwDesiredAccess & GENERIC_WRITE )
    of = (dwDesiredAccess & GENERIC_READ) ? O_RDWR : O_WRONLY;
  else
    of = O_RDONLY;
  if ( dwCreationDisposition == CREATE_ALWAYS )
    of |= O_CREAT | O_TRUNC;
  else if ( dwCreationDisposition == CREATE_NEW )
    of |= O_CREAT | O_EXCL;
  fd = ShimOpenWin(lpFileName, of, 0666); /* case-insensitive lookup like Win9x */
  if ( fd < 0 )
    return INVALID_HANDLE_VALUE;
  for ( i = 0; i < SHIM_NUM_HANDLES; ++i )
    if ( !s_HandleFds[i] )
    {
      s_HandleFds[i] = fd;
      return (HANDLE)(intptr_t)(i + 1);
    }
  close(fd);
  return INVALID_HANDLE_VALUE;
}

int WriteFile(HANDLE hFile, const void *lpBuffer, DWORD nNumberOfBytesToWrite,
              LPDWORD lpNumberOfBytesWritten, void *lpOverlapped)
{
  int fd = ShimHandleToFd(hFile);
  ssize_t n;

  (void)lpOverlapped;
  if ( fd < 0 )
    return 0;
  n = write(fd, lpBuffer, nNumberOfBytesToWrite);
  if ( lpNumberOfBytesWritten )
    *lpNumberOfBytesWritten = (DWORD)(n < 0 ? 0 : n);
  return n >= 0;
}

int ReadFile(HANDLE hFile, void *lpBuffer, DWORD nNumberOfBytesToRead,
             LPDWORD lpNumberOfBytesRead, void *lpOverlapped)
{
  int fd = ShimHandleToFd(hFile);
  ssize_t n;

  (void)lpOverlapped;
  if ( fd < 0 )
    return 0;
  n = read(fd, lpBuffer, nNumberOfBytesToRead);
  if ( lpNumberOfBytesRead )
    *lpNumberOfBytesRead = (DWORD)(n < 0 ? 0 : n);
  return n >= 0;
}

int FlushFileBuffers(HANDLE hFile)
{
  /* no fsync(): DbgPrintf flushes after every line; write() is already
     visible to readers. A real fsync per log line would stall the game. */
  return ShimHandleToFd(hFile) >= 0;
}

int CloseHandle(HANDLE hObject)
{
  int fd = ShimHandleToFd(hObject);
  intptr_t idx = (intptr_t)hObject - 1;

  if ( fd < 0 )
    return 0;
  close(fd);
  if ( idx >= 0 && idx < SHIM_NUM_HANDLES )
    s_HandleFds[idx] = 0;
  return 1;
}

void OutputDebugStringA(LPCSTR lpOutputString)
{
  if ( lpOutputString )
  {
    fputs(lpOutputString, stderr);
    fflush(stderr);
  }
}

/* ===================================================================== */
/* user32: message box / metrics / window management                     */
/* ===================================================================== */

int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
{
  (void)hWnd; (void)uType;
  fprintf(stderr, "[MessageBox] %s: %s\n", lpCaption ? lpCaption : "", lpText ? lpText : "");
  fflush(stderr);
  DBG_LOG("MessageBox: [%s] %s", lpCaption ? lpCaption : "", lpText ? lpText : "");
  return 1;                                /* IDOK */
}

int MessageBeep(UINT uType)
{
  (void)uType;
  return 1;
}

int GetSystemMetrics(int nIndex)
{
  switch ( nIndex )
  {
    case 0:                                 /* SM_CXSCREEN */
    case 1:                                 /* SM_CYSCREEN */
      {
        SDL_DisplayMode dm;
        if ( SDL_GetDesktopDisplayMode(0, &dm) == 0 )
          return nIndex == 0 ? dm.w : dm.h;
        return nIndex == 0 ? 1024 : 768;   /* dummy driver fallback */
      }
    case 4:  return 19;                     /* SM_CYCAPTION */
    case 5:  return 4;                      /* SM_CXBORDER */
    case 6:  return 4;                      /* SM_CYBORDER */
    case 32: return 4;                      /* SM_CXSIZEFRAME */
    case 33: return 4;                      /* SM_CYSIZEFRAME */
    default: return 0;
  }
}

/* ---- the posted-message queue ---- */

#define SHIM_MSG_CAPACITY 256
static MSG s_MsgQueue[SHIM_MSG_CAPACITY];
static int s_MsgHead;
static int s_MsgCount;
static unsigned s_MsgDispatched;               /* messages handed to the game, since the start */
static int s_QuitPosted;
static int s_QuitDelivered;
static int s_PaintPending;
static int s_InDispatch;
static int s_InPump;
static int s_HaveMousePos;                   /* a WM_MOUSEMOVE has been posted at least once */
static int s_MouseGameX, s_MouseGameY;       /* its position, in the game's own coords */

int PostMessageA(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  MSG *m;
  int x, y;
  int i;

  /* What Windows does with a mouse move that has not been picked up yet: it does not queue a
     second one, it merges. Real Win32 states it outright (a WM_MOUSEMOVE is discarded when another
     one is already in the queue), and SDL's own pump does the same with SDL_MOUSEMOTION. Without
     this, a device that reports often - the console's analog stick, which posts a move every poll
     it is held aside - fills the queue with positions the game then walks through one by one, and
     every one of them is a cursor erase, a cursor redraw and a present. The player sees the scene's
     animation stop while the pointer moves: the loop is busy replaying a trail of pointer positions
     the real system would have collapsed into the last one. */
  if ( message == WM_MOUSEMOVE )
  {
    /* Where the pointer now is, as the game will read it out of the message: GetCursorPos is
       answered from this (see there), because on these platforms the only mouse that exists is
       the one the port posts - SDL's own state never sees the console's stick. */
    s_HaveMousePos = 1;
    s_MouseGameX = (short)(lParam & 0xFFFF);
    s_MouseGameY = (short)((lParam >> 16) & 0xFFFF);
    for ( i = 0; i < s_MsgCount; ++i )
    {
      MSG *p = &s_MsgQueue[(s_MsgHead + i) % SHIM_MSG_CAPACITY];
      if ( p->message == WM_MOUSEMOVE && p->hwnd == hWnd )
      {
        p->lParam = lParam;
        p->wParam = wParam;
        p->time = SDL_GetTicks();
        return 1;
      }
    }
  }

  if ( s_MsgCount >= SHIM_MSG_CAPACITY )
  {
    DBG_MSG("PostMessageA: queue full, dropping message");
    return 0;
  }
  m = &s_MsgQueue[(s_MsgHead + s_MsgCount) % SHIM_MSG_CAPACITY];
  m->hwnd = hWnd;
  m->message = message;
  m->wParam = wParam;
  m->lParam = lParam;
  m->time = SDL_GetTicks();
  SDL_GetMouseState(&x, &y);
  m->pt.x = x;
  m->pt.y = y;
  ++s_MsgCount;
  return 1;
}

void PostQuitMessage(int nExitCode)
{
  (void)nExitCode;
  s_QuitPosted = 1;
}

int WinShim_QuitPosted(void)
{
  return s_QuitPosted;
}

int WinShim_MsgQueueDepth(void)
{
  return s_MsgCount;                             /* how many posted messages are waiting to be
                                                    dispatched: the loop profile reads it, because a
                                                    queue that grows while the player moves the
                                                    pointer says the game is not keeping up with the
                                                    port, which is a different illness from a slow
                                                    present and needs a different cure */
}

unsigned WinShim_MsgDispatched(void)
{
  return s_MsgDispatched;
}

HWND WinShimMainWindow(void)
{
  return (HWND)(intptr_t)0x00123400;
}

static void ShimFillMsg(LPMSG lpMsg, HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  int x, y;
  lpMsg->hwnd = hWnd;
  lpMsg->message = message;
  lpMsg->wParam = wParam;
  lpMsg->lParam = lParam;
  lpMsg->time = SDL_GetTicks();
  SDL_GetMouseState(&x, &y);
  lpMsg->pt.x = x;
  lpMsg->pt.y = y;
}

int PeekMessageA(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg)
{
  if ( !lpMsg )
    return 0;

  if ( !s_MsgCount
    && !s_InDispatch && !s_InPump          /* pump SDL once per drain, never re-enter */
    && (wMsgFilterMin == 0 && wMsgFilterMax == 0) )
  {
    s_InPump = 1;
    WinShim_PumpSDLToQueue();
    s_InPump = 0;
  }

  if ( s_MsgCount )
  {
    MSG *m = &s_MsgQueue[s_MsgHead];
    if ( hWnd && m->hwnd && m->hwnd != hWnd )
      return 0;                            /* game always passes hWnd == 0; be safe */
    *lpMsg = *m;
    if ( wRemoveMsg & PM_REMOVE )
    {
      s_MsgHead = (s_MsgHead + 1) % SHIM_MSG_CAPACITY;
      --s_MsgCount;
      ++s_MsgDispatched;                       /* the loop profile counts what the game actually
                                                  took out of the queue, not what was offered */
    }
    return 1;
  }

  /* WM_QUIT has priority over synthesized paint (same as Win32) */
  if ( s_QuitPosted && !s_QuitDelivered )
  {
    s_QuitDelivered = 1;
    ShimFillMsg(lpMsg, 0, WM_QUIT, 0, 0);
    return 1;
  }

  /* InvalidateRect()/expose: one WM_PAINT per invalidation, like Windows */
  if ( s_PaintPending && g_MainWindow )
  {
    s_PaintPending = 0;
    ShimFillMsg(lpMsg, g_MainWindow, WM_PAINT, 0, 0);
    return 1;
  }

  (void)wMsgFilterMin; (void)wMsgFilterMax;
  return 0;
}

int GetMessageA(LPMSG lpMsg, HWND hWnd, uint32_t min, uint32_t max)
{
  /* Host_PlayAvi's modal loop is the one place that BLOCKS on a message: it runs
     until MM_MCINOTIFY clears g_AviPlaying, and what produces that notify is the
     pump inside PeekMessageA (WinShim_PumpSDLToQueue -> WinShim_VideoPump). So
     this is PeekMessageA in a wait loop, exactly as Win32's GetMessage blocks. */
  (void)min; (void)max;
  if ( !lpMsg )
    return 0;
  for ( ;; )
  {
    if ( PeekMessageA(lpMsg, hWnd, 0, 0, PM_REMOVE) )
      return 1;
    if ( s_QuitDelivered )           /* the WM_QUIT was already handed out */
      return 0;
    Sleep(5);
  }
}

int TranslateMessage(const MSG *lpMsg)
{
  (void)lpMsg;
  return 0;                                /* WM_CHAR is synthesized by sdl_backend from
                                              SDL_TEXTINPUT, exactly where Windows would
                                              have produced it in TranslateMessage */
}

LRESULT DispatchMessageA(const MSG *lpMsg)
{
  LRESULT r;
  int saved;

  if ( !lpMsg || !lpMsg->hwnd || lpMsg->message == WM_QUIT )
    return 0;
  saved = s_InDispatch;
  s_InDispatch = 1;
  r = Host_MainWndProc(lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
  s_InDispatch = saved;
  return r;
}

/* The synchronous round trip the old port needed - the AVI child answering
   WM_PALETTECHANGED/WM_QUERYNEWPALETTE by sending 0x311 up from inside the
   game's own SendMessage - belonged to mciavi32 on 8bpp Windows. port-main's
   host.c keeps the game-internal half of that protocol (Host_AviPaletteMessage
   realizes the palette itself), and the MCI child on Linux is a fake handle
   with no window procedure, so SendMessageA only has to do the one thing the
   game relies on: deliver to the main window's own wndproc synchronously. */
LRESULT SendMessageA(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  if ( !hWnd )
    return 0;                              /* e.g. SendMessageA to the not-created MCI child */
  if ( hWnd == g_MainWindow )
    return DispatchMessageA(&(MSG){ hWnd, message, wParam, lParam, 0, { 0, 0 } });
  return DefWindowProcA(hWnd, message, wParam, lParam);
}

LRESULT DefWindowProcA(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  (void)hWnd; (void)message; (void)wParam; (void)lParam;
  return 0;
}

int DestroyWindow(HWND hWnd)
{
  if ( !hWnd )
    return 0;
  if ( hWnd == g_MainWindow )
  {
    /* Win32: DestroyWindow sends WM_DESTROY synchronously; MainWndProc then
       PostQuitMessage(0). The MCI child window has no game wndproc - skip. */
    MSG m;
    ShimFillMsg(&m, hWnd, WM_DESTROY, 0, 0);
    DispatchMessageA(&m);
  }
  return 1;
}

int ShowWindow(HWND hWnd, int nCmdShow)
{
  if ( hWnd )
    SdlBackendShowWindow(nCmdShow != 0 /* SW_HIDE == 0 */);
  return 1;
}

int UpdateWindow(HWND hWnd)
{
  if ( hWnd && g_MainWindow == hWnd && s_PaintPending )
  {
    MSG m;
    s_PaintPending = 0;
    ShimFillMsg(&m, hWnd, WM_PAINT, 0, 0);
    return (int)DispatchMessageA(&m);
  }
  return 1;
}

int InvalidateRect(HWND hWnd, const RECT *lpRect, int bErase)
{
  (void)hWnd; (void)lpRect; (void)bErase;
  s_PaintPending = 1;                      /* full-client approximation of the update region */
  return 1;
}

int GetUpdateRect(HWND hWnd, LPRECT lpRect, int bErase)
{
  /* The update region is the GAME's client rect, not the SDL window's: the original
     window's size was pinned, while a foreign WM may have enlarged ours (fullscreen)
     - an oversized region fed AddDirtyRegion/RenderDirtyRegions beyond the fixed
     backbuffer. Logical size: g_VideoConfig never takes foreign dims (SdlHandleResize). */
  int w = 0, h = 0;

  (void)bErase;
  if ( !lpRect )
    return 0;
  if ( hWnd )
  {
    w = g_Window.clientWidth;
    h = g_Window.clientHeight;
  }
  if ( w > 0 && h > 0 )
  {
    lpRect->left = 0;
    lpRect->top = 0;
    lpRect->right = w;
    lpRect->bottom = h;
    return 1;
  }
  lpRect->left = lpRect->top = lpRect->right = lpRect->bottom = 0;
  return 0;
}

/* fake screen DC: game only passes it through to palette calls and GetDeviceCaps */
#define SHIM_FAKE_DC ((HDC)(intptr_t)0x00001000)

HDC GetDC(HWND hWnd)
{
  (void)hWnd;
  return SHIM_FAKE_DC;
}

int ReleaseDC(HWND hWnd, HDC hDC)
{
  (void)hWnd; (void)hDC;
  return 1;
}

HDC BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint)
{
  if ( lpPaint )
  {
    memset(lpPaint, 0, sizeof(PAINTSTRUCT));
    lpPaint->hdc = SHIM_FAKE_DC;
    if ( hWnd )
    {                                         /* logical game client rect, as in GetUpdateRect */
      lpPaint->rcPaint.right = g_Window.clientWidth;
      lpPaint->rcPaint.bottom = g_Window.clientHeight;
    }
    lpPaint->rcPaint.left = 0;
    lpPaint->rcPaint.top = 0;
  }
  return SHIM_FAKE_DC;
}

int EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint)
{
  (void)hWnd; (void)lpPaint;
  return 1;
}

int MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, int bRepaint)
{
  (void)bRepaint;
  WinShim_MciNotifyMove(hWnd, X, Y, nWidth, nHeight); /* MCI child rect (mci_ffmpeg.c); others ignored */
  return 1;                                /* MCI child positioning: nothing to move */
}

int SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags)
{
  (void)hWnd; (void)hWndInsertAfter; (void)X; (void)Y; (void)cx; (void)cy; (void)uFlags;
  /* The SDL window already has the game's client size (CreateMainWindow passes
     clientW/clientH); the original call resized the outer frame. No-op keeps
     the client untouched - metrics only were consumed from AdjustWindowRectEx. */
  return 1;
}

LONG GetWindowLongA(HWND hWnd, int nIndex)
{
  (void)hWnd;
  if ( nIndex == GWL_STYLE )               /* -16 */
    return (LONG)SdlBackendGetWinStyle();
  if ( nIndex == GWL_EXSTYLE )             /* -20 */
    return 0;
  return 0;
}

int AdjustWindowRectEx(LPRECT lpRect, DWORD dwStyle, int bMenu, DWORD dwExStyle)
{
  enum { FRAME = 4, CAPTION = 19 };
  LONG caption = (dwStyle & 0x00C00000u) ? CAPTION : 0;  /* WS_CAPTION = WS_BORDER|WS_DLGFRAME */

  (void)bMenu; (void)dwExStyle;
  if ( !lpRect )
    return 0;
  lpRect->left -= FRAME;
  lpRect->right += FRAME;
  lpRect->top -= FRAME + caption;
  lpRect->bottom += FRAME;
  return 1;
}

int IsIconic(HWND hWnd)
{
  (void)hWnd;
  return SdlBackendIsMinimized();
}

HCURSOR LoadCursorA(HINSTANCE hInstance, LPCSTR lpCursorName)
{
  (void)hInstance;
  return (HCURSOR)lpCursorName;            /* the game only round-trips the handle */
}

HCURSOR SetCursor(HCURSOR hCursor)
{
  (void)hCursor;
  return 0;                                /* SDL cursor visibility is owned by the config
                                              flag (SdlBackendSetCursorVisible) */
}

int GetCursorPos(LPPOINT lpPoint)
{
  int x, y;
  if ( !lpPoint )
    return 0;
  /* main.c polls the pointer once per frame (and video.c once per wait-loop turn), so this call
     is the game loop's own pulse on the platforms with no user32: when the loop starves, the
     polls stop with it, and SdlLoopProfile counts them.

     The answer is WINDOW CLIENT px, the same space a real GetCursorPos + ScreenToClient lands
     in, and NOT the game's coords: main.c converts with SdlBackendClientToGame itself, and it
     does so to decide inputState - "the pointer is inside the picture". Answering in game coords
     fed that comparison a second transform, which in the letterboxed layout threw most positions
     outside the picture (SDL's own mouse, which on the console has never seen the stick, sat at
     the window's (0,0) - a letterbox corner); inputState went 0 and the engine erased its arrow
     every frame, which is exactly the "the cursor disappears when idle after '-'" report. The
     position comes from the port's own posted WM_MOUSEMOVE - the only mouse these platforms have -
     mapped back out of the game's space; SDL_GetMouseState is only the answer before the first
     move ever, where it is already window px. */
  SdlBackendNoteFramePoll();
  if ( s_HaveMousePos )
  {
    x = s_MouseGameX;
    y = s_MouseGameY;
    SdlBackendGameToClient(&x, &y);          /* game px -> window client px (identity untransformed) */
  }
  else
    SDL_GetMouseState(&x, &y);               /* window coords == client coords here */
  lpPoint->x = x;
  lpPoint->y = y;
  return 1;
}

int SetCursorPos(int X, int Y)
{
  (void)X; (void)Y;
  return 1;                                /* only call site warps the cursor onto itself */
}

int ScreenToClient(HWND hWnd, LPPOINT lpPoint)
{
  (void)hWnd; (void)lpPoint;
  return 1;                                /* GetCursorPos already returned client coords */
}

int ClientToScreen(HWND hWnd, LPPOINT lpPoint)
{
  (void)hWnd; (void)lpPoint;
  return 1;
}

SHORT GetAsyncKeyState(int nVirtKey)
{
  SDL_Keymod m = SDL_GetModState();
  switch ( nVirtKey )
  {
    case VK_SHIFT:   return (m & KMOD_SHIFT) ? (SHORT)0x8001 : 0;
    case VK_CONTROL: return (m & KMOD_CTRL)  ? (SHORT)0x8001 : 0;
    case VK_MENU:    return (m & KMOD_ALT)   ? (SHORT)0x8001 : 0;
    default:         return 0;
  }
}

SHORT GetKeyState(int nVirtKey)
{
  return GetAsyncKeyState(nVirtKey);
}

int WinHelpA(HWND hWnd, LPCSTR lpHelpFile, UINT uCommand, ULONG_PTR dwData)
{
  (void)hWnd; (void)uCommand; (void)dwData;
  DBG_LOG("WinHelpA: ignored (help file '%s')", lpHelpFile ? lpHelpFile : "");
  return 0;                                /* .hlp ships with nothing here: report failure,
                                              never open a viewer */
}

/* ===================================================================== */
/* gdi32                                                                 */
/* ===================================================================== */

HPALETTE CreatePalette(const LOGPALETTE *pgpal)
{
  size_t bytes;
  void *mem;

  if ( !pgpal )
    return 0;
  bytes = sizeof(WORD) * 2 + (size_t)pgpal->palNumEntries * sizeof(PALETTEENTRY);
  mem = malloc(bytes);
  if ( !mem )
    return 0;
  memcpy(mem, pgpal, bytes);
  return (HPALETTE)mem;
}

int DeleteObject(HGDIOBJ hObject)
{
  if ( !hObject )
    return 0;
  free(hObject);                           /* only heap-backed handles are ever passed */
  return 1;
}

int DeleteDC(HDC hdc)
{
  (void)hdc;
  return 1;
}

HPALETTE SelectPalette(HDC hdc, HPALETTE hpal, int fForceBackground)
{
  (void)hdc; (void)fForceBackground;
  return (HPALETTE)0;                      /* "previously selected" */
}

UINT RealizePalette(HDC hdc)
{
  (void)hdc;
  return 1;                                /* nonzero = colors changed (video.c wants that) */
}

/* Win system palette, reserved slots 0-9 / 246-255 (8bpp sessions, Win9x).
   TODO(visual): compare against the Windows build - only video-background
   palettes (SetPalette a1 != 0) hit these entries. */
static const PALETTEENTRY s_SysPalFirst[10] =
{
  {   0,   0,   0, 0 }, { 128,   0,   0, 0 }, {   0, 128,   0, 0 },
  { 128, 128,   0, 0 }, {   0,   0, 128, 0 }, { 128,   0, 128, 0 },
  {   0, 128, 128, 0 }, { 192, 192, 192, 0 }, { 128, 128, 128, 0 },
  { 192, 220, 192, 0 }
};
static const PALETTEENTRY s_SysPalLast[10] =
{
  { 255, 255, 255, 0 }, { 192, 192, 192, 0 }, { 128, 128, 128, 0 },
  {  64,  64,  64, 0 }, { 255,   0,   0, 0 }, {   0, 255,   0, 0 },
  {   0,   0, 255, 0 }, { 255, 255,   0, 0 }, { 255,   0, 255, 0 },
  {   0, 255, 255, 0 }
};

UINT GetSystemPaletteEntries(HDC hdc, UINT nStartIndex, UINT nEntries, PALETTEENTRY *lpEntries)
{
  UINT i;

  (void)hdc;
  if ( !lpEntries )
    return 0;
  for ( i = 0; i < nEntries; ++i )
  {
    UINT idx = nStartIndex + i;
    if ( idx < 10 )
      lpEntries[i] = s_SysPalFirst[idx];
    else if ( idx >= 246 && idx < 256 )
      lpEntries[i] = s_SysPalLast[idx - 246];
    else
      memset(&lpEntries[i], 0, sizeof(PALETTEENTRY));
  }
  return nEntries;
}

int GetDeviceCaps(HDC hdc, int nIndex)
{
  (void)hdc;
  switch ( nIndex )
  {
    case 0:   return 0x0300;               /* VERSION */
    case 12:  return 8;                    /* BITSPIXEL: the game is 8-bit palettized */
    case 14:  return 1;                    /* PLANES */
    case 24:  return 256;                  /* NUMCOLORS */
    case 38:  return 0x100;                /* RASTERCAPS: RC_PALETTE, like 8bpp Win9x */
    default:  return 0;
  }
}

/* ===================================================================== */
/* winmm: timers (MCI lives in mci_ffmpeg.c)                             */
/* ===================================================================== */

DWORD timeGetTime(void)
{
  return (DWORD)SDL_GetTicks();
}

MMRESULT timeGetDevCaps(LPTIMECAPS lpTimeCaps, UINT uSize)
{
  if ( !lpTimeCaps || uSize < sizeof(TIMECAPS) )
    return 99;                             /* TIMERR_NOCANDO */
  lpTimeCaps->wPeriodMin = 1;
  lpTimeCaps->wPeriodMax = 15;
  return 0;                                /* TIMERR_NOERROR: InitMidiOut derives
                                              g_TimerResolution = 10 from this pair */
}

MMRESULT timeBeginPeriod(UINT uPeriod)
{
  (void)uPeriod;
  return 0;
}

MMRESULT timeEndPeriod(UINT uPeriod)
{
  (void)uPeriod;
  return 0;
}

static MMRESULT s_TimerId;
static LPTIMECALLBACK s_TimerCallback;

static Uint32 ShimTimerProc(Uint32 interval, void *param)
{
  (void)param;
  if ( s_TimerCallback )
    s_TimerCallback((UINT)s_TimerId, 0, 0, 0, 0);
  /* Этот тик разобрал дорожку и загнал события в синтез (midiOutShortMsg ->
     SdlMidi_ShortMsg) — теперь тот же тик рендерит их в PCM, как в v0.1, где
     таймерный колбэк audio.c делал и то и другое. Вызов стоит здесь, а не в
     wave.c, потому что игровой код не трогаем: шов — winmm-протокол. Зовётся
     только пока жив таймер (GmMidi_Play..GmMidi_Halt) — это и есть «дорожка
     играет», отдельный флаг кормления здесь не нужен. */
  SdlMidi_Pump();
  return interval;                         /* periodic */
}

MMRESULT timeSetEvent(UINT uDelay, UINT uResolution, LPTIMECALLBACK lpTimeProc,
                      DWORD_PTR dwUser, UINT uType)
{
  (void)uResolution; (void)dwUser; (void)uType;
  if ( !lpTimeProc )
    return 0;
  if ( !(SDL_WasInit(SDL_INIT_TIMER) & SDL_INIT_TIMER) )
  {
    if ( SDL_InitSubSystem(SDL_INIT_TIMER) != 0 )
      return 0;
  }
  s_TimerCallback = lpTimeProc;
  s_TimerId = (MMRESULT)SDL_AddTimer(uDelay ? uDelay : 10, ShimTimerProc, 0);
  return s_TimerId;
}

MMRESULT timeKillEvent(MMRESULT uTimerID)
{
  SDL_RemoveTimer((SDL_TimerID)uTimerID);
  return 0;
}

/* ===================================================================== */
/* waveOut / midiOut                                                      */
/* ===================================================================== */

/*
 * waveOut over SDL: wave.c keeps the winmm protocol byte-for-byte - headers
 * start with WHDR_DONE, waveOutWrite clears it, and the game polls
 * GmWave_FindFreeBuffer for it - so the shim retires headers the way winmm
 * does: each submission records the running byte mark at which its bytes have
 * drained (s_WaveTotalQueued - SdlWave_Queued() is how far the device has
 * consumed), and WinShim_WavePump marks such a header WHDR_DONE and posts
 * MM_WOM_DONE to the CALLBACK_WINDOW window. The pump runs from both SDL pumps
 * (WinShim_PumpSDLToQueue and SdlBackendPumpEvents), because the game's sound
 * chain lives or dies on completions arriving while either loop turns.
 */
#define SHIM_MAX_INFLIGHT 16
static struct {
    WAVEHDR *hdr;
    DWORD_PTR end;              /* running byte mark at which this header has drained */
} s_WaveInFlight[SHIM_MAX_INFLIGHT];
static int s_WaveInFlightCount;
static DWORD_PTR s_WaveTotalQueued;  /* bytes handed to the device since open */
static HWND s_WaveNotifyWindow;
static int s_WaveOpen;

MMRESULT waveOutOpen(HWAVEOUT *phwo, UINT uDeviceID, const WAVEFORMATEX *fmt,
                     DWORD_PTR callback, DWORD_PTR instance, DWORD flags)
{
    (void)uDeviceID;
    (void)instance;
    (void)flags;
    /* The game's only output format is 11025 Hz / mono / U8 and SdlWave_Open is
       built around exactly that; a different fmt would be a game change, not a
       port question, so it is only logged. */
    if (fmt && (fmt->nSamplesPerSec != 11025 || fmt->nChannels != 1 || fmt->wBitsPerSample != 8))
        DBG_LOG("waveOutOpen: unexpected format %u Hz / %u ch / %u bit",
                fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample);
    if (!SdlWave_Open())
        return 1;                       /* MMSYSERR_ALLOCATED stand-in */
    s_WaveNotifyWindow = (HWND)callback;
    s_WaveInFlightCount = 0;
    s_WaveTotalQueued = 0;
    s_WaveOpen = 1;
    if (phwo)
        *phwo = (HWAVEOUT)(intptr_t)1;
    DBG_LOG("waveOutOpen: SDL wave device up (notify window %p)", (void *)s_WaveNotifyWindow);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutClose(HWAVEOUT hWaveOut)
{
    (void)hWaveOut;
    if (s_WaveOpen)
        DBG_LOG("waveOutClose: SDL wave device closed");
    s_WaveOpen = 0;
    s_WaveNotifyWindow = NULL;
    s_WaveInFlightCount = 0;
    SdlWave_Close();
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutGetID(HWAVEOUT hWaveOut, LPUINT puDeviceID)
{
    (void)hWaveOut;
    if (puDeviceID)
        *puDeviceID = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutPrepareHeader(HWAVEOUT hWaveOut, LPWAVEHDR pwh, UINT cbwh)
{
    (void)hWaveOut;
    (void)cbwh;
    if (pwh)
        pwh->dwFlags |= WHDR_PREPARED;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutUnprepareHeader(HWAVEOUT hWaveOut, LPWAVEHDR pwh, UINT cbwh)
{
    (void)hWaveOut;
    (void)cbwh;
    if (pwh)
        pwh->dwFlags &= ~(WHDR_PREPARED | WHDR_INQUEUE);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutWrite(HWAVEOUT hWaveOut, LPWAVEHDR pwh, UINT cbwh)
{
    (void)hWaveOut;
    (void)cbwh;
    if (!pwh || !pwh->lpData)
        return 1;
    if (s_WaveInFlightCount >= SHIM_MAX_INFLIGHT)
        return 1;                       /* no room: the game resets instead of queueing more */
    if (!SdlWave_Queue(pwh->lpData, pwh->dwBufferLength))
        return 1;
    pwh->dwFlags &= ~WHDR_DONE;
    pwh->dwFlags |= WHDR_INQUEUE;
    s_WaveInFlight[s_WaveInFlightCount].hdr = pwh;
    s_WaveTotalQueued += pwh->dwBufferLength;
    s_WaveInFlight[s_WaveInFlightCount].end = s_WaveTotalQueued;
    ++s_WaveInFlightCount;
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutReset(HWAVEOUT hWaveOut)
{
    (void)hWaveOut;
    SdlWave_Clear();                    /* pending bytes are gone: everything in flight reads drained */
    WinShim_WavePump();
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutPause(HWAVEOUT hWaveOut)
{
    (void)hWaveOut;
    SdlWave_Pause(1);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutRestart(HWAVEOUT hWaveOut)
{
    (void)hWaveOut;
    SdlWave_Pause(0);
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutGetVolume(HWAVEOUT hWaveOut, LPDWORD pdwVolume)
{
    (void)hWaveOut;
    if (pdwVolume)
        *pdwVolume = 0xFFFFFFFFu;       /* wave.c applies volume to the samples itself */
    return MMSYSERR_NOERROR;
}

MMRESULT waveOutSetVolume(HWAVEOUT hWaveOut, DWORD dwVolume)
{
    (void)hWaveOut;
    (void)dwVolume;                     /* same reason: software scaling in wave.c */
    return MMSYSERR_NOERROR;
}

/* Retire the headers SDL has drained (see the block comment above). */
void WinShim_WavePump(void)
{
    DWORD_PTR played;
    int i, kept = 0;

    if (!s_WaveOpen || !s_WaveInFlightCount)
        return;
    played = s_WaveTotalQueued - (DWORD_PTR)SdlWave_Queued();
    for (i = 0; i < s_WaveInFlightCount; ++i)
    {
        WAVEHDR *hdr = s_WaveInFlight[i].hdr;
        if (s_WaveInFlight[i].end <= played)
        {
            hdr->dwFlags |= WHDR_DONE;
            hdr->dwFlags &= ~WHDR_INQUEUE;
            if (s_WaveNotifyWindow)
                PostMessageA(s_WaveNotifyWindow, MM_WOM_DONE, WHDR_DONE, (LPARAM)hdr);
        }
        else
        {
            s_WaveInFlight[kept] = s_WaveInFlight[i];
            ++kept;
        }
    }
    s_WaveInFlightCount = kept;
}

/*
 * midiOut over libADLMIDI (SdlMidi_* in sdl_backend.c): wave.c keeps the winmm
 * protocol byte-for-byte - the open hands back a handle, short messages flow,
 * reset cuts the sounding notes - and the device under it is the OPL3 synth in
 * this same process, 10 ms timer and all (ShimTimerProc pulls SdlMidi_Pump
 * after each tick's messages). On Windows none of this file exists: midiOut*
 * there is the real winmm, same as the original game played through.
 *
 * midiOutSetVolume/midiOutGetVolume stay placeholders: port-main applies the
 * music volume to note velocity in wave.c (g_MidiVolume) by design - the
 * original's device volume is what modern Windows cannot do - and no call
 * site reaches these two.
 */

MMRESULT midiOutOpen(HMIDIOUT *phmo, UINT uDeviceID, DWORD_PTR dwCallback,
                     DWORD_PTR dwInstance, DWORD dwFlags)
{
    (void)uDeviceID;
    (void)dwCallback;
    (void)dwInstance;
    (void)dwFlags;
    if (!SdlMidi_Open())
        return MMSYSERR_ERROR;             /* отказ устройства - как отказ устройства в оригинале:
                                               игра продолжит без музыки, а не «успех в пустоту» */
    if (phmo)
        *phmo = (HMIDIOUT)(intptr_t)2;
    return MMSYSERR_NOERROR;
}

MMRESULT midiOutClose(HMIDIOUT hMidiOut)
{
    (void)hMidiOut;
    SdlMidi_Close();
    return MMSYSERR_NOERROR;
}

MMRESULT midiOutGetID(HMIDIOUT hMidiOut, LPUINT puDeviceID)
{
    (void)hMidiOut;
    if (puDeviceID)
        *puDeviceID = 0;
    return MMSYSERR_NOERROR;
}

MMRESULT midiOutShortMsg(HMIDIOUT hMidiOut, DWORD dwMsg)
{
    (void)hMidiOut;
    SdlMidi_ShortMsg((unsigned int)dwMsg); /* status | data1<<8 | data2<<16 - то же упакованное слово */
    return MMSYSERR_NOERROR;
}

MMRESULT midiOutReset(HMIDIOUT hMidiOut)
{
    (void)hMidiOut;
    SdlMidi_Reset();
    return MMSYSERR_NOERROR;
}

MMRESULT midiOutSetVolume(HMIDIOUT hMidiOut, DWORD dwVolume)
{
    (void)hMidiOut;
    (void)dwVolume;
    return MMSYSERR_NOERROR;
}

MMRESULT midiOutGetVolume(HMIDIOUT hMidiOut, LPDWORD pdwVolume)
{
    (void)hMidiOut;
    if (pdwVolume)
        *pdwVolume = 0xFFFFFFFFu;
    return MMSYSERR_NOERROR;
}

/* winmm's sndPlaySound - only wave.c's "stop whatever is playing" call site. */
int sndPlaySoundA(LPCSTR pszSound, UINT uFlags)
{
    (void)pszSound;
    (void)uFlags;
    return 1;
}

/* ===================================================================== */
/* window subclassing pair: no wndprocs per window here - DispatchMessageA */
/* calls Host_MainWndProc directly, so SetWindowLongPtrA(GWLP_WNDPROC) is  */
/* a no-op and CallWindowProcA just calls the proc it is given.            */
/* ===================================================================== */

LONG_PTR SetWindowLongPtrA(HWND hWnd, int nIndex, LONG_PTR dwNewLong)
{
    (void)hWnd;
    (void)nIndex;
    (void)dwNewLong;
    return 0;
}

LRESULT CallWindowProcA(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg,
                        WPARAM wParam, LPARAM lParam)
{
    return lpPrevWndFunc ? lpPrevWndFunc(hWnd, Msg, wParam, lParam) : 0;
}

/* ===================================================================== */
/* entry point: WinMain lives in host.c, untouched                       */
/* ===================================================================== */

int main(int argc, char **argv)
{
  static char s_Args[1024];                   /* lpCmdLine for WinMain: argv[1..] joined with spaces */
  int i;
  size_t n = 0;

  s_Args[0] = 0;
  for ( i = 1; i < argc; ++i )
  {
    size_t len = strlen(argv[i]);
    size_t j;
    int quote = 0;

    /* Windows hands WinMain the raw command line with the shell's quoting still
       in it, so -datapath "D:\My Games\Pilots" arrives as ONE token. argv has
       already thrown that away, and joining with spaces would hand the port
       layer's tokenizer "-datapath D:\My Games\Pilots" as three tokens - the
       switch would get "D:\My" and the spaces in the path would silently pick
       the wrong directory. Put the quotes back where they were. */
    for ( j = 0; j < len; ++j )
      if ( argv[i][j] == ' ' || argv[i][j] == '\t' )
        quote = 1;
    if ( n + len + 4 >= sizeof s_Args )
      break;
    if ( n )
      s_Args[n++] = ' ';
    if ( quote )
      s_Args[n++] = '"';
    memcpy(s_Args + n, argv[i], len);
    n += len;
    s_Args[n] = 0;
    if ( quote )
    {
      s_Args[n++] = '"';
      s_Args[n] = 0;
    }
  }
  return WinMain(0, 0, s_Args, 1);
}

#endif /* !_WIN32 */

#ifdef _WIN64
/* Side setjmp buffer for Win64 (pilots_globals.h routes setjmp/longjmp here):
 * MSVC x64 jmp_buf is 256 bytes (16 x 16-byte _JBTYPE), while the generated
 * g_JumpBuffer field stays the original's 64 bytes - using it directly would
 * overflow into neighbouring globals. */
jmp_buf g_ShimJumpBuf;
#endif
