/* DirectSound on the host: the real library drives the MCPX audio processor
   through MMIO, so its API is replaced. Sound buffers play through audio.c
   (XAudio2); streams are still silent. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
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

/* one vtable per interface; objects are reference counted and freed on
   their last Release: the game creates sound buffers all through a level.
   Layout: +0 vtable, +4 references, +8 interface, and for buffers +12 the
   audio.c voice, +16 the guest memory Lock hands out, +20 its size. */
#define O_IFACE 8
#define O_VOICE 12
#define O_DATA 16
#define O_BYTES 20
uint32_t make_object(uint32_t iface, uint32_t size) {
    static uint32_t vtables[8];
    if (!vtables[iface]) {
        vtables[iface] = kmem_alloc(64 * 4, 16);
        for (uint32_t i = 0; i < 64; i++) WR32(MEM(vtables[iface] + 4 * i), VCALL_BASE | (iface << 8) | i);
    }
    uint32_t o = kmem_alloc(size, 16);
    if (!o) return 0;
    memset(MEM(o), 0, size);
    WR32(MEM(o), vtables[iface]);
    WR32(MEM(o + 4), 1); /* refcount */
    WR32(MEM(o + O_IFACE), iface);
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
    if (!n) {
        if (RD32(MEM(o + O_IFACE)) == IFACE_BUFFER) {
            if (RD32(MEM(o + O_VOICE))) au_buffer_free((int)RD32(MEM(o + O_VOICE)));
            if (RD32(MEM(o + O_DATA))) kmem_free(RD32(MEM(o + O_DATA)));
        }
        kmem_free(o);
    }
    return n;
}
static int voice_of(uint32_t buffer) { return buffer ? (int)RD32(MEM(buffer + O_VOICE)) : 0; }
static float farg(Ctx *ctx, int i) { uint32_t v = ARG(ctx, i); float f; memcpy(&f, &v, 4); return f; }

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
    if (getenv("RECOMP_DSLOG")) { /* each interface slot the game calls, once */
        static uint8_t seen[4][64];
        if (!seen[iface & 3][slot]++) fprintf(stderr, "dsound: first call of interface %u slot %u (frame %u)\n", iface, slot, d3d_frame_count());
    }
    /* stack arguments: for the stdcall AddRef/Release the first is `this`,
       for __thiscall methods `this` is in ECX and ARG(0) is the first argument;
       the stream's methods are stdcall with `this` first */
    uint32_t a0 = ARG(ctx, 0), a1 = ARG(ctx, 1);
    trace("dsound: iface %u slot %u (pops %d)", iface, slot, pops);
    uint32_t result = 0;
    if (slot == 0) result = object_addref(a0);        /* AddRef(this) */
    else if (slot == 1) result = iface == IFACE_DSOUND ? 1 : object_release(a0); /* Release(this); the device stays */
    else if (iface == IFACE_STREAM) {
        /* 6/7: AddRef/Release through the XMediaObject side (adjustor
           thunks), 8/9 the same directly */
        if (slot == 6 || slot == 8) result = object_addref(a0);
        else if (slot == 7 || slot == 9) result = object_release(a0);
        else if (slot == 10 && a1) memset(MEM(a1), 0, 16); /* GetInfo(this, pInfo) */
        else if (slot == 11 && a1) WR32(MEM(a1), 0);       /* GetStatus(this, pdwStatus): idle */
        /* Process(this, pInput, pOutput), Discontinuity, Flush: nothing to do yet */
    } else if (iface == IFACE_BUFFER) {
        if (slot == 3 && a1) WR32(MEM(a1), 0);          /* a (this, out*) getter: report 0 */
    }
    SET_EAX(ctx, result);
    RET_STDCALL(ctx, pops);
}

