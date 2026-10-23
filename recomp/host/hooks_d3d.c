/* Direct3D 8 stand-in: enough of the Xbox D3D API for the game to create
   its device and resources, lock and fill them, and present. Nothing is
   drawn yet. Argument counts come from HOOKARGS_* (the original `ret n`).

   Xbox D3D resources are plain structs the game reads directly:
     D3DResource        +0 Common (refcount | type), +4 Data (address), +8 Lock
     D3DPixelContainer  +C Format, +10 Size   (encoded; decoded below)
   Objects created here carry their plain dimensions after that and a magic
   word at +0x3C; resources the game builds itself from its bundles (then
   registers with D3DResource_Register) only have the encoded fields. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "funcs.h"
#include "kernel.h"
#include "render.h"
#include "shader_xlate.h"

/* PC D3DRENDERSTATETYPE numbers for the dedicated setters */
enum { PC_RS_ZENABLE = 7, PC_RS_CULLMODE = 22, PC_RS_FOGCOLOR = 34, PC_RS_STENCILENABLE = 52, PC_RS_STENCILFAIL = 53, PC_RS_TEXTUREFACTOR = 60 };
static int g_render; /* a host renderer is up (RECOMP_HEADLESS=1 keeps the old counting stubs) */

#define HOOK(name) void hook_##name(Ctx *ctx)
#define RET(name, v) do { trace("hook %s -> %08x (pops %d)", #name, (uint32_t)(v), HOOKARGS_##name); SET_EAX(ctx, (v)); RET_STDCALL(ctx, HOOKARGS_##name); return; } while (0)

#define OBJ_SIZE 0x40
#define OBJ_MAGIC 0x4F424A44u
enum { O_COMMON = 0, O_DATA = 4, O_LOCK = 8, O_FORMAT = 0xC, O_SIZE = 0x10,
       O_WIDTH = 0x20, O_HEIGHT = 0x24, O_DEPTH = 0x28, O_FMT = 0x2C, O_LEVELS = 0x30, O_PITCH = 0x34, O_BYTES = 0x38, O_MAGICOFF = 0x3C };
/* D3DCOMMON type field (bits 16..18) */
enum { T_VERTEXBUFFER = 0, T_INDEXBUFFER = 1, T_PUSHBUFFER = 2, T_PALETTE = 3, T_TEXTURE = 4, T_SURFACE = 5 };

static uint32_t g_device, g_backbuffer, g_depth, g_rendertarget;
static float g_transforms[512][16];
static uint32_t g_frames;
uint32_t d3d_frame_count(void) { return g_frames; }

static int bytes_per_pixel(uint32_t fmt) {
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A: case 0x1C: case 0x1D: case 0x20:
    case 0x27: case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x30: case 0x31: case 0x32: case 0x35: case 0x37:
    case 0x38: case 0x39: case 0x3D: case 0x3E: return 2;
    default: return 4;
    }
}

static uint32_t alloc_or_die(uint32_t size, uint32_t align, const char *what) {
    uint32_t a = kmem_alloc(size, align);
    if (!a) {
        fprintf(stderr, "d3d: out of guest memory allocating %u bytes for %s\n", size, what);
        rt_trap(kernel_current_ctx(), TRAP_UNSUPPORTED, 0);
    }
    return a;
}

static uint32_t new_object(uint32_t type) {
    uint32_t o = alloc_or_die(OBJ_SIZE, 16, "a resource object");
    memset(MEM(o), 0, OBJ_SIZE);
    WR32(MEM(o + O_COMMON), 1 | (type << 16));
    WR32(MEM(o + O_MAGICOFF), OBJ_MAGIC);
    return o;
}
static int is_ours(uint32_t o) { return o && RD32(MEM(o + O_MAGICOFF)) == OBJ_MAGIC; }

/* Width, height, format and pitch of a level of any pixel container. */
static int linear_format(uint32_t fmt) {
    return (fmt >= 0x10 && fmt <= 0x20) || (fmt >= 0x2E && fmt <= 0x31) || (fmt >= 0x35 && fmt <= 0x37) || (fmt >= 0x3D && fmt <= 0x41);
}

/* Always decoded from the live Format and Size fields: the game edits the
   headers of textures it created (it turns some into linear layouts). */
static void describe(uint32_t o, uint32_t level, uint32_t *w, uint32_t *h, uint32_t *fmt, uint32_t *pitch) {
    uint32_t format = RD32(MEM(o + O_FORMAT)), size = RD32(MEM(o + O_SIZE));
    *fmt = (format >> 8) & 0xff;
    if (size) { *w = (size & 0xfff) + 1; *h = ((size >> 12) & 0xfff) + 1; }
    else if (is_ours(o)) { *w = RD32(MEM(o + O_WIDTH)); *h = RD32(MEM(o + O_HEIGHT)); }
    else { *w = 1u << ((format >> 20) & 0xf); *h = 1u << ((format >> 24) & 0xf); }
    for (uint32_t i = 0; i < level; i++) { *w = *w > 1 ? *w / 2 : 1; *h = *h > 1 ? *h / 2 : 1; }
    if (*fmt == 0x0C) *pitch = (*w < 4 ? 1 : *w / 4) * 8;
    else if (*fmt == 0x0E || *fmt == 0x0F) *pitch = (*w < 4 ? 1 : *w / 4) * 16;
    else *pitch = *w * bytes_per_pixel(*fmt);
    if (level == 0 && size >> 24) *pitch = ((size >> 24) + 1) * 64;
}

