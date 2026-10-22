/* Direct3D 9 implementation of render.h.

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
   DrawPrimitiveUP. Programmable shaders are not translated yet: vertex
   shader handles fall back to the fixed function pipeline and pixel shaders
   to "texture modulate diffuse". */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "kernel.h"
#include "render.h"

#pragma comment(lib, "d3d9.lib")

static HWND g_hwnd;
static IDirect3D9 *g_d3d;
static IDirect3DDevice9 *g_dev;
static uint32_t g_width, g_height;
static int g_lost;
static int g_drawlog = -1; /* RECOMP_DRAWLOG=N: describe the first N draws and transforms */

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

static int is_linear(uint32_t fmt) {
    return (fmt >= 0x10 && fmt <= 0x20) || fmt == 0x2E || fmt == 0x2F || fmt == 0x30 || fmt == 0x31 ||
           (fmt >= 0x35 && fmt <= 0x37) || (fmt >= 0x3D && fmt <= 0x41);
}
static int bpp_of(uint32_t fmt) {
    switch (fmt) {
    case 0x00: case 0x01: case 0x0B: case 0x13: case 0x19: case 0x1B: case 0x1F: return 1;
    case 0x02: case 0x03: case 0x04: case 0x05: case 0x10: case 0x11: case 0x1A: case 0x1C: case 0x1D: case 0x20:
    case 0x27: case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x30: case 0x31: case 0x32: case 0x35: case 0x37:
    case 0x38: case 0x39: case 0x3D: case 0x3E: return 2;
    default: return 4;
    }
}

/* Xbox swizzle (Morton order with the bits of the larger dimension continuing
   on their own); after Cxbx-Reloaded's EmuUnswizzleBox */
static void unswizzle(const uint8_t *src, uint32_t w, uint32_t h, uint32_t bpp, uint8_t *dst, uint32_t dst_pitch) {
    uint32_t mask_x = 0, mask_y = 0;
    for (uint32_t i = 1, j = 1; i < w || i < h; i <<= 1) {
        if (i < w) { mask_x |= j; j <<= 1; }
        if (i < h) { mask_y |= j; j <<= 1; }
    }
    uint32_t y_bits = 0;
    for (uint32_t y = 0; y < h; y++) {
        uint32_t x_bits = 0;
        uint8_t *row = dst + y * dst_pitch;
        for (uint32_t x = 0; x < w; x++) {
            memcpy(row + x * bpp, src + (x_bits | y_bits) * bpp, bpp);
            x_bits = (x_bits - mask_x) & mask_x;
        }
        y_bits = (y_bits - mask_y) & mask_y;
    }
}

