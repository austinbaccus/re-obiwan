/* Guest CPU state, memory access and p-code helpers used by generated code. */
#pragma once

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte layout of Ghidra's x86:LE:32 register space (only the parts we use). */
enum {
    REG_EAX = 0x00, REG_ECX = 0x04, REG_EDX = 0x08, REG_EBX = 0x0c,
    REG_ESP = 0x10, REG_EBP = 0x14, REG_ESI = 0x18, REG_EDI = 0x1c,
    REG_FS_OFFSET = 0x110,
    REG_CF = 0x200, REG_PF = 0x202, REG_AF = 0x204, REG_ZF = 0x206, REG_SF = 0x207,
    REG_DF = 0x20a, REG_OF = 0x20b,
    REG_MXCSR = 0x1094, REG_FPUCW = 0x10a0, REG_FPUSW = 0x10a2,
    REG_ST0 = 0x1100, /* ST(i) at REG_ST0 + 0x10 * i, a host double in the first 8 bytes */
    REG_XMM0 = 0x1200, /* XMM(i) at REG_XMM0 + 0x40 * i */
    REG_SPACE_SIZE = 0x2400,
};

typedef enum {
    TRAP_NONE = 0,
    TRAP_MISSING_FUNCTION, /* call or jump to code that was not recompiled */
    TRAP_INTERRUPT,        /* int3 / int n (QA-build asserts) */
    TRAP_DIVIDE,           /* divide by zero or quotient overflow */
    TRAP_UNSUPPORTED,      /* instruction with no host equivalent (port I/O, cpuid) */
} TrapKind;

typedef struct Ctx {
    _Alignas(16) uint8_t r[REG_SPACE_SIZE];
    jmp_buf *trap_jmp;   /* where rt_trap longjmps to; NULL aborts */
    TrapKind trap;
    uint32_t trap_addr;
} Ctx;

typedef void (*GuestFn)(Ctx *ctx);
typedef struct {
    uint32_t addr;
    GuestFn fn;
    const char *name;
} FuncEntry;

extern const FuncEntry g_functab[];
extern const unsigned g_functab_count;

/* Host address of guest address 0. Zero when guest memory is identity-mapped
   (the 32-bit test harness), the base of a reserved 4 GB window otherwise. */
extern uintptr_t g_membase;
#define MEM(a) ((uint8_t *)(g_membase + (uint32_t)(a)))

