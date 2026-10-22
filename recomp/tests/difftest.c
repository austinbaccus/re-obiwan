/* Differential test: run each recompiled function and the original x86 code
   from the same starting state, and compare registers, x87 ST0 and memory.

   32-bit Windows only. The XBE is mapped at its own addresses in this
   process, so the original code runs natively and guest addresses equal host
   addresses (g_membase = 0). Nothing from the game is compiled in: the XBE is
   read at run time and the recompiled code is generated into the build tree.

   difftest.exe <default.xbe>                   run every function, one child process each
   difftest.exe <default.xbe> --one <addr> <seed>  run a single function (used by the driver)
*/
#include <windows.h>
#include <shellapi.h>
#include <intrin.h>
#include <stdio.h>
#include <stdlib.h>

#include "guest.h"

#define SCRATCH_SIZE 0x10000
#define STACK_SIZE 0x10000
#define ARG_AREA 0x1000
#define TIMEOUT_MS 5000

static uint8_t *g_image;      /* the XBE image at its link address */
static uint32_t g_image_size;
static uint8_t *g_pristine;   /* copy of the image right after loading */

/* ---- XBE loading ------------------------------------------------------ */
static int load_xbe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n);
    fread(file, 1, n, f);
    fclose(f);
    uint32_t base = RD32(file + 0x104), headers = RD32(file + 0x108);
    g_image_size = RD32(file + 0x10c);
    /* The loader executable's image covers this range (see loader.c). */
    g_image = (uint8_t *)(uintptr_t)base;
    DWORD old;
    if (!VirtualProtect(g_image, g_image_size, PAGE_EXECUTE_READWRITE, &old)) {
        fprintf(stderr, "cannot make %08x..%08x writable (error %lu); run through difftest.exe\n",
                base, base + g_image_size, GetLastError());
        return 0;
    }
    memcpy(g_image, file, headers);
    uint32_t nsec = RD32(file + 0x11c), sec = RD32(file + 0x120) - base;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = file + sec + i * 56;
        uint32_t va = RD32(s + 4), raw = RD32(s + 12), rsize = RD32(s + 16);
        memcpy(g_image + (va - base), file + raw, rsize);
    }
    free(file);
    g_pristine = malloc(g_image_size);
    memcpy(g_pristine, g_image, g_image_size);
    return 1;
}

/* ---- deterministic starting state ------------------------------------ */
static uint32_t g_rng;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static uint8_t *g_scratch;
static uint32_t random_value(void) {
    switch (rnd() % 4) {
    case 0: return rnd() & 0xff;                                              /* small int */
    case 1: { float v = (float)((int)(rnd() % 20001) - 10000) / 100.0f; uint32_t b; memcpy(&b, &v, 4); return b; }
    default: return (uint32_t)(uintptr_t)(g_scratch + SCRATCH_SIZE / 4 + ((rnd() % (SCRATCH_SIZE / 2)) & ~15u)); /* pointer */
    }
}

typedef struct {
    uint32_t reg[8]; /* EAX ECX EDX EBX ESP EBP ESI EDI */
    uint8_t *stack;
} Start;

static void make_start(Start *s, uint8_t *stack, uint32_t seed) {
    g_rng = seed * 2654435761u + 1;
    memcpy(g_image, g_pristine, g_image_size);
    for (uint32_t i = 0; i < SCRATCH_SIZE; i += 4) WR32(g_scratch + i, random_value());
    for (uint32_t i = 0; i < STACK_SIZE; i += 4) WR32(stack + i, random_value());
    s->stack = stack;
    for (int i = 0; i < 8; i++) s->reg[i] = random_value();
    s->reg[4] = (uint32_t)(uintptr_t)(stack + STACK_SIZE - ARG_AREA); /* ESP -> return address */
}

typedef struct {
    int fault;          /* 0 ok, 1 access violation / other exception, 2 guest trap */
    uint32_t code;
    uint32_t trap_addr;
    uint32_t reg[8];
    int st_depth;       /* x87 values left on the stack (native only) */
    double st0;
} Result;

/* ---- native run -------------------------------------------------------- */
static uint32_t n_in[8], n_out[8], n_target, n_host_esp, n_fsw;
static uint16_t n_cw = 0x027f;
static double n_st0;
static uint32_t n_mxcsr = 0x1f80;