/* one texel of any uncompressed Xbox format -> A8R8G8B8 */
static uint32_t to_argb(uint32_t fmt, const uint8_t *p) {
    uint32_t v16 = p[0] | (p[1] << 8);
    switch (fmt) {
    case 0x06: case 0x12: return RD32(p);
    case 0x07: case 0x1E: return RD32(p) | 0xff000000u;
    case 0x3A: case 0x3F: return (p[3] << 24) | (p[0] << 16) | (p[1] << 8) | p[2];          /* A8B8G8R8 */
    case 0x3B: case 0x40: return (p[0] << 24) | (p[3] << 16) | (p[2] << 8) | p[1];          /* B8G8R8A8 */
    case 0x3C: case 0x41: return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];          /* R8G8B8A8 */
    case 0x05: case 0x11: { /* R5G6B5 */
        uint32_t r = (v16 >> 11) & 31, g = (v16 >> 5) & 63, b = v16 & 31;
        return 0xff000000u | ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
    }
    case 0x02: case 0x10: case 0x03: case 0x1C: { /* A1R5G5B5 / X1R5G5B5 */
        uint32_t a = (fmt == 0x03 || fmt == 0x1C || (v16 & 0x8000)) ? 255 : 0;
        uint32_t r = (v16 >> 10) & 31, g = (v16 >> 5) & 31, b = v16 & 31;
        return (a << 24) | ((r * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31);
    }
    case 0x04: case 0x1D: { /* A4R4G4B4 */
        uint32_t a = (v16 >> 12) & 15, r = (v16 >> 8) & 15, g = (v16 >> 4) & 15, b = v16 & 15;
        return (a * 17 << 24) | (r * 17 << 16) | (g * 17 << 8) | (b * 17);
    }
    case 0x38: case 0x3D: { /* R5G5B5A1 */
        uint32_t r = (v16 >> 11) & 31, g = (v16 >> 6) & 31, b = (v16 >> 1) & 31, a = (v16 & 1) ? 255 : 0;
        return (a << 24) | ((r * 255 / 31) << 16) | ((g * 255 / 31) << 8) | (b * 255 / 31);
    }
    case 0x39: case 0x3E: { /* R4G4B4A4 */
        uint32_t r = (v16 >> 12) & 15, g = (v16 >> 8) & 15, b = (v16 >> 4) & 15, a = v16 & 15;
        return (a * 17 << 24) | (r * 17 << 16) | (g * 17 << 8) | (b * 17);
    }
    case 0x00: case 0x13: return 0xff000000u | (p[0] << 16) | (p[0] << 8) | p[0];          /* L8 */
    case 0x19: case 0x1F: return (p[0] << 24) | 0xffffff;                                   /* A8 */
    case 0x01: case 0x1B: return (p[0] << 24) | (p[0] << 16) | (p[0] << 8) | p[0];          /* AL8 */
    case 0x1A: case 0x20: return (p[1] << 24) | (p[0] << 16) | (p[0] << 8) | p[0];          /* A8L8 */
    case 0x0B: return 0xff000000u | (p[0] << 16) | (p[0] << 8) | p[0];                      /* P8: palette not applied */
    default: return bpp_of(fmt) == 4 ? RD32(p) : (0xff000000u | v16);
    }
}

/* Interim: the front end fills some textures of swizzled formats (its
   bitmaps: logo, portraits, button icons) with linear rows and swizzles
   others itself (font atlases), and why is not understood yet. Pick the
   layout under which horizontally adjacent texels differ least: real
   images are smooth, mis-read ones are not. */
static uint32_t morton_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t r = 0, bit = 0;
    for (uint32_t i = 1; i < w || i < h; i <<= 1) {
        if (i < w) r |= ((x & i) ? 1u : 0u) << bit++;
        if (i < h) r |= ((y & i) ? 1u : 0u) << bit++;
    }
    return r;
}
static int looks_linear(const uint8_t *src, uint32_t w, uint32_t h, uint32_t bpp) {
    if (w < 8 || h < 8 || getenv("RECOMP_NO_LAYOUT_GUESS")) return 0;
    uint64_t lin = 0, swz = 0;
    uint32_t step = h / 32 ? h / 32 : 1;
    for (uint32_t y = 0; y < h; y += step)
        for (uint32_t x = 0; x + 1 < w; x++) {
            const uint8_t *a = src + (y * w + x) * bpp, *b = a + bpp;
            const uint8_t *c = src + morton_index(x, y, w, h) * bpp, *d = src + morton_index(x + 1, y, w, h) * bpp;
            for (uint32_t k = 0; k < bpp; k++) { lin += abs(a[k] - b[k]); swz += abs(c[k] - d[k]); }
        }
    return lin < swz;
}

