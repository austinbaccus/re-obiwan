/* Boot the recompiled game: load the XBE, set up the kernel, run `entry`.

   Lives in obiwan_core.dll and is entered from the fixed-address loader
   (tests/loader.c built as obiwan.exe), which owns the XBE's address range.

   obiwan.exe [default.xbe] [--trace]
   Without a path, extracted\default.xbe (or default.xbe) next to or above
   the executable. Game files are read from the folder containing
   default.xbe; the emulated hard disk is xbox_hdd next to the executable,
   or RECOMP_HDD.

   Obi-Wan.exe is the same loader built as a windowed program, for a
   double-click: no console, settings from obiwan.ini next to it, output in
   obiwan.log, and start-up errors and crashes in a message box. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>

#include <dbghelp.h>

#include "guest.h"
#include "kernel.h"
#include "xbe_image.h"
#include "render.h"
#include "audio.h"

void host_vcall(Ctx *ctx, uint32_t id); /* hooks_dsound.c */

#include <mmsystem.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "winmm.lib")

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

/* host_watch(addr, len): a hardware data watchpoint (debug register 0) on
   the calling thread. Each write to the guest bytes prints the recompiled
   instruction that wrote them, the guest registers and the guest return
   addresses (RECOMP_WATCHMAX hits, default 100). len is 1, 2 or 4, and addr
   must be a multiple of it. For debugging from a TEMP probe. */
#define WATCH_ARM 0xE0A7C4E1u
static uint32_t g_watch_addr, g_watch_len;
static int g_watch_left;

