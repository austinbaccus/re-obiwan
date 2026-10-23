/* Direct3D 11 implementation of render.h, the default renderer
   (RECOMP_RENDERER=d3d9 selects render_d3d9.c, which it was ported from and
   which stays as the reference to compare against).

   Direct3D 11 has no fixed-function pipeline, so everything the Xbox D3D8
   API sets becomes shaders and state objects here:
   - the game's vertex and pixel shaders are its source text translated to
     SM4 HLSL (shader_xlate.c, XS_SM4); a pixel shader is compiled once per
     set of stages that sample a shadow buffer (those compare: SampleCmp),
   - draws without them (FVF vertices, texture stage states) get a vertex
     and a pixel shader generated for that state, computing what Direct3D 9's
     fixed-function pipeline did, and cached by it,
   - fog, the alpha test and ALPHAKILL happen in the pixel shaders,
   - blend, depth-stencil, rasterizer and sampler state objects come from
     the device state kept here (struct St, which is also what a state block
     holds) before each draw.
   Vertex and index data are copied from guest memory into dynamic buffers
   per draw, as DrawPrimitiveUP did for Direct3D 9; quads and fans become
   triangle lists. The game draws into a back buffer texture of its own that
   Present copies into a flip-model swap chain. Pixel centres follow
   Direct3D 9 (half a pixel off Direct3D 11's), as do the rules for
   pretransformed vertices, so the frames match the reference renderer. */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <d3d9types.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RENDER_BACKEND d3d11
#include "render_backend.h"
#include "guest.h"
#include "kernel.h"
#include "render.h"
#include "render_common.h"
#include "shader_xlate.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

#define REL(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static HWND g_hwnd;
static ID3D11Device *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain1 *g_swap;
static ID3D11Texture2D *g_swap_buf;
static ID3D11InfoQueue *g_debug;     /* RECOMP_D3DDEBUG: the debug layer's messages */
static uint32_t g_width, g_height;   /* the back buffer the game sees (640x480) */
/* RECOMP_RES: the host back buffer is g_out_w x g_out_h and back-buffer
   viewports are scaled to it; the game still draws for g_width x g_height.
   The swap chain is g_win_w x g_win_h (the window, or the whole monitor
   with RECOMP_FULLSCREEN) and shows the back buffer centred in it. */
static uint32_t g_out_w, g_out_h, g_win_w, g_win_h;
static float g_sx = 1, g_sy = 1;
static ID3D11RenderTargetView *g_swap_rtv;
static UINT g_present_interval, g_present_flags;
static int g_drawlog = -1;           /* RECOMP_DRAWLOG=N: describe the first N draws and transforms */
static uint32_t g_frame;             /* presents so far */

/* ---- device state ------------------------------------------------------------ */
/* All of it in one place: a state block (D3DSBT_ALL) is a copy. Values are
   PC D3D9 enums, which Direct3D 11 shares for compare functions, blend
   factors and operations, stencil operations and texture address modes. */
typedef struct {
    uint32_t colorop, carg[3], alphaop, aarg[3], result;  /* carg/aarg: ARG0, ARG1, ARG2 */
    uint32_t texflags, tci, alphakill;
    uint32_t addr[3], mag, min, border, maxaniso;
    float bumpenv[4], lscale, loffset;                     /* MAT00, MAT01, MAT10, MAT11 */
} Stage;
typedef struct {
    uint32_t blend, src, dst, blendop, colorwrite, blendfactor;
    uint32_t zenable, zwrite, zfunc;
    uint32_t stencil, sfunc, sref, smask, swrite, sfail, szfail, spass;
    uint32_t cull, shade, polyfill;
    float polyslope, polyunits;
    uint32_t atest, afunc, aref;
    uint32_t fogcolor, tfactor;
    uint32_t fog, fogmode, fogrange;
    float fogstart, fogend, fogdensity;
    uint32_t lighting, specular, localviewer, colorvertex, ambient;
    uint32_t matsrc[4];                                    /* diffuse, ambient, specular, emissive */
    Stage ts[4];
    uint32_t bound[4];                                     /* guest texture objects per stage */
    float texmat[4][16], world[16], view[16], proj[16];
    float vp[6];                                           /* X, Y, Width, Height, MinZ, MaxZ */
    uint32_t fvf, vs_handle, ps_handle, stream_vb, stream_stride;
} St;
static St g_st;

static void default_state(void) {
    static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    St *s = &g_st;
    memset(s, 0, sizeof *s);
    s->src = D3DBLEND_ONE; s->dst = D3DBLEND_ZERO; s->blendop = D3DBLENDOP_ADD; s->colorwrite = 0xF;
    s->zenable = 1; s->zwrite = 1; s->zfunc = D3DCMP_LESSEQUAL;
    s->sfunc = D3DCMP_ALWAYS; s->smask = s->swrite = 0xFF; s->sfail = s->szfail = s->spass = D3DSTENCILOP_KEEP;
    s->cull = D3DCULL_NONE; s->shade = D3DSHADE_GOURAUD;
    s->afunc = D3DCMP_ALWAYS; s->tfactor = 0xFFFFFFFF;
    s->localviewer = 1; s->colorvertex = 1; s->matsrc[0] = D3DMCS_COLOR1; s->matsrc[2] = D3DMCS_COLOR2;
    for (int i = 0; i < 4; i++) {
        Stage *t = &s->ts[i];
        t->colorop = i ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        t->alphaop = i ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        t->carg[0] = t->aarg[0] = D3DTA_CURRENT;
        t->carg[1] = t->aarg[1] = D3DTA_TEXTURE;
        t->carg[2] = t->aarg[2] = i ? D3DTA_CURRENT : D3DTA_DIFFUSE;
        t->result = D3DTA_CURRENT;
        t->tci = i;
        t->addr[0] = t->addr[1] = t->addr[2] = D3DTADDRESS_WRAP;
        t->mag = t->min = D3DTEXF_LINEAR;
        t->maxaniso = 1;
        memcpy(s->texmat[i], identity, 64);
    }
    memcpy(s->world, identity, 64); memcpy(s->view, identity, 64); memcpy(s->proj, identity, 64);
    s->vp[2] = (float)g_width; s->vp[3] = (float)g_height; s->vp[5] = 1;
}

/* ---- window ------------------------------------------------------------------ */
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

/* the debug layer's messages since the last call */
static void debug_messages(void) {
    if (!g_debug) return;
    UINT64 n = ID3D11InfoQueue_GetNumStoredMessages(g_debug);
    for (UINT64 i = 0; i < n && i < 32; i++) {
        SIZE_T len = 0;
        ID3D11InfoQueue_GetMessage(g_debug, i, NULL, &len);
        D3D11_MESSAGE *m = malloc(len);
        if (m && SUCCEEDED(ID3D11InfoQueue_GetMessage(g_debug, i, m, &len))) fprintf(stderr, "d3d11: %.*s\n", (int)m->DescriptionByteLength, m->pDescription);
        free(m);
    }
    ID3D11InfoQueue_ClearStoredMessages(g_debug);
}

int r_ready(void) { return g_dev != NULL; }
int r_has_focus(void) { return g_hwnd && GetForegroundWindow() == g_hwnd; }

/* ---- render targets ---------------------------------------------------------- */
/* The game renders shadow buffers: a throwaway colour surface (a header over
   address 0) with a depth texture (LIN_F16) as its depth buffer, then
   samples that texture through the NV2A's shadow compare. Off the back
   buffer, draws go to a host colour target, and a guest depth texture is a
   host D24S8 depth texture (keyed by the guest texture's memory) sampled
   with a comparison sampler. */
typedef struct { uint32_t data, w, h; ID3D11Texture2D *tex; ID3D11DepthStencilView *dsv; ID3D11ShaderResourceView *srv; } DepthTex;
static DepthTex g_depthtex[8];
static ID3D11Texture2D *g_bb, *g_bb_z, *g_off, *g_off_z;
static ID3D11RenderTargetView *g_bb_rtv, *g_off_rtv, *g_rtv;
static ID3D11DepthStencilView *g_bb_dsv, *g_off_dsv, *g_dsv;
static uint32_t g_off_w, g_off_h, g_offz_w, g_offz_h;
static ID3D11Resource *g_dsv_res; /* what g_dsv draws into, so it is not also sampled */

static ID3D11Texture2D *texture_2d(uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, D3D11_USAGE usage, UINT cpu) {
    D3D11_TEXTURE2D_DESC d = {w, h, 1, 1, fmt, {1, 0}, usage, bind, cpu, 0};
    ID3D11Texture2D *t = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &d, NULL, &t))) return NULL;
    return t;
}

static int depth_buffer(uint32_t w, uint32_t h, ID3D11Texture2D **tex, ID3D11DepthStencilView **dsv) {
    *tex = texture_2d(w, h, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_BIND_DEPTH_STENCIL, D3D11_USAGE_DEFAULT, 0);
    return *tex && SUCCEEDED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)*tex, NULL, dsv));
}

static DepthTex *depth_texture(uint32_t data, uint32_t w, uint32_t h, int create) {
    for (int i = 0; i < 8; i++) if (g_depthtex[i].data == data && g_depthtex[i].tex) return &g_depthtex[i];
    if (!create || !data) return NULL;
    for (int i = 0; i < 8; i++) {
        DepthTex *d = &g_depthtex[i];
        if (d->tex) continue;
        d->tex = texture_2d(w, h, DXGI_FORMAT_R24G8_TYPELESS, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        D3D11_DEPTH_STENCIL_VIEW_DESC dv = {DXGI_FORMAT_D24_UNORM_S8_UINT, D3D11_DSV_DIMENSION_TEXTURE2D};
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {DXGI_FORMAT_R24_UNORM_X8_TYPELESS, D3D11_SRV_DIMENSION_TEXTURE2D};
        sv.Texture2D.MipLevels = 1;
        if (!d->tex || FAILED(ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)d->tex, &dv, &d->dsv)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)d->tex, &sv, &d->srv))) {
            fprintf(stderr, "render: cannot create a %ux%u depth texture\n", w, h);
            REL(d->dsv); REL(d->srv); REL(d->tex);
            return NULL;
        }
        d->data = data; d->w = w; d->h = h;
        return d;
    }
    return NULL;
}

/* Direct3D 9 resets the viewport to the whole new target */
static void full_viewport(uint32_t w, uint32_t h) {
    float vp[6] = {0, 0, (float)w, (float)h, 0, 1};
    memcpy(g_st.vp, vp, sizeof vp);
}

