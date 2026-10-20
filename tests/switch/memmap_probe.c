/* memmap_probe.c - what memory does a homebrew process actually get on this
 * console, and can any of it be below 2 GB?
 *
 * WHY THIS EXISTS
 * The engine keeps one pointer in a genuinely 32-bit field: AllocOpcodeBuffer
 * (memory.c) stores the pool pointer into dword_4115C8, which the generated
 * game_globals_struct.h declares `int` at +0x1554 and which must stay that way,
 * and the opcode handlers read it back as (INT_PTR)dword_4115C8. That round trip
 * is only lossless for an address the CPU gets back unchanged after sign-extending
 * a 32-bit value - in practice, below 2 GB. It is why Shim_AllocLow exists: Linux
 * asks the kernel for MAP_32BIT, and the x64 Windows build died at gameloop.c:338
 * until its GlobalAlloc blocks happened to land low.
 * The Switch port assumed the NRO's .bss would be low, and the first hardware run
 * measured otherwise: &dword_4161F0 = 0x78c3fa888, module base 0xbaf48f000 in
 * another run. hbloader maps the .nro into a region it allocated high, so .bss is
 * ~30-50 GB and useless for this. Nothing else has been measured, so this probe
 * measures: the whole address space, region by region, plus the two services that
 * could still hand out low memory.
 *
 * WHAT IT PRINTS (to PILOT_MAP.log next to the .nro, and to stdout for nxlink)
 *   1. where this program itself lives (code, a static, a malloc block),
 *   2. every mapping and every gap: base, size, MemType_, permissions - and a
 *      summary of how much is free below 2 GB, since that is the number the port
 *      needs,
 *   3. what svcSetHeapSize(0) says the process heap address is. That is a query,
 *      it changes nothing: the process heap belongs to hbloader, and the crash
 *      reports' "Stack Region: 000000001688c000-..." (a 1 MB stack hbloader
 *      allocated for the app) hints it is low. If it is, low memory does exist in
 *      this process - the question then becomes only whether the port may have any
 *      of it, which is a different conversation from whether it exists.
 *   4. ONLY if a file named PILOT_PROBE_GROWHEAP sits next to the .nro: it calls
 *      svcSetHeapSize with a real size. That is deliberately behind a marker,
 *      because growing the process heap remaps it and the kernel does not preserve
 *      the old contents - hbmenu's own heap lives there, and hbmenu is the process
 *      this probe runs in. It answers "can we get N MB low, at all", and it is
 *      allowed to make the return to hbmenu ugly.
 *
 * <switch.h> comes first for the same reason as in SRC/switch_nxlink.c: this file
 * does not include pilots_fwd.h, so the generated alias block (#define offset,
 * #define Format) is not live while libnx's declarations are parsed.
 */

#include <switch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOW_LIMIT 0x80000000ULL          /* the int round trip stops working here */
#define WANTED    (12ULL * 1024 * 1024)  /* the video pool asked for 10098560 bytes */

static FILE *g_Log;

static void P(const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  if ( g_Log )
  {
    va_start(ap, fmt);
    vfprintf(g_Log, fmt, ap);
    va_end(ap);
    fflush(g_Log);
  }
}

static const char *MemTypeName(u32 type)
{
  switch ( type )
  {
    case MemType_Unmapped:          return "Unmapped";
    case MemType_Io:                return "Io";
    case MemType_Normal:            return "Normal";
    case MemType_CodeStatic:        return "CodeStatic";
    case MemType_CodeMutable:       return "CodeMutable";
    case MemType_Heap:              return "Heap";
    case MemType_SharedMem:         return "SharedMem";
    case MemType_WeirdMappedMem:    return "WeirdMappedMem";
    case MemType_ModuleCodeStatic:  return "ModuleCodeStatic";
    case MemType_ModuleCodeMutable: return "ModuleCodeMutable";
    case MemType_MappedMemory:      return "MappedMemory";
    case MemType_ThreadLocal:       return "ThreadLocal";
    case MemType_TransferMem:       return "TransferMem";
    case MemType_ProcessMem:        return "ProcessMem";
    case MemType_Reserved:          return "Reserved";
    default:                        return "?";
  }
}

/* Walks the address space the only way it can be walked: svcQueryMemory answers for
 * the region containing an address, so base+size steps to the next one. */
