/* Direct3D 9 implementation of render.h (RECOMP_RENDERER=d3d9): the first
   host renderer, kept as the reference for render_d3d11.c.

   Xbox D3D8 state comes in three encodings (see docs/recomp/recompiler-notes.md
   and Cxbx-Reloaded's XbD3D8Types.h, which this follows):
   - "simple" render states are NV2A push-buffer methods with GL-style values
     (compare functions 0x200.., blend factors 0x300.., stencil ops 0x1E00..),
   - "deferred" render states and texture stage states use the console's own
     enum indices with D3D8-style values,
   - a handful of states have dedicated entry points with D3D8 values.
   Resources stay in guest memory; textures are uploaded (unswizzled and
   converted to A8R8G8B8, or passed through for DXT) when they are first used
   after being written. Vertex data is drawn straight from guest memory with
   DrawPrimitiveUP. Vertex shaders are the game's source translated to HLSL
   (shader_xlate.c) with their vertices expanded per draw; pixel shaders are
   not translated yet, so their draws use the texture stage states. */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <d3dcommon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RENDER_BACKEND d3d9
#include "render_backend.h"
#include "guest.h"
#include "kernel.h"
#include "render.h"
#include "render_common.h"
#include "shader_xlate.h"

#pragma comment(lib, "d3d9.lib")

static HWND g_hwnd;
static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static uint32_t g_width, g_height;
static int g_lost;
static int g_drawlog = -1; /* RECOMP_DRAWLOG=N: describe the first N draws and transforms */
static uint32_t g_frame;   /* presents so far */

/* ---- window --------------------------------------------------------------- */
static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE: fprintf(stderr, "render: window closed\n"); ExitProcess(0);
    case WM_KEYDOWN: if (wp == VK_F12) { fprintf(stderr, "render: F12\n"); } return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static void pump(void) {
    MSG m;
    while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageA(&m); }
}

int r_ready(void) { return g_dev != NULL; }
int r_has_focus(void) { return g_hwnd && GetForegroundWindow() == g_hwnd; }

int r_init(uint32_t width, uint32_t height) {
    if (g_dev) return 1;
    g_drawlog = getenv("RECOMP_DRAWLOG") ? atoi(getenv("RECOMP_DRAWLOG")) : 0;
    g_width = width; g_height = height;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "ObiWanRecomp";
    RegisterClassA(&wc);
    RECT r = {0, 0, (LONG)width, (LONG)height};
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);
    g_hwnd = CreateWindowA(wc.lpszClassName, "Star Wars: Obi-Wan (recompiled)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                           r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) { fprintf(stderr, "render: no window (error %lu)\n", GetLastError()); return 0; }
    ShowWindow(g_hwnd, SW_SHOW);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d) { fprintf(stderr, "render: Direct3D 9 unavailable\n"); return 0; }
    D3DPRESENT_PARAMETERS pp = {0};
    pp.BackBufferWidth = width; pp.BackBufferHeight = height;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = g_hwnd;
    pp.Windowed = TRUE;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    pp.PresentationInterval = getenv("RECOMP_NOVSYNC") ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_ONE;
    HRESULT hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    if (FAILED(hr))
        hr = IDirect3D9_CreateDevice(g_d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_hwnd,
                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    if (FAILED(hr)) { fprintf(stderr, "render: CreateDevice failed %08lx\n", hr); return 0; }
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ZENABLE, D3DZB_TRUE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, FALSE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    IDirect3DDevice9_SetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    for (int s = 0; s < 4; s++) {
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        IDirect3DDevice9_SetSamplerState(g_dev, s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    IDirect3DDevice9_BeginScene(g_dev);
    fprintf(stderr, "render: Direct3D 9 device, %ux%u window\n", width, height);
    return 1;
}

/* ---- textures -------------------------------------------------------------- */
#define TEX_SLOTS 1024
typedef struct {
    uint32_t guest;              /* guest object address, 0 = free */
    IDirect3DTexture9 *tex;
    uint32_t w, h, fmt;
    int dirty, linear;
} TexSlot;
static TexSlot g_tex[TEX_SLOTS];
static uint32_t g_bound[4];      /* guest objects per stage */
static int g_stage_dirty = 1;
static float g_texmat[4][16] = {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1},
                                {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}};
static uint32_t g_texflags[4];   /* the game's D3DTSS_TEXTURETRANSFORMFLAGS per stage */
static uint32_t g_stage_ops[4][2]; /* COLOROP, ALPHAOP per stage, for the draw log */

static TexSlot *tex_find(uint32_t guest, int create) {
    TexSlot *free_slot = NULL;
    for (int i = 0; i < TEX_SLOTS; i++) {
        if (g_tex[i].guest == guest) return &g_tex[i];
        if (!g_tex[i].guest && !free_slot) free_slot = &g_tex[i];
    }
    if (!create) return NULL;
    if (!free_slot) { fprintf(stderr, "render: out of texture slots\n"); return NULL; }
    memset(free_slot, 0, sizeof *free_slot);
    free_slot->guest = guest;
    free_slot->dirty = 1;
    return free_slot;
}

void r_texture_dirty(uint32_t guest) {
    TexSlot *t = tex_find(guest, 0);
    if (t) t->dirty = 1;
}

void r_resource_freed(uint32_t guest) {
    TexSlot *t = tex_find(guest, 0);
    if (t) {
        if (t->tex) IDirect3DTexture9_Release(t->tex);
        memset(t, 0, sizeof *t);
    }
    for (int s = 0; s < 4; s++) if (g_bound[s] == guest) { g_bound[s] = 0; g_stage_dirty = 1; }
}

static void upload(TexSlot *t) {
    RcTex g;
    if (!rc_texture(t->guest, &g)) return;
    D3DFORMAT hostfmt = g.dxt ? (g.fmt == 0x0C ? D3DFMT_DXT1 : g.fmt == 0x0E ? D3DFMT_DXT3 : D3DFMT_DXT5) : D3DFMT_A8R8G8B8;
    if (t->tex && (t->w != g.w || t->h != g.h || t->fmt != g.fmt)) { IDirect3DTexture9_Release(t->tex); t->tex = NULL; }
    if (!t->tex) {
        if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, g.w, g.h, 1, 0, hostfmt, D3DPOOL_MANAGED, &t->tex, NULL))) {
            fprintf(stderr, "render: cannot create %ux%u texture (xbox format %02x)\n", g.w, g.h, g.fmt);
            t->tex = NULL; t->dirty = 0;
            return;
        }
        t->w = g.w; t->h = g.h; t->fmt = g.fmt;
    }
    t->linear = g.linear;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(t->tex, 0, &lr, NULL, 0))) return;
    if (g.dxt) {
        uint32_t bh = g.h < 4 ? 1 : g.h / 4;
        for (uint32_t y = 0; y < bh; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, g.src + y * g.pitch, g.pitch);
    } else {
        uint32_t *argb = malloc((size_t)g.w * g.h * 4);
        rc_texture_argb(&g, argb);
        for (uint32_t y = 0; y < g.h; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, argb + (size_t)y * g.w, g.w * 4);
        free(argb);
    }
    rc_dump_texture(&g);
    IDirect3DTexture9_UnlockRect(t->tex, 0);
    t->dirty = 0;
}

void r_set_texture(uint32_t stage, uint32_t guest) {
    if (stage >= 4) return;
    g_bound[stage] = guest;
    g_stage_dirty = 1;
}

/* ---- render targets ------------------------------------------------------- */
/* The game renders shadow buffers: a throwaway colour surface (a header over
   address 0) with a depth texture (LIN_F16) as its depth buffer, then
   samples that texture through the NV2A's shadow compare. Off the back
   buffer, draws go to a host colour target, and a guest depth texture is a
   host D24S8 depth texture (keyed by the guest texture's memory), which
   Direct3D 9 drivers sample with the same hardware depth compare. */
typedef struct { uint32_t data, w, h; IDirect3DTexture9 *tex; } DepthTex;
static DepthTex g_depthtex[8];
static IDirect3DSurface9 *g_bb_color, *g_bb_depth, *g_off_color;
static uint32_t g_off_w, g_off_h;

