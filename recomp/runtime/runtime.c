/* Minimal guest runtime: dispatch, traps and x87 helpers. */
#include <stdio.h>
#include <stdlib.h>

#include "guest.h"

uintptr_t g_membase;
const uint8_t g_zero16[16];

static int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }

void rt_pmaddwd(void *o, const void *a, const void *b) {
    int16_t x[4], y[4]; int32_t r[2];
    memcpy(x, a, 8); memcpy(y, b, 8);
    r[0] = x[0] * y[0] + x[1] * y[1];
    r[1] = x[2] * y[2] + x[3] * y[3];
    memcpy(o, r, 8);
}
void rt_psubsw(void *o, const void *a, const void *b) {
    int16_t x[4], y[4], r[4];
    memcpy(x, a, 8); memcpy(y, b, 8);
    for (int i = 0; i < 4; i++) r[i] = sat16(x[i] - y[i]);
    memcpy(o, r, 8);
}
void rt_packssdw(void *o, const void *a, const void *b) {
    int32_t x[2], y[2]; int16_t r[4];
    memcpy(x, a, 8); memcpy(y, b, 8);
    r[0] = sat16(x[0]); r[1] = sat16(x[1]); r[2] = sat16(y[0]); r[3] = sat16(y[1]);
    memcpy(o, r, 8);
}
void rt_psllw(void *o, const void *a, uint64_t c) {
    uint16_t x[4];
    memcpy(x, a, 8);
    for (int i = 0; i < 4; i++) x[i] = c > 15 ? 0 : (uint16_t)(x[i] << c);
    memcpy(o, x, 8);
}
void rt_pshufw(void *o, const void *dst_in, const void *a, uint64_t imm) {
    (void)dst_in;
    uint16_t x[4], r[4];
    memcpy(x, a, 8);
    for (int i = 0; i < 4; i++) r[i] = x[(imm >> (2 * i)) & 3];
    memcpy(o, r, 8);
}

double f80_to_f64(const uint8_t *p) {
    uint64_t mant;
    uint16_t se;
    memcpy(&mant, p, 8);
    memcpy(&se, p + 8, 2);
    int sign = se >> 15, exp = se & 0x7fff;
    double v;
    if (exp == 0 && mant == 0) v = 0.0;
    else if (exp == 0x7fff && (mant << 1)) {
        /* NaN: keep the sign and the top payload bits (the x87 indefinite is 0xFFF8...) */
        uint64_t bits = ((uint64_t)sign << 63) | 0x7ff0000000000000ull | ((mant << 1) >> 12);
        if (!(bits & 0x000fffffffffffffull)) bits |= 0x0008000000000000ull;
        memcpy(&v, &bits, 8);
        return v;
    }
    else if (exp == 0x7fff) v = INFINITY;
    else v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}

void f64_to_f80(uint8_t *p, double v) {
    uint64_t mant = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (isnan(v)) {
        uint64_t bits;
        memcpy(&bits, &v, 8);
        se |= 0x7fff;
        mant = 0x8000000000000000ull | ((bits & 0x000fffffffffffffull) << 11);
    }
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != 0.0) {
        int e;
        double m = frexp(fabs(v), &e);           /* v = m * 2^e, 0.5 <= m < 1 */
        mant = (uint64_t)ldexp(m, 64);            /* explicit integer bit at 63 */
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(p, &mant, 8);
    memcpy(p + 8, &se, 2);
}

double rt_fpatan(double y, double x) { return atan2(y, x); }
double rt_f2xm1(double x) { return exp2(x) - 1.0; }
double rt_fscale(double x, double s) { return ldexp(x, (int)trunc(s)); }

int g_trace_calls; /* RECOMP_TRACE_CALLS=1: log every indirect call and jump */