static void upload(TexSlot *t) {
    uint32_t w, h, fmt, pitch;
    d3d_describe(t->guest, 0, &w, &h, &fmt, &pitch);
    if (!w || !h || w > 4096 || h > 4096) return;
    uint32_t data = RD32(MEM(t->guest + 4));
    if (!data) return;
    int dxt = fmt == 0x0C || fmt == 0x0E || fmt == 0x0F;
    D3DFORMAT hostfmt = dxt ? (fmt == 0x0C ? D3DFMT_DXT1 : fmt == 0x0E ? D3DFMT_DXT3 : D3DFMT_DXT5) : D3DFMT_A8R8G8B8;
    if (t->tex && (t->w != w || t->h != h || t->fmt != fmt)) { IDirect3DTexture9_Release(t->tex); t->tex = NULL; }
    if (!t->tex) {
        if (FAILED(IDirect3DDevice9_CreateTexture(g_dev, w, h, 1, 0, hostfmt, D3DPOOL_MANAGED, &t->tex, NULL))) {
            fprintf(stderr, "render: cannot create %ux%u texture (xbox format %02x)\n", w, h, fmt);
            t->tex = NULL; t->dirty = 0;
            return;
        }
        t->w = w; t->h = h; t->fmt = fmt;
    }
    const uint8_t *src = MEM(data);
    t->linear = is_linear(fmt) || d3d_has_linear_layout(t->guest);
    if (!t->linear && !dxt && looks_linear(src, w, h, bpp_of(fmt))) t->linear = 1;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DTexture9_LockRect(t->tex, 0, &lr, NULL, 0))) return;
    if (dxt) {
        uint32_t bh = h < 4 ? 1 : h / 4;
        for (uint32_t y = 0; y < bh; y++) memcpy((uint8_t *)lr.pBits + y * lr.Pitch, src + y * pitch, pitch);
    } else {
        uint32_t bpp = bpp_of(fmt);
        uint8_t *lin = (uint8_t *)src;
        uint8_t *tmp = NULL;
        if (!t->linear && !getenv("RECOMP_RAWTEX")) { /* swizzled: straighten into a scratch copy first */
            tmp = malloc(w * h * bpp);
            unswizzle(src, w, h, bpp, tmp, w * bpp);
            lin = tmp;
            pitch = w * bpp;
        }
        for (uint32_t y = 0; y < h; y++) {
            uint32_t *out = (uint32_t *)((uint8_t *)lr.pBits + y * lr.Pitch);
            const uint8_t *in = lin + y * pitch;
            for (uint32_t x = 0; x < w; x++) out[x] = to_argb(fmt, in + x * bpp);
        }
        free(tmp);
    }
    if (getenv("RECOMP_DUMPTEX")) { /* debugging: write the uploaded texels as a BMP, and the guest bytes raw */
        char name[64];
        snprintf(name, sizeof name, "build\\tex_%08x_%02x_%ux%u.raw", t->guest, fmt, w, h);
        FILE *raw = fopen(name, "wb");
        if (raw) { fwrite(src, 1, dxt ? pitch * (h < 4 ? 1 : h / 4) : w * h * bpp_of(fmt), raw); fclose(raw); }
        snprintf(name, sizeof name, "build\\tex_%08x_%02x.bmp", t->guest, fmt);
        FILE *f = fopen(name, "wb");
        if (f) {
            uint32_t rowbytes = w * 4, size = 54 + rowbytes * h;
            uint8_t hdr[54] = {'B', 'M', size, size >> 8, size >> 16, size >> 24, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                               w, w >> 8, w >> 16, w >> 24, h, h >> 8, h >> 16, h >> 24, 1, 0, 32, 0};
            fwrite(hdr, 1, 54, f);
            for (int y = (int)h - 1; y >= 0; y--) fwrite((uint8_t *)lr.pBits + y * lr.Pitch, 1, rowbytes, f);
            fclose(f);
        }
    }
    IDirect3DTexture9_UnlockRect(t->tex, 0);
    t->dirty = 0;
}

void r_set_texture(uint32_t stage, uint32_t guest) {
    if (stage >= 4) return;
    g_bound[stage] = guest;
    g_stage_dirty = 1;
}