static void DumpMap(void)
{
  u64 addr = 0;
  int i;
  u64 freeBelowLow = 0, biggestBelowLow = 0, biggestBelowLowAt = 0;
  u64 freeTotal = 0, mappedTotal = 0;

  P("--- map ---\n");
  for ( i = 0; i < 4096; ++i )
  {
    MemoryInfo mi;
    u32 page = 0;

    if ( R_FAILED(svcQueryMemory(&mi, &page, addr)) )
    {
      P("svcQueryMemory(0x%llX) failed, walk stops here\n", (unsigned long long)addr);
      break;
    }
    if ( mi.size == 0 )
      break;

    if ( mi.type == MemType_Unmapped )
    {
      freeTotal += mi.size;
      if ( mi.addr < LOW_LIMIT )
      {
        freeBelowLow += mi.size;
        if ( mi.size > biggestBelowLow )
        {
          biggestBelowLow = mi.size;
          biggestBelowLowAt = mi.addr;
        }
      }
      /* Gaps are the interesting part below 2 GB, so print those in full and only
         the big ones elsewhere - a full walk is otherwise thousands of lines. */
      if ( mi.addr < LOW_LIMIT || mi.size >= WANTED )
        P("free 0x%012llX +0x%09llX %s\n",
          (unsigned long long)mi.addr, (unsigned long long)mi.size, MemTypeName(mi.type));
    }
    else
    {
      mappedTotal += mi.size;
      P("map  0x%012llX +0x%09llX %-18s perm=0x%X attr=0x%X\n",
        (unsigned long long)mi.addr, (unsigned long long)mi.size,
        MemTypeName(mi.type), mi.perm, mi.attr);
    }
    addr = mi.addr + mi.size;
  }

  P("--- summary ---\n");
  P("mapped total       = %llu MB\n", (unsigned long long)(mappedTotal / (1024 * 1024)));
  P("free   total       = %llu MB\n", (unsigned long long)(freeTotal / (1024 * 1024)));
  P("free   below 2 GB  = %llu MB\n", (unsigned long long)(freeBelowLow / (1024 * 1024)));
  P("biggest gap below 2 GB = %llu MB at 0x%llX  (the port needs %llu MB)\n",
    (unsigned long long)(biggestBelowLow / (1024 * 1024)),
    (unsigned long long)biggestBelowLowAt, (unsigned long long)(WANTED / (1024 * 1024)));
}

int main(int argc, char **argv)
{
  void *heap0 = 0;
  void *block;
  Result rc;
  MemoryInfo mi;
  u32 page = 0;

  /* The .nro's own directory is the CWD (libnx chdir's to argv[0]'s directory), so
     the log lands next to the .nro and scripts/deploy_switch.sh can pull it. */
  g_Log = fopen("PILOT_MAP.log", "w");
  P("memmap probe built %s %s\n", __DATE__, __TIME__);
  P("argc=%d argv[0]=%s\n", argc, argc ? argv[0] : "(none)");
  for ( int i = 0; i < argc; ++i )
    P("  argv[%d] = %s\n", i, argv[i]);

  block = malloc(64);
  P("where we live:\n");
  P("  &main   = 0x%llX   (this image)\n", (unsigned long long)(uintptr_t)&main);
  P("  &g_Log  = 0x%llX   (.data/.bss of this image)\n", (unsigned long long)(uintptr_t)&g_Log);
  P("  malloc  = 0x%llX   (sbrk heap)\n", (unsigned long long)(uintptr_t)block);
  P("  stack   = 0x%llX   (a local in main)\n", (unsigned long long)(uintptr_t)&page);
  for ( int i = 0; i < argc; ++i )
    P("  argv[%d] storage = 0x%llX\n", i, (unsigned long long)(uintptr_t)argv[i]);

  DumpMap();

  P("--- the process heap (query only, changes nothing) ---\n");
  rc = svcSetHeapSize(&heap0, 0);
  P("svcSetHeapSize(0)      rc=0x%08X base=0x%llX\n",
    (unsigned)rc, (unsigned long long)(uintptr_t)heap0);
  if ( R_SUCCEEDED(rc) && heap0 )
  {
    if ( R_SUCCEEDED(svcQueryMemory(&mi, &page, (u64)(uintptr_t)heap0)) )
      P("  that address is      %s +0x%llX perm=0x%X  %s 2 GB\n",
        MemTypeName(mi.type), (unsigned long long)mi.size, mi.perm,
        mi.addr < LOW_LIMIT ? "BELOW" : "above");
  }

  /* Destructive on purpose, and only when explicitly asked for: see the header. */
  {
    FILE *marker = fopen("PILOT_PROBE_GROWHEAP", "r");
    if ( marker )
    {
      void *grown = 0;
      fclose(marker);
      P("--- PILOT_PROBE_GROWHEAP present: asking for a real heap (may break the exit to hbmenu) ---\n");
      rc = svcSetHeapSize(&grown, 64ULL * 1024 * 1024);
      P("svcSetHeapSize(64 MB) rc=0x%08X base=0x%llX\n",
        (unsigned)rc, (unsigned long long)(uintptr_t)grown);
      if ( R_SUCCEEDED(rc) && grown )
      {
        memset(grown, 0x5A, 4096);
        P("  wrote 4 KB at the new heap base, first byte reads back 0x%02X, %s 2 GB\n",
          ((unsigned char *)grown)[0],
          (u64)(uintptr_t)grown < LOW_LIMIT ? "BELOW" : "above");
      }
    }
    else
      P("(growth test skipped: no PILOT_PROBE_GROWHEAP next to the .nro)\n");
  }

  P("--- done ---\n");
  if ( g_Log )
    fclose(g_Log);
  return 0;
}