static DepthTex *depth_texture(uint32_t data, uint32_t w, uint32_t h, int create) {
    for (int i = 0; i < 8; i++) if (g_depthtex[i].data == data && g_depthtex[i].tex) return &g_depthtex[i];
    if (!create || !data) return NULL;
    for (int i = 0; i < 8; i++) {
        DepthTex *d = &g_depthtex[i];
        if (d->tex) continue;
        if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, w, h, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D24S8, D3DPOOL_DEFAULT, &d->tex, NULL))) {
            static int warned;
            if (!warned++) fprintf(stderr, "render: no D24S8 depth textures here; shadow buffers are not drawn\n");
            d->tex = NULL;
            return NULL;
        }
        d->data = data; d->w = w; d->h = h;
        return d;
    }
    return NULL;
}

/* z_kind: 0 no depth buffer, 1 the main one, 2 the depth texture at z_data */
void r_set_render_target(int back_buffer, uint32_t w, uint32_t h, int z_kind, uint32_t z_data, uint32_t z_w, uint32_t z_h) {
    if (!g_dev) return;
    if (!g_bb_color) { IDirect3DDevice9_GetRenderTarget(g_dev, 0, &g_bb_color); IDirect3DDevice9_GetDepthStencilSurface(g_dev, &g_bb_depth); }
    if (back_buffer) IDirect3DDevice9_SetRenderTarget(g_dev, 0, g_bb_color);
    else {
        if (!g_off_color || g_off_w != w || g_off_h != h) {
            if (g_off_color) IDirect3DSurface9_Release(g_off_color);
            g_off_color = NULL;
            if (FAILED(IDirect3DDevice9_CreateRenderTarget(g_dev, w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_off_color, NULL))) {
                fprintf(stderr, "render: cannot create a %ux%u render target\n", w, h);
                return;
            }
            g_off_w = w; g_off_h = h;
        }
        IDirect3DDevice9_SetRenderTarget(g_dev, 0, g_off_color);
    }
    IDirect3DSurface9 *ds = NULL;
    if (z_kind == 2) {
        DepthTex *d = depth_texture(z_data, z_w, z_h, 1);
        if (d) IDirect3DTexture9_GetSurfaceLevel(d->tex, 0, &ds);
    }
    IDirect3DDevice9_SetDepthStencilSurface(g_dev, z_kind == 1 ? g_bb_depth : ds);
    if (ds) IDirect3DSurface9_Release(ds);
}

/* bind the guest textures to the device, uploading what changed */
static void apply_textures(void) {
    if (!g_stage_dirty) return;
    g_stage_dirty = 0;
    for (uint32_t s = 0; s < 4; s++) {
        uint32_t g = g_bound[s];
        if (!g) { IDirect3DDevice9_SetTexture(g_dev, s, NULL); continue; }
        TexSlot *t = NULL;
        DepthTex *d = depth_texture(RD32(MEM(g + 4)), 0, 0, 0);
        uint32_t tw, th;
        int linear;
        if (d) { /* a shadow buffer we rendered: linear, addressed in texels */
            IDirect3DDevice9_SetTexture(g_dev, s, (IDirect3DBaseTexture9 *)d->tex);
            tw = d->w; th = d->h; linear = 1;
        } else {
            t = tex_find(g, 1);
            if (!t) continue;
            if (t->dirty) upload(t);
            IDirect3DDevice9_SetTexture(g_dev, s, (IDirect3DBaseTexture9 *)t->tex);
            tw = t->w; th = t->h; linear = t->tex && t->linear;
        }
        /* the game's texture matrix while its TEXTURETRANSFORMFLAGS enable it
           (a matrix left over from an animated texture must not move later
           draws), followed for linear textures, which the console addresses
           in texels, by a scale to normalised coordinates */
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        uint32_t flags = g_texflags[s];
        D3DMATRIX m;
        memcpy(&m, flags ? g_texmat[s] : identity, 64);
        if (linear) {
            float sx = 1.0f / tw, sy = 1.0f / th, sz = d ? rc_depth_unit_scale(g) : 1.0f;
            for (int r = 0; r < 4; r++) { m.m[r][0] *= sx; m.m[r][1] *= sy; m.m[r][2] *= sz; }
            if (!flags) flags = D3DTTFF_COUNT2;
        }
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_TEXTURE0 + s, &m);
        IDirect3DDevice9_SetTextureStageState(g_dev, s, D3DTSS_TEXTURETRANSFORMFLAGS, flags);
    }
}

/* ---- state ------------------------------------------------------------------ */
static void rs(D3DRENDERSTATETYPE s, uint32_t v) { if (g_dev) IDirect3DDevice9_SetRenderState(g_dev, s, v); }

/* polygon offset (SOLIDOFFSETENABLE, POLYGONOFFSETZSLOPESCALE/ZOFFSET): the
   shadow buffer passes rely on it against self-shadowing acne. The offset is
   in units of the smallest depth step; Direct3D 9 wants depth fractions. */
static struct { uint32_t fill; float slope, units; } g_polyoff;
static void apply_polygon_offset(void) {
    float slope = g_polyoff.fill ? g_polyoff.slope : 0, bias = g_polyoff.fill ? g_polyoff.units / 16777215.0f : 0;
    DWORD a, b;
    memcpy(&a, &slope, 4); memcpy(&b, &bias, 4);
    rs(D3DRS_SLOPESCALEDEPTHBIAS, a);
    rs(D3DRS_DEPTHBIAS, b);
}

/* alpha test as the game set it, and ALPHAKILL per texture stage (stage state
   21): the NV2A drops a pixel whose texel alpha is 0. Direct3D 9 has no such
   stage state: pixel shaders clip (texscale.w), and fixed-function draws get
   an alpha test rejecting alpha 0 when the game has none of its own (their
   alpha is the texel's, or that scaled by another input). */
static struct { uint32_t enable, func, ref; } g_atest = {0, D3DCMP_ALWAYS, 0};
static uint32_t g_alphakill[4];
static void apply_alpha_test(int kill) {
    int own = g_atest.enable || !kill;
    rs(D3DRS_ALPHATESTENABLE, own ? g_atest.enable : TRUE);
    rs(D3DRS_ALPHAFUNC, own ? g_atest.func : D3DCMP_GREATER);
    rs(D3DRS_ALPHAREF, own ? g_atest.ref : 0);
}

void r_set_rs_simple(uint32_t method, uint32_t value) {
    /* the caller passes the push-buffer header (dword count in bits 18+,
       subchannel in 13..15), e.g. 0x40304 for ALPHABLENDENABLE */
    float f;
    memcpy(&f, &value, 4);
    switch (method & 0x1FFC) {
    case 0x338: g_polyoff.fill = value != 0; apply_polygon_offset(); goto offset_log;
    case 0x384: g_polyoff.slope = f; apply_polygon_offset(); goto offset_log;
    case 0x388: g_polyoff.units = f; apply_polygon_offset();
    offset_log:
        if (getenv("RECOMP_RSLOG")) fprintf(stderr, "polygon offset: fill %u slope %g units %g\n", g_polyoff.fill, g_polyoff.slope, g_polyoff.units);
        break;
    case 0x354: rs(D3DRS_ZFUNC, rc_gl_cmp(value)); break;
    case 0x33C: g_atest.func = rc_gl_cmp(value); rs(D3DRS_ALPHAFUNC, g_atest.func); break;
    case 0x304: rs(D3DRS_ALPHABLENDENABLE, value != 0); break;
    case 0x300: g_atest.enable = value != 0; rs(D3DRS_ALPHATESTENABLE, g_atest.enable); break;
    case 0x340: g_atest.ref = value & 0xff; rs(D3DRS_ALPHAREF, g_atest.ref); break;
    case 0x344: rs(D3DRS_SRCBLEND, rc_gl_blend(value)); break;
    case 0x348: rs(D3DRS_DESTBLEND, rc_gl_blend(value)); break;
    case 0x35C: rs(D3DRS_ZWRITEENABLE, value != 0); break;
    case 0x310: rs(D3DRS_DITHERENABLE, value != 0); break;
    case 0x37C: rs(D3DRS_SHADEMODE, value == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD); break;
    case 0x358: /* NV2A colour mask: B bit 0, G bit 8, R bit 16, A bit 24 */
        rs(D3DRS_COLORWRITEENABLE, ((value & 1) ? D3DCOLORWRITEENABLE_BLUE : 0) | ((value & 0x100) ? D3DCOLORWRITEENABLE_GREEN : 0) |
                                   ((value & 0x10000) ? D3DCOLORWRITEENABLE_RED : 0) | ((value & 0x1000000) ? D3DCOLORWRITEENABLE_ALPHA : 0));
        break;
    case 0x374: rs(D3DRS_STENCILZFAIL, rc_gl_stencil_op(value)); break;
    case 0x378: rs(D3DRS_STENCILPASS, rc_gl_stencil_op(value)); break;
    case 0x364: rs(D3DRS_STENCILFUNC, rc_gl_cmp(value)); break;
    case 0x368: rs(D3DRS_STENCILREF, value & 0xff); break;
    case 0x36C: rs(D3DRS_STENCILMASK, value & 0xff); break;
    case 0x360: rs(D3DRS_STENCILWRITEMASK, value & 0xff); break;
    case 0x350: rs(D3DRS_BLENDOP, rc_gl_blendop(value)); break;
    case 0x34C: rs(D3DRS_BLENDFACTOR, ((value >> 24) << 24) | ((value & 0xff) << 16) | (value & 0xff00) | ((value >> 16) & 0xff)); break;
    default: break; /* polygon offset, swath, depth clip, stipple: nothing to map */
    }
}

