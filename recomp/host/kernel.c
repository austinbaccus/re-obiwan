/* Host implementation of the Xbox kernel surface the game and its XDK
   libraries import (docs/recomp/kernel-imports.md). The recompiled XAPI runs
   unchanged on top of this: it only ever talks to the kernel.

   32-bit host, identity-mapped guest memory (g_membase == 0). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kernel.h"

int g_trace;

void trace(const char *fmt, ...) {
    if (!g_trace) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* ---- import table ------------------------------------------------------ */
typedef struct {
    uint32_t slot, ordinal;
    const char *name;
    int is_data;
} Import;

static const Import k_imports[] = {
    {0x003bc220, 184, "NtAllocateVirtualMemory", 0},
    {0x003bc224, 199, "NtFreeVirtualMemory", 0},
    {0x003bc228, 217, "NtQueryVirtualMemory", 0},
    {0x003bc22c, 181, "MmQueryStatistics", 0},
    {0x003bc230, 178, "MmPersistContiguousMemory", 0},
    {0x003bc234, 165, "MmAllocateContiguousMemory", 0},
    {0x003bc238, 164, "LaunchDataPage", 1},
    {0x003bc23c, 171, "MmFreeContiguousMemory", 0},
    {0x003bc240, 301, "RtlNtStatusToDosError", 0},
    {0x003bc244, 49, "HalReturnToFirmware", 0},
    {0x003bc248, 187, "NtClose", 0},
    {0x003bc24c, 215, "NtQuerySymbolicLinkObject", 0},
    {0x003bc250, 203, "NtOpenSymbolicLinkObject", 0},
    {0x003bc254, 16, "ExEventObjectType", 1},
    {0x003bc258, 289, "RtlInitAnsiString", 0},
    {0x003bc25c, 225, "NtSetEvent", 0},
    {0x003bc260, 99, "KeDelayExecutionThread", 0},
    {0x003bc264, 226, "NtSetInformationFile", 0},
    {0x003bc268, 202, "NtOpenFile", 0},
    {0x003bc26c, 37, "FscSetCacheSize", 0},
    {0x003bc270, 207, "NtQueryDirectoryFile", 0},
    {0x003bc274, 161, "KfLowerIrql", 0},
    {0x003bc278, 129, "KeRaiseIrqlToDpcLevel", 0},
    {0x003bc27c, 65, "IoCreateDevice", 0},
    {0x003bc280, 14, "ExAllocatePool", 0},
    {0x003bc284, 145, "KeSetEvent", 0},
    {0x003bc288, 159, "KeWaitForSingleObject", 0},
    {0x003bc28c, 113, "KeInitializeTimerEx", 0},
    {0x003bc290, 107, "KeInitializeDpc", 0},
    {0x003bc294, 74, "IoInvalidDeviceRequest", 0},
    {0x003bc298, 149, "KeSetTimer", 0},
    {0x003bc29c, 97, "KeCancelTimer", 0},
    {0x003bc2a0, 250, "ObfDereferenceObject", 0},
    {0x003bc2a4, 246, "ObReferenceObjectByHandle", 0},
    {0x003bc2a8, 166, "MmAllocateContiguousMemoryEx", 0},
    {0x003bc2ac, 180, "MmQueryAllocationSize", 0},
    {0x003bc2b0, 182, "MmSetAddressProtect", 0},
    {0x003bc2b4, 179, "MmQueryAddressProtect", 0},
    {0x003bc2b8, 190, "NtCreateFile", 0},
    {0x003bc2bc, 236, "NtWriteFile", 0},
    {0x003bc2c0, 219, "NtReadFile", 0},
    {0x003bc2c4, 211, "NtQueryInformationFile", 0},
    {0x003bc2c8, 172, "MmFreeSystemMemory", 0},
    {0x003bc2cc, 167, "MmAllocateSystemMemory", 0},
    {0x003bc2d0, 17, "ExFreePool", 0},
    {0x003bc2d4, 83, "IoStartPacket", 0},
    {0x003bc2d8, 87, "IofCompleteRequest", 0},
    {0x003bc2dc, 81, "IoStartNextPacket", 0},
    {0x003bc2e0, 359, "IoMarkIrpMustComplete", 0},
    {0x003bc2e4, 358, "HalIsResetOrShutdownPending", 0},
    {0x003bc2e8, 125, "KeQueryInterruptTime", 0},
    {0x003bc2ec, 322, "XboxHardwareInfo", 1},
    {0x003bc2f0, 360, "HalInitiateShutdown", 0},
    {0x003bc2f4, 294, "RtlLeaveCriticalSection", 0},
    {0x003bc2f8, 277, "RtlEnterCriticalSection", 0},
    {0x003bc2fc, 269, "RtlCompareMemoryUlong", 0},
    {0x003bc300, 291, "RtlInitializeCriticalSection", 0},
    {0x003bc304, 302, "RtlRaiseException", 0},
    {0x003bc308, 218, "NtQueryVolumeInformationFile", 0},
    {0x003bc30c, 67, "IoCreateSymbolicLink", 0},
    {0x003bc310, 128, "KeQuerySystemTime", 0},
    {0x003bc314, 143, "KeSetBasePriorityThread", 0},
    {0x003bc318, 259, "PsThreadObjectType", 1},
    {0x003bc31c, 224, "NtResumeThread", 0},
    {0x003bc320, 258, "PsTerminateSystemThread", 0},
    {0x003bc324, 255, "PsCreateSystemThreadEx", 0},
    {0x003bc328, 44, "HalGetInterruptVector", 0},
    {0x003bc32c, 160, "KfRaiseIrql", 0},
    {0x003bc330, 47, "HalRegisterShutdownNotification", 0},
    {0x003bc334, 98, "KeConnectInterrupt", 0},
    {0x003bc338, 109, "KeInitializeInterrupt", 0},
    {0x003bc33c, 151, "KeStallExecutionProcessor", 0},
    {0x003bc340, 24, "ExQueryNonVolatileSetting", 0},
    {0x003bc344, 356, "HalBootSMCVideoMode", 0},
    {0x003bc348, 279, "RtlEqualString", 0},
    {0x003bc34c, 327, "XeLoadSection", 0},
    {0x003bc350, 328, "XeUnloadSection", 0},
    {0x003bc354, 40, "HalDiskCachePartitionCount", 1},
    {0x003bc358, 200, "NtFsControlFile", 0},
    {0x003bc35c, 173, "MmGetPhysicalAddress", 0},
    {0x003bc360, 175, "MmLockUnlockBufferPages", 0},
    {0x003bc364, 119, "KeInsertQueueDpc", 0},
    {0x003bc368, 176, "MmLockUnlockPhysicalPage", 0},
    {0x003bc36c, 196, "NtDeviceIoControlFile", 0},
    {0x003bc370, 312, "RtlUnwind", 0},
    {0x003bc374, 95, "KeBugCheck", 0},
    {0x003bc378, 336, "XcSHAUpdate", 0},
    {0x003bc37c, 335, "XcSHAInit", 0},
    {0x003bc380, 337, "XcSHAFinal", 0},
    {0x003bc384, 323, "XboxHDKey", 1},
    {0x003bc388, 325, "XboxSignatureKey", 1},
    {0x003bc38c, 305, "RtlTimeToTimeFields", 0},
    {0x003bc390, 156, "KeTickCount", 1},
    {0x003bc394, 304, "RtlTimeFieldsToTime", 0},
    {0x003bc398, 126, "KeQueryPerformanceCounter", 0},
    {0x003bc39c, 127, "KeQueryPerformanceFrequency", 0},
    {0x003bc3a0, 233, "NtWaitForSingleObject", 0},
    {0x003bc3a4, 198, "NtFlushBuffersFile", 0},
    {0x003bc3a8, 232, "NtUserIoApcDispatcher", 0},
    {0x003bc3ac, 1, "AvGetSavedDataAddress", 0},
    {0x003bc3b0, 2, "AvSendTVEncoderOption", 0},
    {0x003bc3b4, 3, "AvSetDisplayMode", 0},
    {0x003bc3b8, 4, "AvSetSavedDataAddress", 0},
    {0x003bc3bc, 8, "DbgPrint", 0},
    {0x003bc3c0, 100, "KeDisconnectInterrupt", 0},
    {0x003bc3c4, 168, "MmClaimGpuInstanceMemory", 0},
    {0x003bc3c8, 23, "ExQueryPoolBlockSize", 0},
    {0x003bc3cc, 15, "ExAllocatePoolWithTag", 0},
    {0x003bc3d0, 137, "KeRemoveQueueDpc", 0},
    {0x003bc3d4, 153, "KeSynchronizeExecution", 0},
    {0x003bc3d8, 142, "KeSaveFloatingPointState", 0},
    {0x003bc3dc, 139, "KeRestoreFloatingPointState", 0},
};
#define N_IMPORTS (sizeof k_imports / sizeof k_imports[0])

static const char *ordinal_name(uint32_t ordinal) {
    for (size_t i = 0; i < N_IMPORTS; i++)
        if (k_imports[i].ordinal == ordinal) return k_imports[i].name;
    return "?";
}

/* ---- guest arena -------------------------------------------------------- */
/* First-fit allocator with a header before each block; page-aligned
   allocations are served from the top so heap pages stay contiguous. */
typedef struct { uint32_t size, used; } Block;
static uint32_t arena_start, arena_end;
static CRITICAL_SECTION arena_lock;