/* z_kind: 0 no depth buffer, 1 the main one, 2 the depth texture at z_data */
void r_set_render_target(int back_buffer, uint32_t w, uint32_t h, int z_kind, uint32_t z_data, uint32_t z_w, uint32_t z_h) {
    if (!g_dev) return;
    DepthTex *d = z_kind == 2 ? depth_texture(z_data, z_w, z_h, 1) : NULL;
    /* Direct3D 11 wants colour and depth views of one size; the colour
       surface of a shadow pass is a throwaway, so it takes the depth's */
    if (d && !back_buffer && (w != d->w || h != d->h)) { w = d->w; h = d->h; }
    if (back_buffer) { g_rtv = g_bb_rtv; w = g_width; h = g_height; }
    else {
        if (!g_off_rtv || g_off_w != w || g_off_h != h) {
            REL(g_off_rtv); REL(g_off);
            g_off = texture_2d(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
            if (!g_off || FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_off, NULL, &g_off_rtv))) {
                fprintf(stderr, "render: cannot create a %ux%u render target\n", w, h);
                REL(g_off);
                return;
            }
            g_off_w = w; g_off_h = h;
        }
        g_rtv = g_off_rtv;
    }
    g_dsv = NULL; g_dsv_res = NULL;
    if (z_kind == 1) {
        if (back_buffer || (w == g_out_w && h == g_out_h)) { g_dsv = g_bb_dsv; g_dsv_res = (ID3D11Resource *)g_bb_z; }
        else { /* an off-screen target of another size: a depth buffer of its own */
            if (!g_off_dsv || g_offz_w != w || g_offz_h != h) {
                REL(g_off_dsv); REL(g_off_z);
                if (!depth_buffer(w, h, &g_off_z, &g_off_dsv)) { REL(g_off_z); REL(g_off_dsv); }
                g_offz_w = w; g_offz_h = h;
                static int warned;
                if (!warned++) fprintf(stderr, "render: %ux%u target with the main depth buffer gets a depth buffer of its own\n", w, h);
            }
            g_dsv = g_off_dsv; g_dsv_res = (ID3D11Resource *)g_off_z;
        }
    } else if (d) { g_dsv = d->dsv; g_dsv_res = (ID3D11Resource *)d->tex; }
    static ID3D11ShaderResourceView *const none[4];
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 4, none); /* a depth texture sampled before may be drawn into now */
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);
    full_viewport(w, h);
}

/* ---- textures ---------------------------------------------------------------- */
#define TEX_SLOTS 1024
typedef struct {
    uint32_t guest;              /* guest object address, 0 = free */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t w, h, fmt;
    int dirty, linear, compressed;
} TexSlot;
static TexSlot g_tex[TEX_SLOTS];
static ID3D11ShaderResourceView *g_black; /* unbound stages: shaders read (0, 0, 0, 1) as on Direct3D 9 */

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
    if (t) { REL(t->srv); REL(t->tex); memset(t, 0, sizeof *t); }
    for (int s = 0; s < 4; s++) if (g_st.bound[s] == guest) g_st.bound[s] = 0;
}

/* DXT stays compressed (BC1..3) where Direct3D 11 takes it: sizes in whole
   blocks; anything else becomes B8G8R8A8 */
static void upload(TexSlot *t) {
    RcTex g;
    if (!rc_texture(t->guest, &g)) return;
    int compressed = g.dxt && g.w % 4 == 0 && g.h % 4 == 0;
    DXGI_FORMAT fmt = !compressed ? DXGI_FORMAT_B8G8R8A8_UNORM : g.fmt == 0x0C ? DXGI_FORMAT_BC1_UNORM : g.fmt == 0x0E ? DXGI_FORMAT_BC2_UNORM : DXGI_FORMAT_BC3_UNORM;
    if (t->tex && (t->w != g.w || t->h != g.h || t->fmt != g.fmt)) { REL(t->srv); REL(t->tex); }
    if (!t->tex) {
        t->tex = texture_2d(g.w, g.h, fmt, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);
        if (!t->tex || FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)t->tex, NULL, &t->srv))) {
            fprintf(stderr, "render: cannot create %ux%u texture (xbox format %02x)\n", g.w, g.h, g.fmt);
            REL(t->tex); t->dirty = 0;
            return;
        }
        t->w = g.w; t->h = g.h; t->fmt = g.fmt;
    }
    t->linear = g.linear;
    t->compressed = compressed;
    if (compressed) ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)t->tex, 0, NULL, g.src, g.pitch, 0);
    else {
        uint32_t *argb = malloc((size_t)g.w * g.h * 4);
        rc_texture_argb(&g, argb);
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)t->tex, 0, NULL, argb, g.w * 4, 0);
        free(argb);
    }
    rc_dump_texture(&g);
    t->dirty = 0;
}

void r_set_texture(uint32_t stage, uint32_t guest) {
    if (stage < 4) g_st.bound[stage] = guest;
}

/* what a draw samples at each stage */
enum { TK_NONE, TK_2D, TK_CUBE, TK_DEPTH };
typedef struct {
    ID3D11ShaderResourceView *srv[4];
    uint8_t kind[4];
    float scale[4][3];  /* to normalised coordinates: 1/w, 1/h for linear textures; depth units to 0..1 */
} Bound;

static void resolve_textures(Bound *b) {
    for (int s = 0; s < 4; s++) {
        uint32_t g = g_st.bound[s];
        b->srv[s] = g_black; b->kind[s] = TK_NONE;
        b->scale[s][0] = b->scale[s][1] = b->scale[s][2] = 1;
        if (!g) continue;
        DepthTex *d = depth_texture(RD32(MEM(g + 4)), 0, 0, 0);
        if (d) { /* a shadow buffer we rendered: linear, addressed in texels */
            if ((ID3D11Resource *)d->tex == g_dsv_res) continue; /* being drawn into */
            b->srv[s] = d->srv; b->kind[s] = TK_DEPTH;
            b->scale[s][0] = 1.0f / d->w; b->scale[s][1] = 1.0f / d->h; b->scale[s][2] = rc_depth_unit_scale(g);
            continue;
        }
        TexSlot *t = tex_find(g, 1);
        if (!t) continue;
        if (t->dirty) upload(t);
        if (!t->srv) continue;
        b->srv[s] = t->srv; b->kind[s] = TK_2D;
        if (t->linear) { b->scale[s][0] = 1.0f / t->w; b->scale[s][1] = 1.0f / t->h; }
    }
}

/* ---- render states ------------------------------------------------------------ */
/* alpha test, as the game set it, and ALPHAKILL per stage (stage state 21:
   the NV2A drops a pixel whose texel alpha is 0) both happen in the pixel
   shaders */