/* Fog: the NV2A turns a per-vertex fog coordinate (eye distance, or oFog from
   a vertex shader) into a factor with the FOGTABLEMODE function, and the
   final combiner blends with the fog colour. Direct3D 9 gets the same from
   vertex fog for fixed-function draws, from a factor the translated vertex
   shader computes (constants c96: start, end, density, mode), and leaves
   draws unfogged whose pixel shader's final combiner ignores fog. */
static struct { uint32_t enable, mode, range; float start, end, density; } g_fog;
static uint32_t g_fog_forced_color;

static float as_float(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

void r_set_rs_deferred(uint32_t state, uint32_t value) {
    switch (state) {
    case XRS_FOGENABLE: g_fog.enable = value != 0; break;
    case XRS_FOGTABLEMODE: g_fog.mode = value <= 3 ? value : 0; break;   /* D3DFOG_NONE, EXP, EXP2, LINEAR */
    case XRS_FOGSTART: g_fog.start = as_float(value); break;
    case XRS_FOGEND: g_fog.end = as_float(value); break;
    case XRS_FOGDENSITY: g_fog.density = as_float(value); break;
    case XRS_RANGEFOGENABLE: g_fog.range = value != 0; break;
    case XRS_WRAP0: case XRS_WRAP1: case XRS_WRAP2: case XRS_WRAP3: rs(D3DRS_WRAP0 + (state - XRS_WRAP0), value); break;
    case XRS_LIGHTING: rs(D3DRS_LIGHTING, value != 0); break;
    case XRS_SPECULARENABLE: rs(D3DRS_SPECULARENABLE, value != 0); break;
    case XRS_LOCALVIEWER: rs(D3DRS_LOCALVIEWER, value != 0); break;
    case XRS_COLORVERTEX: rs(D3DRS_COLORVERTEX, value != 0); break;
    case XRS_SPECULARMATERIALSOURCE: rs(D3DRS_SPECULARMATERIALSOURCE, value); break;
    case XRS_DIFFUSEMATERIALSOURCE: rs(D3DRS_DIFFUSEMATERIALSOURCE, value); break;
    case XRS_AMBIENTMATERIALSOURCE: rs(D3DRS_AMBIENTMATERIALSOURCE, value); break;
    case XRS_EMISSIVEMATERIALSOURCE: rs(D3DRS_EMISSIVEMATERIALSOURCE, value); break;
    case XRS_AMBIENT: rs(D3DRS_AMBIENT, value); break;
    case XRS_POINTSIZE: rs(D3DRS_POINTSIZE, value); break;
    case XRS_POINTSIZE_MIN: rs(D3DRS_POINTSIZE_MIN, value); break;
    case XRS_POINTSPRITEENABLE: rs(D3DRS_POINTSPRITEENABLE, value != 0); break;
    case XRS_POINTSCALEENABLE: rs(D3DRS_POINTSCALEENABLE, value != 0); break;
    case XRS_POINTSCALE_A: rs(D3DRS_POINTSCALE_A, value); break;
    case XRS_POINTSCALE_B: rs(D3DRS_POINTSCALE_B, value); break;
    case XRS_POINTSCALE_C: rs(D3DRS_POINTSCALE_C, value); break;
    case XRS_POINTSIZE_MAX: rs(D3DRS_POINTSIZE_MAX, value); break;
    default: break;
    }
}

/* The deferred states live in D3D_g_RenderState (0x37AC68): the hooked setter
   stores there like the original, and the game's generic SetRenderState and
   inline code write there directly. Apply whatever changed before a draw. */
static void sync_deferred_states(void) {
    static uint32_t seen[XRS_DEFERRED_COUNT];
    static int primed;
    for (uint32_t i = 0; i < XRS_DEFERRED_COUNT; i++) {
        uint32_t v = RD32(MEM(XRS_STATE_ARRAY + 4 * (XRS_DEFERRED_FIRST + i)));
        if (primed && v == seen[i]) continue;
        seen[i] = v;
        if (getenv("RECOMP_RSLOG")) fprintf(stderr, "deferred state %u = %08x\n", XRS_DEFERRED_FIRST + i, v);
        r_set_rs_deferred(XRS_DEFERRED_FIRST + i, v);
    }
    primed = 1;
}

/* fog for the next draw: ps_fog is 0 when the pixel shader's final combiner
   does not blend fog */
static void apply_fog(int ps_fog, int vertex_shader) {
    static int forced = -1;
    if (forced < 0) { /* RECOMP_FOG=start,end,RRGGBB: linear fog everywhere, to check the fog path */
        float s, e; unsigned col;
        const char *f = getenv("RECOMP_FOG");
        forced = f && sscanf(f, "%f,%f,%x", &s, &e, &col) == 3;
        if (forced) { g_fog_forced_color = 0xff000000u | col; g_fog.start = s; g_fog.end = e; }
    }
    if (forced) {
        /* multiplied into the scene (soot decals: DESTCOLOR, ZERO) a draw
           fades to white, as a game sets it up, not to the fog colour */
        DWORD blend = 0, src = 0, dst = 0;
        IDirect3DDevice9_GetRenderState(g_dev, D3DRS_ALPHABLENDENABLE, &blend);
        IDirect3DDevice9_GetRenderState(g_dev, D3DRS_SRCBLEND, &src);
        IDirect3DDevice9_GetRenderState(g_dev, D3DRS_DESTBLEND, &dst);
        int multiply = blend && ((src == D3DBLEND_DESTCOLOR && dst == D3DBLEND_ZERO) || (src == D3DBLEND_ZERO && dst == D3DBLEND_SRCCOLOR));
        g_fog.enable = 1; g_fog.mode = 3;
        rs(D3DRS_FOGCOLOR, multiply ? 0xffffffffu : g_fog_forced_color);
    }
    int on = g_fog.enable && g_fog.mode && ps_fog;
    rs(D3DRS_FOGENABLE, on);
    if (!on) return;
    rs(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    rs(D3DRS_FOGVERTEXMODE, g_fog.mode);
    rs(D3DRS_RANGEFOGENABLE, g_fog.range);
    DWORD start, end, density;
    memcpy(&start, &g_fog.start, 4); memcpy(&end, &g_fog.end, 4); memcpy(&density, &g_fog.density, 4);
    rs(D3DRS_FOGSTART, start); rs(D3DRS_FOGEND, end); rs(D3DRS_FOGDENSITY, density);
    if (vertex_shader) {
        float c[4] = {g_fog.start, g_fog.end, g_fog.density, (float)g_fog.mode};
        IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 96, c, 1);
    }
}

void r_set_rs(uint32_t pc_state, uint32_t value) {
    if (pc_state == D3DRS_CULLMODE) value = rc_xbox_cull(value);
    else if (pc_state == D3DRS_STENCILFAIL) value = rc_gl_stencil_op(value);
    else if (pc_state == D3DRS_ZENABLE) value = value ? D3DZB_TRUE : D3DZB_FALSE;
    rs(pc_state, value);
}

/* Xbox D3DTSS indices of D3D8 3948 (XDK 3911): the stage operations come
   first (COLOROP 0 .. TEXTURETRANSFORMFLAGS 9), then the sampler states
   (ADDRESSU 10 .. ALPHAKILL 21); deferred 0..21, then the rest. Later XDKs
   moved the sampler states to the front. */
void r_set_tss(uint32_t stage, uint32_t type, uint32_t value) {
    if (!g_dev || stage >= 4) return;
    if (g_drawlog > 0) fprintf(stderr, "tss stage %u type %u = %08x\n", stage, type, value);
    switch (type) {
    case 0: g_stage_ops[stage][0] = value; IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLOROP, rc_xbox_top(value)); break;
    case 1: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLORARG0, value); break;
    case 2: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLORARG1, value); break;
    case 3: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLORARG2, value); break;
    case 4: g_stage_ops[stage][1] = value; IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_ALPHAOP, rc_xbox_top(value)); break;
    case 5: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_ALPHAARG0, value); break;
    case 6: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_ALPHAARG1, value); break;
    case 7: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_ALPHAARG2, value); break;
    case 8: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_RESULTARG, value); break;
    case 9: g_texflags[stage] = value; g_stage_dirty = 1; break; /* applied with the texture matrix */
    case 10: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_ADDRESSU, value > 4 ? D3DTADDRESS_CLAMP : value ? value : 1); break;
    case 11: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_ADDRESSV, value > 4 ? D3DTADDRESS_CLAMP : value ? value : 1); break;
    case 12: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_ADDRESSW, value > 4 ? D3DTADDRESS_CLAMP : value ? value : 1); break;
    case 13: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_MAGFILTER, value >= 3 ? D3DTEXF_LINEAR : value); break;
    case 14: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_MINFILTER, value >= 3 ? D3DTEXF_LINEAR : value); break;
    case 15: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE); break; /* one level uploaded */
    case 16: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_MIPMAPLODBIAS, value); break;
    case 17: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_MAXMIPLEVEL, 0); break;
    case 18: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_MAXANISOTROPY, value ? value : 1); break;
    /* 19 COLORKEYOP, 20 COLORSIGN: no Direct3D 9 equivalent */
    case 21: /* applied with the pixel shader or alpha test */
        if (getenv("RECOMP_RSLOG") && g_alphakill[stage] != value) fprintf(stderr, "alphakill: stage %u = %u (frame %u)\n", stage, value, g_frame);
        g_alphakill[stage] = value;
        break;
    case 22: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_BUMPENVMAT00, value); break;
    case 23: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_BUMPENVMAT01, value); break;
    case 24: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_BUMPENVMAT11, value); break;
    case 25: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_BUMPENVMAT10, value); break;
    case 26: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_BUMPENVLSCALE, value); break;
    case 27: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_BUMPENVLOFFSET, value); break;
    case 28: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_TEXCOORDINDEX, value); break;
    case 29: IDirect3DDevice9_SetSamplerState(g_dev, stage, D3DSAMP_BORDERCOLOR, value); break;
    default: break;
    }
}