void kmem_init(uint32_t start, uint32_t size) {
    /* the loader executable's image already spans guest RAM (loader.c) */
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)start, size, PAGE_EXECUTE_READWRITE, &old)) {
        fprintf(stderr, "cannot use guest RAM at %08x (error %lu)\n", start, GetLastError());
        exit(10);
    }
    memset(MEM(start), 0, size);
    arena_start = start;
    arena_end = start + size;
    Block *b = (Block *)MEM(start);
    b->size = size;
    b->used = 0;
    InitializeCriticalSection(&arena_lock);
}

uint32_t kmem_alloc(uint32_t size, uint32_t align) {
    if (align < 16) align = 16;
    size = (size + 15) & ~15u;
    EnterCriticalSection(&arena_lock);
    uint32_t a = arena_start;
    while (a < arena_end) {
        Block *b = (Block *)MEM(a);
        uint32_t payload = (a + sizeof(Block) + align - 1) & ~(align - 1);
        uint32_t need = payload + size - a;
        if (!b->used && b->size >= need) {
            uint32_t rest = b->size - need;
            if (rest > sizeof(Block) + 16) {
                Block *n = (Block *)MEM(a + need);
                n->size = rest;
                n->used = 0;
                b->size = need;
            }
            b->used = payload - a; /* offset of the payload, so free() can find the header */
            memset(MEM(payload), 0, size);
            LeaveCriticalSection(&arena_lock);
            return payload;
        }
        a += b->size;
    }
    LeaveCriticalSection(&arena_lock);
    trace("guest arena: cannot satisfy %u bytes (the game probes for memory; it gets STATUS_NO_MEMORY)", size);
    return 0;
}

static Block *block_of(uint32_t addr) {
    uint32_t a = arena_start;
    while (a < arena_end) {
        Block *b = (Block *)MEM(a);
        if (b->used && a + b->used == addr) return b;
        a += b->size;
    }
    return NULL;
}

void kmem_free(uint32_t addr) {
    if (!addr) return;
    EnterCriticalSection(&arena_lock);
    Block *b = block_of(addr);
    if (b) {
        b->used = 0;
        /* merge with following free blocks */
        uint32_t a = (uint32_t)((uint8_t *)b - MEM(0));
        for (;;) {
            Block *n = (Block *)MEM(a + b->size);
            if (a + b->size >= arena_end || n->used) break;
            b->size += n->size;
        }
    }
    LeaveCriticalSection(&arena_lock);
}

uint32_t kmem_block_size(uint32_t addr) {
    Block *b = block_of(addr);
    return b ? b->size - b->used : 0;
}

/* Payload start of the allocated block containing addr, or 0. */
uint32_t kmem_block_containing(uint32_t addr) {
    uint32_t a = arena_start;
    while (a < arena_end) {
        Block *b = (Block *)MEM(a);
        if (b->used && a + b->used <= addr && addr < a + b->size) return a + b->used;
        a += b->size;
    }
    return 0;
}

/* ---- objects and handles ------------------------------------------------ */
typedef enum { OBJ_FREE, OBJ_FILE, OBJ_DIR, OBJ_EVENT, OBJ_THREAD, OBJ_SYMLINK } ObjKind;
typedef struct {
    ObjKind kind;
    HANDLE host;        /* file, event or thread handle */
    char path[MAX_PATH];/* host path for files and directories */
    uint32_t guest_obj; /* guest address of a KTHREAD / KEVENT this handle refers to */
    char *dir_pattern;  /* pending NtQueryDirectoryFile pattern */
    int dir_started;
} Obj;
#define MAX_HANDLES 1024
static Obj g_objs[MAX_HANDLES];
static CRITICAL_SECTION obj_lock;

static uint32_t handle_alloc(ObjKind kind) {
    EnterCriticalSection(&obj_lock);
    for (uint32_t i = 16; i < MAX_HANDLES; i++)
        if (g_objs[i].kind == OBJ_FREE) {
            memset(&g_objs[i], 0, sizeof g_objs[i]);
            g_objs[i].kind = kind;
            LeaveCriticalSection(&obj_lock);
            return i * 4;
        }
    LeaveCriticalSection(&obj_lock);
    rt_trap(NULL, TRAP_UNSUPPORTED, 0);
}

static Obj *handle_get(uint32_t h) {
    uint32_t i = h / 4;
    if (h % 4 || i >= MAX_HANDLES || g_objs[i].kind == OBJ_FREE) return NULL;
    return &g_objs[i];
}

/* ---- object namespace: symbolic links and device roots ------------------ */
static char g_game_dir[MAX_PATH], g_hdd_dir[MAX_PATH];
typedef struct { char name[64]; char target[256]; } Symlink;
static Symlink g_symlinks[32];
static int g_nsymlinks;

static void add_symlink(const char *name, const char *target) {
    for (int i = 0; i < g_nsymlinks; i++)
        if (_stricmp(g_symlinks[i].name, name) == 0) { strncpy(g_symlinks[i].target, target, 255); return; }
    strncpy(g_symlinks[g_nsymlinks].name, name, 63);
    strncpy(g_symlinks[g_nsymlinks].target, target, 255);
    g_nsymlinks++;
}

static void read_ansi_string(uint32_t as, char *out, size_t n) {
    uint16_t len = RD16(MEM(as));
    uint32_t buf = RD32(MEM(as + 4));
    if (len >= n) len = (uint16_t)(n - 1);
    memcpy(out, MEM(buf), len);
    out[len] = 0;
}

/* Resolve a guest NT path (plus optional root directory handle) to a host
   path. Returns 0 if the device is unknown. */
static int resolve_path(uint32_t root_handle, const char *name, char *out, size_t n) {
    char full[1024];
    if (root_handle == 0xFFFFFFFDu) { /* ObDosDevicesDirectory: names like "d:\file" live under \??\ */
        snprintf(full, sizeof full, "\\??\\%s", name);
        return resolve_path(0, full, out, n);
    }
    if (root_handle == 0xFFFFFFFCu) root_handle = 0; /* ObWin32NamedObjectsDirectory: no files there */
    if (root_handle) {
        Obj *o = handle_get(root_handle);
        if (!o || (o->kind != OBJ_DIR && o->kind != OBJ_SYMLINK)) return 0;
        snprintf(full, sizeof full, "%s%s%s", o->path, (*name && *name != '\\') ? "\\" : "", name);
        /* o->path is already a host path */
        strncpy(out, full, n - 1);
        out[n - 1] = 0;
        return 1;
    }
    strncpy(full, name, sizeof full - 1);
    full[sizeof full - 1] = 0;
    /* expand symbolic links by prefix, a few rounds */
    for (int round = 0; round < 4; round++) {
        int changed = 0;
        for (int i = 0; i < g_nsymlinks; i++) {
            size_t l = strlen(g_symlinks[i].name);
            if (_strnicmp(full, g_symlinks[i].name, l) == 0 && (full[l] == 0 || full[l] == '\\')) {
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%s%s", g_symlinks[i].target, full + l);
                strcpy(full, tmp);
                changed = 1;
                break;
            }
        }
        if (!changed) break;
    }
    const char *rest;
    char base[MAX_PATH];
    if (_strnicmp(full, "\\Device\\CdRom0", 14) == 0) { strcpy(base, g_game_dir); rest = full + 14; }
    else if (_strnicmp(full, "\\Device\\Harddisk0\\Partition", 27) == 0) {
        int part = atoi(full + 27);
        rest = full + 27;
        while (*rest >= '0' && *rest <= '9') rest++;
        if (part == 0) { /* the whole disk: XAPI reads and writes raw sectors to pick a cache partition */
            snprintf(out, n, "%s\\disk0.bin", g_hdd_dir);
            return 1;
        }
        snprintf(base, sizeof base, "%s\\partition%d", g_hdd_dir, part);
    } else return 0;
    snprintf(out, n, "%s%s", base, rest);
    return 1;
}

static void ensure_dir(const char *path) {
    char tmp[MAX_PATH];
    strncpy(tmp, path, sizeof tmp - 1);
    for (char *p = tmp + 3; *p; p++)
        if (*p == '\\') { *p = 0; CreateDirectoryA(tmp, NULL); *p = '\\'; }
    CreateDirectoryA(tmp, NULL);
}

/* ---- time --------------------------------------------------------------- */
static uint64_t qpc_freq;
static uint64_t system_time(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}
static uint64_t interrupt_time(void) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e7 / (double)qpc_freq);
}

/* ---- threads ------------------------------------------------------------- */
/* KPCR layout (Xbox): 0x00 ExceptionList, 0x04 StackBase, 0x08 StackLimit,
   0x18 Self, 0x1C SelfPcr, 0x20 Prcb, 0x24 Irql, 0x28 Prcb.CurrentThread.
   KTHREAD: +0x28 TlsData. Both live in guest memory because guest code reads
   them through fs:. */
#define KPCR_SIZE 0x400
#define KTHREAD_SIZE 0x120
#define TLS_DATA_OFF 0x28
#define GUEST_STACK_HEADROOM 0x100 /* above StackBase: TLS slot at fs:[4] - 20 */

static uint32_t kmem_alloc_or_die(uint32_t size, uint32_t align) {
    uint32_t a = kmem_alloc(size, align);
    if (!a) { fprintf(stderr, "guest arena exhausted (%u bytes for the runtime)\n", size); exit(11); }
    return a;
}

