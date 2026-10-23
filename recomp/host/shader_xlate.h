/* Xbox shader source (DX8-style "vs.1.1" / "xvs.1.1" assembly, as the game
   hands it to XGAssembleShader) to HLSL for the host renderer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* targets: XS_SM2 for Direct3D 9 (vs_2_0 / ps_2_0, constant registers,
   fog left to the device), XS_SM4 for Direct3D 11 (vs_4_0 / ps_4_0,
   constant buffers, the shaders fog and alpha test themselves) */
enum { XS_SM2, XS_SM4 };

/* Translate one vertex shader. Writes HLSL with entry point "main" to out and
   the input registers it reads (bit n = vn) to *inputs. Returns 0, or -1
   with a message in out. Inputs vn have semantic TEXCOORDn.
   SM2: constants c0..c95 are the array "c" in registers c0..c95; c96 holds
   the host's fog parameters (start, end, density, D3DFOG mode) for the fog
   factor computed from oFog.
   SM4: constant buffer b0 holds c[96], the fog parameters and hostpos, whose
   xy are added to the position times w (the Xbox's pixel centres); the
   output is struct VARYINGS (SV_Position, COLOR0/1, TEXCOORD0..3, FOG), the
   colours clamped. Defining COLOR_INTERP (nointerpolation) before the text
   gives the colours flat shading. */
int xs_vs_to_hlsl(const char *src, size_t len, int target, char *out, size_t cap, uint32_t *inputs);

/* Translate one pixel shader ("ps.1.1" / "xps.1.1", NV2A register combiner
   style). HLSL entry "main": constants c0..c7, texture stages 0..3 (2D, or
   cube for texm3x3 lookups), inputs COLOR0/1 and TEXCOORD0..3; texscale[4]
   scales each stage's coordinates (1/w, 1/h for linear textures, which the
   Xbox addresses in texels, else 1; z: depth units to 0..1 for a depth
   texture), and its w is the stage's ALPHAKILL threshold (a texel alpha below
   it drops the pixel; -1 when off). "tex" is projective.
   SM2: registers c0..c7, c8..c11 texscale, samplers s0..s3; *fog is 0 when
   the final combiner leaves fog out (the device must not fog).
   SM4: constant buffer b0 holds c[8], texscale[4], fogcolor (rgb) and
   alphatest (x: D3DCMP function, 0 when off; y: reference 0..255); stage n
   is texture tn with sampler sn, a SamplerComparisonState for the stages in
   depth_stages (bit n), which compare r/q with the depth texel; the input
   is the vertex shader's VARYINGS, whose fog is the fog factor (1 for none). */
int xs_ps_to_hlsl(const char *src, size_t len, int target, uint32_t depth_stages, char *out, size_t cap, int *fog);
/* the source's version token is ps.* / xps.* (vs.* / xvs.*), after any
   comments and #define lines */
int xs_is_pixel_shader(const char *src, size_t len);
int xs_is_vertex_shader(const char *src, size_t len);