void r_set_transform(uint32_t state, const float *m) {
    if (!g_dev) return;
    /* Xbox D3DTRANSFORMSTATETYPE: VIEW 0, PROJECTION 1, TEXTURE0..3 2..5, WORLD..WORLD3 6..9
       (and D3DTS_WORLDMATRIX(n) = 256 + n) */
    if (g_drawlog > 0)
        fprintf(stderr, "transform %u: [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]\n", state,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    if (state == 0) IDirect3DDevice9_SetTransform(g_dev, D3DTS_VIEW, (const D3DMATRIX *)m);
    else if (state == 1) IDirect3DDevice9_SetTransform(g_dev, D3DTS_PROJECTION, (const D3DMATRIX *)m);
    else if (state >= 2 && state <= 5) { memcpy(g_texmat[state - 2], m, 64); g_stage_dirty = 1; }
    else if (state >= 6 && state <= 9) IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLDMATRIX(state - 6), (const D3DMATRIX *)m);
    else if (state >= 256 && state < 512) IDirect3DDevice9_SetTransform(g_dev, D3DTS_WORLDMATRIX(state - 256), (const D3DMATRIX *)m);
}

void r_set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz) {
    if (!g_dev || !w || !h) return;
    if (g_drawlog > 0) fprintf(stderr, "viewport %u %u %u %u z %.2f..%.2f\n", x, y, w, h, minz, maxz);
    D3DVIEWPORT9 vp = {x, y, w, h, minz, maxz};
    IDirect3DDevice9_SetViewport(g_dev, &vp);
}

/* ---- vertices ------------------------------------------------------------- */
static uint32_t g_fvf, g_vs_handle, g_ps_handle;
static uint32_t g_stream_vb, g_stream_stride;
static int g_warned_vs;

/* ---- programmable vertex shaders --------------------------------------------- */
/* The game's shader source becomes HLSL (shader_xlate.c), compiled for vs_2_0
   on first use. The Xbox vertex declaration describes stream 0 in packed
   formats Direct3D 9 lacks (3-byte, 11:11:10) at unaligned offsets, so each
   draw expands the vertices it touches to one float4 per shader input, read
   through TEXCOORDn. */
typedef HRESULT(WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
typedef struct {
    uint32_t handle;
    char *src; size_t len;
    RcVElem elems[16]; int nelems;
    IDirect3DVertexShader9 *vs;
    IDirect3DVertexDeclaration9 *decl;
    uint8_t regs[16]; int nregs;         /* shader inputs, in host vertex order */
    int failed;
} VShader;
#define VSHADERS 64
static VShader g_vshaders[VSHADERS];
static VShader *g_vsh;                   /* the bound shader, NULL for FVF */
static float g_vsc[96][4];               /* constants c0..c95 */

void r_create_vertex_shader(uint32_t handle, const char *src, size_t len, const uint32_t *decl, int ndecl) {
    VShader *s = NULL;
    for (int i = 0; i < VSHADERS && !s; i++) if (!g_vshaders[i].handle) s = &g_vshaders[i];
    if (!s) { fprintf(stderr, "render: out of vertex shader slots\n"); return; }
    memset(s, 0, sizeof *s);
    s->handle = handle;
    s->src = malloc(len);
    memcpy(s->src, src, len);
    s->len = len;
    s->nelems = rc_parse_vertex_decl(decl, ndecl, s->elems, 16);
    if (s->nelems < 0) { s->nelems = 0; s->failed = 1; }
}

void r_delete_vertex_shader(uint32_t handle) {
    for (int i = 0; i < VSHADERS; i++) {
        VShader *s = &g_vshaders[i];
        if (s->handle != handle) continue;
        if (s->vs) IDirect3DVertexShader9_Release(s->vs);
        if (s->decl) IDirect3DVertexDeclaration9_Release(s->decl);
        free(s->src);
        if (g_vsh == s) g_vsh = NULL;
        memset(s, 0, sizeof *s);
    }
}

void r_set_vs_constants(int reg, const float *data, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (reg + (int)i >= 0 && reg + (int)i < 96) memcpy(g_vsc[reg + i], data + 4 * i, 16);
}

/* HLSL to bytecode through d3dcompiler_47 (loaded on first use); NULL on failure */
static ID3DBlob *compile_hlsl(const char *hlsl, const char *target, uint32_t handle) {
    static D3DCompileFn compile;
    static int missing;
    if (!compile && !missing) {
        HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
        compile = dll ? (D3DCompileFn)GetProcAddress(dll, "D3DCompile") : NULL;
        if (!compile) { missing = 1; fprintf(stderr, "render: d3dcompiler_47.dll is missing; shaders are not drawn\n"); }
    }
    if (!compile) return NULL;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = compile(hlsl, strlen(hlsl), target, NULL, NULL, "main", target, 0, 0, &code, &errors);
    if (FAILED(hr))
        fprintf(stderr, "render: %s shader %08x does not compile: %s\n", target, handle, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
    if (errors) ID3D10Blob_Release(errors);
    return SUCCEEDED(hr) ? code : NULL;
}

/* translate and compile on first use */
static int vshader_ready(VShader *s) {
    if (s->vs) return 1;
    if (s->failed || !g_dev) return 0;
    s->failed = 1;
    static char hlsl[1 << 17];
    uint32_t inputs = 0;
    if (xs_vs_to_hlsl(s->src, s->len, XS_SM2, hlsl, sizeof hlsl, &inputs)) {
        fprintf(stderr, "render: vertex shader %08x not translated: %s\n", s->handle, hlsl);
        return 0;
    }
    ID3DBlob *code = compile_hlsl(hlsl, "vs_2_0", s->handle);
    if (!code) return 0;
    HRESULT hr = IDirect3DDevice9_CreateVertexShader(g_dev, (const DWORD *)ID3D10Blob_GetBufferPointer(code), &s->vs);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { fprintf(stderr, "render: CreateVertexShader failed %08lx\n", hr); return 0; }
    D3DVERTEXELEMENT9 el[17];
    s->nregs = 0;
    for (int r = 0; r < 16; r++) {
        if (!(inputs & (1u << r))) continue;
        el[s->nregs] = (D3DVERTEXELEMENT9){0, (WORD)(16 * s->nregs), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, (BYTE)r};
        s->regs[s->nregs++] = (uint8_t)r;
    }
    el[s->nregs] = (D3DVERTEXELEMENT9)D3DDECL_END();
    if (FAILED(IDirect3DDevice9_CreateVertexDeclaration(g_dev, el, &s->decl))) { fprintf(stderr, "render: vertex declaration failed\n"); return 0; }
    s->failed = 0;
    return 1;
}

/* draw with the bound vertex shader: expand the guest vertices a draw
   touches (first..last) into float4 inputs, indices rebased to first */
static D3DPRIMITIVETYPE prim_pc(uint32_t p, uint32_t n, uint32_t *count);
/* RECOMP_PROBE=frame,x,y: after each draw of that frame that targets the back
   buffer, report when the pixel at (x, y) changes and what the draw was */
static void probe(void) {
    static int frame = -1, px, py;
    static uint32_t last, k, kframe;
    static IDirect3DSurface9 *one, *sys;
    if (frame < 0) {
        const char *e = getenv("RECOMP_PROBE");
        frame = 0;
        if (e) sscanf(e, "%d,%d,%d", &frame, &px, &py);
    }
    if (!frame || g_frame + 1 != (uint32_t)frame) return;
    if (kframe != g_frame + 1) { kframe = g_frame + 1; k = 0; last = 0xdeadbeef; }
    k++;
    IDirect3DSurface9 *rt = NULL, *bb = NULL;
    IDirect3DDevice9_GetRenderTarget(g_dev, 0, &rt);
    IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (rt && rt == bb) {
        if (!one) {
            IDirect3DDevice9_CreateRenderTarget(g_dev, 1, 1, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &one, NULL);
            IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, 1, 1, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL);
        }
        RECT r = {px, py, px + 1, py + 1};
        D3DLOCKED_RECT lr;
        IDirect3DDevice9_EndScene(g_dev);
        if (one && sys && SUCCEEDED(IDirect3DDevice9_StretchRect(g_dev, rt, &r, one, NULL, D3DTEXF_POINT)) &&
            SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_dev, one, sys)) && SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
            uint32_t c = *(const uint32_t *)lr.pBits & 0xffffff;
            IDirect3DSurface9_UnlockRect(sys);
            if (c != last)
                fprintf(stderr, "probe: draw %u %06x -> %06x | vs %08x ps %08x fvf %03x tex %08x %08x %08x %08x\n", k, last & 0xffffff, c,
                        g_vsh ? g_vsh->handle : 0, g_ps_handle, g_fvf, g_bound[0], g_bound[1], g_bound[2], g_bound[3]);
            last = c;
        }
        IDirect3DDevice9_BeginScene(g_dev);
    }
    if (rt) IDirect3DSurface9_Release(rt);
    if (bb) IDirect3DSurface9_Release(bb);
}