void r_set_rs_simple(uint32_t method, uint32_t value) {
    /* the caller passes the push-buffer header (dword count in bits 18+,
       subchannel in 13..15), e.g. 0x40304 for ALPHABLENDENABLE */
    float f;
    memcpy(&f, &value, 4);
    St *s = &g_st;
    switch (method & 0x1FFC) {
    /* polygon offset (SOLIDOFFSETENABLE, POLYGONOFFSETZSLOPESCALE/ZOFFSET): the
       shadow buffer passes rely on it against self-shadowing acne */
    case 0x338: s->polyfill = value != 0; goto offset_log;
    case 0x384: s->polyslope = f; goto offset_log;
    case 0x388: s->polyunits = f;
    offset_log:
        if (getenv("RECOMP_RSLOG")) fprintf(stderr, "polygon offset: fill %u slope %g units %g\n", s->polyfill, s->polyslope, s->polyunits);
        break;
    case 0x354: s->zfunc = rc_gl_cmp(value); break;
    case 0x33C: s->afunc = rc_gl_cmp(value); break;
    case 0x304: s->blend = value != 0; break;
    case 0x300: s->atest = value != 0; break;
    case 0x340: s->aref = value & 0xff; break;
    case 0x344: s->src = rc_gl_blend(value); break;
    case 0x348: s->dst = rc_gl_blend(value); break;
    case 0x35C: s->zwrite = value != 0; break;
    case 0x37C: s->shade = value == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD; break;
    case 0x358: /* NV2A colour mask: B bit 0, G bit 8, R bit 16, A bit 24 */
        s->colorwrite = ((value & 1) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | ((value & 0x100) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                        ((value & 0x10000) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) | ((value & 0x1000000) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0);
        break;
    case 0x374: s->szfail = rc_gl_stencil_op(value); break;
    case 0x378: s->spass = rc_gl_stencil_op(value); break;
    case 0x364: s->sfunc = rc_gl_cmp(value); break;
    case 0x368: s->sref = value & 0xff; break;
    case 0x36C: s->smask = value & 0xff; break;
    case 0x360: s->swrite = value & 0xff; break;
    case 0x350: s->blendop = rc_gl_blendop(value); break;
    case 0x34C: s->blendfactor = value; break; /* ARGB */
    default: break; /* dither, swath, depth clip, stipple: nothing to map */
    }
}

static float as_float(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

/* Fog: the NV2A turns a per-vertex fog coordinate (eye distance, or oFog from
   a vertex shader) into a factor with the FOGTABLEMODE function, and the
   final combiner blends with the fog colour. Here the vertex shaders compute
   the factor and the pixel shaders blend (the game's through their final
   combiner, which may leave fog out). */
void r_set_rs_deferred(uint32_t state, uint32_t value) {
    St *s = &g_st;
    switch (state) {
    case XRS_FOGENABLE: s->fog = value != 0; break;
    case XRS_FOGTABLEMODE: s->fogmode = value <= 3 ? value : 0; break;   /* D3DFOG_NONE, EXP, EXP2, LINEAR */
    case XRS_FOGSTART: s->fogstart = as_float(value); break;
    case XRS_FOGEND: s->fogend = as_float(value); break;
    case XRS_FOGDENSITY: s->fogdensity = as_float(value); break;
    case XRS_RANGEFOGENABLE: s->fogrange = value != 0; break;
    case XRS_LIGHTING: s->lighting = value != 0; break;
    case XRS_SPECULARENABLE: s->specular = value != 0; break;
    case XRS_LOCALVIEWER: s->localviewer = value != 0; break;
    case XRS_COLORVERTEX: s->colorvertex = value != 0; break;
    case XRS_DIFFUSEMATERIALSOURCE: s->matsrc[0] = value; break;
    case XRS_AMBIENTMATERIALSOURCE: s->matsrc[1] = value; break;
    case XRS_SPECULARMATERIALSOURCE: s->matsrc[2] = value; break;
    case XRS_EMISSIVEMATERIALSOURCE: s->matsrc[3] = value; break;
    case XRS_AMBIENT: s->ambient = value; break;
    /* WRAP0..3 (cylindrical wrapping) and the point sprite states: not
       emulated; points are drawn one pixel in size */
    default: break;
    }
}

/* The deferred states live in D3D_g_RenderState: the hooked setter stores
   there like the original, and the game's generic SetRenderState and inline
   code write there directly. Apply whatever changed before a draw. */
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

void r_set_rs(uint32_t pc_state, uint32_t value) {
    St *s = &g_st;
    switch (pc_state) {
    case D3DRS_CULLMODE: s->cull = rc_xbox_cull(value); break;
    case D3DRS_STENCILFAIL: s->sfail = rc_gl_stencil_op(value); break;
    case D3DRS_ZENABLE: s->zenable = value != 0; break;
    case D3DRS_FOGCOLOR: s->fogcolor = value; break;
    case D3DRS_STENCILENABLE: s->stencil = value != 0; break;
    case D3DRS_TEXTUREFACTOR: s->tfactor = value; break;
    default: break;
    }
}

/* Xbox D3DTSS indices of D3D8 3948 (XDK 3911): the stage operations come
   first (COLOROP 0 .. TEXTURETRANSFORMFLAGS 9), then the sampler states
   (ADDRESSU 10 .. ALPHAKILL 21); deferred 0..21, then the rest. Later XDKs
   moved the sampler states to the front. */
void r_set_tss(uint32_t stage, uint32_t type, uint32_t value) {
    if (stage >= 4) return;
    if (g_drawlog > 0) fprintf(stderr, "tss stage %u type %u = %08x\n", stage, type, value);
    Stage *t = &g_st.ts[stage];
    switch (type) {
    case 0: t->colorop = rc_xbox_top(value); break;
    case 1: case 2: case 3: t->carg[type - 1] = value; break;
    case 4: t->alphaop = rc_xbox_top(value); break;
    case 5: case 6: case 7: t->aarg[type - 5] = value; break;
    case 8: t->result = value; break;
    case 9: t->texflags = value; break;
    case 10: case 11: case 12: t->addr[type - 10] = value > 4 ? D3DTADDRESS_CLAMP : value ? value : D3DTADDRESS_WRAP; break;
    case 13: t->mag = value >= 3 ? D3DTEXF_LINEAR : value; break;
    case 14: t->min = value >= 3 ? D3DTEXF_LINEAR : value; break;
    /* 15 MIPFILTER, 16 MIPMAPLODBIAS, 17 MAXMIPLEVEL: one level is uploaded */
    case 18: t->maxaniso = value ? value : 1; break;
    /* 19 COLORKEYOP, 20 COLORSIGN: not emulated */
    case 21:
        if (getenv("RECOMP_RSLOG") && t->alphakill != value) fprintf(stderr, "alphakill: stage %u = %u (frame %u)\n", stage, value, g_frame);
        t->alphakill = value;
        break;
    case 22: t->bumpenv[0] = as_float(value); break;
    case 23: t->bumpenv[1] = as_float(value); break;
    case 24: t->bumpenv[3] = as_float(value); break;
    case 25: t->bumpenv[2] = as_float(value); break;
    case 26: t->lscale = as_float(value); break;
    case 27: t->loffset = as_float(value); break;
    case 28: t->tci = value; break;
    case 29: t->border = value; break;
    default: break;
    }
}

void r_set_transform(uint32_t state, const float *m) {
    /* Xbox D3DTRANSFORMSTATETYPE: VIEW 0, PROJECTION 1, TEXTURE0..3 2..5, WORLD..WORLD3 6..9
       (and D3DTS_WORLDMATRIX(n) = 256 + n); vertex blending is not enabled, so
       only the first world matrix counts */
    if (g_drawlog > 0)
        fprintf(stderr, "transform %u: [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f] [%.5f %.5f %.5f %.5f]\n", state,
                m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
    if (state == 0) memcpy(g_st.view, m, 64);
    else if (state == 1) memcpy(g_st.proj, m, 64);
    else if (state >= 2 && state <= 5) memcpy(g_st.texmat[state - 2], m, 64);
    else if (state == 6 || state == 256) memcpy(g_st.world, m, 64);
}

void r_set_viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float minz, float maxz) {
    if (!w || !h) return;
    if (g_drawlog > 0) fprintf(stderr, "viewport %u %u %u %u z %.2f..%.2f\n", x, y, w, h, minz, maxz);
    float vp[6] = {(float)x, (float)y, (float)w, (float)h, minz, maxz};
    memcpy(g_st.vp, vp, sizeof vp);
}

static void argb_to_float(uint32_t c, float *o) {
    o[0] = ((c >> 16) & 255) / 255.0f; o[1] = ((c >> 8) & 255) / 255.0f; o[2] = (c & 255) / 255.0f; o[3] = (c >> 24) / 255.0f;
}

/* blend, depth-stencil, rasterizer and sampler objects for the next draw;
   Direct3D 11 hands back its existing object for a description it has seen */
static void apply_states(const Bound *b) {
    static ID3D11BlendState *bs; static D3D11_BLEND_DESC bd;
    static ID3D11DepthStencilState *ds; static D3D11_DEPTH_STENCIL_DESC dd;
    static ID3D11RasterizerState *rs; static D3D11_RASTERIZER_DESC rd;
    static ID3D11SamplerState *ss[4]; static D3D11_SAMPLER_DESC sd[4];
    St *s = &g_st;

    D3D11_BLEND_DESC nb = {0};
    D3D11_RENDER_TARGET_BLEND_DESC *t = &nb.RenderTarget[0];
    /* without separate alpha factors the alpha channel blends by the alpha
       of the colour factors */
    #define ALPHA_FACTOR(f) ((f) == D3D11_BLEND_SRC_COLOR ? D3D11_BLEND_SRC_ALPHA : (f) == D3D11_BLEND_INV_SRC_COLOR ? D3D11_BLEND_INV_SRC_ALPHA : \
                             (f) == D3D11_BLEND_DEST_COLOR ? D3D11_BLEND_DEST_ALPHA : (f) == D3D11_BLEND_INV_DEST_COLOR ? D3D11_BLEND_INV_DEST_ALPHA : (f))
    t->BlendEnable = s->blend != 0;
    t->SrcBlend = s->src; t->DestBlend = s->dst; t->BlendOp = s->blendop;
    t->SrcBlendAlpha = ALPHA_FACTOR(s->src); t->DestBlendAlpha = ALPHA_FACTOR(s->dst); t->BlendOpAlpha = s->blendop;
    t->RenderTargetWriteMask = (UINT8)s->colorwrite;
    if (!bs || memcmp(&nb, &bd, sizeof nb)) {
        REL(bs);
        bd = nb;
        if (FAILED(ID3D11Device_CreateBlendState(g_dev, &bd, &bs))) fprintf(stderr, "render: blend state %u %u %u refused\n", s->src, s->dst, s->blendop);
    }
    float factor[4];
    argb_to_float(s->blendfactor, factor);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, bs, factor, 0xffffffff);

    D3D11_DEPTH_STENCIL_DESC nd = {0};
    nd.DepthEnable = s->zenable != 0;
    nd.DepthWriteMask = s->zwrite ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    nd.DepthFunc = s->zfunc;
    nd.StencilEnable = s->stencil != 0;
    nd.StencilReadMask = (UINT8)s->smask; nd.StencilWriteMask = (UINT8)s->swrite;
    nd.FrontFace.StencilFailOp = s->sfail; nd.FrontFace.StencilDepthFailOp = s->szfail;
    nd.FrontFace.StencilPassOp = s->spass; nd.FrontFace.StencilFunc = s->sfunc;
    nd.BackFace = nd.FrontFace;
    if (!ds || memcmp(&nd, &dd, sizeof nd)) {
        REL(ds);
        dd = nd;
        if (FAILED(ID3D11Device_CreateDepthStencilState(g_dev, &dd, &ds))) fprintf(stderr, "render: depth-stencil state refused\n");
    }
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, ds, s->sref);

    /* clockwise is front: D3DCULL_CCW culls the back faces; the polygon
       offset is in units of the smallest depth step, as Direct3D 11's */
    D3D11_RASTERIZER_DESC nr = {0};
    nr.FillMode = D3D11_FILL_SOLID;
    nr.CullMode = s->cull == D3DCULL_CW ? D3D11_CULL_FRONT : s->cull == D3DCULL_CCW ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    nr.DepthBias = s->polyfill ? (INT)s->polyunits : 0;
    /* the slope is per pixel: a scaled-up back buffer has flatter slopes */
    int scaled = g_rtv == g_bb_rtv;
    nr.SlopeScaledDepthBias = s->polyfill ? s->polyslope * (scaled ? max(g_sx, g_sy) : 1) : 0;
    nr.DepthClipEnable = TRUE;
    if (!rs || memcmp(&nr, &rd, sizeof nr)) {
        REL(rs);
        rd = nr;
        if (FAILED(ID3D11Device_CreateRasterizerState(g_dev, &rd, &rs))) fprintf(stderr, "render: rasterizer state refused\n");
    }
    ID3D11DeviceContext_RSSetState(g_ctx, rs);

    /* the game's units: the shaders map pretransformed vertices through s->vp */
    float kx = scaled ? g_sx : 1, ky = scaled ? g_sy : 1;
    D3D11_VIEWPORT vp = {s->vp[0] * kx, s->vp[1] * ky, s->vp[2] * kx, s->vp[3] * ky, s->vp[4], s->vp[5]};
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);

    for (int i = 0; i < 4; i++) {
        const Stage *ts = &s->ts[i];
        D3D11_SAMPLER_DESC n = {0};
        int depth = b->kind[i] == TK_DEPTH;
        /* the D3D11_FILTER bits: mag linear 0x4, min linear 0x10, compare 0x80;
           one level, so no mip filter. Shadow buffers compare as Direct3D 9
           drivers do: lit where r/q <= depth (filtered as the stage says) */
        n.Filter = (D3D11_FILTER)((ts->min == D3DTEXF_LINEAR ? 0x10 : 0) | (ts->mag == D3DTEXF_LINEAR ? 0x4 : 0) | (depth ? 0x80 : 0));
        n.AddressU = ts->addr[0]; n.AddressV = ts->addr[1]; n.AddressW = ts->addr[2];
        n.MaxAnisotropy = ts->maxaniso > 16 ? 16 : ts->maxaniso;
        n.ComparisonFunc = depth ? D3D11_COMPARISON_LESS_EQUAL : D3D11_COMPARISON_NEVER;
        argb_to_float(ts->border, n.BorderColor);
        n.MinLOD = 0; n.MaxLOD = 0;
        if (!ss[i] || memcmp(&n, &sd[i], sizeof n)) {
            REL(ss[i]);
            sd[i] = n;
            if (FAILED(ID3D11Device_CreateSamplerState(g_dev, &sd[i], &ss[i]))) fprintf(stderr, "render: sampler state refused\n");
        }
    }
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 4, ss);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 4, b->srv);
}

/* ---- shaders ------------------------------------------------------------------- */
typedef HRESULT(WINAPI *D3DCompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

/* HLSL to bytecode through d3dcompiler_47 (loaded on first use); NULL on failure */
static ID3DBlob *compile_hlsl(const char *hlsl, const char *target, uint32_t handle) {
    static D3DCompileFn compile;
    static int missing;
    if (!compile && !missing) {
        HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
        compile = dll ? (D3DCompileFn)GetProcAddress(dll, "D3DCompile") : NULL;
        if (!compile) { missing = 1; fprintf(stderr, "render: d3dcompiler_47.dll is missing; nothing is drawn\n"); }
    }
    if (!compile) return NULL;
    ID3DBlob *code = NULL, *errors = NULL;
    HRESULT hr = compile(hlsl, strlen(hlsl), target, NULL, NULL, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        fprintf(stderr, "render: %s shader %08x does not compile: %s\n", target, handle, errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
        if (getenv("RECOMP_SHADERLOG")) fprintf(stderr, "%s\n", hlsl);
    }
    if (errors) ID3D10Blob_Release(errors);
    return SUCCEEDED(hr) ? code : NULL;
}

/* constant buffers: the game's vertex shaders, the generated ones, every pixel shader */
typedef struct { float c[96][4], fog[4], pos[4]; } CbVs;
typedef struct { float wvp[16], wv[16], texm[4][16], nrm[16], fog[4], pos[4], vp[4], vpz[4], ambient[4], mat[4][4]; } CbFf;
typedef struct { float c[8][4], texscale[4][4], fogcolor[4], alphatest[4], tfactor[4], misc[4], bumpenv[4][4], bumplum[4][4]; } CbPs;
static ID3D11Buffer *g_cb_vs, *g_cb_ff, *g_cb_ps;

static void upload_cb(ID3D11Buffer *b, const void *data, size_t size) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    memcpy(m.pData, data, size);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)b, 0);
}

/* small text builder for generated HLSL */
typedef struct { char *p; size_t n, cap; } Text;
static void put(Text *t, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(t->p + t->n, t->cap - t->n, fmt, ap);
    va_end(ap);
    if (k > 0) t->n = t->n + k < t->cap ? t->n + k : t->cap - 1;
}

/* as shader_xlate.c's: flat shading defines COLOR_INTERP as nointerpolation */
static const char VARYINGS[] =
    "#ifndef COLOR_INTERP\n#define COLOR_INTERP\n#endif\n"
    "struct VARYINGS { float4 pos : SV_Position; COLOR_INTERP float4 d0 : COLOR0; COLOR_INTERP float4 d1 : COLOR1;\n"
    "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; };\n";
