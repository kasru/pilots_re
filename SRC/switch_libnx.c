/* switch_libnx.c - the one translation unit that parses libnx. Everything below is
 * #ifdef __SWITCH__; the file is listed in every build (CMakeLists.txt, all four
 * bats) and compiles to nothing off the console, the way win32_shim.c compiles to
 * nothing on Windows.
 *
 * WHY A SEPARATE FILE AT ALL
 * game_globals_struct.h is generated and defines aliases for plain, ordinary words
 * - "#define offset", "#define Format" - because that is what the decompiler called
 * those globals. Included in the usual order (pilots_fwd.h first), those aliases are
 * live while libnx's own declarations are being parsed and rewrite the identifiers
 * inside them; the compiler then reports a syntax error inside the generated header,
 * which is precisely the file that must not be edited, so the diagnosis points at
 * the wrong place. Parsing libnx before the aliases exist removes the whole class of
 * problem instead of suppressing it one macro at a time - and switch.h's include
 * guard means the later SDL includes cannot pull it in a second time under them.
 * Anything else that ever needs a libnx header belongs here, not in a file that
 * includes pilots_fwd.h first.
 *
 * TWO THINGS LIVE HERE, both because they need libnx and nothing else does:
 *
 *  - SdlSwitchAttachDebugChannel: the live log. When the app was started by nxlink -
 *    hbmenu's netloader, i.e. "Y" in hbmenu and then nxlink from a machine on the
 *    same network - it hands the process that machine's address, which libnx parses
 *    into __nxlink_host; connecting back and pointing stderr there puts the whole
 *    debug log on the PC as it is produced, because OutputDebugStringA
 *    (win32_shim.c) mirrors every DBG_* line to stderr. Started from the hbmenu list
 *    as usual, __nxlink_host is zero and nothing runs. Reading the log off the card
 *    after a run stays the normal way (the file survives a crash, and Atmosphere
 *    writes its crash report next to it) - what this adds is seeing the numbers
 *    while the process is still alive.
 *  - SdlSwitchReportEnvironment: where the code, heap and stack actually are, and
 *    how much memory this process may use. The port got the first of those wrong
 *    twice (see Shim_AllocLow in win32_shim.c), so the run says it out loud.
 */

#ifdef __SWITCH__

#include <switch.h>
#include <arpa/inet.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include "pilots_fwd.h"

/* wvsprintfA on Windows (and so the DBG_* format subset this project keeps to)
   has no %p and no %ll, so a 64-bit address goes out as two 32-bit halves. */
#define PTRHI(p) (unsigned int)((unsigned long long)(UINT_PTR)(p) >> 32)
#define PTRLO(p) (unsigned int)(unsigned long long)(UINT_PTR)(p)

void SdlSwitchAttachDebugChannel(void)
{
  char addr[INET_ADDRSTRLEN];

  if ( ntohl(__nxlink_host.s_addr) == 0 )
    return;                       /* started from the menu: nobody is listening */

  /* Sockets are needed before the log socket itself can be opened. Both are
     libnx's own: socketInitializeDefault() and nxlinkStdio() (an inline wrapper
     for nxlinkConnectToHost(true, true), see switch/runtime/nxlink.h). */
  if ( R_FAILED(socketInitializeDefault()) )
  {
    DBG_LOG("SdlSwitchAttachDebugChannel: sockets failed, the log stays on the card");
    return;
  }
  strcpy(addr, inet_ntoa(__nxlink_host));
  if ( nxlinkStdio() >= 0 )
    DBG_LOG("SdlSwitchAttachDebugChannel: stderr routed to %s, the log is live here too", addr);
  else
    DBG_LOG("SdlSwitchAttachDebugChannel: could not reach %s, the log stays on the card", addr);
}

/* What this process actually got, measured instead of assumed: where the code, the
   heap and the stack live, how big the heap region is, and how much the applet is
   allowed to use in total. Every one of these has been guessed wrong while bringing
   the game up on the console (the image is NOT low - see Shim_AllocLow in
   win32_shim.c, and the heap is a slice of hbloader's own, which is why it might be
   low even though the image is not), and a run that prints them costs nothing.
   The two svcGetInfo calls are the same pair hbloader itself uses to size the heap
   (setupHbHeap/calculateMaxHeapSize in its forwarder source). */