static void apply_textures(void);
static void apply_pixel_shader(void);
static void draw_vs(uint32_t prim, uint32_t n, const uint8_t *verts, uint32_t stride, const uint16_t *idx) {
    if (!vshader_ready(g_vsh) || !n) return;
    {   /* debugging: RECOMP_NOSHADOW leaves out draws that sample a shadow
           buffer, RECOMP_SHADOWDRAWS=a-b keeps only the a-th to b-th of each frame */
        static uint32_t frame, k;
        int receiver = 0;
        for (int s = 0; s < 4; s++) receiver |= g_bound[s] && depth_texture(RD32(MEM(g_bound[s] + 4)), 0, 0, 0);
        if (frame != g_frame) frame = g_frame, k = 0;
        if (receiver) {
            const char *e = getenv("RECOMP_SHADOWDRAWS");
            unsigned a = 0, b = 0;
            k++;
            if (getenv("RECOMP_NOSHADOW") || (e && sscanf(e, "%u-%u", &a, &b) == 2 && (k < a || k > b))) return;
        }
    }
    uint32_t first = 0, last = n - 1;
    if (idx) {
        first = 0xffff; last = 0;
        for (uint32_t i = 0; i < n; i++) { if (idx[i] < first) first = idx[i]; if (idx[i] > last) last = idx[i]; }
    }
    uint32_t count = last - first + 1, vstride = 16 * g_vsh->nregs;
    static float *buf; static uint32_t cap;
    if (count * g_vsh->nregs * 4 > cap) { cap = count * g_vsh->nregs * 4; buf = realloc(buf, cap * sizeof *buf); }
    rc_expand_vertices(g_vsh->elems, g_vsh->nelems, g_vsh->regs, g_vsh->nregs, verts, stride, first, count, buf);
    apply_textures();
    apply_pixel_shader();
    {   /* RECOMP_SHADOWLOG=frame: the shadow-receiving draws of that frame:
           constants c5..c9 and the light-space coordinates of a few vertices */
        static int frame = -1;
        if (frame < 0) frame = getenv("RECOMP_SHADOWLOG") ? atoi(getenv("RECOMP_SHADOWLOG")) : 0;
        int s = 0;
        while (s < 4 && !(g_bound[s] && depth_texture(RD32(MEM(g_bound[s] + 4)), 0, 0, 0))) s++;
        if (frame && g_frame + 1 == (uint32_t)frame && s < 4) {
            DWORD au, av, bc;
            IDirect3DDevice9_GetSamplerState(g_dev, s, D3DSAMP_ADDRESSU, &au);
            IDirect3DDevice9_GetSamplerState(g_dev, s, D3DSAMP_ADDRESSV, &av);
            IDirect3DDevice9_GetSamplerState(g_dev, s, D3DSAMP_BORDERCOLOR, &bc);
            fprintf(stderr, "shadow: vs %08x ps %08x stage %d addr %lu %lu border %08lx n %u", g_vsh->handle, g_ps_handle, s, au, av, bc, n);
            for (int r = 5; r <= 9; r++) fprintf(stderr, " | c%d %g %g %g %g", r, g_vsc[r][0], g_vsc[r][1], g_vsc[r][2], g_vsc[r][3]);
            fprintf(stderr, "\n");
            int pos = 0;
            while (pos < g_vsh->nregs && g_vsh->regs[pos] != 0) pos++;
            float qmin = 1e30f, qmax = -1e30f, pmin[3] = {1e30f, 1e30f, 1e30f}, pmax[3] = {-1e30f, -1e30f, -1e30f};
            int inside = 0, behind = 0;
            for (uint32_t v = 0; pos < g_vsh->nregs && v < count; v++) {
                const float *p = buf + (size_t)v * g_vsh->nregs * 4 + pos * 4;
                float l[4];
                for (int r = 0; r < 4; r++) l[r] = p[0] * g_vsc[5 + r][0] + p[1] * g_vsc[5 + r][1] + p[2] * g_vsc[5 + r][2] + p[3] * g_vsc[5 + r][3];
                if (l[3] < qmin) qmin = l[3];
                if (l[3] > qmax) qmax = l[3];
                for (int a = 0; a < 3; a++) { if (p[a] < pmin[a]) pmin[a] = p[a]; if (p[a] > pmax[a]) pmax[a] = p[a]; }
                float s = l[0] / l[3], t = l[1] / l[3];
                if (s >= 0 && s < 256 && t >= 0 && t < 256) { if (l[3] > 0) inside++; else behind++; }
            }
            fprintf(stderr, "  verts %u: q %.2f .. %.2f, in map %d (q < 0: %d), pos %.0f..%.0f %.0f..%.0f %.0f..%.0f\n", count, qmin, qmax, inside, behind,
                    pmin[0], pmax[0], pmin[1], pmax[1], pmin[2], pmax[2]);
        }
    }
    {   /* RECOMP_STATELOG=frame: the shader draws of that frame too */
        static int frame = -1;
        if (frame < 0) frame = getenv("RECOMP_STATELOG") ? atoi(getenv("RECOMP_STATELOG")) : 0;
        if (frame && g_frame + 1 == (uint32_t)frame) {
            char head[48];
            const char *p = g_vsh->src;
            while (p < g_vsh->src + g_vsh->len && (*p == ' ' || *p == '\n' || *p == '\r')) p++;
            snprintf(head, sizeof head, "%.40s", p);
            for (char *q = head; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
            fprintf(stderr, "vsdraw: prim %u n %u verts %u vs %08x (%u bytes: %s) ps %08x tex %08x %08x %08x %08x | c5 %.2f %.2f %.2f | c16 %.2f %.2f %.2f | c7 %.2f %.2f %.2f %.0f\n",
                    prim, n, count, g_vsh->handle, (unsigned)g_vsh->len, head, g_ps_handle, g_bound[0], g_bound[1], g_bound[2], g_bound[3],
                    g_vsc[5][0], g_vsc[5][1], g_vsc[5][2], g_vsc[16][0], g_vsc[16][1], g_vsc[16][2], g_vsc[7][0], g_vsc[7][1], g_vsc[7][2], g_vsc[7][3]);
        }
    }
    IDirect3DDevice9_SetVertexDeclaration(g_dev, g_vsh->decl);
    IDirect3DDevice9_SetVertexShader(g_dev, g_vsh->vs);
    IDirect3DDevice9_SetVertexShaderConstantF(g_dev, 0, &g_vsc[0][0], 96);
    static uint16_t *ib; static uint32_t ibcap;
    uint32_t prims, need = prim == 8 ? n / 4 * 6 : n;
    if (need > ibcap) { ibcap = need; ib = realloc(ib, ibcap * 2); }
    if (prim == 8) { /* quads as triangle pairs */
        for (uint32_t q = 0; q < n / 4; q++) {
            uint32_t a = idx ? idx[q * 4] : q * 4, b = idx ? idx[q * 4 + 1] : q * 4 + 1, c = idx ? idx[q * 4 + 2] : q * 4 + 2, d = idx ? idx[q * 4 + 3] : q * 4 + 3;
            uint16_t *t = ib + q * 6;
            t[0] = (uint16_t)(a - first); t[1] = (uint16_t)(b - first); t[2] = (uint16_t)(c - first);
            t[3] = (uint16_t)(a - first); t[4] = (uint16_t)(c - first); t[5] = (uint16_t)(d - first);
        }
        IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 0, count, n / 4 * 2, ib, D3DFMT_INDEX16, buf, vstride);
    } else {
        D3DPRIMITIVETYPE t = prim_pc(prim, n, &prims);
        if (prims && idx) {
            for (uint32_t i = 0; i < n; i++) ib[i] = (uint16_t)(idx[i] - first);
            IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, t, 0, count, prims, ib, D3DFMT_INDEX16, buf, vstride);
        } else if (prims) IDirect3DDevice9_DrawPrimitiveUP(g_dev, t, prims, buf, vstride);
    }
    probe();
}

