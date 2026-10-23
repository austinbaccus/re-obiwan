/* What the host renderers (render_d3d9.c, render_d3d11.c) share: the guest's
   texture and vertex formats, the Xbox D3D8 state encodings, screenshots. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* ---- textures ---------------------------------------------------------------- */
typedef struct {
    uint32_t guest, w, h, fmt, pitch; /* level 0 as d3d_describe gives it */
    const uint8_t *src;               /* the texels in guest memory */
    int dxt, linear;                  /* DXT1/3/5; rows rather than swizzled */
} RcTex;

/* level 0 of a guest texture, or 0 when there is nothing to draw */
int rc_texture(uint32_t guest, RcTex *t);
/* bytes of guest memory level 0 covers */
size_t rc_texture_bytes(const RcTex *t);
/* level 0 as A8R8G8B8 rows of w * 4 bytes (DXT decoded) */
void rc_texture_argb(const RcTex *t, uint32_t *dst);
/* RECOMP_DUMPTEX=1 or a guest texture address: build\tex_*.bmp and the raw bytes */
void rc_dump_texture(const RcTex *t);
/* the NV2A compares shadow r in the depth buffer's own units; the host 0..1 */
float rc_depth_unit_scale(uint32_t guest);

/* ---- vertices ----------------------------------------------------------------- */
typedef struct { uint8_t reg, type; uint16_t offset; } RcVElem;
/* Xbox vertex declaration tokens to stream 0 elements; -1 when unsupported */
int rc_parse_vertex_decl(const uint32_t *decl, int ndecl, RcVElem *elems, int max);
/* the vertices first..first + count - 1 as one float4 per shader input regs[k] */
void rc_expand_vertices(const RcVElem *elems, int nelems, const uint8_t *regs, int nregs, const uint8_t *verts, uint32_t stride,
                        uint32_t first, uint32_t count, float *out);
uint32_t rc_fvf_stride(uint32_t fvf);

/* ---- state encodings -------------------------------------------------------------- */
/* NV2A (GL-style) values of the simple render states to D3D numbering, which
   Direct3D 9 and 11 share for these: D3DCMP / D3D11_COMPARISON, D3DBLEND /
   D3D11_BLEND, D3DSTENCILOP / D3D11_STENCIL_OP, D3DBLENDOP / D3D11_BLEND_OP */
uint32_t rc_gl_cmp(uint32_t v);
uint32_t rc_gl_blend(uint32_t v);
uint32_t rc_gl_stencil_op(uint32_t v);
uint32_t rc_gl_blendop(uint32_t v);
uint32_t rc_xbox_cull(uint32_t v); /* D3DCULL */
uint32_t rc_xbox_top(uint32_t v);  /* Xbox D3DTOP to the PC's */

/* Xbox D3DRS indices of the deferred group in D3D8 3948: right after the 25
   simple states (57..81; D3DDevice_SetRenderState at 0x3690c0 sends states
   below 82 to the simple setter and writes 82..115 into D3D_g_RenderState at
   0x37AC68). The values are the game's own D3D8 values. */
#define XRS_DEFERRED_FIRST 82
#define XRS_DEFERRED_COUNT 34
#define XRS_STATE_ARRAY 0x0037ac68
enum { XRS_FOGENABLE = XRS_DEFERRED_FIRST, XRS_FOGTABLEMODE, XRS_FOGSTART, XRS_FOGEND, XRS_FOGDENSITY, XRS_RANGEFOGENABLE,
       XRS_WRAP0, XRS_WRAP1, XRS_WRAP2, XRS_WRAP3, XRS_LIGHTING, XRS_SPECULARENABLE, XRS_LOCALVIEWER, XRS_COLORVERTEX,
       XRS_BACKSPECULARMATERIALSOURCE, XRS_BACKDIFFUSEMATERIALSOURCE, XRS_BACKAMBIENTMATERIALSOURCE, XRS_BACKEMISSIVEMATERIALSOURCE,
       XRS_SPECULARMATERIALSOURCE, XRS_DIFFUSEMATERIALSOURCE, XRS_AMBIENTMATERIALSOURCE, XRS_EMISSIVEMATERIALSOURCE,
       XRS_BACKAMBIENT, XRS_AMBIENT, XRS_POINTSIZE, XRS_POINTSIZE_MIN, XRS_POINTSPRITEENABLE, XRS_POINTSCALEENABLE,
       XRS_POINTSCALE_A, XRS_POINTSCALE_B, XRS_POINTSCALE_C, XRS_POINTSIZE_MAX, XRS_PATCHEDGESTYLE, XRS_PATCHSEGMENTS };

/* ---- debugging -------------------------------------------------------------------- */
/* RECOMP_SHOT="frame,frame,...": 1 when this present's frame is listed;
   name gets build\shot_<frame>.bmp (or shot_<RECOMP_RUN>_<frame>.bmp) */
int rc_shot_wanted(uint32_t frame, char *name, size_t cap);
/* top-down rows of 32-bit BGRA pixels as a BMP */
int rc_write_bmp(const char *name, const void *rows, size_t pitch, uint32_t w, uint32_t h);