/* A pixel container with a non-zero Size field has rows at the pitch it
   gives, whatever the format code says (Size is zero for swizzled and
   compressed layouts). */
int d3d_has_linear_layout(uint32_t o) { return RD32(MEM(o + O_SIZE)) != 0; }

void d3d_describe(uint32_t o, uint32_t level, uint32_t *w, uint32_t *h, uint32_t *fmt, uint32_t *pitch) { describe(o, level, w, h, fmt, pitch); }

static uint32_t level_offset(uint32_t o, uint32_t level) {
    uint32_t off = 0, w, h, fmt, pitch;
    for (uint32_t i = 0; i < level; i++) {
        describe(o, i, &w, &h, &fmt, &pitch);
        off += pitch * (fmt == 0x0C || fmt == 0x0E || fmt == 0x0F ? (h < 4 ? 1 : h / 4) : h);
    }
    return off;
}

uint32_t d3d_level_offset(uint32_t o, uint32_t level) { return level_offset(o, level); }

static uint32_t new_pixel_container(uint32_t type, uint32_t w, uint32_t h, uint32_t depth, uint32_t levels, uint32_t fmt) {
    if (!levels) { levels = 1; for (uint32_t m = w > h ? w : h; m > 1; m /= 2) levels++; }
    uint32_t o = new_object(type);
    WR32(MEM(o + O_WIDTH), w); WR32(MEM(o + O_HEIGHT), h); WR32(MEM(o + O_DEPTH), depth);
    WR32(MEM(o + O_FMT), fmt); WR32(MEM(o + O_LEVELS), levels);
    uint32_t pitch = w * bytes_per_pixel(fmt);
    WR32(MEM(o + O_PITCH), pitch);
    /* the encoded fields first: describe() and level_offset() decode them */
    uint32_t lg2w = 0, lg2h = 0;
    while ((1u << lg2w) < w) lg2w++;
    while ((1u << lg2h) < h) lg2h++;
    uint32_t lg2d = 0;
    while (depth && (1u << lg2d) < depth) lg2d++;
    WR32(MEM(o + O_FORMAT), (fmt << 8) | ((depth ? 3 : 2) << 4) | ((levels & 0xf) << 16) | (lg2w << 20) | (lg2h << 24) | (lg2d << 28));
    /* Size is only set for linear layouts (it carries the pitch); zero means swizzled or compressed */
    WR32(MEM(o + O_SIZE), linear_format(fmt) ? ((w - 1) & 0xfff) | (((h - 1) & 0xfff) << 12) | (((pitch / 64 ? pitch / 64 - 1 : 0) & 0xff) << 24) : 0);
    uint32_t bytes = (level_offset(o, levels) + 4095) & ~4095u;
    bytes *= depth ? depth : 1;
    WR32(MEM(o + O_BYTES), bytes);
    WR32(MEM(o + O_DATA), alloc_or_die(bytes, 4096, "texture memory"));
    return o;
}

/* ---- device ------------------------------------------------------------- */
/* The device keeps the viewport at +0xB00 (X, Y, Width, Height, MinZ, MaxZ),
   as D3DDevice_SetViewport (0x36aaf0) stores it; D3D's state snapshots
   (0x36b580) and so the game read it from there. */
static void store_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz) {
    WR32(MEM(g_device + 0xB00), x); WR32(MEM(g_device + 0xB04), y);
    WR32(MEM(g_device + 0xB08), w); WR32(MEM(g_device + 0xB0C), h);
    memcpy(MEM(g_device + 0xB10), &minz, 4); memcpy(MEM(g_device + 0xB14), &maxz, 4);
}

HOOK(Direct3D_CreateDevice) {
    uint32_t pparams = ARG(ctx, 4), ppdevice = ARG(ctx, 5);
    uint32_t w = pparams ? RD32(MEM(pparams)) : 640, h = pparams ? RD32(MEM(pparams + 4)) : 480;
    if (!w || w > 4096) w = 640;
    if (!h || h > 4096) h = 480;
    g_device = kmem_alloc(0x4000, 16);
    g_backbuffer = new_pixel_container(T_SURFACE, w, h, 0, 1, 0x12);
    g_depth = new_pixel_container(T_SURFACE, w, h, 0, 1, 0x2E);
    g_rendertarget = g_backbuffer;
    WR32(MEM(0x0037aa30), g_device); /* D3D_g_pDevice */
    if (ppdevice) WR32(MEM(ppdevice), g_device);
    store_viewport(0, 0, w, h, 0.0f, 1.0f);
    for (int i = 0; i < 512; i++) { memset(g_transforms[i], 0, 64); g_transforms[i][0] = g_transforms[i][5] = g_transforms[i][10] = g_transforms[i][15] = 1.0f; }
    fprintf(stderr, "d3d: device created, %ux%u back buffer\n", w, h);
    if (!getenv("RECOMP_HEADLESS")) g_render = r_ready() ? 1 : r_init(w, h);
    if (getenv("RECOMP_TRACE_FROM_D3D")) g_trace_calls = 2; /* debugging aid: full call trace from here on */
    RET(Direct3D_CreateDevice, 0);
}
HOOK(D3D_GetAdapterDisplayMode) {
    uint32_t pmode = ARG(ctx, 1);
    if (pmode) { WR32(MEM(pmode), 640); WR32(MEM(pmode + 4), 480); WR32(MEM(pmode + 8), 60); WR32(MEM(pmode + 12), 0x06); }
    RET(D3D_GetAdapterDisplayMode, 0);
}
/* D3DCAPS8 is 0x35 dwords, copied from D3D's static table as the original
   (0x36a080) does; CD3DContext keeps its viewport right after its copy */
