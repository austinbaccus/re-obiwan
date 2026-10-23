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

/* one vtable per interface; objects are reference counted (+4) and freed on
   their last Release: the game creates sound buffers all through a level */
uint32_t make_object(uint32_t iface, uint32_t size) {
    static uint32_t vtables[8];
    if (!vtables[iface]) {
        vtables[iface] = kmem_alloc(64 * 4, 16);
        for (uint32_t i = 0; i < 64; i++) WR32(MEM(vtables[iface] + 4 * i), VCALL_BASE | (iface << 8) | i);
    }
    uint32_t o = kmem_alloc(size, 16);
    if (!o) return 0;
    WR32(MEM(o), vtables[iface]);
    WR32(MEM(o + 4), 1); /* refcount */
    return o;
}
static uint32_t object_addref(uint32_t o) {
    if (!o) return 0;
    WR32(MEM(o + 4), RD32(MEM(o + 4)) + 1);
    return RD32(MEM(o + 4));
}
static uint32_t object_release(uint32_t o) {
    if (!o || !RD32(MEM(o + 4))) return 0;
    uint32_t n = RD32(MEM(o + 4)) - 1;
    WR32(MEM(o + 4), n);
    if (!n) kmem_free(o);
    return n;
}

/* Pop counts (dwords the original slot function pops on return) of the real
   DSOUND vtables in this image, read from the library: most methods are
   __thiscall (this in ECX, only stack arguments popped), AddRef/Release are
   stdcall. Stream = vtable at 0x3E69C0 (DirectSoundCreateStream objects),
   dsound = 0x3E6A68, buffer = 0x3E6A84 (tentative identification). */
static const int8_t dsound_pops[] = {1, 1, 0, 2, 0, 2, 1};
static const int8_t stream_pops[] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 3, 1, 1, 1};
static const int8_t buffer_pops[] = {1, 1, 0, 2, 0, 2, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 2, 3, 3};

