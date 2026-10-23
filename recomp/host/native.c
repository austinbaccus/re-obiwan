/* RECOMP_NATIVE=addr,addr,...: run these guest functions as the original x86
   code instead of their recompiled C, to bisect a behaviour difference down
   to the function whose translation is wrong. A debugging aid only.

   The XBE is mapped at its own addresses and guest memory is identity-mapped,
   so the original code can run on the host CPU against the same memory. Each
   recompiled f_XXXXXXXX listed has its entry patched to a trampoline into
   native_call(), which copies the stack arguments, loads the guest
   registers, calls the original code on the host stack, and copies the
   registers, the popped argument bytes and an x87 return value back.

   Everything the native code calls runs natively too, so pick functions
   whose callees stay inside the game: a kernel import or a hooked library
   function (D3D, DirectSound, XAPI) reached natively crashes. Not thread
   safe: one native call at a time. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

#include "guest.h"
#include "kernel.h"

#define NATIVE_ARGS 32 /* dwords of stack copied for the callee */

static struct { uint32_t eax, ecx, edx, ebx, esi, edi, ebp; } g_regs;
static uint32_t g_target, g_host_esp, g_call_esp, g_after_esp, g_args[NATIVE_ARGS];
static uint16_t g_sw, g_cw;
static uint32_t g_mxcsr;
static double g_st0;
static volatile LONG g_busy;

#pragma warning(disable: 4731) /* EBP is the guest's during the call, restored after */
static void __cdecl native_call(Ctx *ctx, uint32_t target) {
    if (InterlockedExchange(&g_busy, 1)) {
        fprintf(stderr, "native: %08x entered while another native call runs\n", target);
        abort();
    }
    uint32_t gesp = RD32(ctx->r + REG_ESP);
    memcpy(g_args, MEM(gesp + 4), sizeof g_args);
    g_regs.eax = RD32(ctx->r + REG_EAX); g_regs.ecx = RD32(ctx->r + REG_ECX);
    g_regs.edx = RD32(ctx->r + REG_EDX); g_regs.ebx = RD32(ctx->r + REG_EBX);
    g_regs.esi = RD32(ctx->r + REG_ESI); g_regs.edi = RD32(ctx->r + REG_EDI);
    g_regs.ebp = RD32(ctx->r + REG_EBP);
    g_target = target;
    __asm {
        push ebx
        push esi
        push edi
        push ebp
        fnstcw g_cw
        stmxcsr g_mxcsr
        mov g_host_esp, esp
        sub esp, NATIVE_ARGS * 4
        mov edi, esp
        mov esi, offset g_args
        mov ecx, NATIVE_ARGS
        cld
        rep movsd
        mov eax, g_regs.eax
        mov ecx, g_regs.ecx
        mov edx, g_regs.edx
        mov ebx, g_regs.ebx
        mov esi, g_regs.esi
        mov edi, g_regs.edi
        mov ebp, g_regs.ebp
        mov g_call_esp, esp
        call dword ptr [g_target]
        mov g_after_esp, esp
        mov g_regs.eax, eax
        mov g_regs.ecx, ecx
        mov g_regs.edx, edx
        mov g_regs.ebx, ebx
        mov g_regs.esi, esi
        mov g_regs.edi, edi
        mov g_regs.ebp, ebp
        mov esp, g_host_esp
        fnstsw g_sw
        pop ebp
        pop edi
        pop esi
        pop ebx
    }
    int pushed = ((g_sw >> 11) & 7) != 0; /* TOP moved: the callee returned a value in ST0 */
    if (pushed) __asm { fstp g_st0 }
    __asm {
        fldcw g_cw
        ldmxcsr g_mxcsr
    }
    WR32(ctx->r + REG_EAX, g_regs.eax); WR32(ctx->r + REG_ECX, g_regs.ecx);
    WR32(ctx->r + REG_EDX, g_regs.edx); WR32(ctx->r + REG_EBX, g_regs.ebx);
    WR32(ctx->r + REG_ESI, g_regs.esi); WR32(ctx->r + REG_EDI, g_regs.edi);
    WR32(ctx->r + REG_EBP, g_regs.ebp);
    ctx->ret_ip = RD32(MEM(gesp));
    WR32(ctx->r + REG_ESP, gesp + 4 + (g_after_esp - g_call_esp));
    if (pushed) { /* as FLD: ST(i+1) = ST(i), ST0 = value */
        memmove(ctx->r + REG_ST0 + 0x10, ctx->r + REG_ST0, 0x70);
        memcpy(ctx->r + REG_ST0, &g_st0, 8);
    }
    InterlockedExchange(&g_busy, 0);
}