HOOK(D3DDevice_GetDeviceCaps) { memcpy(MEM(ARG(ctx, 0)), MEM(0x00441ad8), 0x35 * 4); RET(D3DDevice_GetDeviceCaps, 0); }
HOOK(D3DDevice_Release) { RET(D3DDevice_Release, 0); }
HOOK(D3DDevice_Present) {
    g_frames++;
    if (g_frames <= 5 || g_frames % 60 == 0) fprintf(stderr, "d3d: present #%u\n", g_frames);
    if (g_render) r_present();
    RET(D3DDevice_Present, 0);
}
HOOK(D3DDevice_PersistDisplay) { RET(D3DDevice_PersistDisplay, 0); }
HOOK(D3DDevice_Clear) {
    /* (Count, pRects, Flags, Color, Z, Stencil) */
    if (g_render) { uint32_t zbits = ARG(ctx, 4); float z; memcpy(&z, &zbits, 4); r_clear(ARG(ctx, 2), ARG(ctx, 3), z, ARG(ctx, 5)); }
    RET(D3DDevice_Clear, 0);
}
HOOK(D3DDevice_InsertFence) { RET(D3DDevice_InsertFence, 1); }
HOOK(D3DDevice_BlockOnFence) { RET(D3DDevice_BlockOnFence, 0); }

/* back buffer / targets: AddRef and hand out the shared objects */
static void give(Ctx *ctx, uint32_t pp, uint32_t obj) {
    if (obj) WR32(MEM(obj + O_COMMON), RD32(MEM(obj + O_COMMON)) + 1);
    if (pp) WR32(MEM(pp), obj);
}
HOOK(D3DDevice_GetBackBuffer) { give(ctx, ARG(ctx, 2), g_backbuffer); RET(D3DDevice_GetBackBuffer, 0); }
HOOK(D3DDevice_GetRenderTarget) { give(ctx, ARG(ctx, 0), g_rendertarget); RET(D3DDevice_GetRenderTarget, 0); }
HOOK(D3DDevice_GetDepthStencilSurface) { give(ctx, ARG(ctx, 0), g_depth); RET(D3DDevice_GetDepthStencilSurface, 0); }
HOOK(D3DDevice_SetRenderTarget) { if (ARG(ctx, 0)) g_rendertarget = ARG(ctx, 0); RET(D3DDevice_SetRenderTarget, 0); }

/* transforms are read back, so keep them */
HOOK(D3DDevice_SetTransform) {
    uint32_t state = ARG(ctx, 0) & 511, pm = ARG(ctx, 1);
    if (pm) memcpy(g_transforms[state], MEM(pm), 64);
    if (pm && !(fabsf(g_transforms[state][0]) < 1e30f)) { /* a degenerate projection hides everything drawn with it */
        static int warned;
        if (!warned++) fprintf(stderr, "d3d: transform %u has scale %g (set from %08x)\n", state, g_transforms[state][0], RD32(MEM(RD32(ctx->r + REG_ESP))));
    }
    if (g_render && pm) r_set_transform(ARG(ctx, 0), g_transforms[state]);
    RET(D3DDevice_SetTransform, 0);
}
HOOK(D3DDevice_GetTransform) {
    uint32_t state = ARG(ctx, 0) & 511, pm = ARG(ctx, 1);
    if (pm) memcpy(MEM(pm), g_transforms[state], 64);
    RET(D3DDevice_GetTransform, 0);
}

/* state: the game also writes D3D's state arrays directly; nothing to do */
HOOK(D3DDevice_SetViewport) {
    uint32_t vp = ARG(ctx, 0);
    if (vp) {
        float minz, maxz; memcpy(&minz, MEM(vp + 16), 4); memcpy(&maxz, MEM(vp + 20), 4);
        store_viewport(RD32(MEM(vp)), RD32(MEM(vp + 4)), RD32(MEM(vp + 8)), RD32(MEM(vp + 12)), minz, maxz);
        if (g_render) r_set_viewport(RD32(MEM(vp)), RD32(MEM(vp + 4)), RD32(MEM(vp + 8)), RD32(MEM(vp + 12)), minz, maxz);
    }
    RET(D3DDevice_SetViewport, 0);
}
/* the three "deferred/simple" setters are __fastcall: first argument in ECX, second in EDX */
#define ECX(ctx) RD32((ctx)->r + REG_ECX)
#define EDX(ctx) RD32((ctx)->r + REG_EDX)
HOOK(D3DDevice_SetRenderState_Simple) { if (g_render) r_set_rs_simple(ECX(ctx), EDX(ctx)); RET(D3DDevice_SetRenderState_Simple, 0); }
/* like the original (0x3690a0): store into D3D_g_RenderState, where the
   renderer picks deferred states up at the next draw */
