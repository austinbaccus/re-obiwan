/* Boot the recompiled game: load the XBE, set up the kernel, run `entry`.

   Lives in obiwan_core.dll and is entered from the fixed-address loader
   (tests/loader.c built as obiwan.exe), which owns the XBE's address range.

   obiwan.exe <default.xbe> [--trace]
   Game files are read from the folder containing default.xbe; the emulated
   hard disk is build/xbox_hdd next to the executable. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#include <dbghelp.h>

#include "guest.h"
#include "kernel.h"
#include "xbe_image.h"

void host_vcall(Ctx *ctx, uint32_t id); /* hooks_dsound.c */

#pragma comment(lib, "dbghelp.lib")

/* Print where a host crash happened. Recompiled functions are named after
   their guest address (f_XXXXXXXX), so the symbol says which guest function
   it was. */
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *e) {
    static int in_filter;
    if (in_filter++) ExitProcess(20);
    CONTEXT *c = e->ContextRecord;
    fprintf(stderr, "\n*** host exception %08lx at %08lx (eax %08lx ecx %08lx edx %08lx esp %08lx ebp %08lx)\n",
            e->ExceptionRecord->ExceptionCode, (DWORD)(uintptr_t)e->ExceptionRecord->ExceptionAddress,
            c->Eax, c->Ecx, c->Edx, c->Esp, c->Ebp);
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, "    %s address %08lx\n", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (DWORD)e->ExceptionRecord->ExceptionInformation[1]);
    HANDLE proc = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, NULL, TRUE);
    STACKFRAME64 frame = {0};
    frame.AddrPC.Offset = c->Eip; frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = c->Ebp; frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = c->Esp; frame.AddrStack.Mode = AddrModeFlat;
    CONTEXT cc = *c;
    for (int depth = 0; depth < 40; depth++) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof *sym;
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, frame.AddrPC.Offset, &disp, sym))
            fprintf(stderr, "    #%-2d %08llx %s+0x%llx\n", depth, frame.AddrPC.Offset, sym->Name, disp);
        else
            fprintf(stderr, "    #%-2d %08llx ?\n", depth, frame.AddrPC.Offset);
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, proc, GetCurrentThread(), &frame, &cc, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !frame.AddrPC.Offset)
            break;
    }
    fflush(stderr);
    ExitProcess(21);
}

#define GUEST_RAM_START 0x00600000u
#define GUEST_RAM_SIZE  0x03A00000u /* up to 64 MB, like the console */

static int run(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: obiwan <default.xbe> [--trace]\n"); return 2; }
    for (int i = 2; i < argc; i++) if (strcmp(argv[i], "--trace") == 0) g_trace = 1;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(crash_filter);
    if (getenv("RECOMP_TRACE")) g_trace = 1;
    if (getenv("RECOMP_TRACE_CALLS")) g_trace_calls = atoi(getenv("RECOMP_TRACE_CALLS"));

    XbeInfo xbe;
    /* Host threads must exist before the XBE replaces the loader's PE header
       at 0x10000: Windows reads it when creating threads (see kernel.c). */
    kernel_prepare_threads();
    DWORD old;
    VirtualProtect((void *)0x10000, 0x51a000, PAGE_EXECUTE_READWRITE, &old);
    if (!xbe_load(argv[1], &xbe)) return 3;
    kmem_init(GUEST_RAM_START, GUEST_RAM_SIZE);

    char game_dir[MAX_PATH], hdd_dir[MAX_PATH], exe[MAX_PATH];
    strncpy(game_dir, argv[1], MAX_PATH - 1);
    char *slash = strrchr(game_dir, '\\');
    if (!slash) slash = strrchr(game_dir, '/');
    if (slash) *slash = 0; else strcpy(game_dir, ".");
    GetModuleFileNameA(NULL, exe, sizeof exe);
    slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(hdd_dir, sizeof hdd_dir, "%s\\xbox_hdd", exe);
    kernel_init(xbe.kernel_thunk, game_dir, hdd_dir);
    g_kernel_call = kernel_call;
    g_host_vcall = host_vcall;
    fprintf(stderr, "obiwan: image %08x..%08x entry %08x, game files %s, hdd %s\n", xbe.base, xbe.base + xbe.size,
            xbe.entry, game_dir, hdd_dir);

    /* The XBE entry point runs on the initial thread with no SystemRoutine:
       it creates the real main thread and returns. */
    kernel_run_thread(0, 0, 0x10000, 0x14, xbe.entry);
    fprintf(stderr, "obiwan: entry returned; waiting for the game thread\n");
    for (;;) Sleep(1000);
}

__declspec(dllexport) void game_main(void) {
    int argc;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = malloc(sizeof(char *) * argc);
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc(n);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    int rc = run(argc, argv);
    fflush(stderr);
    ExitProcess((UINT)rc);
}