/* bind the guest textures to the device, uploading what changed */
static void apply_textures(void) {
    if (!g_stage_dirty) return;
    g_stage_dirty = 0;
    for (uint32_t s = 0; s < 4; s++) {
        uint32_t g = g_bound[s];
        if (!g) { IDirect3DDevice9_SetTexture(g_dev, s, NULL); continue; }
        TexSlot *t = tex_find(g, 1);
        if (!t) continue;
        if (t->dirty) upload(t);
        IDirect3DDevice9_SetTexture(g_dev, s, (IDirect3DBaseTexture9 *)t->tex);
        /* the game's texture matrix, followed (for linear textures, which the
           console addresses in texels) by a scale to normalised coordinates */
        D3DMATRIX m;
        memcpy(&m, g_texmat[s], 64);
        uint32_t flags = g_texflags[s];
        if (t->tex && t->linear) {
            float sx = 1.0f / t->w, sy = 1.0f / t->h;
            for (int r = 0; r < 4; r++) { m.m[r][0] *= sx; m.m[r][1] *= sy; }
        }
        /* the console applies a set texture matrix without a separate enable;
           the front end maps vertex positions to texels through it */
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (!flags && (t->linear || memcmp(g_texmat[s], identity, 64) != 0)) flags = D3DTTFF_COUNT2;
        IDirect3DDevice9_SetTransform(g_dev, D3DTS_TEXTURE0 + s, &m);
        IDirect3DDevice9_SetTextureStageState(g_dev, s, D3DTSS_TEXTURETRANSFORMFLAGS, flags);
    }
}

/* ---- state ------------------------------------------------------------------ */
static uint32_t gl_cmp(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 + 1 : D3DCMP_ALWAYS; }
static uint32_t gl_blend(uint32_t v) {
    switch (v) {
    case 0: return D3DBLEND_ZERO; case 1: return D3DBLEND_ONE;
    case 0x300: return D3DBLEND_SRCCOLOR; case 0x301: return D3DBLEND_INVSRCCOLOR;
    case 0x302: return D3DBLEND_SRCALPHA; case 0x303: return D3DBLEND_INVSRCALPHA;
    case 0x304: return D3DBLEND_DESTALPHA; case 0x305: return D3DBLEND_INVDESTALPHA;
    case 0x306: return D3DBLEND_DESTCOLOR; case 0x307: return D3DBLEND_INVDESTCOLOR;
    case 0x308: return D3DBLEND_SRCALPHASAT;
    case 0x8001: return D3DBLEND_BLENDFACTOR; case 0x8002: return D3DBLEND_INVBLENDFACTOR;
    default: return D3DBLEND_ONE;
    }
}
static uint32_t gl_stencil_op(uint32_t v) {
    switch (v) {
    case 0x1E00: return D3DSTENCILOP_KEEP; case 0: return D3DSTENCILOP_ZERO; case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT; case 0x1E03: return D3DSTENCILOP_DECRSAT; case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR; case 0x8508: return D3DSTENCILOP_DECR;
    default: return D3DSTENCILOP_KEEP;
    }
}
static uint32_t gl_blendop(uint32_t v) {
    switch (v) {
    case 0x8006: return D3DBLENDOP_ADD; case 0x800A: return D3DBLENDOP_SUBTRACT; case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN; case 0x8008: return D3DBLENDOP_MAX; default: return D3DBLENDOP_ADD;
    }
}
static uint32_t xbox_cull(uint32_t v) { return v == 0x900 ? D3DCULL_CW : v == 0x901 ? D3DCULL_CCW : D3DCULL_NONE; }

static void rs(D3DRENDERSTATETYPE s, uint32_t v) { if (g_dev) IDirect3DDevice9_SetRenderState(g_dev, s, v); }