HOOK(D3DDevice_SetRenderState_Deferred) {
    if (ECX(ctx) < 0x74) WR32(MEM(0x0037ac68 + 4 * ECX(ctx)), EDX(ctx));
    RET(D3DDevice_SetRenderState_Deferred, 0);
}
HOOK(D3DDevice_SetRenderState_CullMode) { if (g_render) r_set_rs(PC_RS_CULLMODE, ARG(ctx, 0)); RET(D3DDevice_SetRenderState_CullMode, 0); }
HOOK(D3DDevice_SetRenderState_FogColor) { if (g_render) r_set_rs(PC_RS_FOGCOLOR, ARG(ctx, 0)); RET(D3DDevice_SetRenderState_FogColor, 0); }
HOOK(D3DDevice_SetRenderState_ShadowFunc) { RET(D3DDevice_SetRenderState_ShadowFunc, 0); }
HOOK(D3DDevice_SetRenderState_StencilEnable) { if (g_render) r_set_rs(PC_RS_STENCILENABLE, ARG(ctx, 0)); RET(D3DDevice_SetRenderState_StencilEnable, 0); }
HOOK(D3DDevice_SetRenderState_StencilFail) { if (g_render) r_set_rs(PC_RS_STENCILFAIL, ARG(ctx, 0)); RET(D3DDevice_SetRenderState_StencilFail, 0); }
HOOK(D3DDevice_SetRenderState_TextureFactor) { if (g_render) r_set_rs(PC_RS_TEXTUREFACTOR, ARG(ctx, 0)); RET(D3DDevice_SetRenderState_TextureFactor, 0); }
HOOK(D3DDevice_SetRenderState_ZBias) { RET(D3DDevice_SetRenderState_ZBias, 0); }
HOOK(D3DDevice_SetRenderState_ZEnable) { if (g_render) r_set_rs(PC_RS_ZENABLE, ARG(ctx, 0)); RET(D3DDevice_SetRenderState_ZEnable, 0); }
HOOK(D3DDevice_SetTextureStageStateDeferred) { if (g_render) r_set_tss(ECX(ctx), EDX(ctx), ARG(ctx, 0)); RET(D3DDevice_SetTextureStageStateDeferred, 0); }
HOOK(D3DDevice_SetTextureState_BorderColor) { if (g_render) r_set_tss(ARG(ctx, 0), 29, ARG(ctx, 1)); RET(D3DDevice_SetTextureState_BorderColor, 0); }
HOOK(D3DDevice_SetTextureState_TexCoordIndex) { if (g_render) r_set_tss(ARG(ctx, 0), 28, ARG(ctx, 1)); RET(D3DDevice_SetTextureState_TexCoordIndex, 0); }
HOOK(D3DDevice_SetTexture) { if (g_render) r_set_texture(ARG(ctx, 0), ARG(ctx, 1)); RET(D3DDevice_SetTexture, 0); }
HOOK(D3DDevice_SetStreamSource) {
    static uint32_t seen[64]; static int nseen;
    uint32_t key = (ARG(ctx, 0) << 16) | (ARG(ctx, 2) & 0xffff), known = 0;
    for (int i = 0; i < nseen; i++) known |= seen[i] == key;
    if (getenv("RECOMP_VSLOG") && !known && nseen < 64) { seen[nseen++] = key; fprintf(stderr, "stream %u stride %u (vb %08x)\n", ARG(ctx, 0), ARG(ctx, 2), ARG(ctx, 1)); }
    if (g_render) r_set_stream(ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2));
    RET(D3DDevice_SetStreamSource, 0);
}
static uint32_t g_indices, g_index_base;
/* (pIndexBuffer, BaseVertexIndex): like the original (0x36b070), publish the
   index data in D3D's global (0x37aa64), which the game's draw code adds its
   start index to, and keep the buffer and base in the device */