void SdlSwitchReportEnvironment(void)
{
  struct mallinfo mi;
  MemoryInfo mem;
  u32 page = 0;
  u64 total = 0, used = 0;
  void *probe;
  char stackMark;

  probe = malloc(64);
  mi = mallinfo();

  DBG_LOG("SdlSwitchReportEnvironment: code=%08X%08X heap=%08X%08X stack=%08X%08X",
          PTRHI(&SdlSwitchReportEnvironment), PTRLO(&SdlSwitchReportEnvironment),
          PTRHI(probe), PTRLO(probe),
          PTRHI(&stackMark), PTRLO(&stackMark));

  if ( probe && R_SUCCEEDED(svcQueryMemory(&mem, &page, (u64)(uintptr_t)probe)) )
    DBG_LOG("SdlSwitchReportEnvironment: the malloc heap sits at %08X%08X + %d MB type %d perm 0x%X%s",
            PTRHI((void *)(uintptr_t)mem.addr), PTRLO((void *)(uintptr_t)mem.addr),
            (int)(mem.size / (1024 * 1024)), (int)mem.type, (int)mem.perm,
            mem.addr < 0x80000000ULL ? ", BELOW 2 GB" : ", above 2 GB");

  DBG_LOG("SdlSwitchReportEnvironment: newlib heap arena=%d KB used=%d KB free=%d KB",
          mi.arena / 1024, mi.uordblks / 1024, mi.fordblks / 1024);

  if ( R_SUCCEEDED(svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0)) &&
       R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0)) )
    DBG_LOG("SdlSwitchReportEnvironment: this process may use %d MB, %d MB already in use",
            (int)(total / (1024 * 1024)), (int)(used / (1024 * 1024)));

  free(probe);
}

/* THE LOW MEMORY THE ENGINE NEEDS, and how to get it on this console.
 *
 * The engine keeps a pool pointer in a genuinely 32-bit field (AllocOpcodeBuffer
 * writes dword_4115C8, declared `int` in the generated game_globals_struct.h, and
 * the opcode handlers read it back through (INT_PTR)), so any address the game
 * stores there has to come back unchanged from sign-extending a 32-bit value -
 * which means below 2 GB. Nothing this process is handed by default is: the image
 * (and its .bss) and the heap both live at 30-55 GB, measured by
 * SdlSwitchReportEnvironment. On Linux the shim gets low memory from MAP_32BIT;
 * there is no mmap here, so the port has to ask the kernel for the mapping itself.
 *
 * Two routes, and both are printed with what they produced:
 *
 *  ROUTE 1, process memory: map a block of our own heap into this same process at a
 *  low address, using the handle the loader gives the app to its own process
 *  (envGetOwnProcessHandle - the mapping calls refuse CUR_PROCESS_HANDLE, so without
 *  that handle there is nothing to pass). Worth having because it keeps the memory's
 *  attributes, i.e. the cache.
 *  ROUTE 2, transfer memory: turn the block into an object and map the object where we
 *  want it - the way libnx maps its IPC buffers, so it is certain to be available. It
 *  is the route the port used first, and the run measured: the frame rate fell, which
 *  is what mapping memory uncached does to a blit loop, and a whole-screen fill (the
 *  checkerboard between screens) is the worst case of exactly that. It is the fallback
 *  now, and the attr line is what proves or refutes that reading.
 *
 * Either way the mapping has to be given back before the process leaves: it holds
 * references to pages hbloader owns and takes back when the app returns to it, and
 * exiting with them in place is what made hbloader break - its own report, 2001-0106
 * (module 2001 is hosloader in libnx's codes), with no PILOT_RV offset anywhere in it.
 * Hence SdlSwitchUnmapLowMemory, called from SdlBackendShutdown.
 */