static const char *trap_name(TrapKind k) {
    switch (k) {
    case TRAP_MISSING_FUNCTION: return "call to code that was not recompiled";
    case TRAP_INTERRUPT: return "int3 / software interrupt (assert)";
    case TRAP_DIVIDE: return "divide error";
    case TRAP_UNSUPPORTED: return "unsupported instruction or kernel function";
    default: return "trap";
    }
}

void rt_trap(Ctx *ctx, TrapKind kind, uint32_t addr) {
    if (ctx) {
        ctx->trap = kind;
        ctx->trap_addr = addr;
        if (ctx->trap_jmp) longjmp(*ctx->trap_jmp, 1);
        fprintf(stderr, "guest trap: %s at %08x\n  eax %08x ecx %08x edx %08x ebx %08x esp %08x ebp %08x esi %08x edi %08x\n",
                trap_name(kind), addr, RD32(ctx->r + REG_EAX), RD32(ctx->r + REG_ECX), RD32(ctx->r + REG_EDX),
                RD32(ctx->r + REG_EBX), RD32(ctx->r + REG_ESP), RD32(ctx->r + REG_EBP), RD32(ctx->r + REG_ESI),
                RD32(ctx->r + REG_EDI));
        uint32_t esp = RD32(ctx->r + REG_ESP);
        fprintf(stderr, "  stack:");
        for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", RD32(MEM(esp + 4 * i)));
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "guest trap: %s at %08x\n", trap_name(kind), addr);
    }
    abort();
}

GuestFn rt_lookup(uint32_t addr) {
    unsigned lo = 0, hi = g_functab_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_functab[mid].addr == addr) return g_functab[mid].fn;
        if (g_functab[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void (*g_kernel_call)(Ctx *ctx, uint32_t ordinal);
void (*g_host_vcall)(Ctx *ctx, uint32_t id);

void rt_call(Ctx *ctx, uint32_t addr) {
    if (addr >= 0xFFFF0000u) { /* patched kernel thunk slot */
        if (!g_kernel_call) rt_trap(ctx, TRAP_MISSING_FUNCTION, addr);
        g_kernel_call(ctx, addr & 0xFFFF);
        return;
    }
    if (addr >= 0xFFFE0000u) { /* vtable slot of a host-provided COM object */
        if (!g_host_vcall) rt_trap(ctx, TRAP_MISSING_FUNCTION, addr);
        g_host_vcall(ctx, addr & 0xFFFF);
        return;
    }
    if ((addr & 0xFFFF0000u) == 0xDEAD0000u) return; /* host-pushed return address: thread or callback ends */
    GuestFn fn = rt_lookup(addr);
    if (g_trace_calls) fprintf(stderr, "  -> %08x%s (esp %08x)\n", addr, fn ? "" : " (missing)", RD32(ctx->r + REG_ESP));
    if (!fn) rt_trap(ctx, TRAP_MISSING_FUNCTION, addr);
    fn(ctx);
}

void rt_trace_call(Ctx *ctx, uint32_t addr) {
    uint32_t esp = RD32(ctx->r + REG_ESP);
    const char *name = "";
    for (unsigned i = 0; i < g_functab_count; i++) if (g_functab[i].addr == addr) { name = g_functab[i].name; break; }
    fprintf(stderr, "  call %08x %s (ecx %08x; args %08x %08x %08x %08x)\n", addr, name, RD32(ctx->r + REG_ECX),
            RD32(MEM(esp)), RD32(MEM(esp + 4)), RD32(MEM(esp + 8)), RD32(MEM(esp + 12)));
}

/* A jump out of the current function: run the target as if tail-called. */
void rt_jump(Ctx *ctx, uint32_t addr) { rt_call(ctx, addr); }

void rt_interrupt(Ctx *ctx, uint32_t addr, uint32_t vector) {
    (void)vector;
    rt_trap(ctx, TRAP_INTERRUPT, addr);
}

uint64_t rt_rdtsc(Ctx *ctx) {
    (void)ctx;
    return 0; /* replaced by a host timer scaled to 733 MHz once the game runs */
}