HOOK(D3DDevice_SetIndices) {
    g_indices = ARG(ctx, 0); g_index_base = ARG(ctx, 1);
    WR32(MEM(0x0037aa64), g_indices ? RD32(MEM(g_indices + O_DATA)) : 0);
    WR32(MEM(g_device + 0x47c), g_indices); WR32(MEM(g_device + 0x478), g_index_base);
    RET(D3DDevice_SetIndices, 0);
}
HOOK(D3DDevice_SetVertexShader) { if (g_render) r_set_vertex_shader(ARG(ctx, 0)); RET(D3DDevice_SetVertexShader, 0); }
/* RECOMP_VSLOG: describe vertex shader creation, constants and streams (milestone 4 survey) */
static int vslog(void) { static int on = -1; if (on < 0) on = getenv("RECOMP_VSLOG") != NULL; return on; }
HOOK(D3DDevice_SetVertexShaderConstant) {
    static uint32_t seen[256]; static int nseen;
    uint32_t key = (ARG(ctx, 0) << 16) | (ARG(ctx, 2) & 0xffff), known = 0;
    for (int i = 0; i < nseen; i++) known |= seen[i] == key;
    if (vslog() && !known && nseen < 256) {
        seen[nseen++] = key;
        int32_t reg = (int32_t)ARG(ctx, 0);
        const float *f = (const float *)MEM(ARG(ctx, 1));
        fprintf(stderr, "vsconst reg %d count %u (frame %u): %g %g %g %g\n", reg, ARG(ctx, 2), g_frames, f[0], f[1], f[2], f[3]);
    }
    /* (Register, pConstantData, ConstantCount) */
    if (g_render && ARG(ctx, 1)) r_set_vs_constants((int32_t)ARG(ctx, 0), (const float *)MEM(ARG(ctx, 1)), ARG(ctx, 2));
    RET(D3DDevice_SetVertexShaderConstant, 0);
}
HOOK(D3DDevice_SetPixelShader) { if (g_render) r_set_pixel_shader(ARG(ctx, 0)); RET(D3DDevice_SetPixelShader, 0); }
/* (Register, pConstantData, ConstantCount) */
HOOK(D3DDevice_SetPixelShaderConstant) {
    if (g_render && ARG(ctx, 1)) r_set_ps_constants((int32_t)ARG(ctx, 0), (const float *)MEM(ARG(ctx, 1)), ARG(ctx, 2));
    RET(D3DDevice_SetPixelShaderConstant, 0);
}
static uint32_t g_handles = 0x1000;
/* shader sources seen by XGAssembleShader; its output buffer holds
   XVS_MAGIC or XPS_MAGIC and the index here instead of microcode or a
   D3DPIXELSHADERDEF */
#define XVS_MAGIC 0x21535658u /* "XVS!" */
#define XPS_MAGIC 0x21535058u /* "XPS!" */
static struct { char *src; size_t len; } g_vs_sources[64];
static int g_nvs_sources;
static int shader_source(uint32_t buf, uint32_t magic) {
    return buf && RD32(MEM(buf)) == magic && RD32(MEM(buf + 4)) < (uint32_t)g_nvs_sources ? (int)RD32(MEM(buf + 4)) : -1;
}
HOOK(D3DDevice_CreatePixelShader) {
    /* (pPSDef, pHandle): a D3DPIXELSHADERDEF, or the tag of a translated source */
    uint32_t def = ARG(ctx, 0), handle = g_handles++;
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), handle);
    int id = shader_source(def, XPS_MAGIC);
    if (g_render && id >= 0) r_create_pixel_shader_source(handle, g_vs_sources[id].src, g_vs_sources[id].len);
    else if (g_render && def) r_create_pixel_shader(handle, (const uint32_t *)MEM(def));
    RET(D3DDevice_CreatePixelShader, 0);
}
HOOK(D3DDevice_DeletePixelShader) { if (g_render) r_delete_pixel_shader(ARG(ctx, 0)); RET(D3DDevice_DeletePixelShader, 0); }
/* Vertex shaders are not translated yet: CreateVertexShader(pDeclaration,
   pFunction, pHandle, Usage) hands out an odd handle (bit 0 set marks a
   shader rather than an FVF code), and the renderer falls back to fixed
   function for draws that use one. */
HOOK(D3DDevice_CreateVertexShader) {
    uint32_t ph = ARG(ctx, 2), decl = ARG(ctx, 0), handle = (g_handles++ << 4) | 1;
    if (vslog()) {
        fprintf(stderr, "vs create %08x: function %08x [%08x %08x] usage %x decl", handle, ARG(ctx, 1),
                ARG(ctx, 1) ? RD32(MEM(ARG(ctx, 1))) : 0, ARG(ctx, 1) ? RD32(MEM(ARG(ctx, 1) + 4)) : 0, ARG(ctx, 3));
        for (int i = 0; decl && i < 40; i++) { uint32_t t = RD32(MEM(decl + 4 * i)); fprintf(stderr, " %08x", t); if (t == 0xFFFFFFFF) break; }
        fprintf(stderr, "\n");
    }
    if (ph) WR32(MEM(ph), handle);
    int id = shader_source(ARG(ctx, 1), XVS_MAGIC);
    if (g_render && id >= 0) {
        uint32_t tokens[64]; int n = 0;
        while (decl && n < 64 && (tokens[n] = RD32(MEM(decl + 4 * n))) != 0xFFFFFFFF) n++;
        r_create_vertex_shader(handle, g_vs_sources[id].src, g_vs_sources[id].len, tokens, n);
    }
    RET(D3DDevice_CreateVertexShader, 0);
}
HOOK(D3DDevice_DeleteVertexShader) { if (g_render) r_delete_vertex_shader(ARG(ctx, 0)); RET(D3DDevice_DeleteVertexShader, 0); }
HOOK(D3DDevice_LoadVertexShader) { if (vslog()) fprintf(stderr, "vs load %08x at %u\n", ARG(ctx, 0), ARG(ctx, 1)); RET(D3DDevice_LoadVertexShader, 0); }
HOOK(D3DDevice_SelectVertexShader) { if (vslog()) fprintf(stderr, "vs select %08x at %u\n", ARG(ctx, 0), ARG(ctx, 1)); RET(D3DDevice_SelectVertexShader, 0); }
HOOK(D3DDevice_SetShaderConstantMode) { if (vslog()) fprintf(stderr, "vs constant mode %u\n", ARG(ctx, 0)); RET(D3DDevice_SetShaderConstantMode, 0); }
/* (Type, pToken): the game only asks for D3DSBT_ALL (CFlashOverlay saves the
   state around its drawing) */