HOOK(DirectSoundCreate) {
    /* (pguidDeviceId, ppDirectSound, pUnkOuter) */
    uint32_t pp = ARG(ctx, 1);
    if (pp) WR32(MEM(pp), make_object(IFACE_DSOUND, 0x100));
    fprintf(stderr, "dsound: created (%s)\n", au_init() ? "XAudio2" : "silent");
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
/* the listener: (pDS, xFront, yFront, zFront, xTop, yTop, zTop, dwApply) and
   (pDS, x, y, z, dwApply); deferred settings take effect at once */
HOOK(IDirectSound_SetOrientation) {
    float front[3] = {farg(ctx, 1), farg(ctx, 2), farg(ctx, 3)}, top[3] = {farg(ctx, 4), farg(ctx, 5), farg(ctx, 6)};
    au_listener(NULL, front, top);
    RET(IDirectSound_SetOrientation, 0);
}
HOOK(IDirectSound_SetPosition) {
    float pos[3] = {farg(ctx, 1), farg(ctx, 2), farg(ctx, 3)};
    au_listener(pos, NULL, NULL);
    RET(IDirectSound_SetPosition, 0);
}
HOOK(IDirectSound_SetVelocity) { RET(IDirectSound_SetVelocity, 0); }
HOOK(IDirectSound_SetI3DL2Listener) { RET(IDirectSound_SetI3DL2Listener, 0); }
HOOK(IDirectSound_CommitDeferredSettings) { RET(IDirectSound_CommitDeferredSettings, 0); }

/* ---- sound buffers --------------------------------------------------------- */
/* (pdsbd, ppBuffer): DSBUFFERDESC { dwSize, dwFlags, dwBufferBytes,
   lpwfxFormat, lpMixBins, dwInputMixBin }; the game fills each buffer once
   (Lock, copy, Unlock) and plays it */
#define DSBCAPS_CTRL3D 0x10
HOOK(DirectSoundCreateBuffer) {
    uint32_t desc = ARG(ctx, 0), pp = ARG(ctx, 1);
    uint32_t o = make_object(IFACE_BUFFER, 0x100);
    if (o && desc) {
        uint32_t flags = RD32(MEM(desc + 4)), bytes = RD32(MEM(desc + 8)), wfx = RD32(MEM(desc + 12));
        if (wfx) {
            AuFormat f = {RD16(MEM(wfx)), RD16(MEM(wfx + 2)), RD32(MEM(wfx + 4)), RD32(MEM(wfx + 8)), RD16(MEM(wfx + 12)), RD16(MEM(wfx + 14))};
            WR32(MEM(o + O_VOICE), (uint32_t)au_buffer_create(&f, (flags & DSBCAPS_CTRL3D) != 0));
            if (getenv("RECOMP_DSLOG"))
                fprintf(stderr, "dsound: buffer %08x flags %08x, %u bytes of format %04x, %u channels, %u Hz, %u bits -> voice %u\n", o, flags,
                        bytes, f.tag, f.channels, f.rate, f.bits, RD32(MEM(o + O_VOICE)));
        }
        uint32_t data = bytes ? kmem_alloc(bytes, 16) : 0;
        WR32(MEM(o + O_DATA), data);
        WR32(MEM(o + O_BYTES), data ? bytes : 0);
    }
    if (pp) WR32(MEM(pp), o);
    RET(DirectSoundCreateBuffer, o ? 0 : 0x88780032u /* DSERR_OUTOFMEMORY */);
}
HOOK(IDirectSoundBuffer_Lock) {
    /* (pThis, dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1, ppvAudioPtr2, pdwAudioBytes2, dwFlags):
       the buffer's own memory, from the offset (or all of it for DSBLOCK_ENTIREBUFFER) */
    uint32_t o = ARG(ctx, 0), off = ARG(ctx, 1), bytes = ARG(ctx, 2), pp1 = ARG(ctx, 3), pb1 = ARG(ctx, 4), pp2 = ARG(ctx, 5), pb2 = ARG(ctx, 6);
    uint32_t data = RD32(MEM(o + O_DATA)), size = RD32(MEM(o + O_BYTES));
    if (ARG(ctx, 7) & 2) { off = 0; bytes = size; }
    if (off > size) off = size;
    if (bytes > size - off) bytes = size - off;
    if (pp1) WR32(MEM(pp1), data ? data + off : 0);
    if (pb1) WR32(MEM(pb1), bytes);
    if (pp2) WR32(MEM(pp2), 0);
    if (pb2) WR32(MEM(pb2), 0);
    RET(IDirectSoundBuffer_Lock, data ? 0 : 0x88780032u /* DSERR_OUTOFMEMORY */);
}
/* (pThis, pvAudioPtr1, dwAudioBytes1, pvAudioPtr2, dwAudioBytes2): the voice takes the whole buffer again */
HOOK(IDirectSoundBuffer_Unlock) {
    uint32_t o = ARG(ctx, 0);
    if (voice_of(o) && RD32(MEM(o + O_DATA))) au_buffer_data(voice_of(o), MEM(RD32(MEM(o + O_DATA))), RD32(MEM(o + O_BYTES)));
    RET(IDirectSoundBuffer_Unlock, 0);
}
/* (pThis, dwReserved1, dwReserved2, dwFlags): DSBPLAY_LOOPING 1 */
HOOK(IDirectSoundBuffer_Play) {
    if (voice_of(ARG(ctx, 0))) au_buffer_play(voice_of(ARG(ctx, 0)), ARG(ctx, 3) & 1);
    RET(IDirectSoundBuffer_Play, 0);
}
HOOK(IDirectSoundBuffer_Stop) {
    if (voice_of(ARG(ctx, 0))) au_buffer_stop(voice_of(ARG(ctx, 0)));
    RET(IDirectSoundBuffer_Stop, 0);
}
HOOK(IDirectSoundBuffer_GetStatus) {
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), voice_of(ARG(ctx, 0)) ? au_buffer_status(voice_of(ARG(ctx, 0))) : 0);
    RET(IDirectSoundBuffer_GetStatus, 0);
}
HOOK(IDirectSoundBuffer_GetCurrentPosition) {
    uint32_t pos = voice_of(ARG(ctx, 0)) ? au_buffer_cursor(voice_of(ARG(ctx, 0))) : 0;
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), pos);
    if (ARG(ctx, 2)) WR32(MEM(ARG(ctx, 2)), pos);
    RET(IDirectSoundBuffer_GetCurrentPosition, 0);
}
HOOK(IDirectSoundBuffer_SetCurrentPosition) {
    if (voice_of(ARG(ctx, 0))) au_buffer_set_cursor(voice_of(ARG(ctx, 0)), ARG(ctx, 1));
    RET(IDirectSoundBuffer_SetCurrentPosition, 0);
}
/* (pThis, lVolume): hundredths of a decibel, 0 = full */
HOOK(IDirectSoundBuffer_SetVolume) {
    if (voice_of(ARG(ctx, 0))) au_buffer_volume(voice_of(ARG(ctx, 0)), (int32_t)ARG(ctx, 1));
    RET(IDirectSoundBuffer_SetVolume, 0);
}
HOOK(IDirectSoundBuffer_SetFrequency) {
    if (voice_of(ARG(ctx, 0))) au_buffer_frequency(voice_of(ARG(ctx, 0)), ARG(ctx, 1));
    RET(IDirectSoundBuffer_SetFrequency, 0);
}
/* (pThis, dwLoopStart, dwLoopLength) in bytes */
HOOK(IDirectSoundBuffer_SetLoopRegion) {
    if (voice_of(ARG(ctx, 0))) au_buffer_loop_region(voice_of(ARG(ctx, 0)), ARG(ctx, 1), ARG(ctx, 2));
    RET(IDirectSoundBuffer_SetLoopRegion, 0);
}
/* 3D: (pThis, x, y, z, dwApply), (pThis, flDistance, dwApply) */
HOOK(IDirectSoundBuffer_SetPosition) {
    if (voice_of(ARG(ctx, 0))) au_buffer_position(voice_of(ARG(ctx, 0)), farg(ctx, 1), farg(ctx, 2), farg(ctx, 3));
    RET(IDirectSoundBuffer_SetPosition, 0);
}
HOOK(IDirectSoundBuffer_SetMinDistance) {
    if (voice_of(ARG(ctx, 0))) au_buffer_distances(voice_of(ARG(ctx, 0)), farg(ctx, 1), -1);
    RET(IDirectSoundBuffer_SetMinDistance, 0);
}
HOOK(IDirectSoundBuffer_SetMaxDistance) {
    if (voice_of(ARG(ctx, 0))) au_buffer_distances(voice_of(ARG(ctx, 0)), -1, farg(ctx, 1));
    RET(IDirectSoundBuffer_SetMaxDistance, 0);
}