static const char FLAT[] = "#define COLOR_INTERP nointerpolation\n";
static const char PS_CONSTANTS[] =
    "cbuffer psc : register(b0) { float4 c[8]; float4 texscale[4]; float4 fogcolor; float4 alphatest; float4 tfactor; float4 ffmisc;\n"
    "  float4 bumpenv[4]; float4 bumplum[4]; };\n";

/* ---- the game's vertex shaders ------------------------------------------------- */
/* The game's shader source becomes HLSL (shader_xlate.c), compiled on first
   use. The Xbox vertex declaration describes stream 0 in packed formats the
   host lacks (3-byte, 11:11:10) at unaligned offsets, so each draw expands
   the vertices it touches to one float4 per shader input, read through
   TEXCOORDn. */
typedef struct {
    uint32_t handle;
    char *src; size_t len;
    RcVElem elems[16]; int nelems;
    ID3D11VertexShader *vs;
    ID3D11InputLayout *layout;
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
        REL(s->vs); REL(s->layout);
        free(s->src);
        if (g_vsh == s) g_vsh = NULL;
        memset(s, 0, sizeof *s);
    }
}

void r_set_vs_constants(int reg, const float *data, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (reg + (int)i >= 0 && reg + (int)i < 96) memcpy(g_vsc[reg + i], data + 4 * i, 16);
}

static int vshader_ready(VShader *s) {
    if (s->vs) return 1;
    if (s->failed || !g_dev) return 0;
    s->failed = 1;
    static char hlsl[1 << 17];
    uint32_t inputs = 0;
    if (xs_vs_to_hlsl(s->src, s->len, XS_SM4, hlsl, sizeof hlsl, &inputs)) {
        fprintf(stderr, "render: vertex shader %08x not translated: %s\n", s->handle, hlsl);
        return 0;
    }
    ID3DBlob *code = compile_hlsl(hlsl, "vs_4_0", s->handle);
    if (!code) return 0;
    D3D11_INPUT_ELEMENT_DESC el[16];
    s->nregs = 0;
    for (int r = 0; r < 16; r++) {
        if (!(inputs & (1u << r))) continue;
        el[s->nregs] = (D3D11_INPUT_ELEMENT_DESC){"TEXCOORD", (UINT)r, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16 * s->nregs, D3D11_INPUT_PER_VERTEX_DATA, 0};
        s->regs[s->nregs++] = (uint8_t)r;
    }
    const void *bc = ID3D10Blob_GetBufferPointer(code);
    SIZE_T size = ID3D10Blob_GetBufferSize(code);
    HRESULT hr = ID3D11Device_CreateVertexShader(g_dev, bc, size, NULL, &s->vs);
    if (SUCCEEDED(hr) && s->nregs) hr = ID3D11Device_CreateInputLayout(g_dev, el, s->nregs, bc, size, &s->layout);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { fprintf(stderr, "render: vertex shader %08x refused (%08lx)\n", s->handle, hr); REL(s->vs); return 0; }
    s->failed = 0;
    return 1;
}

void r_set_vertex_shader(uint32_t handle) {
    g_st.vs_handle = handle;
    g_vsh = NULL;
    if (handle & 1) { /* a vertex shader object (CreateVertexShader) */
        static int warned;
        for (int i = 0; i < VSHADERS; i++) if (g_vshaders[i].handle == handle) g_vsh = &g_vshaders[i];
        if (!g_vsh && !warned++) fprintf(stderr, "render: unknown vertex shader handle %08x\n", handle);
        g_st.fvf = 0;
        return;
    }
    g_st.fvf = handle & ~0x1u;
}

/* ---- the game's pixel shaders ----------------------------------------------------- */
/* Translated like the vertex shaders, once per variant: the set of stages
   bound to shadow buffers (bits 0..3: stage n compares) and flat shading
   (bit 4); a draw without one uses a generated shader for the texture stage
   states */
typedef struct { uint32_t handle; char *src; size_t len; ID3D11PixelShader *ps[32]; uint32_t failed; } PShader;
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
        for (int k = 0; k < 32; k++) REL(s->ps[k]);
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

void r_set_pixel_shader(uint32_t handle) { g_st.ps_handle = handle; g_psh = find_pshader(handle); }

static ID3D11PixelShader *pshader(PShader *s, uint32_t variant) {
    if (s->ps[variant]) return s->ps[variant];
    if ((s->failed >> variant) & 1) return NULL;
    s->failed |= 1u << variant;
    static char hlsl[1 << 16];
    int fog, o = variant & 16 ? snprintf(hlsl, sizeof hlsl, "%s", FLAT) : 0;
    if (xs_ps_to_hlsl(s->src, s->len, XS_SM4, variant & 15, hlsl + o, sizeof hlsl - o, &fog)) {
        fprintf(stderr, "render: pixel shader %08x not translated: %s\n", s->handle, hlsl + o);
        return NULL;
    }
    ID3DBlob *code = compile_hlsl(hlsl, "ps_4_0", s->handle);
    if (!code) return NULL;
    HRESULT hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &s->ps[variant]);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { fprintf(stderr, "render: pixel shader %08x refused (%08lx)\n", s->handle, hr); return NULL; }
    s->failed &= ~(1u << variant);
    return s->ps[variant];
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

/* ---- fixed function: vertex shaders ------------------------------------------------- */
/* Generated from the FVF and the states Direct3D 9's transform and lighting
   stage read: world-view-projection (pretransformed XYZRHW vertices mapped
   back through the viewport), vertex fog from the eye distance, the
   material sources (lights are not forwarded, so lighting leaves the
   emissive and ambient terms), and per stage the texture coordinate set or
   generated coordinates through the stage's texture matrix. */
typedef struct {
    uint32_t fvf;
    uint8_t lighting, colorvertex, localviewer, pad;
    uint8_t matsrc[4];
    uint8_t tci[4];          /* coordinate set | generation << 4 */
} VsKey;
typedef struct { VsKey key; int used; ID3D11VertexShader *vs; ID3D11InputLayout *layout; } FfVs;
#define FF_CACHE 512
static FfVs g_ffvs[FF_CACHE];

static uint32_t hash(const void *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 16777619u;
    return h;
}

/* the cache entry for a key: its shader, or a free entry to fill; NULL when full */
#define CACHE_LOOKUP(table, k, e)                                                          \
    do {                                                                                   \
        uint32_t h_ = hash(k, sizeof *(k));                                                \
        (e) = NULL;                                                                        \
        for (uint32_t i_ = 0; i_ < FF_CACHE && !(e); i_++) {                               \
            if (!table[(h_ + i_) % FF_CACHE].used || !memcmp(&table[(h_ + i_) % FF_CACHE].key, k, sizeof *(k))) \
                (e) = &table[(h_ + i_) % FF_CACHE];                                        \
        }                                                                                  \
        if (!(e)) { static int warned_; if (!warned_++) fprintf(stderr, "render: shader cache full\n"); } \
    } while (0)

