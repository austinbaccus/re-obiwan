/* Boot the recompiled game: load the XBE, set up the kernel, run `entry`.

   Lives in obiwan_core.dll and is entered from the fixed-address loader
   (tests/loader.c built as obiwan.exe), which owns the XBE's address range.

   obiwan.exe <default.xbe> [--trace]
   Game files are read from the folder containing default.xbe; the emulated
   hard disk is xbox_hdd next to the executable, or RECOMP_HDD. */
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
#include "render.h"

void host_vcall(Ctx *ctx, uint32_t id); /* hooks_dsound.c */

#pragma comment(lib, "dbghelp.lib")

/* The generated C carries each guest instruction as a comment on the line
   before its code, so the PDB line of a host address names the guest
   instruction: find the file and line, then scan back for the comment and
   the enclosing L_ label. */
static void print_guest_location(HANDLE proc, DWORD64 pc) {
    IMAGEHLP_LINE64 line = {sizeof line};
    DWORD disp = 0;
    if (!SymGetLineFromAddr64(proc, pc, &disp, &line)) return;
    FILE *f = fopen(line.FileName, "r");
    if (!f) { fprintf(stderr, "         %s:%lu\n", line.FileName, line.LineNumber); return; }
    static char buf[4096], comment[256] = "", label[64] = "";
    for (DWORD n = 1; n <= line.LineNumber && fgets(buf, sizeof buf, f); n++) {
        const char *c = strstr(buf, "/* ");
        if (c) { strncpy(comment, c, sizeof comment - 1); comment[strcspn(comment, "\r\n")] = 0; }
        if (buf[0] == 'L' && buf[1] == '_') { strncpy(label, buf, sizeof label - 1); label[strcspn(label, ":\r\n")] = 0; }
    }
    fclose(f);
    fprintf(stderr, "         guest %s, last block %s  (%s:%lu)\n", comment, label, strrchr(line.FileName, '\\') ? strrchr(line.FileName, '\\') + 1 : line.FileName, line.LineNumber);
}

static void init_symbols(void) {
    static int done;
    if (done++) return;
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
}

/* The guest registers of a thread and the return addresses on its guest stack */
static void print_guest_state(Ctx *g) {
    fprintf(stderr, "    guest eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n",
            RD32(g->r + REG_EAX), RD32(g->r + REG_ECX), RD32(g->r + REG_EDX), RD32(g->r + REG_EBX),
            RD32(g->r + REG_ESP), RD32(g->r + REG_EBP), RD32(g->r + REG_ESI), RD32(g->r + REG_EDI));
    uint32_t gesp = RD32(g->r + REG_ESP);
    if (gesp < 0x10000 || gesp >= 0x4000000) return; /* no guest stack (yet) */
    fprintf(stderr, "    guest stack:");
    for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", RD32(MEM(gesp + 4 * i)));
    fprintf(stderr, "\n    guest return addresses:");
    int n = 0;
    for (uint32_t a = gesp; a < gesp + 0x800 && n < 16; a += 4) {
        uint32_t v = RD32(MEM(a));
        const FuncEntry *e = v >= 0x10000 && v < 0x530000 ? rt_lookup_containing(v) : NULL;
        if (e && v != e->addr) { fprintf(stderr, " %08x(%s)", v, e->name); n++; }
    }
    fprintf(stderr, "\n");
}

/* Print a host stack. Recompiled functions are named after their guest
   address (f_XXXXXXXX), so the symbol says which guest function it was. */
static void print_host_stack(HANDLE thread, const CONTEXT *c) {
    HANDLE proc = GetCurrentProcess();
    init_symbols();
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
        if (depth < 4) print_guest_location(proc, frame.AddrPC.Offset);
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, proc, thread, &frame, &cc, NULL,
                         SymFunctionTableAccess64, SymGetModuleBase64, NULL) || !frame.AddrPC.Offset)
            break;
    }
    fflush(stderr);
}

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
    Ctx *g = kernel_current_ctx();
    if (g) print_guest_state(g);
    /* RECOMP_CRASHDUMP="addr:len,addr:len": guest memory to print (hex) */
    for (const char *p = getenv("RECOMP_CRASHDUMP"); p && *p; ) {
        unsigned a = 0, n = 0;
        if (sscanf(p, "%x:%x", &a, &n) == 2)
            for (unsigned i = 0; i < n; i += 4) {
                if (i % 32 == 0) fprintf(stderr, "%s    %08x:", i ? "\n" : "", a + i);
                fprintf(stderr, " %08x", RD32(MEM(a + i)));
            }
        fprintf(stderr, "\n");
        p = strchr(p, ',');
        if (!p) break;
        p++;
    }
    print_host_stack(GetCurrentThread(), c);
    ExitProcess(21);
}

/* RECOMP_STALL=seconds: when no frame has been presented for that long,
   print where every guest thread is and exit. */
