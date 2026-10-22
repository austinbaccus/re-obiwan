/* DirectSound stand-in: the game gets a listener-less, silent IDirectSound.
   The real library drives the MCPX audio processor through MMIO. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

#include "funcs.h"
#include "kernel.h"

#define HOOK(name) void hook_##name(Ctx *ctx)
#define RET(name, v) do { trace("hook %s -> %08x (pops %d)", #name, (uint32_t)(v), HOOKARGS_##name); SET_EAX(ctx, (v)); RET_STDCALL(ctx, HOOKARGS_##name); return; } while (0)

/* Objects handed to the game: a vtable whose every slot is a host "virtual
   call" id, so a method we have not implemented reports its slot number
   instead of running library code (see rt_call / host_vcall). */
#define VCALL_BASE 0xFFFE0000u
#define IFACE_DSOUND 1
#define IFACE_STREAM 2
#define IFACE_BUFFER 3

static uint32_t make_object(uint32_t iface, uint32_t size) {
    uint32_t vt = kmem_alloc(64 * 4, 16), o = kmem_alloc(size, 16);
    for (uint32_t i = 0; i < 64; i++) WR32(MEM(vt + 4 * i), VCALL_BASE | (iface << 8) | i);
    WR32(MEM(o), vt);
    WR32(MEM(o + 4), 1); /* refcount */
    return o;
}

/* Xbox IDirectSound / IDirectSoundStream / IDirectSoundBuffer vtables: slot
   -> stdcall argument count (including this). Unknown slots trap. */
static const int8_t dsound_nargs[] = {1, 1, 2, 4, 4, 2, 3, 1, 5, 5, 5, 1, 2, 3, 3, 3, 3, 3, 3, 8, 5, 5, 1, 2, 3, 1};
static const int8_t stream_nargs[] = {1, 1, 2, 2, 3, 3, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 2, 2, 2, 3};
static const int8_t buffer_nargs[] = {1, 1, 2, 3, 3, 3, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 4, 2, 2, 2, 3};

void host_vcall(Ctx *ctx, uint32_t id) {
    uint32_t iface = (id >> 8) & 0xff, slot = id & 0xff;
    const int8_t *tab = iface == IFACE_DSOUND ? dsound_nargs : iface == IFACE_STREAM ? stream_nargs : buffer_nargs;
    size_t n = iface == IFACE_DSOUND ? sizeof dsound_nargs : iface == IFACE_STREAM ? sizeof stream_nargs : sizeof buffer_nargs;
    if (slot >= n) {
        fprintf(stderr, "dsound: unknown vtable slot %u on interface %u\n", slot, iface);
        rt_trap(ctx, TRAP_UNSUPPORTED, id);
    }
    trace("dsound: iface %u slot %u (%d args)", iface, slot, tab[slot]);
    uint32_t result = 0;
    if (slot == 0) result = 2;      /* AddRef */
    else if (slot == 1) result = 1; /* Release: never destroy */
    else if (iface == IFACE_DSOUND && (slot == 3 || slot == 4)) {
        /* CreateSoundBuffer / CreateSoundStream (pDesc, ppObject, pUnkOuter) */
        uint32_t pp = ARG(ctx, 2);
        if (pp) WR32(MEM(pp), make_object(slot == 3 ? IFACE_BUFFER : IFACE_STREAM, 0x100));
    } else if (iface == IFACE_BUFFER && slot == 8) {
        /* GetStatus(pdwStatus) */
        if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), 0);
    }
    SET_EAX(ctx, result);
    RET_STDCALL(ctx, tab[slot]);
}

HOOK(DirectSoundCreate) {
    /* (pguidDeviceId, ppDirectSound, pUnkOuter) */
    uint32_t pp = ARG(ctx, 1);
    if (pp) WR32(MEM(pp), make_object(IFACE_DSOUND, 0x100));
    fprintf(stderr, "dsound: created (silent)\n");
    RET(DirectSoundCreate, 0);
}
HOOK(DirectSoundCreateStream) {
    /* (pdssd, ppStream) */
    uint32_t pp = ARG(ctx, 1);
    if (pp) WR32(MEM(pp), make_object(IFACE_STREAM, 0x100));
    RET(DirectSoundCreateStream, 0);
}
HOOK(DirectSoundDoWork) { RET(DirectSoundDoWork, 0); }
HOOK(IDirectSound_Release) { RET(IDirectSound_Release, 0); }
HOOK(IDirectSoundBuffer_Release) { RET(IDirectSoundBuffer_Release, 0); }
HOOK(IDirectSoundStream_Pause) { RET(IDirectSoundStream_Pause, 0); }
HOOK(IDirectSound_GetCaps) {
    uint32_t pcaps = ARG(ctx, 1);
    if (pcaps) { memset(MEM(pcaps), 0, 0x14); WR32(MEM(pcaps + 4), 1024 * 1024); WR32(MEM(pcaps + 8), 1024 * 1024); }
    RET(IDirectSound_GetCaps, 0);
}
HOOK(IDirectSound_DownloadEffectsImage) {
    /* (pThis, pvImageBuffer, dwImageSize, pImageLoc, ppImageDesc) */
    if (ARG(ctx, 4)) WR32(MEM(ARG(ctx, 4)), kmem_alloc(0x100, 16));
    RET(IDirectSound_DownloadEffectsImage, 0);
}
HOOK(IDirectSound_SetMixBinHeadroom) { RET(IDirectSound_SetMixBinHeadroom, 0); }
HOOK(IDirectSound_SetDistanceFactor) { RET(IDirectSound_SetDistanceFactor, 0); }
HOOK(IDirectSound_SetDopplerFactor) { RET(IDirectSound_SetDopplerFactor, 0); }
HOOK(IDirectSound_SetOrientation) { RET(IDirectSound_SetOrientation, 0); }
HOOK(IDirectSound_SetPosition) { RET(IDirectSound_SetPosition, 0); }
HOOK(IDirectSound_SetVelocity) { RET(IDirectSound_SetVelocity, 0); }
HOOK(IDirectSound_SetI3DL2Listener) { RET(IDirectSound_SetI3DL2Listener, 0); }
HOOK(IDirectSound_CommitDeferredSettings) { RET(IDirectSound_CommitDeferredSettings, 0); }