void r_set_vertex_shader(uint32_t handle) {
    g_vs_handle = handle;
    if (handle & 1) { /* a vertex shader object (CreateVertexShader) */
        g_vsh = NULL;
        for (int i = 0; i < VSHADERS; i++) if (g_vshaders[i].handle == handle) g_vsh = &g_vshaders[i];
        if (!g_vsh && !g_warned_vs++) fprintf(stderr, "render: unknown vertex shader handle %08x\n", handle);
        g_fvf = 0;
        return;
    }
    g_vsh = NULL;
    g_fvf = handle;
    if (g_dev) {
        IDirect3DDevice9_SetVertexShader(g_dev, NULL);
        IDirect3DDevice9_SetFVF(g_dev, handle & ~0x1u);
    }
}
/* ---- pixel shaders --------------------------------------------------------- */
/* Translated like the vertex shaders (ps_2_0); a draw binds the current one,
   or none, so the texture stage states apply */
typedef struct { uint32_t handle; char *src; size_t len; IDirect3DPixelShader9 *ps; int failed, fog; } PShader;
#define PSHADERS 64
static PShader g_pshaders[PSHADERS];
static PShader *g_psh;
static float g_psc[8][4];

void r_create_pixel_shader_source(uint32_t handle, const char *src, size_t len) {
    PShader *s = NULL;
    for (int i = 0; i < PSHADERS && !s; i++) if (!g_pshaders[i].handle) s = &g_pshaders[i];
    if (!s) { fprintf(stderr, "render: out of pixel shader slots\n"); return; }
    memset(s, 0, sizeof *s);
    s->handle = handle;
    s->src = malloc(len);
    memcpy(s->src, src, len);
    s->len = len;
}

void r_delete_pixel_shader(uint32_t handle) {
    for (int i = 0; i < PSHADERS; i++) {
        PShader *s = &g_pshaders[i];
        if (s->handle != handle) continue;
        if (s->ps) IDirect3DPixelShader9_Release(s->ps);
        free(s->src);
        if (g_psh == s) g_psh = NULL;
        memset(s, 0, sizeof *s);
    }
}

void r_set_ps_constants(int reg, const float *data, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (reg + (int)i >= 0 && reg + (int)i < 8) memcpy(g_psc[reg + i], data + 4 * i, 16);
}

static PShader *find_pshader(uint32_t handle) {
    for (int i = 0; handle && i < PSHADERS; i++) if (g_pshaders[i].handle == handle) return &g_pshaders[i];
    return NULL;
}

void r_set_pixel_shader(uint32_t handle) { g_ps_handle = handle; g_psh = find_pshader(handle); }

static int pshader_ready(PShader *s) {
    if (s->ps) return 1;
    if (s->failed || !g_dev) return 0;
    s->failed = 1;
    static char hlsl[1 << 16];
    if (xs_ps_to_hlsl(s->src, s->len, XS_SM2, 0, hlsl, sizeof hlsl, &s->fog)) {
        fprintf(stderr, "render: pixel shader %08x not translated: %s\n", s->handle, hlsl);
        return 0;
    }
    ID3DBlob *code = compile_hlsl(hlsl, "ps_2_0", s->handle);
    if (!code) return 0;
    HRESULT hr = IDirect3DDevice9_CreatePixelShader(g_dev, (const DWORD *)ID3D10Blob_GetBufferPointer(code), &s->ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { fprintf(stderr, "render: CreatePixelShader failed %08lx\n", hr); return 0; }
    s->failed = 0;
    return 1;
}

/* the pixel shader for the next draw, or the fixed-function stages; with it
   the deferred render states and fog */
static void apply_pixel_shader(void) {
    sync_deferred_states();
    int ps_fog = 1, kill = 0;
    if (g_psh && pshader_ready(g_psh)) {
        IDirect3DDevice9_SetPixelShader(g_dev, g_psh->ps);
        IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 0, &g_psc[0][0], 8);
        float scale[4][4];
        for (int s = 0; s < 4; s++) {   /* texel coordinates for linear textures; ALPHAKILL */
            scale[s][0] = scale[s][1] = scale[s][2] = 1;
            scale[s][3] = g_alphakill[s] ? 0.5f / 255 : -1;
            if (!g_bound[s]) continue;
            DepthTex *d = depth_texture(RD32(MEM(g_bound[s] + 4)), 0, 0, 0);
            TexSlot *t = d ? NULL : tex_find(g_bound[s], 0);
            if (d) { scale[s][0] = 1.0f / d->w; scale[s][1] = 1.0f / d->h; scale[s][2] = rc_depth_unit_scale(g_bound[s]); }
            else if (t && t->tex && t->linear) { scale[s][0] = 1.0f / t->w; scale[s][1] = 1.0f / t->h; }
        }
        IDirect3DDevice9_SetPixelShaderConstantF(g_dev, 8, &scale[0][0], 4);
        ps_fog = g_psh->fog;
    } else {
        IDirect3DDevice9_SetPixelShader(g_dev, NULL);
        for (int s = 0; s < 4; s++) kill |= g_bound[s] && g_alphakill[s];
    }
    apply_alpha_test(kill);
    apply_fog(ps_fog, g_vsh != NULL);
}

void r_create_pixel_shader(uint32_t handle, const uint32_t *def) {
    /* for now: describe each distinct definition once (RECOMP_DRAWLOG) */
    static uint32_t seen[32][66]; static int nseen;
    for (int i = 0; i < nseen; i++) if (memcmp(seen[i], def, 264) == 0) return;
    if (nseen < 32) memcpy(seen[nseen++], def, 264);
    if (g_drawlog <= 0 && !getenv("RECOMP_PSLOG")) return;
    fprintf(stderr, "pixel shader %08x: count %08x rgbin", handle, def[53]);
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", def[34 + i]);
    fprintf(stderr, "\n   rgbout");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", def[45 + i]);
    fprintf(stderr, "\n   alphain");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", def[i]);
    fprintf(stderr, "\n   alphaout");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %08x", def[26 + i]);
    fprintf(stderr, "\n   final %08x %08x texmodes %08x c0 %08x c1 %08x dot %08x inputtex %08x\n", def[8], def[9], def[54], def[10], def[18], def[55], def[56]);
}
void r_set_stream(uint32_t stream, uint32_t vb, uint32_t stride) {
    if (stream == 0) { g_stream_vb = vb; g_stream_stride = stride; }
}