/* Native code reaching a kernel import calls through the thunk table, whose
   slots hold 0xFFFF0000 | ordinal: the jump there faults, and the call is
   handed to the host kernel with the guest stack pointer on the host stack
   (guest memory is identity-mapped). Any other fault in native code is
   reported here, before the original code's own SEH handlers see it. */
static LONG CALLBACK native_veh(EXCEPTION_POINTERS *e) {
    CONTEXT *c = e->ContextRecord;
    DWORD code = e->ExceptionRecord->ExceptionCode;
    if (!g_busy || code == EXCEPTION_SINGLE_STEP || code == EXCEPTION_BREAKPOINT || (code & 0xF0000000u) == 0xE0000000u)
        return EXCEPTION_CONTINUE_SEARCH;
    Ctx *ctx = kernel_current_ctx();
    if (code == EXCEPTION_ACCESS_VIOLATION && c->Eip >= 0xFFFF0000u && ctx) {
        uint32_t ret = RD32(MEM(c->Esp));
        WR32(ctx->r + REG_ESP, c->Esp);
        WR32(ctx->r + REG_EAX, c->Eax); WR32(ctx->r + REG_ECX, c->Ecx); WR32(ctx->r + REG_EDX, c->Edx);
        kernel_call(ctx, c->Eip & 0xFFFF);
        c->Eax = RD32(ctx->r + REG_EAX); c->Ecx = RD32(ctx->r + REG_ECX); c->Edx = RD32(ctx->r + REG_EDX);
        c->Esp = RD32(ctx->r + REG_ESP);
        c->Eip = ret;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    const FuncEntry *f = rt_lookup_containing(c->Eip);
    fprintf(stderr, "native: exception %08lx at %08lx (%s) in native call to %08x, eax %08lx ecx %08lx edx %08lx esi %08lx edi %08lx esp %08lx\n",
            code, c->Eip, f ? f->name : "?", g_target, c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp);
    if (code == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, "    %s address %08lx\n", e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (DWORD)e->ExceptionRecord->ExceptionInformation[1]);
    fprintf(stderr, "    native stack:");
    for (int i = 0; i < 24; i++) {
        uint32_t v = RD32(MEM(c->Esp + 4 * i));
        const FuncEntry *r = v >= 0x10000 && v < 0x530000 ? rt_lookup_containing(v) : NULL;
        fprintf(stderr, r ? " %08x(%s)" : " %08x", v, r ? r->name : "");
    }
    fprintf(stderr, "\n");
    fflush(stderr);
    ExitProcess(23);
}

void native_init(void) {
    const char *list = getenv("RECOMP_NATIVE");
    if (!list) return;
    AddVectoredExceptionHandler(1, native_veh);
    uint8_t *tramps = VirtualAlloc(NULL, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int n = 0;
    for (const char *p = list; *p && n < 0x10000 / 32; ) {
        uint32_t addr = (uint32_t)strtoul(p, (char **)&p, 16);
        GuestFn fn = rt_lookup(addr);
        if (!fn) fprintf(stderr, "native: no recompiled function at %08x\n", addr);
        else {
            /* push addr; push [esp+8] (ctx); call native_call; add esp, 8; ret */
            uint8_t *t = tramps + 32 * n++;
            t[0] = 0x68; memcpy(t + 1, &addr, 4);
            t[5] = 0xff; t[6] = 0x74; t[7] = 0x24; t[8] = 0x08;
            int32_t rel = (int32_t)((uint8_t *)native_call - (t + 14));
            t[9] = 0xe8; memcpy(t + 10, &rel, 4);
            t[14] = 0x83; t[15] = 0xc4; t[16] = 0x08; t[17] = 0xc3;
            uint8_t *entry = (uint8_t *)fn;
            DWORD old;
            VirtualProtect(entry, 5, PAGE_EXECUTE_READWRITE, &old);
            rel = (int32_t)(t - (entry + 5));
            entry[0] = 0xe9; memcpy(entry + 1, &rel, 4);
            VirtualProtect(entry, 5, old, &old);
            FlushInstructionCache(GetCurrentProcess(), entry, 5);
            fprintf(stderr, "native: %08x runs as the original x86 code\n", addr);
        }
        while (*p == ',' || *p == ' ') p++;
    }
}