static __declspec(naked) void native_call(void) {
    __asm {
        pushad
        mov n_host_esp, esp
        fninit
        fldcw n_cw
        ldmxcsr n_mxcsr
        cld
        mov esp, n_in + 16
        mov dword ptr [esp], offset back   ; the return address slot
        mov eax, n_in + 0
        mov ecx, n_in + 4
        mov edx, n_in + 8
        mov ebx, n_in + 12
        mov ebp, n_in + 20
        mov esi, n_in + 24
        mov edi, n_in + 28
        jmp dword ptr [n_target]
    back:
        mov n_out + 0, eax
        mov n_out + 4, ecx
        mov n_out + 8, edx
        mov n_out + 12, ebx
        mov n_out + 16, esp
        mov n_out + 20, ebp
        mov n_out + 24, esi
        mov n_out + 28, edi
        fnstsw ax
        mov n_fsw, eax
        fstp qword ptr n_st0
        fninit
        mov esp, n_host_esp
        popad
        ret
    }
}

/* A fault in native guest code happens on the guest stack, where the normal
   SEH chain cannot be used safely, so a vectored handler catches it and
   resumes at native_fault, which unwinds back to native_call's caller. */
static volatile LONG n_active;
static uint32_t n_fault;

static __declspec(naked) void native_fault(void) {
    __asm {
        fninit
        cld
        mov esp, n_host_esp
        popad
        ret
    }
}