/* ---- state blocks ----------------------------------------------------------- */
/* The Flash overlay saves the device state around its drawing (CreateStateBlock
   D3DSBT_ALL, Capture, draw, Apply); text drawn afterwards relies on it (its
   FVF, stage setup). A Direct3D 9 block holds the device side, and the shadow
   state the draws read is copied alongside. */
#define STATE_BLOCKS 64
typedef struct {
    IDirect3DStateBlock9 *sb;
    uint32_t bound[4], texflags[4], stage_ops[4][2], alphakill[4];
    float texmat[4][16];
    uint32_t fvf, vs_handle, ps_handle, stream_vb, stream_stride, atest[3];
} StateBlock;
static StateBlock g_sb[STATE_BLOCKS];

uint32_t r_create_state_block(void) {
    for (uint32_t i = 0; i < STATE_BLOCKS; i++) {
        if (g_sb[i].sb) continue;
        if (!g_dev || FAILED(IDirect3DDevice9_CreateStateBlock(g_dev, D3DSBT_ALL, &g_sb[i].sb))) return 0;
        r_capture_state_block(i + 1); /* the device part is captured by the creation */
        return i + 1;
    }
    fprintf(stderr, "render: out of state blocks\n");
    return 0;
}
void r_capture_state_block(uint32_t token) {
    if (token - 1 >= STATE_BLOCKS || !g_sb[token - 1].sb) return;
    StateBlock *b = &g_sb[token - 1];
    IDirect3DStateBlock9_Capture(b->sb);
    memcpy(b->bound, g_bound, sizeof b->bound); memcpy(b->texflags, g_texflags, sizeof b->texflags);
    memcpy(b->stage_ops, g_stage_ops, sizeof b->stage_ops); memcpy(b->texmat, g_texmat, sizeof b->texmat);
    memcpy(b->alphakill, g_alphakill, sizeof b->alphakill); memcpy(b->atest, &g_atest, sizeof b->atest);
    b->fvf = g_fvf; b->vs_handle = g_vs_handle; b->ps_handle = g_ps_handle;
    b->stream_vb = g_stream_vb; b->stream_stride = g_stream_stride;
}
void r_apply_state_block(uint32_t token) {
    if (token - 1 >= STATE_BLOCKS || !g_sb[token - 1].sb) return;
    StateBlock *b = &g_sb[token - 1];
    IDirect3DStateBlock9_Apply(b->sb);
    memcpy(g_bound, b->bound, sizeof g_bound); memcpy(g_texflags, b->texflags, sizeof g_texflags);
    memcpy(g_stage_ops, b->stage_ops, sizeof g_stage_ops); memcpy(g_texmat, b->texmat, sizeof g_texmat);
    memcpy(g_alphakill, b->alphakill, sizeof g_alphakill); memcpy(&g_atest, b->atest, sizeof g_atest);
    g_fvf = b->fvf; g_vs_handle = b->vs_handle; g_ps_handle = b->ps_handle;
    g_stream_vb = b->stream_vb; g_stream_stride = b->stream_stride;
    g_vsh = NULL;
    if (g_vs_handle & 1)
        for (int i = 0; i < VSHADERS; i++) if (g_vshaders[i].handle == g_vs_handle) g_vsh = &g_vshaders[i];
    g_psh = find_pshader(g_ps_handle);
    g_stage_dirty = 1; /* rebind: a bound texture may have been re-uploaded since */
}
void r_delete_state_block(uint32_t token) {
    if (token - 1 >= STATE_BLOCKS || !g_sb[token - 1].sb) return;
    IDirect3DStateBlock9_Release(g_sb[token - 1].sb);
    memset(&g_sb[token - 1], 0, sizeof g_sb[0]);
}

static D3DPRIMITIVETYPE prim_pc(uint32_t p, uint32_t n, uint32_t *count) {
    switch (p) {
    case 1: *count = n; return D3DPT_POINTLIST;
    case 2: *count = n / 2; return D3DPT_LINELIST;
    case 3: case 4: *count = n > 1 ? n - 1 : 0; return D3DPT_LINESTRIP; /* line loop: closing segment dropped */
    case 5: *count = n / 3; return D3DPT_TRIANGLELIST;
    case 6: case 9: *count = n > 2 ? n - 2 : 0; return D3DPT_TRIANGLESTRIP; /* quad strip = triangle strip */
    case 7: case 10: *count = n > 2 ? n - 2 : 0; return D3DPT_TRIANGLEFAN;  /* polygon = fan */
    default: *count = 0; return D3DPT_TRIANGLELIST;
    }
}

static void drawlog(uint32_t prim, uint32_t n, const void *data, uint32_t stride) {
    if (g_drawlog < 0) g_drawlog = getenv("RECOMP_DRAWLOG") ? atoi(getenv("RECOMP_DRAWLOG")) : 0;
    if (g_drawlog <= 0) return;
    g_drawlog--;
    const float *v = data;
    fprintf(stderr, "draw: prim %u n %u fvf %08x stride %u ps %08x tex %08x v0 %.2f %.2f %.2f %.2f %08x\n", prim, n, g_fvf, stride, g_ps_handle,
            g_bound[0], v[0], v[1], v[2], v[3], ((const uint32_t *)data)[4]);
    TexSlot *t = g_bound[0] ? tex_find(g_bound[0], 0) : NULL;
    if (t) fprintf(stderr, "      tex %ux%u fmt %02x linear %d hdr %08x/%08x colorop %u alphaop %u uv0 %.3f %.3f\n", t->w, t->h, t->fmt, t->linear,
                   RD32(MEM(t->guest + 0xC)), RD32(MEM(t->guest + 0x10)), g_stage_ops[0][0], g_stage_ops[0][1], v[(stride - 8) / 4], v[(stride - 4) / 4]);
}

