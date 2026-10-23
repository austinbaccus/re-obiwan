/* render.h on the backend chosen at startup: Direct3D 11, or with
   RECOMP_RENDERER=d3d9 the Direct3D 9 renderer it was ported from (kept as
   the reference to compare against). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render.h"
#include "render_backend.h"

#define DECLARE(ret, name, params, args) ret d3d9_##name params; ret d3d11_##name params;
#define DECLARE_VOID(name, params, args) void d3d9_##name params; void d3d11_##name params;
RENDER_API(DECLARE, DECLARE_VOID)

static int use_d3d9(void) {
    static int d3d9 = -1;
    if (d3d9 < 0) {
        const char *e = getenv("RECOMP_RENDERER");
        d3d9 = e && !_stricmp(e, "d3d9");
    }
    return d3d9;
}

#define FORWARD(ret, name, params, args) ret name params { return use_d3d9() ? d3d9_##name args : d3d11_##name args; }
#define FORWARD_VOID(name, params, args) void name params { if (use_d3d9()) d3d9_##name args; else d3d11_##name args; }
RENDER_API(FORWARD, FORWARD_VOID)
