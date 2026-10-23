/* Shared by the host renderers; see render_common.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "render.h"
#include "render_common.h"

/* ---- textures ---------------------------------------------------------------- */
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

int rc_texture(uint32_t guest, RcTex *t) {
    memset(t, 0, sizeof *t);
    t->guest = guest;
    d3d_describe(guest, 0, &t->w, &t->h, &t->fmt, &t->pitch);
    if (getenv("RECOMP_TEXLOG")) {
        uint32_t f = RD32(MEM(guest + 0xC));
        fprintf(stderr, "texture %08x: format reg %08x (%s, dims %u, fmt %02x, %ux%u, %u levels) data %08x size %08x\n", guest, f,
                (f & 4) ? "cube" : "2d", (f >> 4) & 0xf, t->fmt, t->w, t->h, (f >> 16) & 0xf, RD32(MEM(guest + 4)), RD32(MEM(guest + 0x10)));
    }
    if (!t->w || !t->h || t->w > 4096 || t->h > 4096) return 0;
    uint32_t data = RD32(MEM(guest + 4));
    if (!data) return 0;
    t->src = MEM(data);
    t->dxt = t->fmt == 0x0C || t->fmt == 0x0E || t->fmt == 0x0F;
    /* the format code decides: given the real D3DSURFACE_DESC, the game lays
       its bitmaps out to match their format codes (with zero sizes it had
       written swizzled-format bitmaps as linear rows, #26) */
    t->linear = is_linear(t->fmt) || d3d_has_linear_layout(guest);
    return 1;
}

size_t rc_texture_bytes(const RcTex *t) {
    if (t->dxt) return (size_t)t->pitch * (t->h < 4 ? 1 : t->h / 4);
    return t->linear ? (size_t)t->pitch * t->h : (size_t)t->w * t->h * bpp_of(t->fmt);
}

static uint32_t rgb565(uint32_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}
static uint32_t mix(uint32_t a, uint32_t b, uint32_t wa, uint32_t wb) { /* (wa * a + wb * b) / (wa + wb) per channel */
    uint32_t o = 0;
    for (int s = 0; s < 24; s += 8) o |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / (wa + wb)) << s;
    return o;
}

/* DXT1/3/5 blocks to A8R8G8B8 (for levels the host cannot take compressed) */
static void decode_dxt(const RcTex *t, uint32_t *dst) {
    uint32_t bw = (t->w + 3) / 4, bh = (t->h + 3) / 4, bsize = t->fmt == 0x0C ? 8 : 16;
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            const uint8_t *b = t->src + by * t->pitch + bx * bsize;
            uint32_t alpha[16];
            if (t->fmt == 0x0E) /* DXT3: explicit 4-bit alpha */
                for (int i = 0; i < 16; i++) alpha[i] = ((b[i / 2] >> (4 * (i & 1))) & 15) * 17;
            else if (t->fmt == 0x0F) { /* DXT5: interpolated alpha */
                uint32_t a[8] = {b[0], b[1]};
                for (int i = 2; i < 8; i++)
                    a[i] = b[0] > b[1] ? ((8 - i) * b[0] + (i - 1) * b[1]) / 7 : i < 6 ? ((6 - i) * b[0] + (i - 1) * b[1]) / 5 : i == 6 ? 0 : 255;
                uint64_t bits = 0;
                for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = a[(bits >> (3 * i)) & 7];
            }
            const uint8_t *c = t->fmt == 0x0C ? b : b + 8;
            uint32_t c0 = c[0] | (c[1] << 8), c1 = c[2] | (c[3] << 8), idx = RD32(c + 4);
            uint32_t pal[4] = {0xff000000u | rgb565(c0), 0xff000000u | rgb565(c1)};
            if (c0 > c1 || t->fmt != 0x0C) {
                pal[2] = 0xff000000u | mix(pal[0], pal[1], 2, 1);
                pal[3] = 0xff000000u | mix(pal[0], pal[1], 1, 2);
            } else {
                pal[2] = 0xff000000u | mix(pal[0], pal[1], 1, 1);
                pal[3] = 0; /* transparent black */
            }
            for (int i = 0; i < 16; i++) {
                uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                if (x >= t->w || y >= t->h) continue;
                uint32_t p = pal[(idx >> (2 * i)) & 3];
                if (t->fmt != 0x0C) p = (p & 0xffffff) | (alpha[i] << 24);
                dst[y * t->w + x] = p;
            }
        }
}

void rc_texture_argb(const RcTex *t, uint32_t *dst) {
    if (t->dxt) { decode_dxt(t, dst); return; }
    uint32_t bpp = bpp_of(t->fmt), pitch = t->pitch;
    const uint8_t *lin = t->src;
    uint8_t *tmp = NULL;
    if (!t->linear && !getenv("RECOMP_RAWTEX")) { /* swizzled: straighten into a scratch copy first */
        tmp = malloc((size_t)t->w * t->h * bpp);
        unswizzle(t->src, t->w, t->h, bpp, tmp, t->w * bpp);
        lin = tmp;
        pitch = t->w * bpp;
    }
    for (uint32_t y = 0; y < t->h; y++) {
        const uint8_t *in = lin + (size_t)y * pitch;
        for (uint32_t x = 0; x < t->w; x++) dst[(size_t)y * t->w + x] = to_argb(t->fmt, in + x * bpp);
    }
    free(tmp);
}