static LONG CALLBACK watch_handler(EXCEPTION_POINTERS *e) {
    CONTEXT *c = e->ContextRecord;
    if (e->ExceptionRecord->ExceptionCode == WATCH_ARM) {
        c->Dr0 = g_watch_addr;
        c->Dr7 = (c->Dr7 & ~0xf0003ul) | 1 | 1ul << 16 /* on write */ | (unsigned long)(g_watch_len == 4 ? 3 : g_watch_len - 1) << 18;
        c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(c->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    if (g_watch_left-- > 0) {
        init_symbols();
        char buf[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof *sym;
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        uint32_t v = 0;
        memcpy(&v, MEM(g_watch_addr), g_watch_len);
        fprintf(stderr, "watch frame %u: %08x = %08x by %s\n", d3d_frame_count(), g_watch_addr, v,
                SymFromAddr(GetCurrentProcess(), c->Eip, &disp, sym) ? sym->Name : "?");
        print_guest_location(GetCurrentProcess(), c->Eip);
        Ctx *g = kernel_current_ctx();
        if (g) print_guest_state(g);
    } else {
        c->Dr7 &= ~3ul; /* done */
    }
    c->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
    return EXCEPTION_CONTINUE_EXECUTION;
}

void host_watch(uint32_t addr, uint32_t len) {
    static int installed;
    if (!installed++) {
        AddVectoredExceptionHandler(1, watch_handler);
        g_watch_left = getenv("RECOMP_WATCHMAX") ? atoi(getenv("RECOMP_WATCHMAX")) : 100;
    }
    g_watch_addr = addr;
    g_watch_len = len;
    fprintf(stderr, "watch: %u bytes at %08x\n", len, addr);
    RaiseException(WATCH_ARM, 0, 0, NULL);
}

/* ---- launching without a terminal (#23) -------------------------------------- */
static int g_windowed; /* entered as Obi-Wan.exe */

static void exe_dir(char *out, size_t n) {
    GetModuleFileNameA(NULL, out, (DWORD)n);
    char *s = strrchr(out, '\\');
    if (s) *s = 0;
}

/* to stderr, and in the windowed build a message box too */
static void notice(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", msg);
    fflush(stderr);
    if (g_windowed) MessageBoxA(NULL, msg, "Star Wars: Obi-Wan", MB_OK | MB_ICONERROR | MB_TOPMOST | MB_SETFOREGROUND);
}

/* stderr to obiwan.log next to the executable, unless it already goes
   somewhere (a redirect); the relaunch into a level appends */
static void open_log(void) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    char path[MAX_PATH];
    exe_dir(path, sizeof path);
    strncat(path, "\\obiwan.log", sizeof path - strlen(path) - 1);
    if (freopen(path, getenv("RECOMP_LOG_OPEN") ? "a" : "w", stderr)) _putenv_s("RECOMP_LOG_OPEN", "1");
}

static const char INI_TEMPLATE[] =
    "; Settings for Obi-Wan.exe, the double-click launch. obiwan.exe in a\n"
    "; terminal ignores this file and takes RECOMP_* environment variables;\n"
    "; a variable that is set in the environment also wins over this file.\n"
    "; Remove the ; in front of a line to use it.\n"
    "\n"
    "; The game's default.xbe, if it is not extracted\\default.xbe next to\n"
    "; or above this folder. Relative to this folder.\n"
    ";xbe=..\\..\\extracted\\default.xbe\n"
    "\n"
    "; Picture size, and a borderless window over the whole primary monitor\n"
    ";RECOMP_RES=1920x1440\n"
    ";RECOMP_FULLSCREEN=1\n"
    "\n"
    "; The performance overlay from the start (F3 toggles it)\n"
    ";RECOMP_FPS=1\n"
    "\n"
    "; Straight into a level (launch files are in build\\launches)\n"
    ";RECOMP_LAUNCH=..\\launches\\L01_1.bin\n";

/* obiwan.ini: xbe=path (relative to the executable's folder) and RECOMP_*
   settings for those not already in the environment; written as a
   commented template when missing */
static void read_ini(char *xbe, size_t n) {
    char dir[MAX_PATH], path[MAX_PATH], line[1024];
    exe_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%s\\obiwan.ini", dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        if ((f = fopen(path, "w")) != NULL) { fputs(INI_TEMPLATE, f); fclose(f); }
        return;
    }
    while (fgets(line, sizeof line, f)) {
        char *key = line, *eq, *end;
        while (isspace((unsigned char)*key)) key++;
        if (!*key || *key == ';' || *key == '#' || *key == '[' || !(eq = strchr(key, '='))) continue;
        char *val = eq + 1;
        for (end = eq; end > key && isspace((unsigned char)end[-1]); end--) {}
        *end = 0;
        while (isspace((unsigned char)*val)) val++;
        for (end = val + strlen(val); end > val && isspace((unsigned char)end[-1]); end--) {}
        *end = 0;
        if (!_stricmp(key, "xbe")) {
            int absolute = val[0] == '\\' || (val[0] && val[1] == ':');
            snprintf(xbe, n, absolute ? "%s%s" : "%s\\%s", absolute ? "" : dir, val);
        } else if (!_strnicmp(key, "RECOMP_", 7)) {
            if (!getenv(key)) _putenv_s(key, val);
        } else fprintf(stderr, "obiwan.ini: %s is not a setting\n", key);
    }
    fclose(f);
}

/* extracted\default.xbe or default.xbe in the executable's folder or above */
static int find_xbe(char *out, size_t n) {
    char dir[MAX_PATH];
    exe_dir(dir, sizeof dir);
    for (int up = 0; up < 6; up++) {
        snprintf(out, n, "%s\\extracted\\default.xbe", dir);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
        snprintf(out, n, "%s\\default.xbe", dir);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 1;
        char *s = strrchr(dir, '\\');
        if (!s) break;
        *s = 0;
    }
    return 0;
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
    if (g_windowed) notice("The game stopped with an error (exception %08lx). The details are in obiwan.log next to Obi-Wan.exe.",
                           e->ExceptionRecord->ExceptionCode);
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

/* RECOMP_PROFILE=seconds: after that long, sample every guest thread's host
   instruction pointer about once a millisecond for ten seconds, then print
   the functions found running most often (self time; recompiled functions
   are named after their guest address) and the modules outside the game. */
#define PROFILE_MAX 200000
static DWORD64 *g_samples;
static int g_nsamples;

static void sample_thread(HANDLE thread, Ctx *g, const char *what, void *arg) {
    (void)g; (void)what; (void)arg;
    if (SuspendThread(thread) == (DWORD)-1) return;
    CONTEXT c = {0};
    c.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext(thread, &c) && g_nsamples < PROFILE_MAX) g_samples[g_nsamples++] = c.Eip;
    ResumeThread(thread);
}

static int cmp_count(const void *a, const void *b) {
    const DWORD64 *x = a, *y = b;
    return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}

static DWORD WINAPI profiler(LPVOID p) {
    Sleep((DWORD)(intptr_t)p * 1000);
    g_samples = malloc(PROFILE_MAX * sizeof *g_samples);
    timeBeginPeriod(1);
    uint32_t f0 = d3d_frame_count();
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 10000) { kernel_threads(sample_thread, NULL); Sleep(1); }
    uint32_t frames = d3d_frame_count() - f0;
    init_symbols();
    /* (function start, count) pairs */
    static DWORD64 fn[PROFILE_MAX][2];
    int nf = 0;
    HANDLE proc = GetCurrentProcess();
    for (int i = 0; i < g_nsamples; i++) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {0};
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof *sym;
        sym->MaxNameLen = 255;
        DWORD64 disp = 0, start = SymFromAddr(proc, g_samples[i], &disp, sym) ? sym->Address : SymGetModuleBase64(proc, g_samples[i]);
        int k = 0;
        while (k < nf && fn[k][0] != start) k++;
        if (k == nf) { fn[nf][0] = start; fn[nf][1] = 0; nf++; }
        fn[k][1]++;
    }
    qsort(fn, nf, sizeof fn[0], cmp_count);
    fprintf(stderr, "\nprofile: %d samples over 10 s, %u frames presented\n", g_nsamples, frames);
    for (int k = 0; k < nf && k < 40; k++) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {0}, mod[MAX_PATH] = "?";
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        sym->SizeOfStruct = sizeof *sym;
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        HMODULE m = NULL;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)(uintptr_t)fn[k][0], &m);
        if (m) GetModuleFileNameA(m, mod, sizeof mod);
        const char *name = SymFromAddr(proc, fn[k][0], &disp, sym) && !disp ? sym->Name : strrchr(mod, '\\') ? strrchr(mod, '\\') + 1 : mod;
        fprintf(stderr, "profile: %5.1f%%  %s\n", 100.0 * fn[k][1] / g_nsamples, name);
    }
    fflush(stderr);
    return 0;
}

