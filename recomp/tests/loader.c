/* Tiny loader that owns the XBE's address range.

   Windows maps system data at the bottom of a new 32-bit process, so a normal
   program cannot get 0x00010000..0x005295E0 for the XBE. An executable whose
   own image is fixed at 0x00010000 can: this one links there with a large
   empty section covering the whole XBE range, then loads the real harness
   (difftest_core.dll, at a normal address) and hands over. The harness
   overwrites this image with the XBE, including the code below, so control
   never returns here: difftest_main ends the process itself.

   Built without the CRT; see CMakeLists.txt for the link options. */
#include <windows.h>

#pragma section(".xbe", read, execute)
/* 0x00013000.. with the loader's own headers and code in the first pages:
   the image ends past 0x005295E0, the end of the XBE. */
#ifndef LOADER_RESERVE
#define LOADER_RESERVE 0x51a000 /* just the XBE; the game build covers all 64 MB of guest RAM */
#endif
__declspec(allocate(".xbe")) unsigned char g_xbe_range[LOADER_RESERVE];

#ifndef LOADER_DLL
#define LOADER_DLL "difftest_core.dll"
#define LOADER_ENTRY "difftest_main"
#endif

typedef void (*MainFn)(void);

void loader_start(void) {
    (void)*(volatile unsigned char *)g_xbe_range; /* keep the section */
    HMODULE core = LoadLibraryA(LOADER_DLL);
    MainFn main_fn = core ? (MainFn)GetProcAddress(core, LOADER_ENTRY) : NULL;
    if (!main_fn) ExitProcess(4);
    main_fn(); /* does not return */
    ExitProcess(5);
}