void r_set_rs_simple(uint32_t method, uint32_t value) {
    /* the caller passes the push-buffer header (dword count in bits 18+,
       subchannel in 13..15), e.g. 0x40304 for ALPHABLENDENABLE */
    switch (method & 0x1FFC) {
    case 0x354: rs(D3DRS_ZFUNC, gl_cmp(value)); break;
    case 0x33C: rs(D3DRS_ALPHAFUNC, gl_cmp(value)); break;
    case 0x304: rs(D3DRS_ALPHABLENDENABLE, value != 0); break;
    case 0x300: rs(D3DRS_ALPHATESTENABLE, value != 0); break;
    case 0x340: rs(D3DRS_ALPHAREF, value & 0xff); break;
    case 0x344: rs(D3DRS_SRCBLEND, gl_blend(value)); break;
    case 0x348: rs(D3DRS_DESTBLEND, gl_blend(value)); break;
    case 0x35C: rs(D3DRS_ZWRITEENABLE, value != 0); break;
    case 0x310: rs(D3DRS_DITHERENABLE, value != 0); break;
    case 0x37C: rs(D3DRS_SHADEMODE, value == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD); break;
    case 0x358: /* NV2A colour mask: B bit 0, G bit 8, R bit 16, A bit 24 */
        rs(D3DRS_COLORWRITEENABLE, ((value & 1) ? D3DCOLORWRITEENABLE_BLUE : 0) | ((value & 0x100) ? D3DCOLORWRITEENABLE_GREEN : 0) |
                                   ((value & 0x10000) ? D3DCOLORWRITEENABLE_RED : 0) | ((value & 0x1000000) ? D3DCOLORWRITEENABLE_ALPHA : 0));
        break;
    case 0x374: rs(D3DRS_STENCILZFAIL, gl_stencil_op(value)); break;
    case 0x378: rs(D3DRS_STENCILPASS, gl_stencil_op(value)); break;
    case 0x364: rs(D3DRS_STENCILFUNC, gl_cmp(value)); break;
    case 0x368: rs(D3DRS_STENCILREF, value & 0xff); break;
    case 0x36C: rs(D3DRS_STENCILMASK, value & 0xff); break;
    case 0x360: rs(D3DRS_STENCILWRITEMASK, value & 0xff); break;
    case 0x350: rs(D3DRS_BLENDOP, gl_blendop(value)); break;
    case 0x34C: rs(D3DRS_BLENDFACTOR, ((value >> 24) << 24) | ((value & 0xff) << 16) | (value & 0xff00) | ((value >> 16) & 0xff)); break;
    default: break; /* polygon offset, swath, depth clip, stipple: nothing to map */
    }
}

/* Xbox D3DRS indices of the deferred group (XDK 3911 numbering, per Cxbx) */
enum { XRS_FOGENABLE = 92, XRS_FOGTABLEMODE, XRS_FOGSTART, XRS_FOGEND, XRS_FOGDENSITY, XRS_RANGEFOGENABLE,
       XRS_WRAP0, XRS_WRAP1, XRS_WRAP2, XRS_WRAP3, XRS_LIGHTING, XRS_SPECULARENABLE, XRS_LOCALVIEWER, XRS_COLORVERTEX,
       XRS_BACKSPECULARMATERIALSOURCE, XRS_BACKDIFFUSEMATERIALSOURCE, XRS_BACKAMBIENTMATERIALSOURCE, XRS_BACKEMISSIVEMATERIALSOURCE,
       XRS_SPECULARMATERIALSOURCE, XRS_DIFFUSEMATERIALSOURCE, XRS_AMBIENTMATERIALSOURCE, XRS_EMISSIVEMATERIALSOURCE,
       XRS_BACKAMBIENT, XRS_AMBIENT, XRS_POINTSIZE, XRS_POINTSIZE_MIN, XRS_POINTSPRITEENABLE, XRS_POINTSCALEENABLE,
       XRS_POINTSCALE_A, XRS_POINTSCALE_B, XRS_POINTSCALE_C, XRS_POINTSIZE_MAX, XRS_PATCHEDGESTYLE, XRS_PATCHSEGMENTS };