void host_vcall(Ctx *ctx, uint32_t id) {
    uint32_t iface = (id >> 8) & 0xff, slot = id & 0xff;
    const int8_t *tab = iface == IFACE_DSOUND ? dsound_pops : iface == IFACE_STREAM ? stream_pops : buffer_pops;
    size_t n = iface == IFACE_DSOUND ? sizeof dsound_pops : iface == IFACE_STREAM ? sizeof stream_pops : sizeof buffer_pops;
    if (slot >= n) {
        fprintf(stderr, "dsound: unknown vtable slot %u on interface %u\n", slot, iface);
        rt_trap(ctx, TRAP_UNSUPPORTED, id);
    }
    int pops = tab[slot];
    /* stack arguments: for the stdcall AddRef/Release the first is `this`,
       for __thiscall methods `this` is in ECX and ARG(0) is the first argument */
    uint32_t a0 = ARG(ctx, 0), a1 = ARG(ctx, 1);
    trace("dsound: iface %u slot %u (pops %d)", iface, slot, pops);
    uint32_t result = 0;
    if (slot == 0) result = object_addref(a0);        /* AddRef(this) */
    else if (slot == 1) result = iface == IFACE_DSOUND ? 1 : object_release(a0); /* Release(this); the device stays */
    else if (iface == IFACE_STREAM) {
        if (slot == 10 && a0) memset(MEM(a0), 0, 16);   /* GetInfo(pInfo) */
        else if (slot == 11 && a0) WR32(MEM(a0), 0);    /* GetStatus(pdwStatus): idle */
        /* Process(pInput, pOutput), Discontinuity, Flush, ...: nothing to do */
    } else if (iface == IFACE_BUFFER) {
        if (slot == 3 && a1) WR32(MEM(a1), 0);          /* a (this, out*) getter: report 0 */
    }
    (void)a1;
    SET_EAX(ctx, result);
    RET_STDCALL(ctx, pops);
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
HOOK(IDirectSoundBuffer_Release) { RET(IDirectSoundBuffer_Release, object_release(ARG(ctx, 0))); }
HOOK(IDirectSoundStream_Pause) { RET(IDirectSoundStream_Pause, 0); }
HOOK(IDirectSound_GetCaps) {
    uint32_t pcaps = ARG(ctx, 1);
    /* DSCAPS: dwFree2DBuffers, dwFree3DBuffers, dwFreeBufferSGEs, dwMemoryAllocated */
    if (pcaps) { WR32(MEM(pcaps), 256); WR32(MEM(pcaps + 4), 256); WR32(MEM(pcaps + 8), 1024); WR32(MEM(pcaps + 12), 0); }
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

/* ---- every other DirectSound entry point the game calls: accept and do nothing ---- */
HOOK(IDirectSoundBuffer_Unlock) { RET(IDirectSoundBuffer_Unlock, 0); }
HOOK(IDirectSoundBuffer_SetVolume) { RET(IDirectSoundBuffer_SetVolume, 0); }
HOOK(IDirectSoundBuffer_SetFilter) { RET(IDirectSoundBuffer_SetFilter, 0); }
HOOK(IDirectSoundBuffer_SetHeadroom) { RET(IDirectSoundBuffer_SetHeadroom, 0); }
HOOK(IDirectSoundBuffer_SetMixBinVolumes_12) { RET(IDirectSoundBuffer_SetMixBinVolumes_12, 0); }
HOOK(IDirectSoundBuffer_SetConeAngles) { RET(IDirectSoundBuffer_SetConeAngles, 0); }
HOOK(IDirectSoundBuffer_SetConeOrientation) { RET(IDirectSoundBuffer_SetConeOrientation, 0); }
HOOK(IDirectSoundBuffer_SetConeOutsideVolume) { RET(IDirectSoundBuffer_SetConeOutsideVolume, 0); }
HOOK(IDirectSoundBuffer_SetMaxDistance) { RET(IDirectSoundBuffer_SetMaxDistance, 0); }
HOOK(IDirectSoundBuffer_SetMinDistance) { RET(IDirectSoundBuffer_SetMinDistance, 0); }
HOOK(IDirectSoundBuffer_SetPosition) { RET(IDirectSoundBuffer_SetPosition, 0); }
HOOK(IDirectSoundBuffer_SetVelocity) { RET(IDirectSoundBuffer_SetVelocity, 0); }
HOOK(IDirectSoundBuffer_SetLoopRegion) { RET(IDirectSoundBuffer_SetLoopRegion, 0); }
HOOK(IDirectSoundBuffer_GetStatus) {
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), 0); /* not playing */
    RET(IDirectSoundBuffer_GetStatus, 0);
}
HOOK(IDirectSoundBuffer_GetCurrentPosition) {
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), 0);
    if (ARG(ctx, 2)) WR32(MEM(ARG(ctx, 2)), 0);
    RET(IDirectSoundBuffer_GetCurrentPosition, 0);
}
HOOK(IDirectSoundBuffer_SetCurrentPosition) { RET(IDirectSoundBuffer_SetCurrentPosition, 0); }
HOOK(IDirectSoundBuffer_Lock) {
    /* (pThis, dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags):
       hand out scratch memory the game can fill; nothing plays it */
    uint32_t bytes = ARG(ctx, 2), pp1 = ARG(ctx, 3), pb1 = ARG(ctx, 4), pp2 = ARG(ctx, 5), pb2 = ARG(ctx, 6);
    uint32_t mem = kmem_alloc(bytes ? bytes : 16, 16);
    if (pp1) WR32(MEM(pp1), mem);
    if (pb1) WR32(MEM(pb1), bytes);
    if (pp2) WR32(MEM(pp2), 0);
    if (pb2) WR32(MEM(pb2), 0);
    RET(IDirectSoundBuffer_Lock, mem ? 0 : 0x88780032u /* DSERR_OUTOFMEMORY */);
}
HOOK(CDirectSoundStream_SetVolume) { RET(CDirectSoundStream_SetVolume, 0); }
HOOK(CDirectSoundStream_SetFilter) { RET(CDirectSoundStream_SetFilter, 0); }
HOOK(IDirectSoundStream_SetMixBinVolumes_12) { RET(IDirectSoundStream_SetMixBinVolumes_12, 0); }
HOOK(IDirectSoundStream_SetConeAngles) { RET(IDirectSoundStream_SetConeAngles, 0); }
HOOK(IDirectSoundStream_SetConeOrientation) { RET(IDirectSoundStream_SetConeOrientation, 0); }
HOOK(IDirectSoundStream_SetConeOutsideVolume) { RET(IDirectSoundStream_SetConeOutsideVolume, 0); }
HOOK(IDirectSoundStream_SetMaxDistance) { RET(IDirectSoundStream_SetMaxDistance, 0); }
HOOK(IDirectSoundStream_SetMinDistance) { RET(IDirectSoundStream_SetMinDistance, 0); }
HOOK(IDirectSoundStream_SetPosition) { RET(IDirectSoundStream_SetPosition, 0); }
HOOK(IDirectSoundBuffer_SetFrequency) { RET(IDirectSoundBuffer_SetFrequency, 0); }
HOOK(IDirectSoundBuffer_SetMixBins) { RET(IDirectSoundBuffer_SetMixBins, 0); }
HOOK(IDirectSoundBuffer_Play) { RET(IDirectSoundBuffer_Play, 0); }
HOOK(IDirectSoundBuffer_Stop) { RET(IDirectSoundBuffer_Stop, 0); }
HOOK(CDirectSoundStream_SetFrequency) { RET(CDirectSoundStream_SetFrequency, 0); }
HOOK(CDirectSoundStream_SetMixBins) { RET(CDirectSoundStream_SetMixBins, 0); }
HOOK(DirectSoundCreateBuffer) {
    /* (pdsbd, ppBuffer) */
    uint32_t pp = ARG(ctx, 1);
    if (pp) WR32(MEM(pp), make_object(IFACE_BUFFER, 0x100));
    RET(DirectSoundCreateBuffer, 0);
}