/* Candidate addresses, all below the 2 GB the round trip dies at and all 2 MB-aligned.
   A list rather than a search: the process's regions are in the tens of gigabytes, so the
   low part of the space is mostly one hole, and hunting for the first hole wide enough
   would be as much of a guess as naming an address. Each candidate is still checked with
   svcQueryMemory before use. */
static const unsigned long long Candidates[] =
{
  0x20000000ULL, 0x40000000ULL, 0x60000000ULL, 0x10000000ULL, 0x70000000ULL
};

/* What was mapped, so the exit can undo it. KIND is 1 for the process-memory route and
   2 for transfer memory (whose handle has to be closed as well as unmapped). */
static int s_AliasKind;
static unsigned long long s_Alias;
static unsigned long long s_AliasSrc;
static size_t s_AliasSize;
static Handle s_AliasHandle;

static void Shim_RememberAlias(int kind, const char *name, unsigned long long addr,
                               unsigned long long srcAddr, size_t size, Handle handle)
{
  MemoryInfo mem;
  u32 page = 0;

  s_AliasKind = kind;
  s_Alias = addr;
  s_AliasSrc = srcAddr;
  s_AliasSize = size;
  s_AliasHandle = handle;

  /* Written through before the game is handed it, because an alias that maps but is not
     writable would otherwise be discovered by a sprite, not by a log line. And with it
     goes the one property no layout log shows: attribute bit 3 means uncached. */
  memset((void *)(uintptr_t)addr, 0, 4096);
  *(unsigned *)(uintptr_t)addr = 0x5A5A5A5Au;
  if ( R_SUCCEEDED(svcQueryMemory(&mem, &page, addr)) )
    DBG_LOG("SdlSwitchMapLowMemory: %d KB aliased at %08X%08X as %s, %s 2 GB, attr 0x%X (%s), reads back 0x%08X",
            (int)(size / 1024), PTRHI((void *)(uintptr_t)addr), PTRLO((void *)(uintptr_t)addr),
            name, addr < 0x80000000ULL ? "BELOW" : "above", (int)mem.attr,
            (mem.attr & MemAttr_IsUncached) ? "UNCACHED" : "cached",
            *(unsigned *)(uintptr_t)addr);
  else
    DBG_LOG("SdlSwitchMapLowMemory: %d KB aliased at %08X%08X as %s, reads back 0x%08X",
            (int)(size / 1024), PTRHI((void *)(uintptr_t)addr), PTRLO((void *)(uintptr_t)addr),
            name, *(unsigned *)(uintptr_t)addr);
}

/* Returns a 2 MB-aligned address below 2 GB where `size` bytes can be mapped, judging by
 * what svcQueryMemory says about the region that contains it. */
static unsigned long long Shim_PickLowAddress(size_t size)
{
  unsigned int i;

  for ( i = 0; i < sizeof(Candidates) / sizeof(Candidates[0]); ++i )
  {
    MemoryInfo mem;
    u32 page = 0;

    if ( R_FAILED(svcQueryMemory(&mem, &page, Candidates[i]))
      || mem.type != MemType_Unmapped
      || mem.addr > Candidates[i]
      || Candidates[i] - mem.addr + size > mem.size )
    {
      DBG_LOG("SdlSwitchMapLowMemory: 0x%08X%08X has no room for %d KB, trying the next one",
              (unsigned int)(Candidates[i] >> 32), (unsigned int)Candidates[i],
              (int)(size / 1024));
      continue;
    }
    return Candidates[i];
  }
  return 0;
}