void rc_dump_texture(const RcTex *t) {
    const char *dump = getenv("RECOMP_DUMPTEX");
    if (!dump || (strcmp(dump, "1") && strtoul(dump, NULL, 16) != t->guest)) return;
    char name[64];
    snprintf(name, sizeof name, "build\\tex_%08x_%02x_%ux%u.raw", t->guest, t->fmt, t->w, t->h);
    FILE *raw = fopen(name, "wb");
    if (raw) { fwrite(t->src, 1, rc_texture_bytes(t), raw); fclose(raw); }
    uint32_t *argb = malloc((size_t)t->w * t->h * 4);
    rc_texture_argb(t, argb);
    snprintf(name, sizeof name, "build\\tex_%08x_%02x.bmp", t->guest, t->fmt);
    rc_write_bmp(name, argb, (size_t)t->w * 4, t->w, t->h);
    free(argb);
}

float rc_depth_unit_scale(uint32_t guest) {
    switch ((RD32(MEM(guest + 0xC)) >> 8) & 0xff) {
    case 0x2A: case 0x2E: return 1.0f / 16777215.0f;   /* D24S8 */
    case 0x2C: case 0x30: return 1.0f / 65535.0f;      /* D16 */
    case 0x2D: case 0x31: return 1.0f / 511.9375f;     /* F16: 0 .. 511.9375 */
    default: return 1.0f;
    }
}

/* ---- vertices ----------------------------------------------------------------- */
static uint32_t vsdt_size(uint32_t type) {
    switch (type) {
    case 0x12: return 4;  case 0x22: return 8;  case 0x32: return 12; case 0x42: return 16;  /* FLOAT1..4 */
    case 0x40: return 4;  case 0x16: return 4;  case 0x72: return 12;                         /* D3DCOLOR, NORMPACKED3, FLOAT2H */
    case 0x11: return 2;  case 0x21: return 4;  case 0x31: return 6;  case 0x41: return 8;   /* NORMSHORT1..4 */
    case 0x15: return 2;  case 0x25: return 4;  case 0x35: return 6;  case 0x45: return 8;   /* SHORT1..4 */
    case 0x14: return 1;  case 0x24: return 2;  case 0x34: return 3;  case 0x44: return 4;   /* PBYTE1..4 */
    case 0x02: return 0;                                                                      /* NONE */
    default: return 0xffff;
    }
}

/* one element of an Xbox vertex as float4, missing components (0, 0, 0, 1) */
static void vsdt_fetch(uint32_t type, const uint8_t *p, float *o) {
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    uint32_t n = (type >> 4) & 0xf;
    switch (type & 0xf) {
    case 0x2:
        if (type == 0x72) { memcpy(o, p, 8); memcpy(o + 3, p + 8, 4); break; }   /* FLOAT2H: x, y, w */
        memcpy(o, p, 4 * n);
        break;
    case 0x0: /* D3DCOLOR: B, G, R, A in memory */
        o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f;
        break;
    case 0x6: { /* NORMPACKED3: signed 11:11:10 */
        uint32_t v; memcpy(&v, p, 4);
        int x = (int)(v << 21) >> 21, y = (int)(v << 10) >> 21, z = (int)v >> 22;
        o[0] = x / 1023.0f; o[1] = y / 1023.0f; o[2] = z / 511.0f;
        break;
    }
    case 0x1: for (uint32_t i = 0; i < n; i++) { int16_t s; memcpy(&s, p + 2 * i, 2); o[i] = s / 32767.0f; } break;
    case 0x5: for (uint32_t i = 0; i < n; i++) { int16_t s; memcpy(&s, p + 2 * i, 2); o[i] = s; } break;
    case 0x4: for (uint32_t i = 0; i < n; i++) o[i] = p[i] / 255.0f; break;
    }
}

int rc_parse_vertex_decl(const uint32_t *decl, int ndecl, RcVElem *elems, int max) {
    /* Xbox D3DVSD tokens: STREAM (1 << 29 | n), STREAMDATA (2 << 29 | type << 16 | reg),
       SKIP (2 << 29 | 0x10000000 | dwords << 16), END 0xFFFFFFFF */
    uint32_t stream = 0, offset = 0;
    int n = 0;
    for (int i = 0; i < ndecl && decl[i] != 0xFFFFFFFF; i++) {
        uint32_t t = decl[i], kind = t >> 29;
        if (kind == 1) { stream = t & 0xf; offset = 0; continue; }
        if (kind != 2) { fprintf(stderr, "render: vertex declaration token %08x not handled\n", t); continue; }
        if (t & 0x10000000) { offset += ((t >> 16) & 0xf) * 4; continue; }
        uint32_t type = (t >> 16) & 0xff, size = vsdt_size(type);
        if (size == 0xffff) { fprintf(stderr, "render: vertex data type %02x not handled\n", type); return -1; }
        if (stream != 0) { fprintf(stderr, "render: vertex stream %u not handled\n", stream); return -1; }
        if (n < max) elems[n++] = (RcVElem){(uint8_t)(t & 0x1f), (uint8_t)type, (uint16_t)offset};
        offset += size;
    }
    return n;
}

