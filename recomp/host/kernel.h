/* Host implementation of the xboxkrnl.exe functions the game imports. */
#pragma once
#include "guest.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel thunk slots are patched with KERNEL_MAGIC | ordinal; rt_call routes
   those to kernel_call. Data exports get a pointer to a host-owned variable
   in guest memory instead. */
#define KERNEL_MAGIC 0xFFFF0000u

typedef int32_t NTSTATUS_;
#define STATUS_SUCCESS_ 0
#define STATUS_UNSUCCESSFUL_ ((NTSTATUS_)0xC0000001)
#define STATUS_NOT_IMPLEMENTED_ ((NTSTATUS_)0xC0000002)
#define STATUS_INVALID_HANDLE_ ((NTSTATUS_)0xC0000008)
#define STATUS_INVALID_PARAMETER_ ((NTSTATUS_)0xC000000D)
#define STATUS_NO_MEMORY_ ((NTSTATUS_)0xC0000017)
#define STATUS_OBJECT_NAME_NOT_FOUND_ ((NTSTATUS_)0xC0000034)
#define STATUS_OBJECT_NAME_COLLISION_ ((NTSTATUS_)0xC0000035)
#define STATUS_OBJECT_PATH_NOT_FOUND_ ((NTSTATUS_)0xC000003A)
#define STATUS_NO_MORE_FILES_ ((NTSTATUS_)0x80000006)
#define STATUS_END_OF_FILE_ ((NTSTATUS_)0xC0000011)
#define STATUS_OBJECT_TYPE_MISMATCH_ ((NTSTATUS_)0xC0000024)
#define STATUS_DIRECTORY_NOT_EMPTY_ ((NTSTATUS_)0xC0000101)
#define STATUS_TIMEOUT_ 0x102
#define STATUS_ALERTED_ 0x101

/* stdcall helpers for host-implemented guest functions: arguments are on the
   guest stack above the return address; the callee pops them. */
#define ARG(ctx, i) RD32(MEM(RD32((ctx)->r + REG_ESP) + 4 + 4 * (i)))
#define SET_EAX(ctx, v) WR32((ctx)->r + REG_EAX, (uint32_t)(v))
#define RET_STDCALL(ctx, nargs) WR32((ctx)->r + REG_ESP, RD32((ctx)->r + REG_ESP) + 4 + 4 * (nargs))
#define RET_CDECL(ctx) WR32((ctx)->r + REG_ESP, RD32((ctx)->r + REG_ESP) + 4)
#define RET_ESP(ctx, v) WR32((ctx)->r + REG_ESP, (v))

/* Guest-memory arena for everything the kernel hands out (heaps, stacks,
   contiguous memory, kernel objects). */
void kmem_init(uint32_t start, uint32_t size);
uint32_t kmem_alloc(uint32_t size, uint32_t align);   /* 0 when the arena is exhausted */
void kmem_free(uint32_t addr);
uint32_t kmem_block_size(uint32_t addr);
uint32_t kmem_block_containing(uint32_t addr);

/* Patch the thunk table in the loaded image and set up the object namespace. */
void kernel_prepare_threads(void); /* before the XBE is loaded */
void kernel_init(uint32_t thunk_table, const char *game_dir, const char *hdd_dir);
void kernel_call(Ctx *ctx, uint32_t ordinal);

/* Per-thread processor control block (what fs: points at) and the main
   thread's guest stack. */
uint32_t kernel_create_kpcr(uint32_t stack_base, uint32_t stack_limit, uint32_t tls_data);
void kernel_run_thread(uint32_t start_routine, uint32_t start_context, uint32_t stack_size,
                       uint32_t tls_size, uint32_t system_routine);

Ctx *kernel_current_ctx(void); /* the guest context of the calling host thread, or NULL */
void kernel_each_thread(void (*fn)(HANDLE thread, Ctx *ctx, const char *what, void *arg), void *arg); /* every running guest thread */
void kernel_threads(void (*fn)(HANDLE thread, Ctx *ctx, const char *what, void *arg), void *arg); /* the same, quietly */
uint32_t d3d_frame_count(void); /* hooks_d3d.c: frames presented so far */
void host_watch(uint32_t addr, uint32_t len); /* boot.c: log writes to guest bytes (debugging) */
void native_init(void); /* native.c: RECOMP_NATIVE, original x86 code for chosen functions (debugging) */
int host_override(uint32_t addr, GuestFn replacement); /* native.c: a host function in place of a recompiled one */
void widescreen_init(void); /* widescreen.c: wider camera views for RECOMP_RES wider than 4:3 */
void debug_keys(void);      /* debugkeys.c: F1 restores health; call on the game's thread */
void framerate_init(void);  /* hooks_d3d.c: the host paces frames instead of the game's spin (RECOMP_MAXFPS) */
void kernel_frame_presented(void); /* RECOMP_FIXEDFPS: advance the guest's virtual clock a frame */
extern int g_trace; /* RECOMP_TRACE=1: log every kernel call and hook */
void trace(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