void *SdlSwitchMapLowMemory(size_t bytes)
{
  size_t size;
  void *block;
  Handle own;
  unsigned long long src;
  unsigned long long low;
  Handle tmem;
  Result rc;
  Result rc2;

  size = (bytes + 0x1FFFFFULL) & ~(size_t)0x1FFFFFULL;   /* kernel granularity */

  block = memalign(0x200000, size);
  if ( !block )
  {
    DBG_LOG("SdlSwitchMapLowMemory: memalign(%d KB) failed", (int)(size / 1024));
    return 0;
  }
  memset(block, 0, size);          /* the pool's own bookkeeping relies on zeroed blocks */
  src = (unsigned long long)(uintptr_t)block;

  low = Shim_PickLowAddress(size);
  if ( !low )
  {
    free(block);                   /* nothing is mapped yet, so the block is still ours to give back */
    return 0;
  }

  /* ROUTE 1, the one worth having: map the block into this process again at the low
     address, through a real handle to it. envGetOwnProcessHandle is why that is possible
     at all - the loader hands the app a handle to its own process, and the mapping calls
     refuse CUR_PROCESS_HANDLE, so without it there is nothing to pass. The source needs
     the map bit in its permissions first; BIT(3) is written out because this libnx names
     Perm_R/Perm_W/Perm_X but not that one.
     What this route buys over route 2 is the cache: it maps the same physical pages with
     the same attributes, and the game's blits run at cached speed. Which route won and
     what the result's memory attribute actually is are both printed, because that is the
     difference between 60 frames and 15 and no log line about the layout can see it. */
  own = envGetOwnProcessHandle();
  if ( own != 0 && own != CUR_PROCESS_HANDLE )
  {
    rc = svcSetProcessMemoryPermission(own, src, size, Perm_Rw | BIT(3));
    rc2 = R_FAILED(rc) ? rc : svcMapProcessMemory((void *)(uintptr_t)low, own, src, size);
    if ( R_SUCCEEDED(rc2) )
    {
      Shim_RememberAlias(1, "process memory", low, src, size, 0);
      return (void *)(uintptr_t)low;
    }
    DBG_LOG("SdlSwitchMapLowMemory: process-memory route rc=0x%08X/0x%08X, trying transfer memory",
            (unsigned)rc, (unsigned)rc2);
  }
  else
    DBG_LOG("SdlSwitchMapLowMemory: the loader gave no process handle, going straight to transfer memory");

  /* ROUTE 2: transfer memory, the way libnx maps its IPC buffers. It works here, but it
     is the route the port used first and the run measured: the frame rate dropped, which
     is what mapping memory uncached does to a blit loop - so it is the fallback now, and
     the attribute line below is what proves or refutes that reading.
     Note it also takes the source away from this process, so on this route the block is
     reachable only through the alias. */
  rc = svcCreateTransferMemory(&tmem, (void *)(uintptr_t)src, size, Perm_Rw);
  if ( R_FAILED(rc) )
  {
    DBG_LOG("SdlSwitchMapLowMemory: svcCreateTransferMemory(%d KB) rc=0x%08X",
            (int)(size / 1024), (unsigned)rc);
    return 0;                        /* the source block is unreachable now, so it is not freed */
  }

  low = Shim_PickLowAddress(size);
  if ( !low )
  {
    svcCloseHandle(tmem);          /* the object is created but never mapped: do not keep the handle */
    return 0;
  }
  rc = svcMapTransferMemory(tmem, (void *)(uintptr_t)low, size, Perm_Rw);
  if ( R_FAILED(rc) )
  {
    DBG_LOG("SdlSwitchMapLowMemory: svcMapTransferMemory at 0x%08X%08X rc=0x%08X",
            (unsigned int)(low >> 32), (unsigned int)low, (unsigned)rc);
    svcCloseHandle(tmem);
    return 0;
  }

  Shim_RememberAlias(2, "transfer memory", low, src, size, tmem);
  return (void *)(uintptr_t)low;
}

/* Undo the alias, because leaving it mapped is not free on this console: the pages
 * belong to the region the loader gave the app, and it wants them back when the app
 * returns. Exiting with the alias in place is what produced hbloader's own break
 * (2001-0106, no PILOT_RV offset in the report) - a fatal in the loader looks exactly
 * like "the game crashed on exit" from the sofa, so the port gives the mapping back as
 * the last thing it does. Called from SdlBackendShutdown, after the game's own cleanup
 * has finished with the pool.
 *
 * The alias itself is not touched afterwards, so unmapping it cannot hurt the game;
 * the source block stays in the heap, which the C runtime and the loader own. */