void r_set_rs_deferred(uint32_t state, uint32_t value) {
    switch (state) {
    case XRS_FOGENABLE: rs(D3DRS_FOGENABLE, value != 0); break;
    case XRS_FOGTABLEMODE: rs(D3DRS_FOGTABLEMODE, value == 0x2601 ? D3DFOG_LINEAR : value == 0x800 ? D3DFOG_EXP : value == 0x801 ? D3DFOG_EXP2 : D3DFOG_NONE); break;
    case XRS_FOGSTART: rs(D3DRS_FOGSTART, value); break;
    case XRS_FOGEND: rs(D3DRS_FOGEND, value); break;
    case XRS_FOGDENSITY: rs(D3DRS_FOGDENSITY, value); break;
    case XRS_RANGEFOGENABLE: rs(D3DRS_RANGEFOGENABLE, value != 0); break;
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

void r_set_rs(uint32_t pc_state, uint32_t value) {
    if (pc_state == D3DRS_CULLMODE) value = xbox_cull(value);
    else if (pc_state == D3DRS_STENCILFAIL) value = gl_stencil_op(value);
    else if (pc_state == D3DRS_ZENABLE) value = value ? D3DZB_TRUE : D3DZB_FALSE;
    rs(pc_state, value);
}

/* Xbox D3DTOP -> PC: equal up to BLENDDIFFUSEALPHA (12), reordered after
   (per Cxbx-Reloaded) */
static uint32_t xbox_top(uint32_t v) {
    static const uint8_t pc[] = {0, D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
                                 D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT,
                                 D3DTOP_ADDSMOOTH, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDTEXTUREALPHA,
                                 D3DTOP_BLENDFACTORALPHA, D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_PREMODULATE,
                                 D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR,
                                 D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP,
                                 D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE};
    return v < sizeof pc && v ? pc[v] : D3DTOP_DISABLE;
}

/* Xbox D3DTSS indices of D3D8 3948 (XDK 3911): the stage operations come
   first (COLOROP 0 .. TEXTURETRANSFORMFLAGS 9), then the sampler states
   (ADDRESSU 10 .. ALPHAKILL 21); deferred 0..21, then the rest. Later XDKs
   moved the sampler states to the front. */
void r_set_tss(uint32_t stage, uint32_t type, uint32_t value) {
    if (!g_dev || stage >= 4) return;
    if (g_drawlog > 0) fprintf(stderr, "tss stage %u type %u = %08x\n", stage, type, value);
    switch (type) {
    case 0: g_stage_ops[stage][0] = value; IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLOROP, xbox_top(value)); break;
    case 1: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLORARG0, value); break;
    case 2: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLORARG1, value); break;
    case 3: IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_COLORARG2, value); break;
    case 4: g_stage_ops[stage][1] = value; IDirect3DDevice9_SetTextureStageState(g_dev, stage, D3DTSS_ALPHAOP, xbox_top(value)); break;
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
    /* 19 COLORKEYOP, 20 COLORSIGN, 21 ALPHAKILL: no Direct3D 9 equivalent */
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

static uint32_t fvf_stride(uint32_t fvf) {
    uint32_t n = 0;
    switch (fvf & 0xE) {
    case 0x2: n = 12; break;                           /* XYZ */
    case 0x4: n = 16; break;                           /* XYZRHW */
    case 0x6: n = 16; break; case 0x8: n = 20; break;  /* XYZB1..B4 */
    case 0xA: n = 24; break; case 0xC: n = 28; break;
    }
    if (fvf & 0x10) n += 12;  /* normal */
    if (fvf & 0x20) n += 4;   /* point size */
    if (fvf & 0x40) n += 4;   /* diffuse */
    if (fvf & 0x80) n += 4;   /* specular */
    uint32_t tc = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < tc; i++) {
        uint32_t sz = (fvf >> (16 + i * 2)) & 3; /* 0: 2 floats, 1: 3, 2: 4, 3: 1 */
        n += sz == 0 ? 8 : sz == 1 ? 12 : sz == 2 ? 16 : 4;
    }
    return n;
}