static inline uint8_t RD8(const void *p) { return *(const uint8_t *)p; }
static inline uint16_t RD16(const void *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static inline uint32_t RD32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline uint64_t RD64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void WR8(void *p, uint8_t v) { *(uint8_t *)p = v; }
static inline void WR16(void *p, uint16_t v) { memcpy(p, &v, 2); }
static inline void WR32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline void WR64(void *p, uint64_t v) { memcpy(p, &v, 8); }

static inline float RDF32(const void *p) { float v; memcpy(&v, p, 4); return v; }
static inline double RDF64(const void *p) { double v; memcpy(&v, p, 8); return v; }
static inline void WRF32(void *p, float v) { memcpy(p, &v, 4); }
static inline void WRF64(void *p, double v) { memcpy(p, &v, 8); }
/* 10-byte x87 slot holding a host double */
static inline void WRF80SLOT(void *p, double v) { memcpy(p, &v, 8); memset((uint8_t *)p + 8, 0, 2); }
static inline float bits_f32(uint32_t b) { float v; memcpy(&v, &b, 4); return v; }
static inline double bits_f64(uint64_t b) { double v; memcpy(&v, &b, 8); return v; }

double f80_to_f64(const uint8_t *p);
void f64_to_f80(uint8_t *p, double v);

/* Flag helpers: carry / signed overflow of a+b, signed overflow of a-b. */
#define DEF_FLAGS(N, U, S)                                                              \
    static inline uint8_t carry##N(U a, U b) { return (U)(a + b) < a; }                \
    static inline uint8_t scarry##N(U a, U b) { U r = (U)(a + b); return (S)((a ^ r) & (b ^ r)) < 0; } \
    static inline uint8_t sborrow##N(U a, U b) { U r = (U)(a - b); return (S)((a ^ b) & (a ^ r)) < 0; } \
    static inline U shl##N(U a, uint64_t s) { return s >= N ? 0 : (U)(a << s); }        \
    static inline U shr##N(U a, uint64_t s) { return s >= N ? 0 : (U)(a >> s); }        \
    static inline U sar##N(U a, uint64_t s) { return (U)((S)a >> (s >= N ? N - 1 : s)); }
DEF_FLAGS(8, uint8_t, int8_t)
DEF_FLAGS(16, uint16_t, int16_t)
DEF_FLAGS(32, uint32_t, int32_t)
DEF_FLAGS(64, uint64_t, int64_t)

static inline uint8_t popcount64(uint64_t v) {
    v = v - ((v >> 1) & 0x5555555555555555ull);
    v = (v & 0x3333333333333333ull) + ((v >> 2) & 0x3333333333333333ull);
    v = (v + (v >> 4)) & 0x0f0f0f0f0f0f0f0full;
    return (uint8_t)((v * 0x0101010101010101ull) >> 56);
}

/* float -> int with x86 semantics: NaN and out-of-range give the "integer indefinite" value */
#define DEF_F2I(N, S, MIN, MAX)                                                \
    static inline uint##N##_t f2i##N(double v) {                                \
        if (!(v >= (double)(MIN) && v < -(double)(MIN))) return (uint##N##_t)(MIN); \
        return (uint##N##_t)(S)v;                                               \
    }
DEF_F2I(16, int16_t, INT16_MIN, INT16_MAX)
DEF_F2I(32, int32_t, INT32_MIN, INT32_MAX)
DEF_F2I(64, int64_t, INT64_MIN, INT64_MAX)
static inline uint8_t f2i8(double v) { return (uint8_t)f2i16(v); }

extern const uint8_t g_zero16[16];
static inline void wide_and(void *o, const void *a, const void *b, int n) { for (int i = 0; i < n; i++) ((uint8_t *)o)[i] = ((const uint8_t *)a)[i] & ((const uint8_t *)b)[i]; }
static inline void wide_or(void *o, const void *a, const void *b, int n) { for (int i = 0; i < n; i++) ((uint8_t *)o)[i] = ((const uint8_t *)a)[i] | ((const uint8_t *)b)[i]; }
static inline void wide_xor(void *o, const void *a, const void *b, int n) { for (int i = 0; i < n; i++) ((uint8_t *)o)[i] = ((const uint8_t *)a)[i] ^ ((const uint8_t *)b)[i]; }

/* MMX helpers: 64-bit packed integers */
void rt_pmaddwd(void *o, const void *a, const void *b);
void rt_psubsw(void *o, const void *a, const void *b);
void rt_packssdw(void *o, const void *a, const void *b);
void rt_psllw(void *o, const void *a, const void *count);
void rt_pshufw(void *o, const void *a, int imm);

double rt_fpatan(double y, double x);
double rt_f2xm1(double x);
double rt_fscale(double x, double s);

_Noreturn void rt_trap(Ctx *ctx, TrapKind kind, uint32_t addr);
void rt_call(Ctx *ctx, uint32_t addr);
void rt_jump(Ctx *ctx, uint32_t addr);
void rt_interrupt(Ctx *ctx, uint32_t addr, uint32_t vector);
uint64_t rt_rdtsc(Ctx *ctx);
GuestFn rt_lookup(uint32_t addr);

#define DEF_DIV(N, U, S)                                                                    \
    static inline U rt_udiv##N(Ctx *c, U a, U b) { if (!b) rt_trap(c, TRAP_DIVIDE, 0); return a / b; } \
    static inline U rt_urem##N(Ctx *c, U a, U b) { if (!b) rt_trap(c, TRAP_DIVIDE, 0); return a % b; } \
    static inline U rt_sdiv##N(Ctx *c, U a, U b) { if (!b || ((S)b == -1 && (S)a == (S)((U)1 << (N - 1)))) rt_trap(c, TRAP_DIVIDE, 0); return (U)((S)a / (S)b); } \
    static inline U rt_srem##N(Ctx *c, U a, U b) { if (!b || ((S)b == -1 && (S)a == (S)((U)1 << (N - 1)))) rt_trap(c, TRAP_DIVIDE, 0); return (U)((S)a % (S)b); }
DEF_DIV(8, uint8_t, int8_t)
DEF_DIV(16, uint16_t, int16_t)
DEF_DIV(32, uint32_t, int32_t)
DEF_DIV(64, uint64_t, int64_t)

#ifdef __cplusplus
}
#endif