void SdlSwitchUnmapLowMemory(void)
{
  Result rc = 0;

  if ( !s_AliasKind )
    return;

  if ( s_AliasKind == 2 )
  {
    rc = svcUnmapTransferMemory(s_AliasHandle, (void *)(uintptr_t)s_Alias, s_AliasSize);
    svcCloseHandle(s_AliasHandle);             /* the object is ours until closed, whatever the unmap said */
  }
  else
    rc = svcUnmapProcessMemory((void *)(uintptr_t)s_Alias, envGetOwnProcessHandle(),
                               s_AliasSrc, s_AliasSize);

  s_AliasKind = 0;
  DBG_LOG("SdlSwitchUnmapLowMemory: the low mapping is given back, rc=0x%08X", (unsigned)rc);
}

/* The console's own stick and buttons. SDL is not an option for this one: the SDL2
 * this SDK ships has a touch driver (nm shows SDL_switchtouch.o calling
 * hidGetTouchScreenStates) and keyboard/mouse, but no npad driver - nothing in
 * libSDL2.a references hidGetNpadStates*, so the controls have to be read from hid
 * directly. Plain int/unsigned on the way out, because no libnx type may cross into a
 * file that includes pilots_fwd.h first (see the header of this file).
 *
 * The stick comes out as hid reports it: +-0x7FFF, y positive upwards - the port
 * layer decides what that means for a cursor. Buttons come out as the SWITCH_PAD_*
 * bits. Which controller answered is logged once: whether the console feeds a running
 * app through the "handheld" id or through player 1 is exactly the sort of thing not
 * to assume, and the answer is one line in the log instead of a guess in the code.
 */
void SdlSwitchPollPad(int *stickX, int *stickY, unsigned int *buttons)
{
  static const HidNpadIdType Ids[] = { HidNpadIdType_Handheld, HidNpadIdType_No1 };
  static int s_Inited;
  static int s_SourceReported;
  HidNpadCommonState st;
  int have;

  *stickX = 0;
  *stickY = 0;
  *buttons = 0;

  if ( !s_Inited )
  {
    s_Inited = 1;
    /* hid only feeds an app the styles and player slots it declared, so without these
       two calls every read below answers with zero states and looks like a controller
       that is not plugged in. */
    hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadFullCtrl);
    hidSetSupportedNpadIdType(Ids, sizeof(Ids) / sizeof(Ids[0]));
  }

  have = (int)hidGetNpadStatesHandheld(HidNpadIdType_Handheld, &st, 1);
  if ( !have )
    have = (int)hidGetNpadStatesFullKey(HidNpadIdType_No1, &st, 1);
  if ( !have )
    return;                                    /* nothing in a style we read is connected */

  if ( !s_SourceReported )
  {
    s_SourceReported = 1;
    DBG_LOG("SdlSwitchPollPad: controls are being read (handheld or player 1, whichever answered first)");
  }

  *stickX = (int)st.analog_stick_l.x;
  *stickY = (int)st.analog_stick_l.y;
  if ( st.buttons & HidNpadButton_A )
    *buttons |= SWITCH_PAD_A;
  if ( st.buttons & HidNpadButton_B )
    *buttons |= SWITCH_PAD_B;
  if ( st.buttons & HidNpadButton_X )
    *buttons |= SWITCH_PAD_X;
  if ( st.buttons & HidNpadButton_Y )
    *buttons |= SWITCH_PAD_Y;
  if ( st.buttons & HidNpadButton_L )
    *buttons |= SWITCH_PAD_L;                 /* the Pro Controller's shoulders are the same buttons */
  if ( st.buttons & HidNpadButton_R )
    *buttons |= SWITCH_PAD_R;
  /* The Joy-Con "-" and "+". Read as plain buttons: the port layer decides what they mean
     ("-" is the picture's stretch toggle), and hid delivers both to an app that declares
     FullCtrl - unlike Home, which the system keeps for itself. */
  if ( st.buttons & HidNpadButton_Minus )
    *buttons |= SWITCH_PAD_MINUS;
}

#endif /* __SWITCH__ */