static void dump_thread(HANDLE thread, Ctx *g, const char *what, void *arg) {
    (void)arg;
    fprintf(stderr, "\n  thread %s:\n", what);
    if (SuspendThread(thread) == (DWORD)-1) { fprintf(stderr, "    cannot suspend (error %lu)\n", GetLastError()); return; }
    CONTEXT c = {0};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(thread, &c)) {
        if (g) print_guest_state(g);
        print_host_stack(thread, &c);
    } else fprintf(stderr, "    cannot read the context (error %lu)\n", GetLastError());
    ResumeThread(thread);
}

static DWORD WINAPI stall_watchdog(LPVOID p) {
    int limit = (int)(intptr_t)p;
    uint32_t last = d3d_frame_count();
    for (int quiet = 0, dumps = 0;;) {
        Sleep(1000);
        uint32_t now = d3d_frame_count();
        if (now != last) { last = now; quiet = 0; continue; }
        if (++quiet < limit) continue;
        fprintf(stderr, "\n*** no frame presented for %d s (frame %u): guest threads\n", limit, now);
        kernel_each_thread(dump_thread, NULL);
        fflush(stderr);
        if (++dumps == 3) ExitProcess(22); /* a few samples show whether it moves */
        quiet = 0;
    }
}

#define GUEST_RAM_START 0x00600000u
#define GUEST_RAM_SIZE  0x03A00000u /* up to 64 MB, like the console */

static int run(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: obiwan <default.xbe> [--trace]\n"); return 2; }
    for (int i = 2; i < argc; i++) if (strcmp(argv[i], "--trace") == 0) g_trace = 1;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(crash_filter);
    if (getenv("RECOMP_TRACE")) g_trace = 1;
    if (getenv("RECOMP_STALL"))
        CreateThread(NULL, 0, stall_watchdog, (void *)(intptr_t)atoi(getenv("RECOMP_STALL")), 0, NULL);
    if (getenv("RECOMP_TRACE_CALLS")) g_trace_calls = atoi(getenv("RECOMP_TRACE_CALLS"));

    XbeInfo xbe;
    /* Host threads must exist before the XBE replaces the loader's PE header
       at 0x10000: Windows reads it when creating threads (see kernel.c). */
    /* The window and the Direct3D device are created before the XBE is loaded:
       afterwards Direct3D's device creation blocks (its driver threads never
       come up in this process), see render_d3d9.c. */
    if (!getenv("RECOMP_HEADLESS")) r_init(640, 480);
    kernel_prepare_threads();
    DWORD old;
    VirtualProtect((void *)0x10000, 0x51a000, PAGE_EXECUTE_READWRITE, &old);
    /* Windows reads the executable's PE headers through the PEB's
       ImageBaseAddress when creating threads (default stack sizes), so once
       the XBE has replaced them at 0x10000, CreateThread fails with error 193
       for everyone, including Direct3D's driver. Keep a copy of the headers
       and point the PEB at it. */
    void *headers = VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    memcpy(headers, (void *)0x10000, 0x1000);
    if (!xbe_load(argv[1], &xbe)) return 3;
    *(void **)((char *)__readfsdword(0x30) + 8) = headers; /* PEB->ImageBaseAddress */
    {
        HANDLE t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Sleep, NULL, 0, NULL);
        if (t) CloseHandle(t); else fprintf(stderr, "obiwan: CreateThread still fails after the image load (error %lu)\n", GetLastError());
    }
    kmem_init(GUEST_RAM_START, GUEST_RAM_SIZE);

    char game_dir[MAX_PATH], hdd_dir[MAX_PATH], exe[MAX_PATH];
    strncpy(game_dir, argv[1], MAX_PATH - 1);
    char *slash = strrchr(game_dir, '\\');
    if (!slash) slash = strrchr(game_dir, '/');
    if (slash) *slash = 0; else strcpy(game_dir, ".");
    GetModuleFileNameA(NULL, exe, sizeof exe);
    slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    if (getenv("RECOMP_HDD")) snprintf(hdd_dir, sizeof hdd_dir, "%s", getenv("RECOMP_HDD")); /* a scratch disk for test runs */
    else snprintf(hdd_dir, sizeof hdd_dir, "%s\\xbox_hdd", exe);
    kernel_init(xbe.kernel_thunk, game_dir, hdd_dir);
    g_kernel_call = kernel_call;
    g_host_vcall = host_vcall;
    fprintf(stderr, "obiwan: image %08x..%08x entry %08x, game files %s, hdd %s\n", xbe.base, xbe.base + xbe.size,
            xbe.entry, game_dir, hdd_dir);

    /* The XBE entry point runs on the initial thread with no SystemRoutine:
       it creates the real main thread and returns. */
    kernel_run_thread(0, 0, 0x10000, 0x14, xbe.entry);
    fprintf(stderr, "obiwan: entry returned; waiting for the game thread\n");
    /* the window belongs to this thread: pump its messages while the game runs */
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
        Sleep(10);
    }
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