void r_set_vertex_shader(uint32_t handle) {
    g_vs_handle = handle;
    if (handle & 1) { /* a vertex shader object, not translated yet */
        if (!g_warned_vs++) fprintf(stderr, "render: programmable vertex shaders are not translated yet (handle %08x)\n", handle);
        g_fvf = 0;
        return;
    }
    g_fvf = handle;
    if (g_dev) IDirect3DDevice9_SetFVF(g_dev, handle & ~0x1u);
}
void r_set_pixel_shader(uint32_t handle) { g_ps_handle = handle; }

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
    uint32_t bound[4], texflags[4], stage_ops[4][2];
    float texmat[4][16];
    uint32_t fvf, vs_handle, ps_handle, stream_vb, stream_stride;
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
    b->fvf = g_fvf; b->vs_handle = g_vs_handle; b->ps_handle = g_ps_handle;
    b->stream_vb = g_stream_vb; b->stream_stride = g_stream_stride;
}
void r_apply_state_block(uint32_t token) {
    if (token - 1 >= STATE_BLOCKS || !g_sb[token - 1].sb) return;
    StateBlock *b = &g_sb[token - 1];
    IDirect3DStateBlock9_Apply(b->sb);
    memcpy(g_bound, b->bound, sizeof g_bound); memcpy(g_texflags, b->texflags, sizeof g_texflags);
    memcpy(g_stage_ops, b->stage_ops, sizeof g_stage_ops); memcpy(g_texmat, b->texmat, sizeof g_texmat);
    g_fvf = b->fvf; g_vs_handle = b->vs_handle; g_ps_handle = b->ps_handle;
    g_stream_vb = b->stream_vb; g_stream_stride = b->stream_stride;
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
static uint32_t g_frame;
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
    if (!g_dev || !g_fvf || !n || !stride) return;
    drawlog(prim, n, data, stride);
    statelog(prim, n, data, stride);
    apply_textures();
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
        return;
    }
    uint32_t count;
    D3DPRIMITIVETYPE t = prim_pc(prim, n, &count);
    if (count) IDirect3DDevice9_DrawPrimitiveUP(g_dev, t, count, data, stride);
}

void r_draw(uint32_t prim, uint32_t start, uint32_t n) {
    if (!g_stream_vb) return;
    uint32_t stride = g_stream_stride ? g_stream_stride : fvf_stride(g_fvf);
    uint32_t data = RD32(MEM(g_stream_vb + 4));
    if (!data) return;
    draw_up(prim, n, MEM(data + start * stride), stride);
}

void r_draw_up(uint32_t prim, uint32_t n, uint32_t data, uint32_t stride) {
    if (!data) return;
    draw_up(prim, n, MEM(data), stride ? stride : fvf_stride(g_fvf));
}

void r_draw_indexed(uint32_t prim, uint32_t n, uint32_t index_data) {
    if (!g_dev || !g_fvf || !g_stream_vb || !index_data || !n) return;
    uint32_t stride = g_stream_stride ? g_stream_stride : fvf_stride(g_fvf);
    uint32_t data = RD32(MEM(g_stream_vb + 4));
    if (!data) return;
    const uint16_t *idx = (const uint16_t *)MEM(index_data);
    uint32_t maxv = 0;
    for (uint32_t i = 0; i < n; i++) if (idx[i] > maxv) maxv = idx[i];
    apply_textures();
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
        return;
    }
    if (count) IDirect3DDevice9_DrawIndexedPrimitiveUP(g_dev, t, 0, maxv + 1, count, idx, D3DFMT_INDEX16, MEM(data), stride);
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
    const char *list = getenv("RECOMP_SHOT");
    for (const char *p = list; p && *p; p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
        if ((uint32_t)atoi(p) == frame) goto take;
    return;
take:;
    IDirect3DSurface9 *bb = NULL, *sys = NULL;
    D3DSURFACE_DESC d;
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(g_dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    IDirect3DSurface9_GetDesc(bb, &d);
    if (SUCCEEDED(IDirect3DDevice9_CreateOffscreenPlainSurface(g_dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL)) &&
        SUCCEEDED(IDirect3DDevice9_GetRenderTargetData(g_dev, bb, sys)) &&
        SUCCEEDED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) {
        char name[64];
        snprintf(name, sizeof name, "build\\shot_%u.bmp", frame);
        FILE *f = fopen(name, "wb");
        if (f) {
            uint32_t w = d.Width, h = d.Height, size = 54 + w * 4 * h;
            uint8_t hdr[54] = {'B', 'M', size, size >> 8, size >> 16, size >> 24, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                               w, w >> 8, w >> 16, w >> 24, h, h >> 8, h >> 16, h >> 24, 1, 0, 32, 0};
            fwrite(hdr, 1, 54, f);
            for (int y = (int)h - 1; y >= 0; y--) fwrite((uint8_t *)lr.pBits + y * lr.Pitch, 1, w * 4, f);
            fclose(f);
            fprintf(stderr, "render: saved %s\n", name);
        }
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