HOOK(D3DDevice_CreateStateBlock) {
    uint32_t token = g_render ? r_create_state_block() : 0; /* 0: no block, the other calls ignore it */
    if (ARG(ctx, 1)) WR32(MEM(ARG(ctx, 1)), token);
    RET(D3DDevice_CreateStateBlock, 0);
}
HOOK(D3DDevice_CaptureStateBlock) { if (g_render) r_capture_state_block(ARG(ctx, 0)); RET(D3DDevice_CaptureStateBlock, 0); }
HOOK(D3DDevice_ApplyStateBlock) { if (g_render) r_apply_state_block(ARG(ctx, 0)); RET(D3DDevice_ApplyStateBlock, 0); }
HOOK(D3DDevice_DeleteStateBlock) { if (g_render) r_delete_state_block(ARG(ctx, 0)); RET(D3DDevice_DeleteStateBlock, 0); }

/* drawing */
HOOK(D3DDevice_DrawVertices) { if (g_render) r_draw(ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2)); RET(D3DDevice_DrawVertices, 0); }
HOOK(D3DDevice_DrawVerticesUP) { if (g_render) r_draw_up(ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2), ARG(ctx, 3)); RET(D3DDevice_DrawVerticesUP, 0); }
HOOK(D3DDevice_DrawIndexedVertices) {
    /* (PrimitiveType, VertexCount, pIndexData) */
    if (g_render) r_draw_indexed(ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2), g_index_base);
    RET(D3DDevice_DrawIndexedVertices, 0);
}
HOOK(D3DDevice_DrawIndexedVerticesUP) {
    static int warned;
    if (!warned++) fprintf(stderr, "d3d: DrawIndexedVerticesUP is not drawn yet\n");
    RET(D3DDevice_DrawIndexedVerticesUP, 0);
}

/* Xbox D3DSURFACE_DESC (see the original Get2DSurfaceDesc at 0x36dd50):
   Format +0, Type +4, Usage +8, Size +12, MultiSampleType +16, Width +20, Height +24 */
static void write_surface_desc(uint32_t d, uint32_t o, uint32_t level) {
    uint32_t w, h, fmt, pitch;
    describe(o, level, &w, &h, &fmt, &pitch);
    uint32_t t = (RD32(MEM(o + O_COMMON)) >> 16) & 7;
    uint32_t rows = fmt == 0x0C || fmt == 0x0E || fmt == 0x0F ? (h < 4 ? 1 : h / 4) : h;
    WR32(MEM(d + 0), fmt);
    WR32(MEM(d + 4), t == T_SURFACE ? 1 : 3); /* D3DRTYPE_SURFACE / TEXTURE */
    WR32(MEM(d + 8), 0);
    WR32(MEM(d + 12), pitch * rows);
    WR32(MEM(d + 16), 0x11);                  /* D3DMULTISAMPLE_NONE, as the original reports */
    WR32(MEM(d + 20), w);
    WR32(MEM(d + 24), h);
}

HOOK(D3DTexture_GetLevelDesc) {
    /* (pThis, Level, pDesc) */
    write_surface_desc(ARG(ctx, 2), ARG(ctx, 0), ARG(ctx, 1));
    RET(D3DTexture_GetLevelDesc, 0);
}
HOOK(D3D_AllocContiguousMemory) {
    /* (Size, Alignment) */
    uint32_t a = kmem_alloc(ARG(ctx, 0) ? ARG(ctx, 0) : 16, ARG(ctx, 1) ? ARG(ctx, 1) : 4096);
    RET(D3D_AllocContiguousMemory, a);
}

/* ---- XGraphics shader assembler ---------------------------------------- */
/* This XGraphics has no COM buffer: an XGBUFFER is {refcount, pData, size}
   from the game's heap, read through C accessors (XGBuffer_GetBufferPointer
   0x397fb4 returns +4) and freed by XGBuffer_Release (0x397f8b). So the
   game's own XGBufferCreate(size, ppBuffer) (0x397fc8) makes it. */
static uint32_t make_xgbuffer(Ctx *ctx, uint32_t size) {
    static uint32_t pp; /* the out slot: startup only, one thread */
    if (!pp) pp = kmem_alloc(4, 4);
    uint32_t args[2] = {size, pp};
    WR32(MEM(pp), 0);
    if (rt_host_call(ctx, 0x00397fc8, 2, args) != 0) return 0;
    uint32_t buf = RD32(MEM(pp));
    memset(MEM(RD32(MEM(buf + 4))), 0, size);
    return buf;
}

/* XGAssembleShader(pSrcName, pSrcData, SrcDataLen, Flags, ppConstants,
   ppCompiledShader, ppErrorLog, ppShaderType, ...): the game assembles its
   shaders from source at startup. The host translates vertex shader source
   itself (shader_xlate.c), so the "compiled" vertex shader is a tag that
   CreateVertexShader turns back into the source: XVS_MAGIC and an index.
   Pixel shaders get an empty D3DPIXELSHADERDEF-sized buffer for now. */