static FfVs *ff_vs(const VsKey *k) {
    FfVs *e;
    CACHE_LOOKUP(g_ffvs, k, e);
    if (!e) return NULL;
    if (e->used) return e->vs ? e : NULL;
    e->used = 1;
    e->key = *k;

    static char buf[1 << 15];
    Text t = {buf, 0, sizeof buf};
    D3D11_INPUT_ELEMENT_DESC el[16];
    int ne = 0;
    uint32_t fvf = k->fvf, off = 0, pos = fvf & 0xE;
    int rhw = pos == 0x4, nrm = (fvf & 0x10) != 0, dif = (fvf & 0x40) != 0, spe = (fvf & 0x80) != 0, ntex = (fvf >> 8) & 0xf;
    int tsize[8] = {0};
    put(&t, "cbuffer ffvs : register(b0) { row_major float4x4 wvp; row_major float4x4 wv; row_major float4x4 texm[4]; row_major float4x4 nrm;\n"
            "  float4 fog, hostpos, vp, vpz, ambient, mat[4]; };\n%s", VARYINGS);
    put(&t, "struct VS_IN {\n");
    #define ELEMENT(name, index, format, bytes) \
        (el[ne++] = (D3D11_INPUT_ELEMENT_DESC){name, index, format, 0, off, D3D11_INPUT_PER_VERTEX_DATA, 0}, off += (bytes))
    if (rhw) { put(&t, "  float4 pos : POSITION;\n"); ELEMENT("POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 16); }
    else { put(&t, "  float3 pos : POSITION;\n"); ELEMENT("POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 12); }
    if (pos >= 0x6) off += 4 * ((pos - 0x4) / 2);   /* blend weights: vertex blending is off */
    if (nrm) { put(&t, "  float3 nrm : NORMAL;\n"); ELEMENT("NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 12); }
    if (fvf & 0x20) off += 4;                        /* point size */
    if (dif) { put(&t, "  float4 dif : COLOR0;\n"); ELEMENT("COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 4); }
    if (spe) { put(&t, "  float4 spe : COLOR1;\n"); ELEMENT("COLOR", 1, DXGI_FORMAT_R8G8B8A8_UNORM, 4); }
    static const DXGI_FORMAT tformat[5] = {0, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    for (int i = 0; i < ntex && i < 8; i++) {
        uint32_t sz = (fvf >> (16 + i * 2)) & 3; /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
        tsize[i] = sz == 0 ? 2 : sz == 1 ? 3 : sz == 2 ? 4 : 1;
        put(&t, "  float%d tc%d : TEXCOORD%d;\n", tsize[i], i, i);
        ELEMENT("TEXCOORD", (UINT)i, tformat[tsize[i]], 4 * tsize[i]);
    }
    #undef ELEMENT
    put(&t, "};\nVARYINGS main(VS_IN v) {\n  VARYINGS o;\n");
    /* D3DCOLOR is B, G, R, A in memory; absent colours are white and black */
    put(&t, "  float4 dif = %s, spe = %s, eye = 0; float3 n = %s;\n", dif ? "v.dif.zyxw" : "1", spe ? "v.spe.zyxw" : "0", "0");
    if (rhw) {
        /* screen coordinates at Direct3D 9 pixel centres, z as the depth, w
           for perspective-correct interpolation */
        put(&t, "  float w = v.pos.w != 0 ? 1 / v.pos.w : 1;\n"
                "  float3 ndc = float3((v.pos.x + 0.5 - vp.x) / vp.z * 2 - 1, 1 - (v.pos.y + 0.5 - vp.y) / vp.w * 2,\n"
                "                      vpz.y != vpz.x ? (v.pos.z - vpz.x) / (vpz.y - vpz.x) : v.pos.z);\n"
                "  o.pos = float4(ndc * w, w);\n");
        /* no vertex fog for pretransformed vertices: the specular alpha is the factor */
        put(&t, "  o.fog = %s;\n", spe ? "spe.a" : "1");
    } else {
        put(&t, "  float4 p = float4(v.pos, 1);\n  o.pos = mul(p, wvp); o.pos.xy += hostpos.xy * o.pos.w;\n  eye = mul(p, wv);\n");
        if (nrm) put(&t, "  n = mul(v.nrm, (float3x3)nrm);\n");
        put(&t, "  float d = hostpos.z != 0 ? length(eye.xyz) : abs(eye.z), f = 1;\n"
                "  if (fog.w == 3) f = (fog.y - d) / (fog.y - fog.x);\n"
                "  else if (fog.w == 1) f = exp(-d * fog.z);\n"
                "  else if (fog.w == 2) f = exp(-(d * fog.z) * (d * fog.z));\n"
                "  o.fog = saturate(f);\n");
    }
    if (k->lighting) {
        /* D3DMCS_MATERIAL 0, COLOR1 1, COLOR2 2 */
        const char *src[4];
        for (int i = 0; i < 4; i++) {
            static const char *const mat[4] = {"mat[0]", "mat[1]", "mat[2]", "mat[3]"};
            src[i] = k->colorvertex && k->matsrc[i] == 1 && dif ? "dif" : k->colorvertex && k->matsrc[i] == 2 && spe ? "spe" : mat[i];
        }
        put(&t, "  o.d0 = saturate(float4(%s.rgb + ambient.rgb * %s.rgb, %s.a));\n  o.d1 = 0;\n", src[3], src[1], src[0]);
    } else put(&t, "  o.d0 = saturate(dif); o.d1 = saturate(spe);\n");
    for (int s = 0; s < 4; s++) {
        int set = k->tci[s] & 7, gen = k->tci[s] >> 4;
        /* coordinates are padded as Direct3D 9 does before the texture
           matrix: (u, 1, 0, 0), (u, v, 1, 0), (u, v, w, 1) */
        if (gen == 1) put(&t, "  o.t%d = mul(float4(n, 1), texm[%d]);\n", s, s);
        else if (gen == 2) put(&t, "  o.t%d = mul(float4(eye.xyz, 1), texm[%d]);\n", s, s);
        else if (gen == 3) {
            put(&t, "  { float3 e = %s; o.t%d = mul(float4(2 * dot(e, n) * n - e, 1), texm[%d]); }\n",
                k->localviewer ? "-normalize(eye.xyz)" : "float3(0, 0, -1)", s, s);
        } else if (set < ntex) {
            static const char *const pad[5] = {"", "float4(v.tc%d, 1, 0, 0)", "float4(v.tc%d, 1, 0)", "float4(v.tc%d, 1)", "v.tc%d"};
            char in[48];
            snprintf(in, sizeof in, pad[tsize[set]], set);
            put(&t, "  o.t%d = mul(%s, texm[%d]);\n", s, in, s);
        } else put(&t, "  o.t%d = float4(0, 0, 0, 1);\n", s);
    }
    put(&t, "  return o;\n}\n");

    ID3DBlob *code = compile_hlsl(buf, "vs_4_0", fvf);
    if (!code) return NULL;
    const void *bc = ID3D10Blob_GetBufferPointer(code);
    SIZE_T size = ID3D10Blob_GetBufferSize(code);
    HRESULT hr = ID3D11Device_CreateVertexShader(g_dev, bc, size, NULL, &e->vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(g_dev, el, ne, bc, size, &e->layout);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { fprintf(stderr, "render: fixed-function vertex shader for FVF %03x refused (%08lx)\n", fvf, hr); REL(e->vs); REL(e->layout); return NULL; }
    if (getenv("RECOMP_SHADERLOG")) fprintf(stderr, "render: vertex shader for FVF %03x:\n%s", fvf, buf);
    return e;
}

/* ---- fixed function: pixel shaders -------------------------------------------------- */
/* Generated from the texture stage states: Direct3D 9's stage arithmetic in
   order up to the first disabled stage, each result clamped, then the
   specular colour, fog and the alpha test. A stage whose colour reads a
   missing texture is a disabled one; otherwise a missing texture reads
   white. */
typedef struct { uint8_t cop, aop, carg[3], aarg[3], result, kind, proj, kill; } PsStage;
typedef struct { PsStage st[4]; uint8_t flat; } PsKey; /* stages up to the first disabled one; cop 0 after */
typedef struct { PsKey key; int used; ID3D11PixelShader *ps; } FfPs;
static FfPs g_ffps[FF_CACHE];

/* a stage argument (D3DTA_*) as a float4 expression */
static const char *ps_arg(uint32_t a, char *buf, size_t cap) {
    static const char *const base[] = {"dif", "cur", "tx", "tfactor", "spec", "temp"};
    const char *b = (a & 0xF) < 6 ? base[a & 0xF] : "cur";
    if (a & 0x10) snprintf(buf, cap, "(1 - %s)%s", b, (a & 0x20) ? ".aaaa" : "");
    else snprintf(buf, cap, "%s%s", b, (a & 0x20) ? ".aaaa" : "");
    return buf;
}

/* a D3DTOP of arguments a0, a1, a2 (already float4 expressions) */
static void ps_op(Text *t, const char *dst, uint32_t op, const char *a0, const char *a1, const char *a2) {
    switch (op) {
    case D3DTOP_SELECTARG1: put(t, "  %s = %s;\n", dst, a1); break;
    case D3DTOP_SELECTARG2: put(t, "  %s = %s;\n", dst, a2); break;
    case D3DTOP_MODULATE: put(t, "  %s = %s * %s;\n", dst, a1, a2); break;
    case D3DTOP_MODULATE2X: put(t, "  %s = %s * %s * 2;\n", dst, a1, a2); break;
    case D3DTOP_MODULATE4X: put(t, "  %s = %s * %s * 4;\n", dst, a1, a2); break;
    case D3DTOP_ADD: put(t, "  %s = %s + %s;\n", dst, a1, a2); break;
    case D3DTOP_ADDSIGNED: put(t, "  %s = %s + %s - 0.5;\n", dst, a1, a2); break;
    case D3DTOP_ADDSIGNED2X: put(t, "  %s = (%s + %s - 0.5) * 2;\n", dst, a1, a2); break;
    case D3DTOP_SUBTRACT: put(t, "  %s = %s - %s;\n", dst, a1, a2); break;
    case D3DTOP_ADDSMOOTH: put(t, "  %s = %s + %s - %s * %s;\n", dst, a1, a2, a1, a2); break;
    case D3DTOP_BLENDDIFFUSEALPHA: put(t, "  %s = lerp(%s, %s, dif.a);\n", dst, a2, a1); break;
    case D3DTOP_BLENDTEXTUREALPHA: put(t, "  %s = lerp(%s, %s, tx.a);\n", dst, a2, a1); break;
    case D3DTOP_BLENDFACTORALPHA: put(t, "  %s = lerp(%s, %s, tfactor.a);\n", dst, a2, a1); break;
    case D3DTOP_BLENDTEXTUREALPHAPM: put(t, "  %s = %s + %s * (1 - tx.a);\n", dst, a1, a2); break;
    case D3DTOP_BLENDCURRENTALPHA: put(t, "  %s = lerp(%s, %s, cur.a);\n", dst, a2, a1); break;
    case D3DTOP_MODULATEALPHA_ADDCOLOR: put(t, "  %s = %s + (%s).aaaa * %s;\n", dst, a1, a1, a2); break;
    case D3DTOP_MODULATECOLOR_ADDALPHA: put(t, "  %s = %s * %s + (%s).aaaa;\n", dst, a1, a2, a1); break;
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR: put(t, "  %s = (1 - (%s).aaaa) * %s + %s;\n", dst, a1, a2, a1); break;
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA: put(t, "  %s = (1 - %s) * %s + (%s).aaaa;\n", dst, a1, a2, a1); break;
    case D3DTOP_DOTPRODUCT3: put(t, "  %s = saturate(4 * dot((%s).rgb - 0.5, (%s).rgb - 0.5)).xxxx;\n", dst, a1, a2); break;
    case D3DTOP_MULTIPLYADD: put(t, "  %s = %s + %s * %s;\n", dst, a0, a1, a2); break;
    case D3DTOP_LERP: put(t, "  %s = lerp(%s, %s, %s);\n", dst, a2, a1, a0); break;
    default: put(t, "  %s = %s;\n", dst, a1); break;   /* PREMODULATE: this stage's argument */
    case D3DTOP_BUMPENVMAP: case D3DTOP_BUMPENVMAPLUMINANCE: put(t, "  %s = cur;\n", dst); break;
    }
}

static FfPs *ff_ps(const PsKey *k) {
    FfPs *e;
    CACHE_LOOKUP(g_ffps, k, e);
    if (!e) return NULL;
    if (e->used) return e->ps ? e : NULL;
    e->used = 1;
    e->key = *k;

    static char buf[1 << 15];
    Text t = {buf, 0, sizeof buf};
    put(&t, "%s%s%s", k->flat ? FLAT : "", PS_CONSTANTS, VARYINGS);
    for (int s = 0; s < 4 && k->st[s].cop; s++) {
        if (k->st[s].kind == TK_DEPTH) put(&t, "Texture2D<float> tex%d : register(t%d); SamplerComparisonState smp%d : register(s%d);\n", s, s, s, s);
        else if (k->st[s].kind) put(&t, "Texture2D tex%d : register(t%d); SamplerState smp%d : register(s%d);\n", s, s, s, s);
    }
    put(&t, "float4 main(VARYINGS i) : SV_Target {\n"
            "  float4 dif = i.d0, spec = i.d1, cur = dif, temp = 0, tx = 1, q = 0, rc = 0, ra = 0;\n"
            "  float2 bump = 0; float lum = 1;\n");
    for (int s = 0; s < 4 && k->st[s].cop; s++) {
        const PsStage *S = &k->st[s];
        /* texscale: linear textures in texels under the game's vertex shaders
           (the generated ones scale in the texture matrix); a projected
           lookup divides by the last of COUNTn components */
        put(&t, "  // stage %d\n  q = i.t%d * float4(texscale[%d].xyz, 1); q.xy += bump; bump = 0;\n", s, s, s);
        static const char *const divisor[5] = {"1", "1", "q.y", "q.z", "q.w"};
        const char *div = divisor[S->proj <= 4 ? S->proj : 0];
        if (S->kind == TK_DEPTH) put(&t, "  tx = tex%d.SampleCmpLevelZero(smp%d, q.xy / %s, q.z / %s).xxxx;\n", s, s, div, div);
        else if (S->kind) put(&t, "  tx = tex%d.Sample(smp%d, q.xy / %s);\n", s, s, div);
        else put(&t, "  tx = 1;\n");
        put(&t, "  tx.rgb *= lum; lum = 1;\n");
        if (S->kill) put(&t, "  clip(tx.a - 0.5 / 255);\n");
        char c0[32], c1[32], c2[32], a0[32], a1[32], a2[32];
        ps_op(&t, "rc", S->cop, ps_arg(S->carg[0], c0, 32), ps_arg(S->carg[1], c1, 32), ps_arg(S->carg[2], c2, 32));
        if (S->aop == D3DTOP_DISABLE) put(&t, "  ra = cur;\n");
        else ps_op(&t, "ra", S->aop, ps_arg(S->aarg[0] | 0x20, a0, 32), ps_arg(S->aarg[1] | 0x20, a1, 32), ps_arg(S->aarg[2] | 0x20, a2, 32));
        /* DOTPRODUCT3 replicates into alpha as well; the last stage always
           writes the current colour */
        int last = s == 3 || !k->st[s + 1].cop;
        put(&t, "  %s = saturate(float4(rc.rgb, %s));\n", S->result == D3DTA_TEMP && !last ? "temp" : "cur", S->cop == D3DTOP_DOTPRODUCT3 ? "rc.a" : "ra.a");
        /* bump mapping perturbs the next stage's coordinates (and scales its
           texel by the luminance): du in red, dv in green */
        if (S->cop == D3DTOP_BUMPENVMAP || S->cop == D3DTOP_BUMPENVMAPLUMINANCE)
            put(&t, "  bump = float2(bumpenv[%d].x * tx.r + bumpenv[%d].z * tx.g, bumpenv[%d].y * tx.r + bumpenv[%d].w * tx.g);\n", s, s, s, s);
        if (S->cop == D3DTOP_BUMPENVMAPLUMINANCE) put(&t, "  lum = saturate(tx.b * bumplum[%d].x + bumplum[%d].y);\n", s, s);
    }
    put(&t, "  float4 o = cur;\n"
            "  if (ffmisc.x != 0) o.rgb = saturate(o.rgb + spec.rgb);\n"
            "  o.rgb = lerp(fogcolor.rgb, o.rgb, saturate(i.fog));\n"
            "  float a8 = round(o.a * 255), ref = alphatest.y; int f = (int)alphatest.x;\n"
            "  if (f == 1 || (f == 2 && !(a8 < ref)) || (f == 3 && a8 != ref) || (f == 4 && a8 > ref) ||\n"
            "      (f == 5 && !(a8 > ref)) || (f == 6 && a8 == ref) || (f == 7 && a8 < ref)) discard;\n"
            "  return o;\n}\n");

    ID3DBlob *code = compile_hlsl(buf, "ps_4_0", 0);
    if (!code) return NULL;
    HRESULT hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &e->ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { fprintf(stderr, "render: fixed-function pixel shader refused (%08lx)\n", hr); return NULL; }
    if (getenv("RECOMP_SHADERLOG")) fprintf(stderr, "render: stage pixel shader:\n%s", buf);
    return e;
}

/* ---- per draw ------------------------------------------------------------------------ */
static void mat_mul(const float *a, const float *b, float *o) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) r[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(o, r, sizeof r);
}

/* normals go through the inverse transpose of world-view (its 3x3 part) */
static void normal_matrix(const float *m, float *o) {
    float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
    float A = e * i - f * h, B = f * g - d * i, C = d * h - e * g, det = a * A + b * B + c * C;
    float s = det != 0 ? 1 / det : 0;
    float inv[9] = {A * s, (c * h - b * i) * s, (b * f - c * e) * s, B * s, (a * i - c * g) * s, (c * d - a * f) * s,
                    C * s, (b * g - a * h) * s, (a * e - b * d) * s};
    memset(o, 0, 64);
    for (int r = 0; r < 3; r++)
        for (int k = 0; k < 3; k++) o[r * 4 + k] = inv[k * 3 + r];
    o[15] = 1;
}

/* fog for the next draw: the factor's function (0 when off) and the colour */
static void fog_state(float *params, float *color) {
    static int forced = -1;
    static uint32_t forced_color;
    St *s = &g_st;
    if (forced < 0) { /* RECOMP_FOG=start,end,RRGGBB: linear fog everywhere, to check the fog path */
        float a, b; unsigned col;
        const char *f = getenv("RECOMP_FOG");
        forced = f && sscanf(f, "%f,%f,%x", &a, &b, &col) == 3;
        if (forced) { forced_color = 0xff000000u | col; s->fogstart = a; s->fogend = b; }
    }
    uint32_t c = s->fogcolor;
    if (forced) {
        /* multiplied into the scene (soot decals: DESTCOLOR, ZERO) a draw
           fades to white, as a game sets it up, not to the fog colour */
        int multiply = s->blend && ((s->src == D3DBLEND_DESTCOLOR && s->dst == D3DBLEND_ZERO) || (s->src == D3DBLEND_ZERO && s->dst == D3DBLEND_SRCCOLOR));
        s->fog = 1; s->fogmode = 3;
        c = multiply ? 0xffffffffu : forced_color;
    }
    params[0] = s->fogstart; params[1] = s->fogend; params[2] = s->fogdensity;
    params[3] = s->fog && s->fogmode ? (float)s->fogmode : 0;
    argb_to_float(c, color);
}

static uint32_t depth_mask(const Bound *b) {
    uint32_t m = 0;
    for (int s = 0; s < 4; s++) if (b->kind[s] == TK_DEPTH) m |= 1u << s;
    return m;
}

/* shaders and constants for the next draw; 0 when it cannot be drawn */
static int apply_shaders(const Bound *b) {
    St *s = &g_st;
    float fog[4], fogcolor[4];
    fog_state(fog, fogcolor);
    /* Direct3D 9 pixel centres: half a pixel right and down, in clip units */
    float hostpos[4] = {1.0f / s->vp[2], -1.0f / s->vp[3], (float)s->fogrange, 0};

    if (g_vsh) {
        static CbVs cb;
        memcpy(cb.c, g_vsc, sizeof cb.c);
        memcpy(cb.fog, fog, 16); memcpy(cb.pos, hostpos, 16);
        upload_cb(g_cb_vs, &cb, sizeof cb);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vsh->vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb_vs);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, g_vsh->layout);
    } else {
        VsKey k;
        memset(&k, 0, sizeof k);
        k.fvf = s->fvf;
        k.lighting = s->lighting && (s->fvf & 0xE) != 0x4;
        k.colorvertex = (uint8_t)s->colorvertex;
        k.localviewer = (uint8_t)s->localviewer;
        for (int i = 0; i < 4; i++) {
            k.matsrc[i] = (uint8_t)(k.lighting ? s->matsrc[i] : 0);
            uint32_t gen = (s->ts[i].tci >> 16) & 0xF;
            k.tci[i] = (uint8_t)((s->ts[i].tci & 7) | ((gen <= 3 ? gen : 0) << 4));
        }
        if (!k.lighting) k.colorvertex = 0;
        FfVs *e = ff_vs(&k);
        if (!e) return 0;
        static CbFf cb;
        mat_mul(s->world, s->view, cb.wv);
        mat_mul(cb.wv, s->proj, cb.wvp);
        normal_matrix(cb.wv, cb.nrm);
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        for (int i = 0; i < 4; i++) {
            /* the game's texture matrix while its TEXTURETRANSFORMFLAGS enable
               it (a matrix left over from an animated texture must not move
               later draws), then for linear textures the scale to normalised
               coordinates */
            memcpy(cb.texm[i], s->ts[i].texflags ? s->texmat[i] : identity, 64);
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 3; c++) cb.texm[i][r * 4 + c] *= b->scale[i][c];
        }
        memcpy(cb.fog, fog, 16); memcpy(cb.pos, hostpos, 16);
        memcpy(cb.vp, s->vp, 16);
        cb.vpz[0] = s->vp[4]; cb.vpz[1] = s->vp[5];
        argb_to_float(s->ambient, cb.ambient);
        memset(cb.mat, 0, sizeof cb.mat); /* SetMaterial is not forwarded: Direct3D 9's default material */
        upload_cb(g_cb_ff, &cb, sizeof cb);
        ID3D11DeviceContext_VSSetShader(g_ctx, e->vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb_ff);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, e->layout);
    }

    static CbPs cb;
    memcpy(cb.c, g_psc, sizeof cb.c);
    memcpy(cb.fogcolor, fogcolor, 16);
    cb.alphatest[0] = s->atest ? (float)s->afunc : 0; cb.alphatest[1] = (float)s->aref;
    argb_to_float(s->tfactor, cb.tfactor);
    cb.misc[0] = (float)s->specular;
    for (int i = 0; i < 4; i++) {
        memcpy(cb.bumpenv[i], s->ts[i].bumpenv, 16);
        cb.bumplum[i][0] = s->ts[i].lscale; cb.bumplum[i][1] = s->ts[i].loffset;
    }
    int flat = s->shade == D3DSHADE_FLAT;
    ID3D11PixelShader *ps = g_psh ? pshader(g_psh, depth_mask(b) | (flat ? 16 : 0)) : NULL;
    if (ps) {
        for (int i = 0; i < 4; i++) {   /* texel coordinates for linear textures; ALPHAKILL */
            memcpy(cb.texscale[i], b->scale[i], 12);
            cb.texscale[i][3] = s->ts[i].alphakill ? 0.5f / 255 : -1;
        }
    } else {
        PsKey k;
        memset(&k, 0, sizeof k);
        k.flat = (uint8_t)flat;
        for (int i = 0; i < 4; i++) {
            const Stage *ts = &s->ts[i];
            if (ts->colorop == D3DTOP_DISABLE || !ts->colorop) break;
            /* a stage whose colour operation reads a texture that is not
               there ends the stages, as on Direct3D 9 (and DXVK) */
            if (!b->kind[i]) {
                uint32_t used = ts->colorop == D3DTOP_SELECTARG1 || ts->colorop == D3DTOP_PREMODULATE ? 2
                              : ts->colorop == D3DTOP_SELECTARG2 ? 4
                              : ts->colorop == D3DTOP_MULTIPLYADD || ts->colorop == D3DTOP_LERP ? 7
                              : ts->colorop == D3DTOP_BUMPENVMAP || ts->colorop == D3DTOP_BUMPENVMAPLUMINANCE ? 0 : 6;
                int reads = 0;
                for (int a = 0; a < 3; a++) reads |= (used >> a & 1) && (ts->carg[a] & D3DTA_SELECTMASK) == D3DTA_TEXTURE;
                if (reads) break;
            }
            k.st[i].cop = (uint8_t)ts->colorop; k.st[i].aop = (uint8_t)ts->alphaop;
            for (int a = 0; a < 3; a++) { k.st[i].carg[a] = (uint8_t)ts->carg[a]; k.st[i].aarg[a] = (uint8_t)ts->aarg[a]; }
            k.st[i].result = (uint8_t)ts->result;
            k.st[i].kind = b->kind[i];
            k.st[i].proj = (uint8_t)((ts->texflags & D3DTTFF_PROJECTED) ? ts->texflags & 0xF : 0);
            k.st[i].kill = ts->alphakill && b->kind[i];
            /* under the game's vertex shaders linear textures still need texel coordinates */
            for (int c = 0; c < 3; c++) cb.texscale[i][c] = g_vsh ? b->scale[i][c] : 1;
            cb.texscale[i][3] = -1;
        }
        FfPs *e = ff_ps(&k);
        if (!e) return 0;
        ps = e->ps;
    }
    upload_cb(g_cb_ps, &cb, sizeof cb);
    ID3D11DeviceContext_PSSetShader(g_ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb_ps);
    return 1;
}

/* ---- dynamic vertex and index buffers --------------------------------------------- */
#define VB_SIZE (16u << 20)
#define IB_SIZE (4u << 20)
static ID3D11Buffer *g_vb, *g_ib;
static uint32_t g_vb_pos, g_ib_pos;

/* room for size bytes in a ring buffer; *offset gets where. Unmap after writing. */
static void *ring_map(ID3D11Buffer *b, uint32_t *pos, uint32_t cap, uint32_t size, uint32_t *offset) {
    if (size > cap) return NULL;
    D3D11_MAP how = D3D11_MAP_WRITE_NO_OVERWRITE;
    *pos = (*pos + 15) & ~15u;
    if (*pos + size > cap) { *pos = 0; how = D3D11_MAP_WRITE_DISCARD; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)b, 0, how, 0, &m))) return NULL;
    *offset = *pos;
    *pos += size;
    return (uint8_t *)m.pData + *offset;
}

/* RECOMP_PROBE=frame,x,y: after each draw of that frame that targets the back
   buffer, report when the pixel at (x, y) changes and what the draw was */
static void probe(void) {
    static int frame = -1, px, py;
    static uint32_t last, k, kframe;
    static ID3D11Texture2D *one;
    if (frame < 0) {
        const char *e = getenv("RECOMP_PROBE");
        frame = 0;
        if (e) sscanf(e, "%d,%d,%d", &frame, &px, &py);
    }
    if (!frame || g_frame + 1 != (uint32_t)frame || g_rtv != g_bb_rtv) return;
    if (kframe != g_frame + 1) { kframe = g_frame + 1; k = 0; last = 0xdeadbeef; }
    k++;
    if (!one) one = texture_2d(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!one) return;
    UINT hx = (UINT)((px + 0.5f) * g_sx), hy = (UINT)((py + 0.5f) * g_sy); /* the game's pixel, on the host back buffer */
    D3D11_BOX box = {hx, hy, 0, hx + 1, hy + 1, 1};
    ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)one, 0, 0, 0, 0, (ID3D11Resource *)g_bb, 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)one, 0, D3D11_MAP_READ, 0, &m))) return;
    uint32_t c = *(const uint32_t *)m.pData & 0xffffff;
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)one, 0);
    if (c != last)
        fprintf(stderr, "probe: draw %u %06x -> %06x | vs %08x ps %08x fvf %03x tex %08x %08x %08x %08x\n", k, last & 0xffffff, c,
                g_vsh ? g_vsh->handle : 0, g_st.ps_handle, g_st.fvf, g_st.bound[0], g_st.bound[1], g_st.bound[2], g_st.bound[3]);
    last = c;
}