/* ---- the rest the game calls: accept and do nothing ------------------------------- */
HOOK(IDirectSoundBuffer_SetFilter) { RET(IDirectSoundBuffer_SetFilter, 0); }
HOOK(IDirectSoundBuffer_SetHeadroom) { RET(IDirectSoundBuffer_SetHeadroom, 0); }
HOOK(IDirectSoundBuffer_SetMixBinVolumes_12) { RET(IDirectSoundBuffer_SetMixBinVolumes_12, 0); }
HOOK(IDirectSoundBuffer_SetConeAngles) { RET(IDirectSoundBuffer_SetConeAngles, 0); }
HOOK(IDirectSoundBuffer_SetConeOrientation) { RET(IDirectSoundBuffer_SetConeOrientation, 0); }
HOOK(IDirectSoundBuffer_SetConeOutsideVolume) { RET(IDirectSoundBuffer_SetConeOutsideVolume, 0); }
HOOK(IDirectSoundBuffer_SetVelocity) { RET(IDirectSoundBuffer_SetVelocity, 0); }
HOOK(CDirectSoundStream_SetVolume) { RET(CDirectSoundStream_SetVolume, 0); }
HOOK(CDirectSoundStream_SetFilter) { RET(CDirectSoundStream_SetFilter, 0); }
HOOK(IDirectSoundStream_SetMixBinVolumes_12) { RET(IDirectSoundStream_SetMixBinVolumes_12, 0); }
HOOK(IDirectSoundStream_SetConeAngles) { RET(IDirectSoundStream_SetConeAngles, 0); }
HOOK(IDirectSoundStream_SetConeOrientation) { RET(IDirectSoundStream_SetConeOrientation, 0); }
HOOK(IDirectSoundStream_SetConeOutsideVolume) { RET(IDirectSoundStream_SetConeOutsideVolume, 0); }
HOOK(IDirectSoundStream_SetMaxDistance) { RET(IDirectSoundStream_SetMaxDistance, 0); }
HOOK(IDirectSoundStream_SetMinDistance) { RET(IDirectSoundStream_SetMinDistance, 0); }
HOOK(IDirectSoundStream_SetPosition) { RET(IDirectSoundStream_SetPosition, 0); }
HOOK(IDirectSoundBuffer_SetMixBins) { RET(IDirectSoundBuffer_SetMixBins, 0); }
HOOK(CDirectSoundStream_SetFrequency) { RET(CDirectSoundStream_SetFrequency, 0); }
HOOK(CDirectSoundStream_SetMixBins) { RET(CDirectSoundStream_SetMixBins, 0); }
