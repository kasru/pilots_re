/*
 * Fake Windows headers for the Linux build. CMake adds SRC/winshim to the include
 * path ONLY on Linux; on Windows the real SDK headers are found first (the .bat
 * builds never see this directory). Every fake header funnels into win32_shim.h,
 * so game sources keep their original #include <windows.h> lines untouched.
 */
#include "../win32_shim.h"