/* The draw: vertices first..last of the guest data (a vertex shader's inputs
   expanded to float4s, FVF vertices as they are), and indices rebased to
   first; quads, fans and polygons become triangle lists and a line loop is
   closed */
static void draw(uint32_t prim, uint32_t n, const uint8_t *verts, uint32_t stride, const uint16_t *idx) {
    if (!g_dev || !n || !stride) return;
    if (g_vsh ? !vshader_ready(g_vsh) : !g_st.fvf || !(g_st.fvf & 0xE)) return;
    sync_deferred_states();

    Bound b;
    resolve_textures(&b);
    {   /* debugging: RECOMP_NOSHADOW leaves out draws that sample a shadow
           buffer, RECOMP_SHADOWDRAWS=a-b keeps only the a-th to b-th of each frame */
        static uint32_t frame, k;
        if (frame != g_frame) frame = g_frame, k = 0;
        if (depth_mask(&b)) {
            const char *e = getenv("RECOMP_SHADOWDRAWS");
            unsigned lo = 0, hi = 0;
            k++;
            if (getenv("RECOMP_NOSHADOW") || (e && sscanf(e, "%u-%u", &lo, &hi) == 2 && (k < lo || k > hi))) return;
        }
    }

    uint32_t first = 0, last = n - 1;
    if (idx) {
        first = 0xffff; last = 0;
        for (uint32_t i = 0; i < n; i++) { if (idx[i] < first) first = idx[i]; if (idx[i] > last) last = idx[i]; }
    }
    uint32_t count = last - first + 1;

    /* indices for the host topology */
    D3D11_PRIMITIVE_TOPOLOGY topo;
    uint32_t ni = 0, need = n * 3 + 1;
    static uint16_t *ib; static uint32_t ibcap;
    if (need > ibcap) { ibcap = need; ib = realloc(ib, ibcap * 2); }
    #define IDX(i) ((uint16_t)((idx ? idx[i] : (i)) - first))
    switch (prim) {
    case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case 3: case 4: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case 5: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    case 6: case 9: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break; /* quad strip = triangle strip */
    case 7: case 10: case 8: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; break;
    default: return;
    }
    if (prim == 8) /* quads as triangle pairs */
        for (uint32_t q = 0; q + 3 < n; q += 4) {
            ib[ni++] = IDX(q); ib[ni++] = IDX(q + 1); ib[ni++] = IDX(q + 2);
            ib[ni++] = IDX(q); ib[ni++] = IDX(q + 2); ib[ni++] = IDX(q + 3);
        }
    else if (prim == 7 || prim == 10) /* fans and polygons */
        for (uint32_t i = 1; i + 1 < n; i++) { ib[ni++] = IDX(0); ib[ni++] = IDX(i); ib[ni++] = IDX(i + 1); }
    else if (idx || prim == 3) {
        for (uint32_t i = 0; i < n; i++) ib[ni++] = IDX(i);
        if (prim == 3 && n > 2) ib[ni++] = IDX(0);
    }
    #undef IDX
    uint32_t prims = topo == D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST ? (ni ? ni : n) / 3 : n;
    if (!prims || (prim == 2 && n < 2) || ((prim == 6 || prim == 9) && n < 3)) return;

    /* vertices */
    uint32_t vstride = g_vsh ? 16u * g_vsh->nregs : stride, voff;
    if (!vstride) vstride = 16;
    uint8_t *vdst = ring_map(g_vb, &g_vb_pos, VB_SIZE, count * vstride, &voff);
    if (!vdst) { static int warned; if (!warned++) fprintf(stderr, "render: draw of %u vertices too large\n", count); return; }
    if (g_vsh) rc_expand_vertices(g_vsh->elems, g_vsh->nelems, g_vsh->regs, g_vsh->nregs, verts, stride, first, count, (float *)vdst);
    else memcpy(vdst, verts + (size_t)first * stride, (size_t)count * stride);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &vstride, &voff);
    uint32_t ioff = 0;
    if (ni) {
        uint16_t *idst = ring_map(g_ib, &g_ib_pos, IB_SIZE, ni * 2, &ioff);
        if (!idst) return;
        memcpy(idst, ib, ni * 2);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_ib, 0);
        ID3D11DeviceContext_IASetIndexBuffer(g_ctx, g_ib, DXGI_FORMAT_R16_UINT, ioff);
    }
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, topo);

    apply_states(&b);
    if (!apply_shaders(&b)) return;
    if (ni) ID3D11DeviceContext_DrawIndexed(g_ctx, ni, 0, 0);
    else ID3D11DeviceContext_Draw(g_ctx, n, 0);
    probe();
}

