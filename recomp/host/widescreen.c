/* Widescreen: when RECOMP_RES is wider than 4:3, the game's cameras keep
   their vertical field of view and see further to the sides ("Hor+").

   Every camera sets its field of view through FUN_0002f2a0(h, v), which
   stores the angles in degrees and their half-angle cotangents. The
   projection (CD3DContext::vf19), the view frustum used for culling and the
   game's own world-to-screen projection are all built from those, so
   widening h there widens all three together. The 2D layer stays 4:3; the
   renderer squeezes it into the middle (render_d3d11.c). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>

#include "guest.h"
#include "kernel.h"
#include "render.h"

#define FOV_SET 0x0002f2a0u       /* camera->SetFov(h, v), thiscall, ret 8 */
#define DEG_TO_RAD 0x0048ccd0u    /* the game's pi / 180 (a float, set at startup) */

static float g_wide = 1;          /* how many times wider than 4:3 */

static float rdf(uint32_t a) { float f; memcpy(&f, MEM(a), 4); return f; }
static void wrf(uint32_t a, float f) { memcpy(MEM(a), &f, 4); }

/* the original, with tan(h/2) scaled by g_wide */
static void set_fov(Ctx *ctx) {
    uint32_t cam = RD32(ctx->r + REG_ECX), esp = RD32(ctx->r + REG_ESP);
    float h = rdf(esp + 4), v = rdf(esp + 8);
    double rad = rdf(DEG_TO_RAD);
    if (rad == 0) rad = 3.14159265358979 / 180;
    double th = tan(h * 0.5 * rad) * g_wide, tv = tan(v * 0.5 * rad);
    wrf(cam + 0x38, (float)(2 * atan(th) / rad));
    wrf(cam + 0x48, v);
    wrf(cam + 0x3c, (float)(1 / th));
    wrf(cam + 0x4c, (float)(1 / tv));
    wrf(cam + 0x44, (float)(1 / th * rdf(cam + 0x40)));
    wrf(cam + 0x54, (float)(1 / tv * rdf(cam + 0x50)));
    ctx->ret_ip = RD32(MEM(esp));
    WR32(ctx->r + REG_ESP, esp + 12);
}

void widescreen_init(void) {
    g_wide = r_wide();
    if (g_wide < 1.001f) return;
    if (host_override(FOV_SET, set_fov))
        fprintf(stderr, "widescreen: %.3f times wider than 4:3, cameras see further to the sides\n", g_wide);
}