uint32_t kernel_create_kpcr(uint32_t stack_base, uint32_t stack_limit, uint32_t tls) {
    uint32_t kpcr = kmem_alloc_or_die(KPCR_SIZE, 16);
    uint32_t kthread = kmem_alloc_or_die(KTHREAD_SIZE, 16);
    WR32(MEM(kpcr + 0x00), 0xffffffff);
    WR32(MEM(kpcr + 0x04), stack_base);
    WR32(MEM(kpcr + 0x08), stack_limit);
    WR32(MEM(kpcr + 0x18), kpcr);
    WR32(MEM(kpcr + 0x1c), kpcr);
    WR32(MEM(kpcr + 0x20), kpcr + 0x28);
    WR32(MEM(kpcr + 0x28), kthread);
    WR32(MEM(kthread + TLS_DATA_OFF), tls);
    WR32(MEM(kthread + 0x1c), stack_base);  /* KTHREAD.StackBase */
    WR32(MEM(kthread + 0x20), stack_limit); /* KTHREAD.StackLimit */
    return kpcr;
}

typedef struct {
    uint32_t start_routine, start_context, stack_size, tls_size, system_routine;
    uint32_t handle, kthread;
    HANDLE resume_event;
    int suspended;
} ThreadStart;

/* Host threads are created up front, before the XBE overwrites the loader's
   PE header at 0x10000: after that Windows refuses CreateThread (error 193,
   it reads the executable's header for the stack sizes). Guest threads run
   on these pooled host threads. */
#define POOL_SIZE 32
typedef struct {
    HANDLE thread, wake;
    void (*fn)(void *);
    void *arg;
    volatile LONG busy;
} PoolThread;
static PoolThread g_pool[POOL_SIZE];

static DWORD WINAPI pool_main(LPVOID p) {
    PoolThread *t = p;
    for (;;) {
        WaitForSingleObject(t->wake, INFINITE);
        t->fn(t->arg);
        InterlockedExchange(&t->busy, 0);
    }
}

void kernel_prepare_threads(void) {
    for (int i = 0; i < POOL_SIZE; i++) {
        g_pool[i].wake = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_pool[i].thread = CreateThread(NULL, 64 << 20, pool_main, &g_pool[i], 0, NULL);
    }
}

static HANDLE pool_run(void (*fn)(void *), void *arg) {
    for (int i = 0; i < POOL_SIZE; i++)
        if (!InterlockedCompareExchange(&g_pool[i].busy, 1, 0)) {
            g_pool[i].fn = fn;
            g_pool[i].arg = arg;
            SetEvent(g_pool[i].wake);
            return g_pool[i].thread;
        }
    fprintf(stderr, "out of pooled host threads (%d)", POOL_SIZE);
    exit(13);
}

static void guest_thread_main(void *param) {
    ThreadStart *ts = param;
    if (ts->suspended) WaitForSingleObject(ts->resume_event, INFINITE);
    kernel_run_thread(ts->start_routine, ts->start_context, ts->stack_size, ts->tls_size, ts->system_routine);
    free(ts);
}

static __declspec(thread) Ctx *t_ctx;
static __declspec(thread) jmp_buf *t_thread_exit;

void kernel_run_thread(uint32_t start_routine, uint32_t start_context, uint32_t stack_size,
                       uint32_t tls_size, uint32_t system_routine) {
    Ctx *ctx = _aligned_malloc(sizeof(Ctx), 16);
    memset(ctx, 0, sizeof *ctx);
    t_ctx = ctx;
    /* The kernel carves the TLS block off the top of the stack: TlsData sits
       at StackBase - TlsDataSize, which is what XAPI's _tls_index (-size/4)
       addresses through fs:[4]. XapiThreadStartup fills it in. */
    uint32_t tls_round = (tls_size + 15) & ~15u;
    uint32_t stack = kmem_alloc_or_die(stack_size + tls_round + GUEST_STACK_HEADROOM, 4096);
    uint32_t tls = stack + stack_size;
    uint32_t stack_base = tls + tls_size;
    uint32_t kpcr = kernel_create_kpcr(stack_base, stack, tls);
    WR32(ctx->r + REG_FS_OFFSET, kpcr);
    WR16(ctx->r + REG_FPUCW, 0x027f);
    WR32(ctx->r + REG_MXCSR, 0x1f80);
    for (int i = 0; i < 8; i++) WRF80SLOT(ctx->r + REG_ST0 + 0x10 * i, bits_f64(0xfff8000000000000ull));
    /* SystemRoutine(StartRoutine, StartContext), stdcall */
    uint32_t esp = tls - 16; /* the stack proper ends where the TLS block starts */
    WR32(MEM(esp + 4), start_routine);
    WR32(MEM(esp + 8), start_context);
    WR32(MEM(esp), 0xdead0000u); /* return address: the thread exits instead */
    WR32(ctx->r + REG_ESP, esp);
    trace("thread start: system %08x start %08x ctx %08x stack %08x..%08x", system_routine, start_routine,
          start_context, stack, stack_base);
    jmp_buf exit_jb;
    jmp_buf *prev = t_thread_exit;
    t_thread_exit = &exit_jb;
    if (!setjmp(exit_jb)) rt_call(ctx, system_routine);
    t_thread_exit = prev;
    trace("thread %08x returned", start_routine);
}

/* ---- kernel functions ------------------------------------------------------ */
#define K(name) static void k_##name(Ctx *ctx)
#define RET(v, n) do { SET_EAX(ctx, (v)); RET_STDCALL(ctx, (n)); return; } while (0)
#define RETC(v) do { SET_EAX(ctx, (v)); RET_CDECL(ctx); return; } while (0)

static uint32_t g_tick_count_addr, g_hardware_info_addr, g_launch_data_page_addr, g_hd_key_addr, g_sig_key_addr,
    g_disk_cache_count_addr, g_event_type_addr, g_thread_type_addr;

K(NtAllocateVirtualMemory) {
    uint32_t pbase = ARG(ctx, 0), zero_bits = ARG(ctx, 1), psize = ARG(ctx, 2), type = ARG(ctx, 3), prot = ARG(ctx, 4);
    (void)zero_bits; (void)prot;
    uint32_t base = RD32(MEM(pbase)), size = RD32(MEM(psize)), requested = base;
    size = (size + 0xfff) & ~0xfffu;
    if (!base) {
        /* MEM_RESERVE and MEM_COMMIT both get real memory; committing or
           reserving inside an earlier range is a no-op (identity mapping) */
        base = kmem_alloc(size, 4096);
        if (!base) RET(STATUS_NO_MEMORY_, 5);
    } else {
        base &= ~0xfffu;
        uint32_t blk = kmem_block_containing(base);
        if (!blk || base + size > blk + kmem_block_size(blk)) {
            if (blk) { /* commit past the end of the reservation: grow by moving? we cannot; refuse */
                trace("NtAllocateVirtualMemory: %08x+%x exceeds its reservation (%08x+%x)", base, size, blk, kmem_block_size(blk));
            } else {
                trace("NtAllocateVirtualMemory: %08x is not inside any reservation", base);
            }
            RET((NTSTATUS_)0xC0000018 /* STATUS_CONFLICTING_ADDRESSES */, 5);
        }
    }
    WR32(MEM(pbase), base);
    WR32(MEM(psize), size);
    trace("NtAllocateVirtualMemory(%08x) -> %08x size %x type %x", requested, base, size, type);
    RET(STATUS_SUCCESS_, 5);
}
K(NtFreeVirtualMemory) {
    uint32_t pbase = ARG(ctx, 0), type = ARG(ctx, 2);
    if (type & 0x8000) kmem_free(RD32(MEM(pbase))); /* MEM_RELEASE */
    RET(STATUS_SUCCESS_, 3);
}
K(NtQueryVirtualMemory) {
    uint32_t addr = ARG(ctx, 0), info = ARG(ctx, 1);
    /* MEMORY_BASIC_INFORMATION: BaseAddress, AllocationBase, AllocationProtect, RegionSize, State, Protect, Type */
    uint32_t size = kmem_block_size(addr);
    WR32(MEM(info + 0), addr);
    WR32(MEM(info + 4), addr);
    WR32(MEM(info + 8), 4);
    WR32(MEM(info + 12), size ? size : 0x1000);
    WR32(MEM(info + 16), size ? 0x1000 : 0x10000); /* MEM_COMMIT or MEM_FREE */
    WR32(MEM(info + 20), 4);
    WR32(MEM(info + 24), 0x20000);
    RET(STATUS_SUCCESS_, 2);
}
K(MmAllocateContiguousMemoryEx) {
    uint32_t size = ARG(ctx, 0), align = ARG(ctx, 3);
    uint32_t a = kmem_alloc(size, align ? align + 1 : 4096); /* 0 on failure, like the kernel */
    trace("MmAllocateContiguousMemoryEx %x -> %08x", size, a);
    RET(a, 5);
}
K(MmAllocateContiguousMemory) { RET(kmem_alloc(ARG(ctx, 0), 4096), 1); }
K(MmAllocateSystemMemory) { RET(kmem_alloc(ARG(ctx, 0), 4096), 4); }
K(MmFreeContiguousMemory) { kmem_free(ARG(ctx, 0)); RET(0, 1); }
K(MmFreeSystemMemory) { kmem_free(ARG(ctx, 0)); RET(0, 2); }
K(MmQueryAllocationSize) { RET(kmem_block_size(ARG(ctx, 0)), 1); }
K(MmPersistContiguousMemory) { RET(0, 3); }
K(MmSetAddressProtect) { RET(0, 3); }
K(MmQueryAddressProtect) { RET(4 /* PAGE_READWRITE */, 1); }
K(MmGetPhysicalAddress) { RET(ARG(ctx, 0), 1); }
K(MmLockUnlockBufferPages) { RET(0, 3); }
K(MmLockUnlockPhysicalPage) { RET(0, 2); }
K(MmClaimGpuInstanceMemory) { RET(0x84000000u, 2); }
K(MmQueryStatistics) {
    uint32_t stats = ARG(ctx, 0);
    uint32_t len = RD32(MEM(stats));
    /* MM_STATISTICS: Length, TotalPhysicalPages, AvailablePages, VirtualMemoryBytesCommitted, ... */
    for (uint32_t i = 4; i < len; i += 4) WR32(MEM(stats + i), 0);
    WR32(MEM(stats + 4), 0x4000);
    WR32(MEM(stats + 8), 0x2000);
    RET(STATUS_SUCCESS_, 1);
}
K(ExAllocatePoolWithTag) { RET(kmem_alloc(ARG(ctx, 0), 16), 2); }
K(ExAllocatePool) { RET(kmem_alloc(ARG(ctx, 0), 16), 1); }
K(ExFreePool) { kmem_free(ARG(ctx, 0)); RET(0, 1); }
K(ExQueryPoolBlockSize) { RET(kmem_block_size(ARG(ctx, 0)), 1); }