HOOK(XGAssembleShader) {
    const char *text = ARG(ctx, 1) ? (const char *)MEM(ARG(ctx, 1)) : "";
    size_t text_len = ARG(ctx, 2);
    const char *head = text;
    while (head < text + text_len && (*head == ' ' || *head == '\t' || *head == '\r' || *head == '\n')) head++;
    int ps = xs_is_pixel_shader(text, text_len);
    if (ARG(ctx, 5) && (ps || !strncmp(head, "vs.", 3) || !strncmp(head, "xvs.", 4)) && g_nvs_sources < 64) {
        int id = g_nvs_sources++;
        g_vs_sources[id].src = malloc(text_len);
        memcpy(g_vs_sources[id].src, text, text_len);
        g_vs_sources[id].len = text_len;
        /* pixel shaders keep the D3DPIXELSHADERDEF size in case the game reads it */
        uint32_t buf = make_xgbuffer(ctx, ps ? 0xA4 : 16), data = buf ? RD32(MEM(buf + 4)) : 0;
        if (data) { WR32(MEM(data), ps ? XPS_MAGIC : XVS_MAGIC); WR32(MEM(data + 4), (uint32_t)id); }
        WR32(MEM(ARG(ctx, 5)), buf);
        if (ARG(ctx, 4)) WR32(MEM(ARG(ctx, 4)), 0);
        if (ARG(ctx, 6)) WR32(MEM(ARG(ctx, 6)), 0);
        if (ARG(ctx, 7)) WR32(MEM(ARG(ctx, 7)), 0);
        RET(XGAssembleShader, 0);
    }
    const char *name = ARG(ctx, 0) ? (const char *)MEM(ARG(ctx, 0)) : "?";
    uint32_t pp_const = ARG(ctx, 4), pp_shader = ARG(ctx, 5), pp_err = ARG(ctx, 6), pp_type = ARG(ctx, 7);
    trace("XGAssembleShader %s (%u bytes of source, flags %08x)", name, ARG(ctx, 2), ARG(ctx, 3));
    if (getenv("RECOMP_VSLOG"))
        fprintf(stderr, "xgasm %s: source %08x (%u bytes) flags %x out %08x/%08x/%08x\n", name, ARG(ctx, 1), ARG(ctx, 2), ARG(ctx, 3),
                ARG(ctx, 4), ARG(ctx, 5), ARG(ctx, 6));
    if (pp_const) WR32(MEM(pp_const), 0);
    if (pp_shader) WR32(MEM(pp_shader), make_xgbuffer(ctx, 0xA4));
    if (pp_err) WR32(MEM(pp_err), 0);
    if (pp_type) WR32(MEM(pp_type), 0);
    RET(XGAssembleShader, 0);
}

/* ---- resources ----------------------------------------------------------- */
HOOK(D3DDevice_CreateTexture) {
    /* (Width, Height, Levels, Usage, Format, Pool, ppTexture) */
    uint32_t o = new_pixel_container(T_TEXTURE, ARG(ctx, 0), ARG(ctx, 1), 0, ARG(ctx, 2), ARG(ctx, 4));
    if (ARG(ctx, 6)) WR32(MEM(ARG(ctx, 6)), o);
    trace("d3d: texture %ux%u fmt %02x levels %u -> %08x", ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 4), ARG(ctx, 2), o);
    RET(D3DDevice_CreateTexture, 0);
}
HOOK(D3DDevice_CreateVolumeTexture) {
    /* (Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture) */
    uint32_t o = new_pixel_container(T_TEXTURE, ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2), ARG(ctx, 3), ARG(ctx, 5));
    if (ARG(ctx, 7)) WR32(MEM(ARG(ctx, 7)), o);
    RET(D3DDevice_CreateVolumeTexture, 0);
}
static void create_buffer(Ctx *ctx, uint32_t type, uint32_t length, uint32_t pp) {
    uint32_t o = new_object(type);
    length = (length + 15) & ~15u;
    WR32(MEM(o + O_BYTES), length);
    WR32(MEM(o + O_DATA), alloc_or_die(length ? length : 16, 16, "buffer memory"));
    if (pp) WR32(MEM(pp), o);
}
HOOK(D3DDevice_CreateVertexBuffer) { create_buffer(ctx, T_VERTEXBUFFER, ARG(ctx, 0), ARG(ctx, 4)); RET(D3DDevice_CreateVertexBuffer, 0); }
HOOK(D3DDevice_CreateIndexBuffer) { create_buffer(ctx, T_INDEXBUFFER, ARG(ctx, 0), ARG(ctx, 4)); RET(D3DDevice_CreateIndexBuffer, 0); }

