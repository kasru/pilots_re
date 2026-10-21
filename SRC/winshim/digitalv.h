/* Fake digitalv.h for the Linux build - see windows.h in this directory.
   host.c includes it for MCI_DGV_STATUS_HWND, which win32_shim.h defines
   (real Digitalv.h value 0x00004001) along with the rest of the MCI subset. */
#include "../win32_shim.h"