K(RtlNtStatusToDosError) {
    int32_t st = (int32_t)ARG(ctx, 0);
    uint32_t err = st >= 0 ? 0 : st == STATUS_OBJECT_NAME_NOT_FOUND_ ? 2 : st == STATUS_OBJECT_PATH_NOT_FOUND_ ? 3
                 : st == STATUS_NO_MEMORY_ ? 8 : st == STATUS_INVALID_PARAMETER_ ? 87 : 317;
    RET(err, 1);
}
K(RtlInitAnsiString) {
    uint32_t as = ARG(ctx, 0), src = ARG(ctx, 1);
    uint16_t len = src ? (uint16_t)strlen((char *)MEM(src)) : 0;
    WR16(MEM(as), len);
    WR16(MEM(as + 2), src ? len + 1 : 0);
    WR32(MEM(as + 4), src);
    RET(0, 2);
}
K(RtlEqualString) {
    char a[1024], b[1024];
    read_ansi_string(ARG(ctx, 0), a, sizeof a);
    read_ansi_string(ARG(ctx, 1), b, sizeof b);
    RET(ARG(ctx, 2) ? _stricmp(a, b) == 0 : strcmp(a, b) == 0, 3);
}
K(RtlCompareMemoryUlong) {
    uint32_t src = ARG(ctx, 0), len = ARG(ctx, 1), pat = ARG(ctx, 2), n = 0;
    while (n + 4 <= len && RD32(MEM(src + n)) == pat) n += 4;
    RET(n, 3);
}
/* RTL_CRITICAL_SECTION on Xbox is 0x1C bytes; we store a host CS pointer in it */
K(RtlInitializeCriticalSection) {
    uint32_t cs = ARG(ctx, 0);
    CRITICAL_SECTION *h = malloc(sizeof *h);
    InitializeCriticalSection(h);
    WR32(MEM(cs + 0x10), (uint32_t)(uintptr_t)h);
    WR32(MEM(cs + 0x14), 0x4353); /* marker */
    RET(0, 1);
}
static CRITICAL_SECTION *cs_of(uint32_t cs) {
    if (RD32(MEM(cs + 0x14)) != 0x4353) { /* used before initialization: init lazily */
        CRITICAL_SECTION *h = malloc(sizeof *h);
        InitializeCriticalSection(h);
        WR32(MEM(cs + 0x10), (uint32_t)(uintptr_t)h);
        WR32(MEM(cs + 0x14), 0x4353);
    }
    return (CRITICAL_SECTION *)(uintptr_t)RD32(MEM(cs + 0x10));
}
K(RtlEnterCriticalSection) { EnterCriticalSection(cs_of(ARG(ctx, 0))); RET(0, 1); }
K(RtlLeaveCriticalSection) { LeaveCriticalSection(cs_of(ARG(ctx, 0))); RET(0, 1); }
K(RtlTimeToTimeFields) {
    uint64_t t = RD64(MEM(ARG(ctx, 0)));
    uint32_t tf = ARG(ctx, 1);
    FILETIME ft = {(DWORD)t, (DWORD)(t >> 32)};
    SYSTEMTIME st;
    FileTimeToSystemTime(&ft, &st);
    WR16(MEM(tf), st.wYear); WR16(MEM(tf + 2), st.wMonth); WR16(MEM(tf + 4), st.wDay);
    WR16(MEM(tf + 6), st.wHour); WR16(MEM(tf + 8), st.wMinute); WR16(MEM(tf + 10), st.wSecond);
    WR16(MEM(tf + 12), st.wMilliseconds); WR16(MEM(tf + 14), st.wDayOfWeek);
    RET(0, 2);
}
K(RtlTimeFieldsToTime) {
    uint32_t tf = ARG(ctx, 0);
    SYSTEMTIME st = {RD16(MEM(tf)), RD16(MEM(tf + 2)), RD16(MEM(tf + 14)), RD16(MEM(tf + 4)),
                     RD16(MEM(tf + 6)), RD16(MEM(tf + 8)), RD16(MEM(tf + 10)), RD16(MEM(tf + 12))};
    FILETIME ft;
    int ok = SystemTimeToFileTime(&st, &ft);
    WR64(MEM(ARG(ctx, 1)), ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
    RET(ok, 2);
}
K(RtlRaiseException) {
    uint32_t rec = ARG(ctx, 0);
    fprintf(stderr, "RtlRaiseException code %08x at %08x (C++ throw not supported yet)\n", RD32(MEM(rec)),
            RD32(MEM(rec + 12)));
    rt_trap(ctx, TRAP_UNSUPPORTED, RD32(MEM(rec + 12)));
}
K(RtlUnwind) { fprintf(stderr, "RtlUnwind not supported yet\n"); rt_trap(ctx, TRAP_UNSUPPORTED, 0); }

K(KeQueryPerformanceCounter) {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    /* 64-bit return in EDX:EAX; scaled to the Xbox's 733 MHz counter */
    uint64_t v = (uint64_t)((double)c.QuadPart * 733333333.0 / (double)qpc_freq);
    WR32(ctx->r + REG_EDX, (uint32_t)(v >> 32));
    RET((uint32_t)v, 0);
}
K(KeQueryPerformanceFrequency) { WR32(ctx->r + REG_EDX, 0); RET(733333333u, 0); }
K(KeQuerySystemTime) { WR64(MEM(ARG(ctx, 0)), system_time()); RET(0, 1); }
K(KeQueryInterruptTime) {
    uint64_t v = interrupt_time();
    WR32(ctx->r + REG_EDX, (uint32_t)(v >> 32));
    RET((uint32_t)v, 0);
}
K(KeDelayExecutionThread) {
    uint32_t pint = ARG(ctx, 2);
    int64_t t = pint ? (int64_t)RD64(MEM(pint)) : 0;
    DWORD ms = t < 0 ? (DWORD)((-t) / 10000) : 0;
    Sleep(ms);
    RET(STATUS_SUCCESS_, 3);
}
K(KeStallExecutionProcessor) { RET(0, 1); }
K(KeTickCountUpdate) { (void)ctx; }

/* Kernel objects the game manipulates directly: events, timers, DPCs. We
   keep them as host handles stored inside the guest structure. */
#define KOBJ_HOST(g) ((HANDLE)(uintptr_t)RD32(MEM((g) + 8)))
K(KeInitializeDpc) { RET(0, 3); }
K(KeInsertQueueDpc) { RET(1, 3); }
K(KeRemoveQueueDpc) { RET(0, 1); }
K(KeInitializeTimerEx) {
    uint32_t timer = ARG(ctx, 0);
    HANDLE h = CreateWaitableTimerA(NULL, ARG(ctx, 1) == 0, NULL);
    WR32(MEM(timer + 8), (uint32_t)(uintptr_t)h);
    RET(0, 2);
}
K(KeSetTimer) {
    uint32_t timer = ARG(ctx, 0);
    LARGE_INTEGER due;
    due.QuadPart = (int64_t)(((uint64_t)ARG(ctx, 2) << 32) | ARG(ctx, 1));
    SetWaitableTimer(KOBJ_HOST(timer), &due, 0, NULL, NULL, FALSE);
    RET(0, 4);
}
K(KeCancelTimer) { CancelWaitableTimer(KOBJ_HOST(ARG(ctx, 0))); RET(1, 1); }
static void ensure_event(uint32_t ev) {
    if (RD32(MEM(ev + 12)) != 0x4556) {
        /* DISPATCHER_HEADER: Type(0)=Notification 1=Synchronization, SignalState at +4 */
        int manual = RD8(MEM(ev)) == 0;
        HANDLE h = CreateEventA(NULL, manual, RD32(MEM(ev + 4)) != 0, NULL);
        WR32(MEM(ev + 8), (uint32_t)(uintptr_t)h);
        WR32(MEM(ev + 12), 0x4556);
    }
}
K(KeSetEvent) { uint32_t ev = ARG(ctx, 0); ensure_event(ev); SetEvent(KOBJ_HOST(ev)); WR32(MEM(ev + 4), 1); RET(0, 3); }
K(KeWaitForSingleObject) {
    uint32_t obj = ARG(ctx, 0), pint = ARG(ctx, 4);
    int64_t t = pint ? (int64_t)RD64(MEM(pint)) : 0;
    DWORD ms = pint ? (t < 0 ? (DWORD)((-t) / 10000) : 0) : INFINITE;
    ensure_event(obj);
    DWORD r = WaitForSingleObject(KOBJ_HOST(obj), ms);
    RET(r == WAIT_TIMEOUT ? STATUS_TIMEOUT_ : STATUS_SUCCESS_, 5);
}
K(KeSetBasePriorityThread) { RET(0, 2); }
K(KeRaiseIrqlToDpcLevel) { RET(0, 0); }
K(KfRaiseIrql) { RET(0, 1); }
K(KfLowerIrql) { RET(0, 1); }
K(KeBugCheck) { fprintf(stderr, "KeBugCheck %08x\n", ARG(ctx, 0)); exit(12); }
K(KeSaveFloatingPointState) { RET(0, 1); }
K(KeRestoreFloatingPointState) { RET(0, 1); }
K(KeSynchronizeExecution) {
    /* call the routine directly: SynchronizeRoutine(SynchronizeContext) */
    uint32_t routine = ARG(ctx, 1), arg = ARG(ctx, 2);
    RET_STDCALL(ctx, 3);
    uint32_t esp = RD32(ctx->r + REG_ESP) - 8;
    WR32(MEM(esp + 4), arg);
    WR32(MEM(esp), 0xdead0001u);
    WR32(ctx->r + REG_ESP, esp);
    rt_call(ctx, routine);
    SET_EAX(ctx, 1);
}
K(HalReturnToFirmware) { fprintf(stderr, "HalReturnToFirmware(%u): game exited\n", ARG(ctx, 0)); exit(0); }
K(HalInitiateShutdown) { fprintf(stderr, "HalInitiateShutdown\n"); exit(0); }
K(HalIsResetOrShutdownPending) { RET(0, 0); }
K(HalRegisterShutdownNotification) { RET(0, 2); }
K(HalGetInterruptVector) { WR32(MEM(ARG(ctx, 1)), 0); RET(0x30 + ARG(ctx, 0), 2); }
K(HalBootSMCVideoMode) { RET(0, 0); }
K(KeConnectInterrupt) { RET(1, 1); }
K(KeDisconnectInterrupt) { RET(1, 1); }
K(KeInitializeInterrupt) { RET(0, 7); }
K(ExQueryNonVolatileSetting) {
    uint32_t index = ARG(ctx, 0), ptype = ARG(ctx, 1), value = ARG(ctx, 2), len = ARG(ctx, 3), presult = ARG(ctx, 4);
    /* EEPROM settings: 0x0A timezone bias.. we answer the ones XAPI asks for */
    uint32_t v = 0;
    switch (index) {
    case 7: v = 0x00000001; break;  /* XC_LANGUAGE: English */
    case 8: v = 0x00000001; break;  /* XC_VIDEO_FLAGS: 480p? widescreen flags */
    case 9: v = 0x00000000; break;  /* XC_AUDIO_FLAGS: stereo */
    case 10: v = 0; break;          /* XC_PARENTAL_CONTROL_GAMES */
    case 0x11: v = 0x00400100; break; /* XC_FACTORY_AV_REGION / misc */
    default: break;
    }
    if (ptype) WR32(MEM(ptype), 4);
    if (len >= 4) WR32(MEM(value), v);
    if (presult) WR32(MEM(presult), 4);
    trace("ExQueryNonVolatileSetting %u -> %08x", index, v);
    RET(STATUS_SUCCESS_, 5);
}
K(FscSetCacheSize) { RET(STATUS_SUCCESS_, 1); }
K(XeLoadSection) { RET(STATUS_SUCCESS_, 1); }
K(XeUnloadSection) { RET(STATUS_SUCCESS_, 1); }
K(AvGetSavedDataAddress) { RET(0, 0); }
K(AvSetSavedDataAddress) { RET(0, 1); }
K(AvSendTVEncoderOption) { if (ARG(ctx, 3)) WR32(MEM(ARG(ctx, 3)), 0); RET(0, 4); }
K(AvSetDisplayMode) { RET(0, 6); }
K(DbgPrint) {
    /* cdecl, printf-style; format the common cases by hand */
    const char *fmt = (const char *)MEM(ARG(ctx, 0));
    char buf[1024];
    uint32_t *args = (uint32_t *)MEM(RD32(ctx->r + REG_ESP) + 8);
    /* arguments are 32-bit on the guest; feed them to the host vsnprintf as such */
    snprintf(buf, sizeof buf, fmt, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
    fprintf(stderr, "[DbgPrint] %s", buf);
    if (!strchr(buf, '\n')) fputc('\n', stderr);
    RETC(0);
}

/* SHA-1 (XcSHA*): context layout = 4 bytes unused + state[5] + count + buffer */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t buflen; } Sha1;
static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
static void sha1_block(Sha1 *s, const uint8_t *p) {
    uint32_t w[80], a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (int i = 0; i < 16; i++) w[i] = (p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static Sha1 *sha_ctx(uint32_t g) { return (Sha1 *)MEM(g + 4); }
K(XcSHAInit) {
    Sha1 *s = sha_ctx(ARG(ctx, 0));
    memset(s, 0, sizeof *s);
    s->h[0] = 0x67452301; s->h[1] = 0xefcdab89; s->h[2] = 0x98badcfe; s->h[3] = 0x10325476; s->h[4] = 0xc3d2e1f0;
    RET(0, 1);
}
K(XcSHAUpdate) {
    Sha1 *s = sha_ctx(ARG(ctx, 0));
    const uint8_t *p = MEM(ARG(ctx, 1));
    uint32_t n = ARG(ctx, 2);
    s->len += n;
    while (n) {
        uint32_t take = 64 - s->buflen;
        if (take > n) take = n;
        memcpy(s->buf + s->buflen, p, take);
        s->buflen += take; p += take; n -= take;
        if (s->buflen == 64) { sha1_block(s, s->buf); s->buflen = 0; }
    }
    RET(0, 3);
}
K(XcSHAFinal) {
    Sha1 *s = sha_ctx(ARG(ctx, 0));
    uint8_t *out = MEM(ARG(ctx, 1));
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80;
    s->buf[s->buflen++] = pad;
    if (s->buflen > 56) { memset(s->buf + s->buflen, 0, 64 - s->buflen); sha1_block(s, s->buf); s->buflen = 0; }
    memset(s->buf + s->buflen, 0, 56 - s->buflen);
    for (int i = 0; i < 8; i++) s->buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
    sha1_block(s, s->buf);
    for (int i = 0; i < 5; i++) { out[4 * i] = s->h[i] >> 24; out[4 * i + 1] = s->h[i] >> 16; out[4 * i + 2] = s->h[i] >> 8; out[4 * i + 3] = s->h[i]; }
    RET(0, 2);
}

/* ---- files -------------------------------------------------------------- */
static uint32_t fill_network_open_info(Obj *o, uint32_t info) {
    BY_HANDLE_FILE_INFORMATION fi = {0};
    WIN32_FILE_ATTRIBUTE_DATA ad = {0};
    uint64_t ct = 0, at = 0, wt = 0, size = 0;
    uint32_t attrs = 0x80;
    if (o->kind == OBJ_FILE && GetFileInformationByHandle(o->host, &fi)) {
        ct = ((uint64_t)fi.ftCreationTime.dwHighDateTime << 32) | fi.ftCreationTime.dwLowDateTime;
        at = ((uint64_t)fi.ftLastAccessTime.dwHighDateTime << 32) | fi.ftLastAccessTime.dwLowDateTime;
        wt = ((uint64_t)fi.ftLastWriteTime.dwHighDateTime << 32) | fi.ftLastWriteTime.dwLowDateTime;
        size = ((uint64_t)fi.nFileSizeHigh << 32) | fi.nFileSizeLow;
        attrs = fi.dwFileAttributes;
    } else if (GetFileAttributesExA(o->path, GetFileExInfoStandard, &ad)) {
        wt = ((uint64_t)ad.ftLastWriteTime.dwHighDateTime << 32) | ad.ftLastWriteTime.dwLowDateTime;
        ct = at = wt;
        size = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
        attrs = ad.dwFileAttributes;
    }
    /* FILE_NETWORK_OPEN_INFORMATION: Creation, LastAccess, LastWrite, Change, AllocationSize, EndOfFile, Attributes */
    WR64(MEM(info + 0), ct); WR64(MEM(info + 8), at); WR64(MEM(info + 16), wt); WR64(MEM(info + 24), wt);
    WR64(MEM(info + 32), (size + 0xfff) & ~0xfffull); WR64(MEM(info + 40), size);
    WR32(MEM(info + 48), attrs & 0x37);
    return 0x38;
}

static void nt_open(Ctx *ctx, int create, int nargs) {
    uint32_t phandle = ARG(ctx, 0), access = ARG(ctx, 1), oa = ARG(ctx, 2), iosb = ARG(ctx, 3);
    uint32_t disposition = create ? ARG(ctx, 7) : 1 /* FILE_OPEN */;
    uint32_t options = create ? ARG(ctx, 8) : ARG(ctx, 5);
    uint32_t share = create ? ARG(ctx, 6) : ARG(ctx, 4);
    (void)share;
    /* OBJECT_ATTRIBUTES (Xbox): RootDirectory, ObjectName (ANSI_STRING*), Attributes */
    uint32_t root = RD32(MEM(oa)), pname = RD32(MEM(oa + 4));
    char name[1024] = "", path[1024];
    if (pname) read_ansi_string(pname, name, sizeof name);
    NTSTATUS_ st = STATUS_SUCCESS_;
    uint32_t h = 0, info = 1;
    if (!resolve_path(root, name, path, sizeof path)) {
        trace("%s %s (root %x): unknown device", create ? "NtCreateFile" : "NtOpenFile", name, root);
        st = STATUS_OBJECT_PATH_NOT_FOUND_;
    } else {
        /* strip a trailing backslash */
        size_t l = strlen(path);
        if (l > 3 && path[l - 1] == '\\') path[l - 1] = 0;
        DWORD attrs = GetFileAttributesA(path);
        int is_dir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        if ((options & 1) /* FILE_DIRECTORY_FILE */ || is_dir) {
            if (!is_dir) {
                if (disposition == 1) st = STATUS_OBJECT_NAME_NOT_FOUND_;
                else if (!CreateDirectoryA(path, NULL)) st = STATUS_OBJECT_NAME_NOT_FOUND_;
                else info = 2;
            } else if (disposition == 2 /* FILE_CREATE */) st = STATUS_OBJECT_NAME_COLLISION_;
            if (st == STATUS_SUCCESS_) {
                h = handle_alloc(OBJ_DIR);
                strncpy(handle_get(h)->path, path, MAX_PATH - 1);
            }
        } else {
            DWORD cd;
            switch (disposition) {
            case 0: cd = CREATE_ALWAYS; break;    /* FILE_SUPERSEDE */
            case 1: cd = OPEN_EXISTING; break;    /* FILE_OPEN */
            case 2: cd = CREATE_NEW; break;       /* FILE_CREATE */
            case 3: cd = OPEN_ALWAYS; break;      /* FILE_OPEN_IF */
            case 4: cd = TRUNCATE_EXISTING; break;/* FILE_OVERWRITE */
            default: cd = CREATE_ALWAYS; break;   /* FILE_OVERWRITE_IF */
            }
            DWORD da = 0;
            if (access & 0x80000000) da |= GENERIC_READ;
            if (access & 0x40000000) da |= GENERIC_WRITE;
            if (access & 0x0001) da |= FILE_READ_DATA;
            if (access & 0x0002) da |= FILE_WRITE_DATA;
            if (access & 0x0004) da |= FILE_APPEND_DATA;
            if (access & 0x0080) da |= FILE_READ_ATTRIBUTES;
            if (!da) da = GENERIC_READ;
            HANDLE hf = CreateFileA(path, da, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, cd, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hf == INVALID_HANDLE_VALUE) {
                DWORD e = GetLastError();
                st = e == ERROR_FILE_EXISTS ? STATUS_OBJECT_NAME_COLLISION_
                   : e == ERROR_PATH_NOT_FOUND ? STATUS_OBJECT_PATH_NOT_FOUND_ : STATUS_OBJECT_NAME_NOT_FOUND_;
            } else {
                h = handle_alloc(OBJ_FILE);
                handle_get(h)->host = hf;
                strncpy(handle_get(h)->path, path, MAX_PATH - 1);
                info = (cd == CREATE_NEW || (cd == OPEN_ALWAYS && GetLastError() != ERROR_ALREADY_EXISTS)) ? 2 : 1;
            }
        }
    }
    trace("%s %s -> host %s : %08x handle %x", create ? "NtCreateFile" : "NtOpenFile", name, path, st, h);
    if (phandle) WR32(MEM(phandle), h);
    if (iosb) { WR32(MEM(iosb), st); WR32(MEM(iosb + 4), info); }
    RET(st, nargs);
}
K(NtCreateFile) { nt_open(ctx, 1, 9); }
K(NtOpenFile) { nt_open(ctx, 0, 6); }
K(NtClose) {
    uint32_t h = ARG(ctx, 0);
    Obj *o = handle_get(h);
    if (!o) RET(STATUS_INVALID_HANDLE_, 1);
    if (o->host && (o->kind == OBJ_FILE || o->kind == OBJ_EVENT || o->kind == OBJ_THREAD)) CloseHandle(o->host);
    free(o->dir_pattern);
    o->kind = OBJ_FREE;
    RET(STATUS_SUCCESS_, 1);
}
K(NtReadFile) {
    uint32_t h = ARG(ctx, 0), iosb = ARG(ctx, 4), buf = ARG(ctx, 5), len = ARG(ctx, 6), poff = ARG(ctx, 7);
    Obj *o = handle_get(h);
    if (!o || o->kind != OBJ_FILE) RET(STATUS_INVALID_HANDLE_, 8);
    if (poff) {
        int64_t off = (int64_t)RD64(MEM(poff));
        if (off >= 0) { LARGE_INTEGER li; li.QuadPart = off; SetFilePointerEx(o->host, li, NULL, FILE_BEGIN); }
    }
    LARGE_INTEGER before = {0}, zero = {0};
    SetFilePointerEx(o->host, zero, &before, FILE_CURRENT);
    DWORD got = 0;
    BOOL ok = ReadFile(o->host, MEM(buf), len, &got, NULL);
    NTSTATUS_ st = !ok ? STATUS_UNSUCCESSFUL_ : got == 0 && len ? STATUS_END_OF_FILE_ : STATUS_SUCCESS_;
    trace("  read %s: %u bytes at %lld -> %lu (buf %08x, offset arg %s)", o->path, len, before.QuadPart, got, buf,
          poff ? ((int64_t)RD64(MEM(poff)) < 0 ? "current" : "explicit") : "none");
    if (iosb) { WR32(MEM(iosb), st); WR32(MEM(iosb + 4), got); }
    RET(st, 8);
}
K(NtWriteFile) {
    uint32_t h = ARG(ctx, 0), iosb = ARG(ctx, 4), buf = ARG(ctx, 5), len = ARG(ctx, 6), poff = ARG(ctx, 7);
    Obj *o = handle_get(h);
    if (!o || o->kind != OBJ_FILE) RET(STATUS_INVALID_HANDLE_, 8);
    if (poff) {
        int64_t off = (int64_t)RD64(MEM(poff));
        if (off >= 0) { LARGE_INTEGER li; li.QuadPart = off; SetFilePointerEx(o->host, li, NULL, FILE_BEGIN); }
    }
    DWORD put = 0;
    BOOL ok = WriteFile(o->host, MEM(buf), len, &put, NULL);
    if (iosb) { WR32(MEM(iosb), ok ? 0 : STATUS_UNSUCCESSFUL_); WR32(MEM(iosb + 4), put); }
    RET(ok ? STATUS_SUCCESS_ : STATUS_UNSUCCESSFUL_, 8);
}
K(NtFlushBuffersFile) { Obj *o = handle_get(ARG(ctx, 0)); if (o && o->kind == OBJ_FILE) FlushFileBuffers(o->host); RET(0, 2); }
K(NtQueryInformationFile) {
    uint32_t h = ARG(ctx, 0), iosb = ARG(ctx, 1), info = ARG(ctx, 2), len = ARG(ctx, 3), cls = ARG(ctx, 4);
    Obj *o = handle_get(h);
    if (!o) RET(STATUS_INVALID_HANDLE_, 5);
    NTSTATUS_ st = STATUS_SUCCESS_;
    uint32_t written = 0;
    LARGE_INTEGER size = {0}, pos = {0}, zero = {0};
    if (o->kind == OBJ_FILE) { GetFileSizeEx(o->host, &size); SetFilePointerEx(o->host, zero, &pos, FILE_CURRENT); }
    switch (cls) {
    case 0x22: written = fill_network_open_info(o, info); break;              /* FileNetworkOpenInformation */
    case 5:  WR64(MEM(info), (uint64_t)size.QuadPart); WR64(MEM(info + 8), (uint64_t)size.QuadPart);
             WR32(MEM(info + 16), 1); WR8(MEM(info + 20), 0); WR8(MEM(info + 21), o->kind == OBJ_DIR); written = 24; break; /* FileStandardInformation */
    case 14: WR64(MEM(info), (uint64_t)pos.QuadPart); written = 8; break;       /* FilePositionInformation */
    case 4: { /* FileBasicInformation: 4 times + attributes = 0x28 bytes */
        uint8_t tmp[0x38];
        uint32_t save = kmem_alloc(0x38, 16);
        fill_network_open_info(o, save);
        memcpy(tmp, MEM(save), 0x38);
        kmem_free(save);
        memcpy(MEM(info), tmp, 32);
        WR32(MEM(info + 32), RD32((uint8_t *)tmp + 48));
        WR32(MEM(info + 36), 0);
        written = 0x28;
        break;
    }
    default: trace("NtQueryInformationFile class %u not handled", cls); st = STATUS_NOT_IMPLEMENTED_; break;
    }
    (void)len;
    if (iosb) { WR32(MEM(iosb), st); WR32(MEM(iosb + 4), written); }
    RET(st, 5);
}
K(NtSetInformationFile) {
    uint32_t h = ARG(ctx, 0), iosb = ARG(ctx, 1), info = ARG(ctx, 2), cls = ARG(ctx, 4);
    Obj *o = handle_get(h);
    if (!o) RET(STATUS_INVALID_HANDLE_, 5);
    NTSTATUS_ st = STATUS_SUCCESS_;
    LARGE_INTEGER li;
    trace("  set-info class %u on %s", cls, o->path);
    switch (cls) {
    case 14: li.QuadPart = (int64_t)RD64(MEM(info)); if (o->kind == OBJ_FILE) SetFilePointerEx(o->host, li, NULL, FILE_BEGIN);
             trace("  seek to %lld", li.QuadPart); break;
    case 20: case 19: /* FileEndOfFile / FileAllocation */
        li.QuadPart = (int64_t)RD64(MEM(info));
        if (o->kind == OBJ_FILE) { LARGE_INTEGER cur, z = {0}; SetFilePointerEx(o->host, z, &cur, FILE_CURRENT);
            SetFilePointerEx(o->host, li, NULL, FILE_BEGIN); SetEndOfFile(o->host); SetFilePointerEx(o->host, cur, NULL, FILE_BEGIN); }
        break;
    case 4: break;  /* FileBasicInformation: attributes; ignore */
    case 13: break; /* FileDispositionInformation: delete on close */
    default: trace("NtSetInformationFile class %u not handled", cls); break;
    }
    if (iosb) { WR32(MEM(iosb), st); WR32(MEM(iosb + 4), 0); }
    RET(st, 5);
}
K(NtQueryVolumeInformationFile) {
    uint32_t iosb = ARG(ctx, 1), info = ARG(ctx, 2), cls = ARG(ctx, 4);
    uint32_t written = 0;
    if (cls == 3) { /* FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits, SectorsPerUnit, BytesPerSector */
        WR64(MEM(info), 0x100000); WR64(MEM(info + 8), 0x80000); WR32(MEM(info + 16), 32); WR32(MEM(info + 20), 512);
        written = 24;
    } else if (cls == 1) { /* FileFsVolumeInformation */
        memset(MEM(info), 0, 0x12); WR32(MEM(info + 8), 0x4f424957); written = 0x12;
    } else if (cls == 4) { /* FileFsDeviceInformation: DeviceType, Characteristics */
        WR32(MEM(info), 7); WR32(MEM(info + 4), 0); written = 8;
    }
    if (iosb) { WR32(MEM(iosb), 0); WR32(MEM(iosb + 4), written); }
    RET(STATUS_SUCCESS_, 5);
}
K(NtQueryDirectoryFile) {
    /* (FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length,
        FileInformationClass, FileName, RestartScan): one FILE_DIRECTORY_INFORMATION per call */
    uint32_t h = ARG(ctx, 0), iosb = ARG(ctx, 4), info = ARG(ctx, 5), pname = ARG(ctx, 8), restart = ARG(ctx, 9);
    Obj *o = handle_get(h);
    if (!o || o->kind != OBJ_DIR) RET(STATUS_INVALID_HANDLE_, 10);
    if (pname) {
        char pat[MAX_PATH];
        read_ansi_string(pname, pat, sizeof pat);
        free(o->dir_pattern);
        o->dir_pattern = _strdup(pat);
        o->dir_started = 0;
    }
    if (restart) o->dir_started = 0;
    char spec[MAX_PATH];
    snprintf(spec, sizeof spec, "%s\\%s", o->path, o->dir_pattern ? o->dir_pattern : "*");
    WIN32_FIND_DATAA fd;
    NTSTATUS_ st = STATUS_NO_MORE_FILES_;
    if (!o->dir_started) {
        if (o->host) FindClose(o->host);
        o->host = FindFirstFileA(spec, &fd);
        o->dir_started = 1;
        if (o->host == INVALID_HANDLE_VALUE) { o->host = NULL; }
        else st = STATUS_SUCCESS_;
    } else if (o->host && FindNextFileA(o->host, &fd)) st = STATUS_SUCCESS_;
    while (st == STATUS_SUCCESS_ && (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0))
        st = FindNextFileA(o->host, &fd) ? STATUS_SUCCESS_ : STATUS_NO_MORE_FILES_;
    uint32_t written = 0;
    if (st == STATUS_SUCCESS_) {
        uint32_t namelen = (uint32_t)strlen(fd.cFileName);
        WR32(MEM(info), 0); WR32(MEM(info + 4), 0);
        WR64(MEM(info + 8), ((uint64_t)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime);
        WR64(MEM(info + 16), ((uint64_t)fd.ftLastAccessTime.dwHighDateTime << 32) | fd.ftLastAccessTime.dwLowDateTime);
        WR64(MEM(info + 24), ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
        WR64(MEM(info + 32), ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
        WR64(MEM(info + 40), ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        WR64(MEM(info + 48), ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
        WR32(MEM(info + 56), fd.dwFileAttributes & 0x37);
        WR32(MEM(info + 60), namelen);
        memcpy(MEM(info + 64), fd.cFileName, namelen + 1);
        written = 64 + namelen + 1;
    }
    if (iosb) { WR32(MEM(iosb), st); WR32(MEM(iosb + 4), written); }
    RET(st, 10);
}
/* Volume and disk IOCTLs (cache partition format/verify): there is nothing
   to do on a host directory, so report success and no output. */
static void ioctl_ok(Ctx *ctx) {
    uint32_t iosb = ARG(ctx, 4), code = ARG(ctx, 5);
    trace("ioctl %08x on handle %x: ok", code, ARG(ctx, 0));
    if (iosb) { WR32(MEM(iosb), 0); WR32(MEM(iosb + 4), 0); }
    RET(STATUS_SUCCESS_, 10);
}
K(NtFsControlFile) { ioctl_ok(ctx); }
K(NtDeviceIoControlFile) { ioctl_ok(ctx); }
K(IoCreateSymbolicLink) {
    char name[256], target[256];
    read_ansi_string(ARG(ctx, 0), name, sizeof name);
    read_ansi_string(ARG(ctx, 1), target, sizeof target);
    trace("IoCreateSymbolicLink %s -> %s", name, target);
    add_symlink(name, target);
    RET(STATUS_SUCCESS_, 2);
}
K(NtOpenSymbolicLinkObject) {
    uint32_t phandle = ARG(ctx, 0), oa = ARG(ctx, 1);
    char name[256];
    read_ansi_string(RD32(MEM(oa + 4)), name, sizeof name);
    for (int i = 0; i < g_nsymlinks; i++)
        if (_stricmp(g_symlinks[i].name, name) == 0) {
            uint32_t h = handle_alloc(OBJ_SYMLINK);
            strncpy(handle_get(h)->path, g_symlinks[i].target, MAX_PATH - 1);
            WR32(MEM(phandle), h);
            RET(STATUS_SUCCESS_, 2);
        }
    RET(STATUS_OBJECT_NAME_NOT_FOUND_, 2);
}
K(NtQuerySymbolicLinkObject) {
    Obj *o = handle_get(ARG(ctx, 0));
    uint32_t as = ARG(ctx, 1), plen = ARG(ctx, 2);
    if (!o || o->kind != OBJ_SYMLINK) RET(STATUS_INVALID_HANDLE_, 3);
    uint16_t max = RD16(MEM(as + 2)), len = (uint16_t)strlen(o->path);
    if (len > max) len = max;
    memcpy(MEM(RD32(MEM(as + 4))), o->path, len);
    WR16(MEM(as), len);
    if (plen) WR32(MEM(plen), len);
    RET(STATUS_SUCCESS_, 3);
}
K(IoCreateDevice) { RET(STATUS_NOT_IMPLEMENTED_, 6); }
K(IoInvalidDeviceRequest) { RET(STATUS_NOT_IMPLEMENTED_, 2); }
K(IoStartPacket) { RET(0, 3); }
K(IoStartNextPacket) { RET(0, 1); }
K(IofCompleteRequest) { RET(0, 2); }
K(IoMarkIrpMustComplete) { RET(0, 1); }

/* events through handles */
K(NtSetEvent) {
    Obj *o = handle_get(ARG(ctx, 0));
    if (!o) RET(STATUS_INVALID_HANDLE_, 2);
    if (o->kind == OBJ_EVENT) SetEvent(o->host);
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), 0);
    RET(STATUS_SUCCESS_, 2);
}
K(NtWaitForSingleObject) {
    Obj *o = handle_get(ARG(ctx, 0));
    uint32_t pint = ARG(ctx, 2);
    if (!o) RET(STATUS_INVALID_HANDLE_, 3);
    int64_t t = pint ? (int64_t)RD64(MEM(pint)) : 0;
    DWORD ms = pint ? (t < 0 ? (DWORD)((-t) / 10000) : 0) : INFINITE;
    DWORD r = WaitForSingleObject(o->host, ms);
    RET(r == WAIT_TIMEOUT ? STATUS_TIMEOUT_ : STATUS_SUCCESS_, 3);
}
K(ObReferenceObjectByHandle) {
    Obj *o = handle_get(ARG(ctx, 0));
    uint32_t pobj = ARG(ctx, 2);
    if (!o) RET(STATUS_INVALID_HANDLE_, 3);
    WR32(MEM(pobj), o->guest_obj ? o->guest_obj : ARG(ctx, 0));
    RET(STATUS_SUCCESS_, 3);
}
K(ObfDereferenceObject) { RET(0, 0); /* fastcall, arg in ECX */ }
K(NtUserIoApcDispatcher) { RET(0, 3); }

/* threads */
K(PsCreateSystemThreadEx) {
    /* (ThreadHandle*, ThreadExtraSize, KernelStackSize, TlsDataSize, ThreadId*, StartContext1,
        StartContext2, CreateSuspended, DebuggerThread, SystemRoutine) */
    uint32_t phandle = ARG(ctx, 0), stack_size = ARG(ctx, 2), tls_size = ARG(ctx, 3), pid = ARG(ctx, 4);
    uint32_t ctx1 = ARG(ctx, 5), ctx2 = ARG(ctx, 6), suspended = ARG(ctx, 7), sysroutine = ARG(ctx, 9);
    ThreadStart *ts = calloc(1, sizeof *ts);
    ts->start_routine = ctx1;
    ts->start_context = ctx2;
    ts->stack_size = stack_size ? stack_size : 0x4000;
    ts->tls_size = tls_size;
    ts->system_routine = sysroutine;
    ts->suspended = suspended & 0xff;
    ts->resume_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    uint32_t h = handle_alloc(OBJ_THREAD);
    Obj *o = handle_get(h);
    o->host = pool_run(guest_thread_main, ts);
    DWORD tid = GetThreadId(o->host);
    o->guest_obj = (uint32_t)(uintptr_t)ts; /* used by NtResumeThread */
    if (phandle) WR32(MEM(phandle), h);
    if (pid) WR32(MEM(pid), tid);
    trace("PsCreateSystemThreadEx start %08x ctx %08x stack %x tls %x suspended %u -> handle %x", ctx1, ctx2,
          stack_size, tls_size, suspended, h);
    RET(STATUS_SUCCESS_, 10);
}
K(NtResumeThread) {
    Obj *o = handle_get(ARG(ctx, 0));
    if (!o || o->kind != OBJ_THREAD) RET(STATUS_INVALID_HANDLE_, 2);
    ThreadStart *ts = (ThreadStart *)(uintptr_t)o->guest_obj;
    if (ts) SetEvent(ts->resume_event);
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), 1);
    RET(STATUS_SUCCESS_, 2);
}
K(PsTerminateSystemThread) {
    trace("PsTerminateSystemThread(%08x)", ARG(ctx, 0));
    if (t_thread_exit) longjmp(*t_thread_exit, 1); /* back to the pool loop */
    ExitThread(ARG(ctx, 0));
}

/* ---- dispatch ------------------------------------------------------------- */
typedef void (*KFn)(Ctx *);
typedef struct { const char *name; KFn fn; } KEntry;
#define E(n) {#n, k_##n}
static const KEntry k_table[] = {
    E(NtAllocateVirtualMemory), E(NtFreeVirtualMemory), E(NtQueryVirtualMemory), E(MmQueryStatistics),
    E(MmPersistContiguousMemory), E(MmAllocateContiguousMemory), E(MmFreeContiguousMemory), E(RtlNtStatusToDosError),
    E(HalReturnToFirmware), E(NtClose), E(NtQuerySymbolicLinkObject), E(NtOpenSymbolicLinkObject), E(RtlInitAnsiString),
    E(NtSetEvent), E(KeDelayExecutionThread), E(NtSetInformationFile), E(NtOpenFile), E(FscSetCacheSize),
    E(NtQueryDirectoryFile), E(KfLowerIrql), E(KeRaiseIrqlToDpcLevel), E(IoCreateDevice), E(ExAllocatePool), E(KeSetEvent),
    E(KeWaitForSingleObject), E(KeInitializeTimerEx), E(KeInitializeDpc), E(IoInvalidDeviceRequest), E(KeSetTimer),
    E(KeCancelTimer), E(ObfDereferenceObject), E(ObReferenceObjectByHandle), E(MmAllocateContiguousMemoryEx),
    E(MmQueryAllocationSize), E(MmSetAddressProtect), E(MmQueryAddressProtect), E(NtCreateFile), E(NtWriteFile),
    E(NtReadFile), E(NtQueryInformationFile), E(MmFreeSystemMemory), E(MmAllocateSystemMemory), E(ExFreePool),
    E(IoStartPacket), E(IofCompleteRequest), E(IoStartNextPacket), E(IoMarkIrpMustComplete),
    E(HalIsResetOrShutdownPending), E(KeQueryInterruptTime), E(HalInitiateShutdown), E(RtlLeaveCriticalSection),
    E(RtlEnterCriticalSection), E(RtlCompareMemoryUlong), E(RtlInitializeCriticalSection), E(RtlRaiseException),
    E(NtQueryVolumeInformationFile), E(IoCreateSymbolicLink), E(KeQuerySystemTime), E(KeSetBasePriorityThread),
    E(NtResumeThread), E(PsTerminateSystemThread), E(PsCreateSystemThreadEx), E(HalGetInterruptVector), E(KfRaiseIrql),
    E(HalRegisterShutdownNotification), E(KeConnectInterrupt), E(KeInitializeInterrupt), E(KeStallExecutionProcessor),
    E(ExQueryNonVolatileSetting), E(HalBootSMCVideoMode), E(RtlEqualString), E(XeLoadSection), E(XeUnloadSection),
    E(NtFsControlFile), E(MmGetPhysicalAddress), E(MmLockUnlockBufferPages), E(KeInsertQueueDpc),
    E(MmLockUnlockPhysicalPage), E(NtDeviceIoControlFile), E(RtlUnwind), E(KeBugCheck), E(XcSHAUpdate), E(XcSHAInit),
    E(XcSHAFinal), E(RtlTimeToTimeFields), E(RtlTimeFieldsToTime), E(KeQueryPerformanceCounter),
    E(KeQueryPerformanceFrequency), E(NtWaitForSingleObject), E(NtFlushBuffersFile), E(NtUserIoApcDispatcher),
    E(AvGetSavedDataAddress), E(AvSendTVEncoderOption), E(AvSetDisplayMode), E(AvSetSavedDataAddress), E(DbgPrint),
    E(KeDisconnectInterrupt), E(MmClaimGpuInstanceMemory), E(ExQueryPoolBlockSize), E(ExAllocatePoolWithTag),
    E(KeRemoveQueueDpc), E(KeSynchronizeExecution), E(KeSaveFloatingPointState), E(KeRestoreFloatingPointState),
};
#undef E

static KFn k_by_ordinal[400];

void kernel_call(Ctx *ctx, uint32_t ordinal) {
    KFn fn = ordinal < 400 ? k_by_ordinal[ordinal] : NULL;
    if (g_trace) trace("kernel %s(%08x, %08x, %08x, %08x)", ordinal_name(ordinal), ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2), ARG(ctx, 3));
    if (!fn) {
        fprintf(stderr, "kernel function %s (ordinal %u) not implemented\n", ordinal_name(ordinal), ordinal);
        rt_trap(ctx, TRAP_UNSUPPORTED, KERNEL_MAGIC | ordinal);
    }
    fn(ctx);
}

static void tick_thread(void *p) {
    (void)p;
    for (;;) { Sleep(1); WR32(MEM(g_tick_count_addr), GetTickCount()); }
}

void kernel_init(uint32_t thunk_table, const char *game_dir, const char *hdd_dir) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    qpc_freq = (uint64_t)f.QuadPart;
    InitializeCriticalSection(&obj_lock);
    strncpy(g_game_dir, game_dir, MAX_PATH - 1);
    strncpy(g_hdd_dir, hdd_dir, MAX_PATH - 1);
    for (int p = 1; p <= 7; p++) { char d[MAX_PATH]; snprintf(d, sizeof d, "%s\\partition%d", hdd_dir, p); ensure_dir(d); }
    {   /* raw disk image for \Device\Harddisk0\Partition0: a few zeroed sectors are enough */
        char d[MAX_PATH];
        snprintf(d, sizeof d, "%s\\disk0.bin", hdd_dir);
        HANDLE h = CreateFileA(d, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            static const uint8_t zero[0x10000];
            DWORD put;
            WriteFile(h, zero, sizeof zero, &put, NULL);
            CloseHandle(h);
        }
    }

    for (size_t i = 0; i < sizeof k_table / sizeof k_table[0]; i++)
        for (size_t j = 0; j < N_IMPORTS; j++)
            if (strcmp(k_table[i].name, k_imports[j].name) == 0) k_by_ordinal[k_imports[j].ordinal] = k_table[i].fn;

    /* data exports live in guest memory */
    g_tick_count_addr = kmem_alloc(4, 4);
    g_hardware_info_addr = kmem_alloc(16, 4);
    g_launch_data_page_addr = kmem_alloc(4, 4); /* LaunchDataPage is a pointer variable: NULL = cold boot */
    g_hd_key_addr = kmem_alloc(16, 4);
    g_sig_key_addr = kmem_alloc(16, 4);
    g_disk_cache_count_addr = kmem_alloc(4, 4);
    g_event_type_addr = kmem_alloc(32, 4);
    g_thread_type_addr = kmem_alloc(32, 4);
    WR32(MEM(g_hardware_info_addr), 0x00000000); /* Flags: retail, no special hardware */
    WR32(MEM(g_disk_cache_count_addr), 3);
    WR32(MEM(g_tick_count_addr), GetTickCount());
    pool_run(tick_thread, NULL);

    for (size_t j = 0; j < N_IMPORTS; j++) {
        const Import *im = &k_imports[j];
        uint32_t slot = thunk_table + (im->slot - 0x003bc220);
        uint32_t v = KERNEL_MAGIC | im->ordinal;
        if (im->is_data) {
            if (strcmp(im->name, "KeTickCount") == 0) v = g_tick_count_addr;
            else if (strcmp(im->name, "XboxHardwareInfo") == 0) v = g_hardware_info_addr;
            else if (strcmp(im->name, "LaunchDataPage") == 0) v = g_launch_data_page_addr;
            else if (strcmp(im->name, "XboxHDKey") == 0) v = g_hd_key_addr;
            else if (strcmp(im->name, "XboxSignatureKey") == 0) v = g_sig_key_addr;
            else if (strcmp(im->name, "HalDiskCachePartitionCount") == 0) v = g_disk_cache_count_addr;
            else if (strcmp(im->name, "ExEventObjectType") == 0) v = g_event_type_addr;
            else if (strcmp(im->name, "PsThreadObjectType") == 0) v = g_thread_type_addr;
        }
        WR32(MEM(slot), v);
    }
    /* devices the XBE loader would have linked */
    add_symlink("\\??\\D:", "\\Device\\CdRom0");
}