void rc_expand_vertices(const RcVElem *elems, int nelems, const uint8_t *regs, int nregs, const uint8_t *verts, uint32_t stride,
                        uint32_t first, uint32_t count, float *out) {
    for (uint32_t v = 0; v < count; v++) {
        const uint8_t *src = verts + (size_t)(first + v) * stride;
        for (int k = 0; k < nregs; k++) {
            float *o = out + ((size_t)v * nregs + k) * 4;
            o[0] = o[1] = o[2] = 0; o[3] = 1;
            for (int e = 0; e < nelems; e++)
                if (elems[e].reg == regs[k]) vsdt_fetch(elems[e].type, src + elems[e].offset, o);
        }
    }
}

uint32_t rc_fvf_stride(uint32_t fvf) {
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

/* ---- state encodings -------------------------------------------------------------- */
uint32_t rc_gl_cmp(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 + 1 : D3DCMP_ALWAYS; }
uint32_t rc_gl_blend(uint32_t v) {
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
uint32_t rc_gl_stencil_op(uint32_t v) {
    switch (v) {
    case 0x1E00: return D3DSTENCILOP_KEEP; case 0: return D3DSTENCILOP_ZERO; case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT; case 0x1E03: return D3DSTENCILOP_DECRSAT; case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR; case 0x8508: return D3DSTENCILOP_DECR;
    default: return D3DSTENCILOP_KEEP;
    }
}
uint32_t rc_gl_blendop(uint32_t v) {
    switch (v) {
    case 0x8006: return D3DBLENDOP_ADD; case 0x800A: return D3DBLENDOP_SUBTRACT; case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN; case 0x8008: return D3DBLENDOP_MAX; default: return D3DBLENDOP_ADD;
    }
}
uint32_t rc_xbox_cull(uint32_t v) { return v == 0x900 ? D3DCULL_CW : v == 0x901 ? D3DCULL_CCW : D3DCULL_NONE; }

/* Xbox D3DTOP -> PC: equal up to BLENDDIFFUSEALPHA (12), reordered after
   (per Cxbx-Reloaded) */
uint32_t rc_xbox_top(uint32_t v) {
    static const uint8_t pc[] = {0, D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X,
                                 D3DTOP_MODULATE4X, D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT,
                                 D3DTOP_ADDSMOOTH, D3DTOP_BLENDDIFFUSEALPHA, D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDTEXTUREALPHA,
                                 D3DTOP_BLENDFACTORALPHA, D3DTOP_BLENDTEXTUREALPHAPM, D3DTOP_PREMODULATE,
                                 D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR,
                                 D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP,
                                 D3DTOP_BUMPENVMAP, D3DTOP_BUMPENVMAPLUMINANCE};
    return v < sizeof pc && v ? pc[v] : D3DTOP_DISABLE;
}

/* ---- debugging -------------------------------------------------------------------- */
int rc_shot_wanted(uint32_t frame, char *name, size_t cap) {
    const char *list = getenv("RECOMP_SHOT"), *end = list ? strchr(list, '|') : NULL; /* '|': the next run's list */
    for (const char *p = list; p && *p && (!end || p < end); p = strchr(p, ',') ? strchr(p, ',') + 1 : "")
        if ((uint32_t)atoi(p) == frame) {
            if (getenv("RECOMP_RUN")) snprintf(name, cap, "build\\shot_%s_%u.bmp", getenv("RECOMP_RUN"), frame); /* a relaunched run */
            else snprintf(name, cap, "build\\shot_%u.bmp", frame);
            return 1;
        }
    return 0;
}

int rc_write_bmp(const char *name, const void *rows, size_t pitch, uint32_t w, uint32_t h) {
    FILE *f = fopen(name, "wb");
    if (!f) return 0;
    uint32_t size = 54 + w * 4 * h;
    uint8_t hdr[54] = {'B', 'M', size, size >> 8, size >> 16, size >> 24, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0,
                       w, w >> 8, w >> 16, w >> 24, h, h >> 8, h >> 16, h >> 24, 1, 0, 32, 0};
    fwrite(hdr, 1, 54, f);
    for (int y = (int)h - 1; y >= 0; y--) fwrite((const uint8_t *)rows + y * pitch, 1, w * 4, f);
    fclose(f);
    return 1;
}