HOOK(D3DResource_AddRef) {
    uint32_t o = ARG(ctx, 0);
    uint32_t c = RD32(MEM(o + O_COMMON)) + 1;
    WR32(MEM(o + O_COMMON), c);
    RET(D3DResource_AddRef, c & 0xffff);
}
HOOK(D3DResource_Release) {
    uint32_t o = ARG(ctx, 0);
    uint32_t c = RD32(MEM(o + O_COMMON));
    if (c & 0xffff) c--;
    WR32(MEM(o + O_COMMON), c);
    if ((c & 0xffff) == 0 && is_ours(o) && o != g_backbuffer && o != g_depth && o != g_rendertarget) {
        /* last reference: free the backing memory and the object, as D3D would */
        if (RD32(MEM(o + O_BYTES))) kmem_free(RD32(MEM(o + O_DATA))); /* surfaces from GetSurfaceLevel alias their texture */
        if (g_render) r_resource_freed(o);
        WR32(MEM(o + O_MAGICOFF), 0);
        kmem_free(o);
    }
    RET(D3DResource_Release, c & 0xffff);
}
HOOK(D3DResource_Register) {
    /* resources from the game's bundles: Data holds an offset into the base the game passes */
    uint32_t o = ARG(ctx, 0), base = ARG(ctx, 1);
    WR32(MEM(o + O_DATA), RD32(MEM(o + O_DATA)) + base);
    if (g_render) r_texture_dirty(o);
    RET(D3DResource_Register, 0);
}
HOOK(D3DResource_GetType) {
    uint32_t o = ARG(ctx, 0);
    uint32_t t = (RD32(MEM(o + O_COMMON)) >> 16) & 7;
    static const uint32_t map[8] = {6, 7, 8, 9, 3, 1, 10, 0}; /* D3DRESOURCETYPE */
    uint32_t r = map[t];
    if (t == T_TEXTURE) { /* textures: 2D, volume (dimensions 3) or cube */
        uint32_t format = RD32(MEM(o + O_FORMAT));
        if (is_ours(o) ? RD32(MEM(o + O_DEPTH)) != 0 : ((format >> 4) & 0xf) == 3) r = 4;
        else if (format & 4) r = 5;
    }
    RET(D3DResource_GetType, r);
}
HOOK(D3DResource_BlockUntilNotBusy) { RET(D3DResource_BlockUntilNotBusy, 0); }
HOOK(D3DBaseTexture_GetLevelCount) {
    uint32_t o = ARG(ctx, 0);
    uint32_t n = is_ours(o) ? RD32(MEM(o + O_LEVELS)) : (RD32(MEM(o + O_FORMAT)) >> 16) & 0xf;
    RET(D3DBaseTexture_GetLevelCount, n ? n : 1);
}
HOOK(Get2DSurfaceDesc) {
    /* (pPixelContainer, Level, pDesc) */
    write_surface_desc(ARG(ctx, 2), ARG(ctx, 0), ARG(ctx, 1));
    RET(Get2DSurfaceDesc, 0);
}
HOOK(D3DTexture_GetSurfaceLevel) {
    uint32_t tex = ARG(ctx, 0), level = ARG(ctx, 1), pp = ARG(ctx, 2), w, h, fmt, pitch;
    describe(tex, level, &w, &h, &fmt, &pitch);
    uint32_t o = new_object(T_SURFACE);
    WR32(MEM(o + O_WIDTH), w); WR32(MEM(o + O_HEIGHT), h); WR32(MEM(o + O_FMT), fmt);
    WR32(MEM(o + O_LEVELS), 1); WR32(MEM(o + O_PITCH), pitch);
    WR32(MEM(o + O_DATA), RD32(MEM(tex + O_DATA)) + level_offset(tex, level));
    WR32(MEM(o + O_FORMAT), RD32(MEM(tex + O_FORMAT)));
    WR32(MEM(o + O_SIZE), RD32(MEM(tex + O_SIZE)));
    if (pp) WR32(MEM(pp), o);
    RET(D3DTexture_GetSurfaceLevel, 0);
}
static void lock_rect(uint32_t o, uint32_t level, uint32_t plocked) {
    uint32_t w, h, fmt, pitch;
    describe(o, level, &w, &h, &fmt, &pitch);
    WR32(MEM(plocked), pitch);
    WR32(MEM(plocked + 4), RD32(MEM(o + O_DATA)) + level_offset(o, level));
    if (g_render) r_texture_dirty(o);
}
HOOK(D3DTexture_LockRect) { lock_rect(ARG(ctx, 0), ARG(ctx, 1), ARG(ctx, 2)); RET(D3DTexture_LockRect, 0); }
HOOK(D3DSurface_LockRect) { lock_rect(ARG(ctx, 0), 0, ARG(ctx, 1)); RET(D3DSurface_LockRect, 0); }
HOOK(D3DVolumeTexture_LockBox) {
    /* (pThis, Level, pLockedBox {RowPitch, SlicePitch, pBits}, pBox, Flags) */
    uint32_t o = ARG(ctx, 0), level = ARG(ctx, 1), pb = ARG(ctx, 2), w, h, fmt, pitch;
    describe(o, level, &w, &h, &fmt, &pitch);
    WR32(MEM(pb), pitch); WR32(MEM(pb + 4), pitch * h); WR32(MEM(pb + 8), RD32(MEM(o + O_DATA)) + level_offset(o, level));
    RET(D3DVolumeTexture_LockBox, 0);
}
HOOK(D3DVertexBuffer_Lock) {
    /* (pThis, OffsetToLock, SizeToLock, ppbData, Flags) */
    uint32_t o = ARG(ctx, 0), off = ARG(ctx, 1), pp = ARG(ctx, 3);
    if (pp) WR32(MEM(pp), RD32(MEM(o + O_DATA)) + off);
    RET(D3DVertexBuffer_Lock, 0);
}
