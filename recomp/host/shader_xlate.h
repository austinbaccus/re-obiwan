/* Xbox shader source (DX8-style "vs.1.1" / "xvs.1.1" assembly, as the game
   hands it to XGAssembleShader) to HLSL for the host renderer. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Translate one vertex shader. Writes HLSL with entry point "main" (target
   vs_2_0) to out and the input registers it reads (bit n = vn) to *inputs.
   Returns 0, or -1 with a message in out. Constants c0..c95 are the array
   "c" in registers c0..c95; inputs vn have semantic TEXCOORDn. */
int xs_vs_to_hlsl(const char *src, size_t len, char *out, size_t cap, uint32_t *inputs);