void r_set_stream(uint32_t stream, uint32_t vb, uint32_t stride) {
    if (stream == 0) { g_st.stream_vb = vb; g_st.stream_stride = stride; }
}

/* ---- state blocks ----------------------------------------------------------- */
/* The Flash overlay saves the device state around its drawing (CreateStateBlock
   D3DSBT_ALL, Capture, draw, Apply); text drawn afterwards relies on it (its
   FVF, stage setup). Everything a draw reads but the shader constants and the
   render target is in St. */
#define STATE_BLOCKS 64
static St *g_sb[STATE_BLOCKS];

uint32_t r_create_state_block(void) {
    for (uint32_t i = 0; i < STATE_BLOCKS; i++) {
        if (g_sb[i]) continue;
        g_sb[i] = malloc(sizeof(St));
        r_capture_state_block(i + 1);
        return i + 1;
    }
    fprintf(stderr, "render: out of state blocks\n");
    return 0;
}
void r_capture_state_block(uint32_t token) {
    if (token - 1 < STATE_BLOCKS && g_sb[token - 1]) *g_sb[token - 1] = g_st;
}
void r_apply_state_block(uint32_t token) {
    if (token - 1 >= STATE_BLOCKS || !g_sb[token - 1]) return;
    g_st = *g_sb[token - 1];
    g_vsh = NULL;
    if (g_st.vs_handle & 1)
        for (int i = 0; i < VSHADERS; i++) if (g_vshaders[i].handle == g_st.vs_handle) g_vsh = &g_vshaders[i];
    g_psh = find_pshader(g_st.ps_handle);
}
void r_delete_state_block(uint32_t token) {
    if (token - 1 >= STATE_BLOCKS) return;
    free(g_sb[token - 1]);
    g_sb[token - 1] = NULL;
}

/* ---- draws --------------------------------------------------------------------- */
static void drawlog(uint32_t prim, uint32_t n, const void *data, uint32_t stride) {
    if (g_drawlog < 0) g_drawlog = getenv("RECOMP_DRAWLOG") ? atoi(getenv("RECOMP_DRAWLOG")) : 0;
    if (g_drawlog <= 0) return;
    g_drawlog--;
    const float *v = data;
    fprintf(stderr, "draw: prim %u n %u fvf %08x vs %08x stride %u ps %08x tex %08x v0 %.2f %.2f %.2f %.2f %08x\n", prim, n, g_st.fvf,
            g_vsh ? g_vsh->handle : 0, stride, g_st.ps_handle, g_st.bound[0], v[0], v[1], v[2], v[3], ((const uint32_t *)data)[4]);
    TexSlot *t = g_st.bound[0] ? tex_find(g_st.bound[0], 0) : NULL;
    if (t) fprintf(stderr, "      tex %ux%u fmt %02x linear %d hdr %08x/%08x colorop %u alphaop %u\n", t->w, t->h, t->fmt, t->linear,
                   RD32(MEM(t->guest + 0xC)), RD32(MEM(t->guest + 0x10)), g_st.ts[0].colorop, g_st.ts[0].alphaop);
}

/* RECOMP_STATELOG=frame: the device state behind every draw of that frame */
static void statelog(uint32_t prim, uint32_t n, const void *data, uint32_t stride) {
    static int frame = -1;
    if (frame < 0) frame = getenv("RECOMP_STATELOG") ? atoi(getenv("RECOMP_STATELOG")) : 0;
    if (!frame || g_frame + 1 != (uint32_t)frame) return;
    const St *s = &g_st;
    const Stage *t = &s->ts[0];
    fprintf(stderr, "state: prim %u n %u fvf %03x vs %08x ps %08x stride %u tex %08x %08x | light %u fog %u/%u/%08x blend %u %u/%u/%u write %x "
            "stencil %u func %u ref %u mask %x/%x ops %u/%u/%u atest %u/%u/%u z %u/%u/%u cull %u tfactor %08x | stage0 c %u(%u,%u) a %u(%u,%u) flags %x\n",
            prim, n, s->fvf, g_vsh ? g_vsh->handle : 0, s->ps_handle, stride, s->bound[0], s->bound[1], s->lighting, s->fog, s->fogmode, s->fogcolor,
            s->blend, s->src, s->dst, s->blendop, s->colorwrite, s->stencil, s->sfunc, s->sref, s->smask, s->swrite, s->sfail, s->szfail, s->spass,
            s->atest, s->afunc, s->aref, s->zenable, s->zwrite, s->zfunc, s->cull,
            s->tfactor, t->colorop, t->carg[1], t->carg[2], t->alphaop, t->aarg[1], t->aarg[2], t->texflags);
    if (s->bound[0] && !g_vsh) {
        fprintf(stderr, "       tex0 data %08x format %08x size %08x, verts", RD32(MEM(s->bound[0] + 4)), RD32(MEM(s->bound[0] + 0xC)), RD32(MEM(s->bound[0] + 0x10)));
        for (uint32_t i = 0; i < n && i < 8; i++) {
            const float *q = (const float *)((const uint8_t *)data + i * stride);
            fprintf(stderr, " (");
            for (uint32_t k = 0; k < stride / 4 && k < 8; k++) fprintf(stderr, k ? " %g" : "%g", q[k]);
            fprintf(stderr, ")");
        }
        fprintf(stderr, "\n");
    }
}

static void draw_up(uint32_t prim, uint32_t n, const void *data, uint32_t stride) {
    drawlog(prim, n, data, stride);
    statelog(prim, n, data, stride);
    draw(prim, n, data, stride, NULL);
}