#define GUEST_RAM_START 0x00600000u
#define GUEST_RAM_SIZE  0x03A00000u /* up to 64 MB, like the console */

static int run(int argc, char **argv) {
    char xbe_path[MAX_PATH] = "", here[MAX_PATH];
    if (g_windowed) { open_log(); read_ini(xbe_path, sizeof xbe_path); }
    int first_option = 1;
    if (argc >= 2 && strncmp(argv[1], "--", 2) != 0) { snprintf(xbe_path, sizeof xbe_path, "%s", argv[1]); first_option = 2; }
    if (!xbe_path[0] && !find_xbe(xbe_path, sizeof xbe_path)) {
        exe_dir(here, sizeof here);
        notice("usage: obiwan [default.xbe] [--trace]\n\nNo extracted\\default.xbe was found in %s or the folders above it. "
               "Give the path to default.xbe%s.", here, g_windowed ? " in obiwan.ini (xbe=...)" : "");
        return 2;
    }
    for (int i = first_option; i < argc; i++) if (strcmp(argv[i], "--trace") == 0) g_trace = 1;
    SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
    SetUnhandledExceptionFilter(crash_filter);
    if (getenv("RECOMP_TRACE")) g_trace = 1;
    if (getenv("RECOMP_STALL"))
        CreateThread(NULL, 0, stall_watchdog, (void *)(intptr_t)atoi(getenv("RECOMP_STALL")), 0, NULL);
    if (getenv("RECOMP_TRACE_CALLS")) g_trace_calls = atoi(getenv("RECOMP_TRACE_CALLS"));
    if (getenv("RECOMP_PROFILE"))
        CreateThread(NULL, 0, profiler, (void *)(intptr_t)atoi(getenv("RECOMP_PROFILE")), 0, NULL);

    XbeInfo xbe;
    /* Host threads must exist before the XBE replaces the loader's PE header
       at 0x10000: Windows reads it when creating threads (see kernel.c). */
    /* The window and the Direct3D device are created before the XBE is loaded:
       afterwards Direct3D's device creation blocks (its driver threads never
       come up in this process), see render_d3d9.c. */
    if (!getenv("RECOMP_HEADLESS")) r_init(640, 480);
    au_init(); /* XAudio2's threads, likewise */
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
    if (!xbe_load(xbe_path, &xbe)) {
        notice("The game could not start: %s could not be loaded as default.xbe.", xbe_path);
        return 3;
    }
    *(void **)((char *)__readfsdword(0x30) + 8) = headers; /* PEB->ImageBaseAddress */
    {
        HANDLE t = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Sleep, NULL, 0, NULL);
        if (t) CloseHandle(t); else fprintf(stderr, "obiwan: CreateThread still fails after the image load (error %lu)\n", GetLastError());
    }
    kmem_init(GUEST_RAM_START, GUEST_RAM_SIZE);

    char game_dir[MAX_PATH], hdd_dir[MAX_PATH], exe[MAX_PATH];
    strncpy(game_dir, xbe_path, MAX_PATH - 1);
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
    native_init();
    widescreen_init();
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

/* Obi-Wan.exe: the same, as a windowed program */
__declspec(dllexport) void game_main_windowed(void) {
    g_windowed = 1;
    game_main();
}