/* RECOMP_STATELOG=frame: the device state behind every draw of that frame */
static void statelog(uint32_t prim, uint32_t n, const void *data, uint32_t stride) {
    static int frame = -1;
    if (frame < 0) frame = getenv("RECOMP_STATELOG") ? atoi(getenv("RECOMP_STATELOG")) : 0;
    if (!frame || g_frame + 1 != (uint32_t)frame) return;
    DWORD v[16];
    static const D3DRENDERSTATETYPE rsl[] = {D3DRS_LIGHTING, D3DRS_FOGENABLE, D3DRS_FOGCOLOR, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND,
                                             D3DRS_DESTBLEND, D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_ALPHATESTENABLE,
                                             D3DRS_ZENABLE, D3DRS_CULLMODE, D3DRS_COLORVERTEX, D3DRS_TEXTUREFACTOR};
    for (int i = 0; i < 13; i++) IDirect3DDevice9_GetRenderState(g_dev, rsl[i], &v[i]);
    DWORD cop, ca1, ca2, aop, aa1, aa2;
    IDirect3DDevice9_GetTextureStageState(g_dev, 0, D3DTSS_COLOROP, &cop); IDirect3DDevice9_GetTextureStageState(g_dev, 0, D3DTSS_COLORARG1, &ca1);
    IDirect3DDevice9_GetTextureStageState(g_dev, 0, D3DTSS_COLORARG2, &ca2); IDirect3DDevice9_GetTextureStageState(g_dev, 0, D3DTSS_ALPHAOP, &aop);
    IDirect3DDevice9_GetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG1, &aa1); IDirect3DDevice9_GetTextureStageState(g_dev, 0, D3DTSS_ALPHAARG2, &aa2);
    if (g_bound[0])
        fprintf(stderr, "       tex0 data %08x format %08x size %08x, verts", RD32(MEM(g_bound[0] + 4)), RD32(MEM(g_bound[0] + 0xC)), RD32(MEM(g_bound[0] + 0x10)));
    for (uint32_t i = 0; g_bound[0] && i < n && i < 8; i++) {
        const float *q = (const float *)((const uint8_t *)data + i * stride);
        fprintf(stderr, " (");
        for (uint32_t k = 0; k < stride / 4 && k < 8; k++) fprintf(stderr, k ? " %g" : "%g", q[k]);
        fprintf(stderr, ")");
    }
    if (g_bound[0]) fprintf(stderr, "\n");
    /* where the first vertex lands: world * view * projection, then the viewport */
    D3DMATRIX m[3];
    D3DVIEWPORT9 vp;
    IDirect3DDevice9_GetTransform(g_dev, D3DTS_WORLD, &m[0]); IDirect3DDevice9_GetTransform(g_dev, D3DTS_VIEW, &m[1]);
    IDirect3DDevice9_GetTransform(g_dev, D3DTS_PROJECTION, &m[2]); IDirect3DDevice9_GetViewport(g_dev, &vp);
    const float *f = data;
    float p[4] = {f[0], f[1], f[2], 1};
    for (int k = 0; k < 3; k++) {
        float q[4];
        for (int c = 0; c < 4; c++) q[c] = p[0] * m[k].m[0][c] + p[1] * m[k].m[1][c] + p[2] * m[k].m[2][c] + p[3] * m[k].m[3][c];
        memcpy(p, q, sizeof p);
    }
    float sx = vp.X + (p[0] / p[3] + 1) * vp.Width / 2, sy = vp.Y + (1 - p[1] / p[3]) * vp.Height / 2;
    fprintf(stderr, "state: prim %u n %u fvf %03x stride %u tex %08x color0 %08x | light %lu fog %lu/%08lx blend %lu %lu/%lu write %lx stencil %lu atest %lu z %lu cull %lu colorvertex %lu tfactor %08lx | "
            "stage0 c %lu(%lu,%lu) a %lu(%lu,%lu) | v0 (%.1f %.1f %.1f) -> screen %.1f %.1f (world scale %.4f)\n", prim, n, g_fvf, stride, g_bound[0],
            (g_fvf & D3DFVF_DIFFUSE) ? ((const uint32_t *)data)[3] : 0,
            v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], cop, ca1, ca2, aop, aa1, aa2,
            f[0], f[1], f[2], sx, sy, m[0].m[0][0]);
    if (!(p[3] > 0) || sx != sx)
        for (int k = 0; k < 3; k++)
            fprintf(stderr, "       %s [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]\n", k == 0 ? "world" : k == 1 ? "view " : "proj ",
                    m[k].m[0][0], m[k].m[0][1], m[k].m[0][2], m[k].m[0][3], m[k].m[1][0], m[k].m[1][1], m[k].m[1][2], m[k].m[1][3],
                    m[k].m[2][0], m[k].m[2][1], m[k].m[2][2], m[k].m[2][3], m[k].m[3][0], m[k].m[3][1], m[k].m[3][2], m[k].m[3][3]);
}

static void draw_up(uint32_t prim, uint32_t n, const void *data, uint32_t stride) {
    if (g_dev && g_vsh && n && stride) { draw_vs(prim, n, data, stride, NULL); return; }
    if (!g_dev || !g_fvf || !n || !stride) return;
    drawlog(prim, n, data, stride);
    statelog(prim, n, data, stride);
    apply_textures();
    apply_pixel_shader();
    if (prim == 8) { /* quad list: two triangles per quad through an index list */
        uint32_t quads = n / 4;
        static uint16_t *idx; static uint32_t idx_cap;
        if (quads * 6 > idx_cap) { idx_cap = quads * 6; idx = realloc(idx, idx_cap * 2); }
        for (uint32_t q = 0; q < quads; q++) {
            uint16_t b = (uint16_t)(q * 4);
            idx[q * 6] = b; idx[q * 6 + 1] = b + 1; idx[q * 6 + 2] = b + 2;
            idx[q * 6 + 3] = b; idx[q * 6 + 4] = b + 2; idx[q * 6 + 5] = b + 3;
        }
        IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 0, quads * 4, quads * 2, idx, D3DFMT_INDEX16, data, stride);
        probe();
        return;
    }
    uint32_t count;
    D3DPRIMITIVETYPE t = prim_pc(prim, n, &count);
    if (count) IDirect3DDevice9_DrawPrimitiveUP(g_dev, t, count, data, stride);
    probe();
}

void r_draw(uint32_t prim, uint32_t start, uint32_t n) {
    if (!g_stream_vb) return;
    uint32_t stride = g_stream_stride ? g_stream_stride : rc_fvf_stride(g_fvf);
    uint32_t data = RD32(MEM(g_stream_vb + 4));
    if (!data) return;
    draw_up(prim, n, MEM(data + start * stride), stride);
}

void r_draw_up(uint32_t prim, uint32_t n, uint32_t data, uint32_t stride) {
    if (!data) return;
    draw_up(prim, n, MEM(data), stride ? stride : rc_fvf_stride(g_fvf));
}

void r_draw_indexed(uint32_t prim, uint32_t n, uint32_t index_data, uint32_t base_vertex) {
    if (!g_dev || (!g_fvf && !g_vsh) || !g_stream_vb || !n) return;
    if (index_data < 0x10000) { /* no index buffer behind it: report once rather than read low memory */
        static int warned;
        if (!warned++) fprintf(stderr, "render: indexed draw with index data %08x skipped\n", index_data);
        return;
    }
    uint32_t stride = g_stream_stride ? g_stream_stride : rc_fvf_stride(g_fvf);
    uint32_t data = RD32(MEM(g_stream_vb + 4));
    if (!data) return;
    data += base_vertex * stride; /* SetIndices' BaseVertexIndex offsets every index */
    const uint16_t *idx = (const uint16_t *)MEM(index_data);
    if (g_vsh) { draw_vs(prim, n, MEM(data), stride, idx); return; }
    uint32_t maxv = 0;
    for (uint32_t i = 0; i < n; i++) if (idx[i] > maxv) maxv = idx[i];
    apply_textures();
    apply_pixel_shader();
    uint32_t count;
    D3DPRIMITIVETYPE t = prim_pc(prim, n, &count);
    if (prim == 8) { /* quads: expand to triangles */
        static uint16_t *tri; static uint32_t cap;
        uint32_t quads = n / 4;
        if (quads * 6 > cap) { cap = quads * 6; tri = realloc(tri, cap * 2); }
        for (uint32_t q = 0; q < quads; q++) {
            const uint16_t *s = idx + q * 4;
            tri[q * 6] = s[0]; tri[q * 6 + 1] = s[1]; tri[q * 6 + 2] = s[2];
            tri[q * 6 + 3] = s[0]; tri[q * 6 + 4] = s[2]; tri[q * 6 + 5] = s[3];
        }
        IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, D3DPT_TRIANGLELIST, 0, maxv + 1, quads * 2, tri, D3DFMT_INDEX16, MEM(data), stride);
        probe();
        return;
    }
    if (count) IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, t, 0, maxv + 1, count, idx, D3DFMT_INDEX16, MEM(data), stride);
    probe();
}

/* ---- frame ------------------------------------------------------------------ */
void r_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!g_dev) return;
    DWORD f = 0;
    if (flags & 0xF0) f |= D3DCLEAR_TARGET;
    if (flags & 0x01) f |= D3DCLEAR_ZBUFFER;
    if (flags & 0x02) f |= D3DCLEAR_STENCIL;
    if (f) IDirect3DDevice9_Clear(g_dev, 0, NULL, f, color, z, stencil);
}

/* RECOMP_SHOT="frame,frame,...": save the back buffer as build\shot_<frame>.bmp
   before those presents; works whether or not the window is on screen */
static void shot(uint32_t frame) {
    char name[64];
    if (!rc_shot_wanted(frame, name, sizeof name)) return;
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_dev, bb, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        if (rc_write_bmp(name, lr.pBits, lr.Pitch, d.Width, d.Height)) fprintf(stderr, "render: saved %s\n", name);
        IDirect3DSurface9_UnlockRect(sys);
    }
    if (sys) IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(bb);
}

void r_present(void) {
    if (!g_dev) return;
    IDirect3DDevice9_EndScene(g_dev);
    shot(++g_frame);
    HRESULT hr = IDirect3DDevice9_Present(g_dev, NULL, NULL, NULL, NULL);
    if (hr == D3DERR_DEVICELOST && !g_lost) { fprintf(stderr, "render: device lost\n"); g_lost = 1; }
    pump();
    IDirect3DDevice9_BeginScene(g_dev);
}