void r_draw(uint32_t prim, uint32_t start, uint32_t n) {
    if (!g_st.stream_vb) return;
    uint32_t stride = g_st.stream_stride ? g_st.stream_stride : rc_fvf_stride(g_st.fvf);
    uint32_t data = RD32(MEM(g_st.stream_vb + 4));
    if (!data) return;
    draw_up(prim, n, MEM(data + start * stride), stride);
}

void r_draw_up(uint32_t prim, uint32_t n, uint32_t data, uint32_t stride) {
    if (!data) return;
    draw_up(prim, n, MEM(data), stride ? stride : rc_fvf_stride(g_st.fvf));
}

void r_draw_indexed(uint32_t prim, uint32_t n, uint32_t index_data, uint32_t base_vertex) {
    if (!g_dev || (!g_st.fvf && !g_vsh) || !g_st.stream_vb || !n) return;
    if (index_data < 0x10000) { /* no index buffer behind it: report once rather than read low memory */
        static int warned;
        if (!warned++) fprintf(stderr, "render: indexed draw with index data %08x skipped\n", index_data);
        return;
    }
    uint32_t stride = g_st.stream_stride ? g_st.stream_stride : rc_fvf_stride(g_st.fvf);
    uint32_t data = RD32(MEM(g_st.stream_vb + 4));
    if (!data) return;
    data += base_vertex * stride; /* SetIndices' BaseVertexIndex offsets every index */
    statelog(prim, n, MEM(data), stride);
    draw(prim, n, MEM(data), stride, (const uint16_t *)MEM(index_data));
}

/* ---- frame ------------------------------------------------------------------ */
/* Clear covers the viewport, as Direct3D 9's does */
void r_clear(uint32_t flags, uint32_t color, float z, uint32_t stencil) {
    if (!g_dev) return;
    const float *vp = g_st.vp;
    uint32_t w = g_rtv == g_bb_rtv ? g_width : g_off_w, h = g_rtv == g_bb_rtv ? g_height : g_off_h;
    if (vp[0] != 0 || vp[1] != 0 || (uint32_t)vp[2] < w || (uint32_t)vp[3] < h) {
        static int warned;
        if (!warned++) fprintf(stderr, "render: clear of a %gx%g viewport clears the whole %ux%u target\n", vp[2], vp[3], w, h);
    }
    if ((flags & 0xF0) && g_rtv) {
        float c[4];
        argb_to_float(color, c);
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rtv, c);
    }
    UINT f = ((flags & 0x01) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 0x02) ? D3D11_CLEAR_STENCIL : 0);
    if (f && g_dsv) ID3D11DeviceContext_ClearDepthStencilView(g_ctx, g_dsv, f, z, (UINT8)stencil);
}

/* RECOMP_SHOT="frame,frame,...": save the back buffer as build\shot_<frame>.bmp
   before those presents; works whether or not the window is on screen */
static void shot(uint32_t frame) {
    char name[64];
    if (!rc_shot_wanted(frame, name, sizeof name)) return;
    ID3D11Texture2D *sys = texture_2d(g_out_w, g_out_h, DXGI_FORMAT_B8G8R8A8_UNORM, 0, D3D11_USAGE_STAGING, D3D11_CPU_ACCESS_READ);
    if (!sys) return;
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)sys, (ID3D11Resource *)g_bb);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)sys, 0, D3D11_MAP_READ, 0, &m))) {
        if (rc_write_bmp(name, m.pData, m.RowPitch, g_out_w, g_out_h)) fprintf(stderr, "render: saved %s\n", name);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)sys, 0);
    }
    REL(sys);
}

void r_present(void) {
    if (!g_dev) return;
    shot(++g_frame);
    if (g_win_w == g_out_w && g_win_h == g_out_h)
        ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_swap_buf, (ID3D11Resource *)g_bb);
    else { /* centred, pixel for pixel, black around it (cropped if larger) */
        static const float black[4] = {0, 0, 0, 1};
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_swap_rtv, black);
        UINT w = min(g_out_w, g_win_w), h = min(g_out_h, g_win_h);
        D3D11_BOX box = {(g_out_w - w) / 2, (g_out_h - h) / 2, 0, (g_out_w - w) / 2 + w, (g_out_h - h) / 2 + h, 1};
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_swap_buf, 0, (g_win_w - w) / 2, (g_win_h - h) / 2, 0,
                                                  (ID3D11Resource *)g_bb, 0, &box);
    }
    HRESULT hr = IDXGISwapChain1_Present(g_swap, g_present_interval, g_present_flags);
    if (FAILED(hr)) { static int warned; if (!warned++) fprintf(stderr, "render: present failed %08lx\n", hr); }
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv); /* flip model unbinds */
    pump();
    debug_messages();
}

/* ---- device ------------------------------------------------------------------ */
static ID3D11Buffer *buffer(UINT size, UINT bind) {
    D3D11_BUFFER_DESC d = {size, D3D11_USAGE_DYNAMIC, bind, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *b = NULL;
    return SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &d, NULL, &b)) ? b : NULL;
}

int r_init(uint32_t width, uint32_t height) {
    if (g_dev) return 1;
    g_drawlog = getenv("RECOMP_DRAWLOG") ? atoi(getenv("RECOMP_DRAWLOG")) : 0;
    SetProcessDPIAware(); /* window and monitor sizes in real pixels, not scaled by Windows */
    g_width = width; g_height = height;
    g_out_w = width; g_out_h = height;
    const char *res = getenv("RECOMP_RES");
    unsigned rw, rh;
    if (res && sscanf(res, "%ux%u", &rw, &rh) == 2 && rw >= 64 && rh >= 48 && rw <= 16384 && rh <= 16384) { g_out_w = rw; g_out_h = rh; }
    g_sx = (float)g_out_w / width; g_sy = (float)g_out_h / height;
    int fullscreen = getenv("RECOMP_FULLSCREEN") != NULL;
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = fullscreen ? NULL : LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = "ObiWanRecomp";
    RegisterClassA(&wc);
    if (fullscreen) { /* borderless, covering the primary monitor */
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        g_win_w = mi.rcMonitor.right - mi.rcMonitor.left; g_win_h = mi.rcMonitor.bottom - mi.rcMonitor.top;
        g_hwnd = CreateWindowA(wc.lpszClassName, "Star Wars: Obi-Wan (recompiled)", WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top,
                               g_win_w, g_win_h, NULL, NULL, wc.hInstance, NULL);
    } else {
        g_win_w = g_out_w; g_win_h = g_out_h;
        RECT r = {0, 0, (LONG)g_win_w, (LONG)g_win_h};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&r, style, FALSE);
        g_hwnd = CreateWindowA(wc.lpszClassName, "Star Wars: Obi-Wan (recompiled)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                               r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    }
    if (!g_hwnd) { fprintf(stderr, "render: no window (error %lu)\n", GetLastError()); return 0; }
    ShowWindow(g_hwnd, SW_SHOW);

    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_SINGLETHREADED | (getenv("RECOMP_D3DDEBUG") ? D3D11_CREATE_DEVICE_DEBUG : 0);
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) { /* no SDK layers installed */
        fprintf(stderr, "render: no Direct3D 11 debug layer here\n");
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags, levels, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    }
    if (FAILED(hr)) { fprintf(stderr, "render: D3D11CreateDevice failed %08lx\n", hr); return 0; }
    if (flags & D3D11_CREATE_DEVICE_DEBUG) ID3D11Device_QueryInterface(g_dev, &IID_ID3D11InfoQueue, (void **)&g_debug);

    /* the swap chain comes from the factory behind the device's adapter */
    IDXGIDevice1 *dxgi = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    if (FAILED(ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice1, (void **)&dxgi)) || FAILED(IDXGIDevice1_GetAdapter(dxgi, &adapter)) ||
        FAILED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2, (void **)&factory))) {
        fprintf(stderr, "render: no DXGI 1.2 factory\n");
        return 0;
    }
    IDXGIDevice1_SetMaximumFrameLatency(dxgi, 1);
    DXGI_ADAPTER_DESC ad;
    IDXGIAdapter_GetDesc(adapter, &ad);
    BOOL tearing = FALSE;
    IDXGIFactory5 *f5 = NULL;
    if (SUCCEEDED(IDXGIFactory2_QueryInterface(factory, &IID_IDXGIFactory5, (void **)&f5))) {
        if (FAILED(IDXGIFactory5_CheckFeatureSupport(f5, DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) tearing = FALSE;
        REL(f5);
    }
    int novsync = getenv("RECOMP_NOVSYNC") != NULL;
    DXGI_SWAP_CHAIN_DESC1 sd = {g_win_w, g_win_h, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT, 2,
                                DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_DISCARD, DXGI_ALPHA_MODE_UNSPECIFIED,
                                tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0};
    hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, g_hwnd, &sd, NULL, NULL, &g_swap);
    if (FAILED(hr)) { /* before Windows 10 */
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.BufferCount = 1; sd.Flags = 0; tearing = FALSE;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(factory, (IUnknown *)g_dev, g_hwnd, &sd, NULL, NULL, &g_swap);
    }
    IDXGIFactory2_MakeWindowAssociation(factory, g_hwnd, DXGI_MWA_NO_ALT_ENTER);
    REL(factory); REL(adapter); REL(dxgi);
    if (FAILED(hr)) { fprintf(stderr, "render: CreateSwapChainForHwnd failed %08lx\n", hr); return 0; }
    g_present_interval = novsync ? 0 : 1;
    g_present_flags = novsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0;
    IDXGISwapChain1_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&g_swap_buf);

    g_bb = texture_2d(g_out_w, g_out_h, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_RENDER_TARGET, D3D11_USAGE_DEFAULT, 0);
    if (!g_swap_buf || !g_bb || FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_bb, NULL, &g_bb_rtv)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_swap_buf, NULL, &g_swap_rtv)) ||
        !depth_buffer(g_out_w, g_out_h, &g_bb_z, &g_bb_dsv)) {
        fprintf(stderr, "render: no back buffer\n");
        return 0;
    }
    g_cb_vs = buffer(sizeof(CbVs), D3D11_BIND_CONSTANT_BUFFER);
    g_cb_ff = buffer(sizeof(CbFf), D3D11_BIND_CONSTANT_BUFFER);
    g_cb_ps = buffer(sizeof(CbPs), D3D11_BIND_CONSTANT_BUFFER);
    g_vb = buffer(VB_SIZE, D3D11_BIND_VERTEX_BUFFER);
    g_ib = buffer(IB_SIZE, D3D11_BIND_INDEX_BUFFER);
    static const uint32_t black = 0xff000000u;
    D3D11_TEXTURE2D_DESC td = {1, 1, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
    D3D11_SUBRESOURCE_DATA init = {&black, 4, 4};
    ID3D11Texture2D *bt = NULL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &bt))) ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)bt, NULL, &g_black);
    REL(bt);
    if (!g_cb_vs || !g_cb_ff || !g_cb_ps || !g_vb || !g_ib || !g_black) { fprintf(stderr, "render: no buffers\n"); return 0; }

    default_state();
    g_rtv = g_bb_rtv; g_dsv = g_bb_dsv; g_dsv_res = (ID3D11Resource *)g_bb_z;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rtv, g_dsv);
    fprintf(stderr, "render: Direct3D 11 device (feature level %u.%u, %ls), %ux%u rendered at %ux%u in a %ux%u %s%s\n", got >> 12,
            (got >> 8) & 0xf, ad.Description, width, height, g_out_w, g_out_h, g_win_w, g_win_h, fullscreen ? "borderless window" : "window",
            g_debug ? ", debug layer" : "");
    debug_messages();
    return 1;
}