static LONG CALLBACK native_veh(EXCEPTION_POINTERS *e) {
    if (!InterlockedExchange(&n_active, 0)) return EXCEPTION_CONTINUE_SEARCH;
    n_fault = e->ExceptionRecord->ExceptionCode;
    e->ContextRecord->Eip = (DWORD)(uintptr_t)native_fault;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void run_native(uint32_t addr, const Start *s, Result *r) {
    static PVOID veh;
    if (!veh) veh = AddVectoredExceptionHandler(1, native_veh);
    memcpy(n_in, s->reg, sizeof n_in);
    n_target = addr;
    n_fault = 0;
    memset(r, 0, sizeof *r);
    /* guest code may leave an SEH frame registered on the guest stack if it
       faults; put the host's chain back afterwards */
    DWORD saved_seh = __readfsdword(0);
    InterlockedExchange(&n_active, 1);
    native_call();
    InterlockedExchange(&n_active, 0);
    __writefsdword(0, saved_seh);
    if (n_fault) {
        r->fault = 1;
        r->code = n_fault;
        return;
    }
    memcpy(r->reg, n_out, sizeof r->reg);
    r->st_depth = (8 - ((n_fsw >> 11) & 7)) & 7;
    r->st0 = n_st0;
}

/* ---- recompiled run ---------------------------------------------------- */
static void run_recompiled(GuestFn fn, const Start *s, Result *r) {
    static Ctx ctx;
    jmp_buf jb;
    memset(&ctx, 0, sizeof ctx);
    for (int i = 0; i < 8; i++) WR32(ctx.r + 4 * i, s->reg[i]);
    WR32(MEM(s->reg[4]), 0xdeadbee0u); /* return address (only its slot matters) */
    WR16(ctx.r + REG_FPUCW, 0x027f);
    /* fs: points at a private copy of this thread's TEB, so SEH frame links
       and stack-base reads see the same values as the native run without the
       guest's fs:[0] writes reaching the host's exception chain */
    static _Alignas(16) uint8_t teb_copy[0x1000];
    memcpy(teb_copy, (void *)(uintptr_t)__readfsdword(0x18), sizeof teb_copy);
    WR32(ctx.r + REG_FS_OFFSET, (uint32_t)(uintptr_t)teb_copy);
    for (int i = 0; i < 8; i++) WRF80SLOT(ctx.r + REG_ST0 + 0x10 * i, bits_f64(0xfff8000000000000ull));
    WR32(ctx.r + REG_MXCSR, 0x1f80);
    ctx.trap_jmp = &jb;
    memset(r, 0, sizeof *r);
    if (setjmp(jb)) { r->fault = 2; r->code = ctx.trap; r->trap_addr = ctx.trap_addr; return; }
    __try {
        fn(&ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r->fault = 1;
        r->code = GetExceptionCode();
        return;
    }
    for (int i = 0; i < 8; i++) r->reg[i] = RD32(ctx.r + 4 * i);
    r->st0 = RDF64(ctx.r + REG_ST0);
}

/* ---- one function ------------------------------------------------------ */
static const char *REGN[8] = {"eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"};
static int same_double(double a, double b) {
    return (a == b) || (isnan(a) && isnan(b)) || (fabs(a - b) <= 1e-9 * fmax(fabs(a), fabs(b)));
}

static int test_one(uint32_t addr, uint32_t seed) {
    GuestFn fn = rt_lookup(addr);
    const char *name = "?";
    for (unsigned i = 0; i < g_functab_count; i++) if (g_functab[i].addr == addr) name = g_functab[i].name;
    if (!fn) { printf("{\"addr\":\"%08x\",\"result\":\"not-recompiled\"}\n", addr); return 1; }

    /* the guest stack and scratch live inside the host stack so SEH accepts them */
    uint8_t *stack = _alloca(STACK_SIZE + 16);
    stack = (uint8_t *)(((uintptr_t)stack + 15) & ~(uintptr_t)15);
    uint8_t *scratch = _alloca(SCRATCH_SIZE + 16);
    g_scratch = (uint8_t *)(((uintptr_t)scratch + 15) & ~(uintptr_t)15);
    /* Everything the two runs could possibly read must look the same to both,
       so all host allocations happen before the first run. */
    size_t snap_size = g_image_size + SCRATCH_SIZE + STACK_SIZE;
    uint8_t *snap = VirtualAlloc(NULL, snap_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint8_t *snap_img = snap, *snap_scr = snap + g_image_size, *snap_stk = snap_scr + SCRATCH_SIZE;
    Start s;
    Result rn, rr;
    DWORD oldprot;

    make_start(&s, stack, seed);
    if (getenv("DIFFTEST_DUMP")) {
        for (int i = 0; i < 8; i++) fprintf(stderr, "%s=%08x ", REGN[i], s.reg[i]);
        fprintf(stderr, "\n[ecx-4]=%08x scratch=%p stack=%p\n", RD32(MEM(s.reg[1] - 4)), g_scratch, stack);
    }
    VirtualProtect(snap, snap_size, PAGE_NOACCESS, &oldprot);
    run_native(addr, &s, &rn);
    VirtualProtect(snap, snap_size, PAGE_READWRITE, &oldprot);
    memcpy(snap_img, g_image, g_image_size);
    memcpy(snap_scr, g_scratch, SCRATCH_SIZE);
    memcpy(snap_stk, stack, STACK_SIZE);

    make_start(&s, stack, seed);
    VirtualProtect(snap, snap_size, PAGE_NOACCESS, &oldprot);
    run_recompiled(fn, &s, &rr);
    VirtualProtect(snap, snap_size, PAGE_READWRITE, &oldprot);

    char why[256] = "";
    const char *verdict = "pass";
    if (rn.fault) {
        verdict = "native-fault";
        snprintf(why, sizeof why, "native exception %08x, recompiled fault %d", rn.code, rr.fault);
    } else if (rr.fault == 2 && rr.code == TRAP_MISSING_FUNCTION) {
        verdict = "missing-callee"; /* reached code that was not recompiled (a library) */
        snprintf(why, sizeof why, "guest address %08x", rr.trap_addr);
    } else if (rr.fault) {
        verdict = "fail";
        snprintf(why, sizeof why, "recompiled fault %d code %08x", rr.fault, rr.code);
    } else {
        for (int i = 0; i < 8 && !why[0]; i++)
            if (rn.reg[i] != rr.reg[i])
                snprintf(why, sizeof why, "%s native %08x recompiled %08x", REGN[i], rn.reg[i], rr.reg[i]);
        if (!why[0] && rn.st_depth && !same_double(rn.st0, rr.st0))
            snprintf(why, sizeof why, "st0 native %.17g recompiled %.17g", rn.st0, rr.st0);
        if (!why[0]) {
            /* compare memory; the slot holding the return address differs by design */
            WR32(MEM(s.reg[4]), RD32(snap_stk + (s.reg[4] - (uint32_t)(uintptr_t)stack)));
            for (uint32_t i = 0; i < g_image_size && !why[0]; i++)
                if (snap_img[i] != g_image[i])
                    snprintf(why, sizeof why, "image byte %08x native %02x recompiled %02x",
                             (uint32_t)(uintptr_t)(g_image + i), snap_img[i], g_image[i]);
            for (uint32_t i = 0; i < SCRATCH_SIZE && !why[0]; i++)
                if (snap_scr[i] != g_scratch[i])
                    snprintf(why, sizeof why, "scratch +%x native %02x recompiled %02x", i, snap_scr[i], g_scratch[i]);
            for (uint32_t i = 0; i < STACK_SIZE && !why[0]; i++)
                if (snap_stk[i] != stack[i])
                    snprintf(why, sizeof why, "stack +%x native %02x recompiled %02x", i, snap_stk[i], stack[i]);
        }
        if (why[0]) verdict = "fail";
    }
    printf("{\"addr\":\"%08x\",\"name\":\"%s\",\"seed\":%u,\"result\":\"%s\",\"detail\":\"%s\"}\n",
           addr, name, seed, verdict, why);
    return strcmp(verdict, "fail") == 0;
}

/* ---- driver: one child process per function, with a timeout ------------- */
static int run_child(const char *exe, const char *xbe, uint32_t addr, uint32_t seed, char *out, size_t outsz) {
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" --one %08x %u", exe, xbe, addr, seed);
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE rd, wr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si = {sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return -1;
    CloseHandle(wr);
    DWORD w = WaitForSingleObject(pi.hProcess, TIMEOUT_MS);
    if (w == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 99);
    DWORD got = 0, total = 0;
    while (total + 1 < outsz && ReadFile(rd, out + total, (DWORD)(outsz - 1 - total), &got, NULL) && got) total += got;
    out[total] = 0;
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return w == WAIT_TIMEOUT ? 1 : 0;
}

static int run(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: difftest <default.xbe> [--one addr seed] [seeds]\n"); return 2; }
    if (argc >= 5 && strcmp(argv[2], "--one") == 0) {
        if (!load_xbe(argv[1])) return 3;
        return test_one(strtoul(argv[3], NULL, 16), (uint32_t)strtoul(argv[4], NULL, 10));
    }
    unsigned seeds = argc >= 3 ? (unsigned)atoi(argv[2]) : 3;
    char exe[MAX_PATH], buf[4096];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    unsigned pass = 0, fail = 0, nfault = 0, timeout = 0, crash = 0, missing = 0;
    for (unsigned i = 0; i < g_functab_count; i++) {
        uint32_t a = g_functab[i].addr;
        int fn_fail = 0, fn_pass = 0;
        for (unsigned seed = 1; seed <= seeds; seed++) {
            int t = run_child(exe, argv[1], a, seed, buf, sizeof buf);
            if (t == 1) { printf("{\"addr\":\"%08x\",\"seed\":%u,\"result\":\"timeout\"}\n", a, seed); timeout++; continue; }
            if (!strstr(buf, "\"result\"")) { printf("{\"addr\":\"%08x\",\"seed\":%u,\"result\":\"harness-crash\"}\n", a, seed); crash++; continue; }
            fputs(buf, stdout);
            if (strstr(buf, "\"result\":\"pass\"")) fn_pass++;
            else if (strstr(buf, "\"result\":\"fail\"")) fn_fail++;
            else if (strstr(buf, "\"result\":\"missing-callee\"")) missing++;
            else nfault++;
        }
        if (fn_fail) fail++; else if (fn_pass) pass++;
    }
    fprintf(stderr, "functions: %u passed on every run that executed, %u failed; runs: %u native faults, %u missing callees, %u timeouts, %u harness crashes\n",
            pass, fail, nfault, missing, timeout, crash);
    return fail ? 1 : 0;
}

/* Called by loader.c. Never returns: the loader's code is gone by then. */
__declspec(dllexport) void difftest_main(void) {
    int argc;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    char **argv = malloc(sizeof(char *) * argc);
    for (int i = 0; i < argc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc(n);
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n, NULL, NULL);
    }
    int rc = run(argc, argv);
    fflush(stdout);
    fflush(stderr);
    ExitProcess((UINT)rc);
}
