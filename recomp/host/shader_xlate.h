/* Xbox shader source (DX8-style "vs.1.1" / "xvs.1.1" assembly, as the game
   hands it to XGAssembleShader) to HLSL for the host renderer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Translate one vertex shader. Writes HLSL with entry point "main" (target
   vs_2_0) to out and the input registers it reads (bit n = vn) to *inputs.
   Returns 0, or -1 with a message in out. Constants c0..c95 are the array
   "c" in registers c0..c95; inputs vn have semantic TEXCOORDn. c96 holds the
   host's fog parameters (start, end, density, D3DFOG mode) for the fog
   factor computed from oFog. */
int xs_vs_to_hlsl(const char *src, size_t len, char *out, size_t cap, uint32_t *inputs);

/* Translate one pixel shader ("ps.1.1" / "xps.1.1", NV2A register combiner
   style). HLSL entry "main", target ps_2_0: constants c0..c7, samplers
   s0..s3 (2D, or cube for texm3x3 lookups), inputs COLOR0/1 and TEXCOORD0..3.
   *fog is 0 when the final combiner leaves fog out (the device must not fog). */
int xs_ps_to_hlsl(const char *src, size_t len, char *out, size_t cap, int *fog);
int xs_is_pixel_shader(const char *src, size_t len);
