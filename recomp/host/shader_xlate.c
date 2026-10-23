/* Xbox vertex shader source to HLSL; see shader_xlate.h.

   The game assembles all its shaders at startup from DX8-style source text
   (XGAssembleShader), so the host translates that text rather than NV2A
   microcode. The language: one instruction per line ("op dst, src, ..."),
   ';' and '//' comments, "#define NAME text", "def cN, a, b, c, d",
   "macro NAME param ... endm" with %param and %inc(%param) (the register
   after it). Registers: vN inputs, rN temporaries (r12 reads oPos on the
   Xbox), cN / c[a0.x + N] constants, a0.x, and the outputs oPos, oD0/oD1,
   oT0..oT3, oFog, oPts, oB0/oB1. */
#include "shader_xlate.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char name[64], value[128]; } Define;
typedef struct { char name[64], params[4][32]; int nparams; char *body[64]; int nbody; } Macro;

typedef struct {
    char *body; size_t body_len, body_cap;
    Define defs[128]; int ndefs;
    Macro macros[8]; int nmacros;
    float defc[96][4]; uint8_t defined[96];
    uint32_t inputs;
    int depth, failed;
    char msg[200];
    /* pixel shaders */
    int ps, final_written, final_fog, m3_rows;
    uint8_t sampler[4];       /* 0 unused, 1 2D, 2 cube */
} Xs;

static void fail(Xs *x, const char *fmt, ...) {
    if (x->failed) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(x->msg, sizeof x->msg, fmt, ap);
    va_end(ap);
    x->failed = 1;
}

static void emit(Xs *x, const char *fmt, ...) {
    char line[512];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (x->body_len + n + 1 > x->body_cap) {
        x->body_cap = (x->body_cap + n + 1) * 2;
        x->body = realloc(x->body, x->body_cap);
    }
    memcpy(x->body + x->body_len, line, n + 1);
    x->body_len += n;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int is_ident(int c) { return isalnum(c) || c == '_'; }

/* replace whole-word occurrences of each #define, a few rounds for nesting */
static void substitute_defines(Xs *x, char *line, size_t cap) {
    for (int round = 0; round < 4; round++) {
        int changed = 0;
        char out[512]; size_t o = 0;
        for (const char *p = line; *p && o + 1 < sizeof out; ) {
            if (is_ident((unsigned char)*p) && (p == line || !is_ident((unsigned char)p[-1]))) {
                const char *e = p;
                while (is_ident((unsigned char)*e)) e++;
                const Define *d = NULL;
                for (int i = 0; i < x->ndefs; i++)
                    if (strlen(x->defs[i].name) == (size_t)(e - p) && !memcmp(x->defs[i].name, p, e - p)) d = &x->defs[i];
                if (d) {
                    size_t n = strlen(d->value);
                    if (o + n + 1 < sizeof out) { memcpy(out + o, d->value, n); o += n; }
                    p = e; changed = 1;
                    continue;
                }
                while (p < e && o + 1 < sizeof out) out[o++] = *p++;
                continue;
            }
            out[o++] = *p++;
        }
        out[o] = 0;
        snprintf(line, cap, "%s", out);
        if (!changed) return;
    }
}

/* split "a, b, c" at top-level commas (not inside [...]) */
static int split_operands(char *s, char **ops, int max) {
    int n = 0, depth = 0;
    char *start = s;
    for (char *p = s; ; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        else if ((*p == ',' && depth == 0) || *p == 0) {
            int end = *p == 0;
            *p = 0;
            if (n < max) ops[n++] = trim(start);
            if (end) break;
            start = p + 1;
        }
    }
    return n;
}

static const char *const OUTPUTS[] = {"oPos", "oD0", "oD1", "oT0", "oT1", "oT2", "oT3", "oFog", "oPts", "oB0", "oB1"};

/* a source operand as an HLSL float4 expression; scalar ops take a
   one-component swizzle, defaulting to .w as DX8 does */
static int source(Xs *x, const char *op, int scalar, char *out, size_t cap) {
    char base[96] = "", swz[8] = "";
    int neg = 0;
    while (isspace((unsigned char)*op)) op++;
    if (*op == '-') { neg = 1; op++; while (isspace((unsigned char)*op)) op++; }
    /* register part up to the swizzle dot (a dot inside [...] belongs to a0.x) */
    const char *dot = NULL;
    int depth = 0;
    for (const char *p = op; *p; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        else if (*p == '.' && depth == 0) dot = p;
    }
    size_t rlen = dot ? (size_t)(dot - op) : strlen(op);
    char reg[96];
    snprintf(reg, sizeof reg, "%.*s", (int)rlen, op);
    char *r = trim(reg);
    if (dot) {
        const char *s = dot + 1;
        size_t n = 0;
        for (; *s && n < 4; s++) {
            char c = (char)tolower((unsigned char)*s);
            if (c == 'r') c = 'x'; else if (c == 'g') c = 'y'; else if (c == 'b') c = 'z'; else if (c == 'a') c = 'w';
            if (c != 'x' && c != 'y' && c != 'z' && c != 'w') break;
            swz[n++] = c;
        }
        while (n > 0 && n < 4) { swz[n] = swz[n - 1]; n++; }
        swz[4] = 0;
    }
    if (scalar && !swz[0]) strcpy(swz, "wwww");

    if ((r[0] == 'v' || r[0] == 'V') && isdigit((unsigned char)r[1])) {
        int n = atoi(r + 1);
        if (n > 15) { fail(x, "bad input %s", r); return -1; }
        x->inputs |= 1u << n;
        snprintf(base, sizeof base, "v%d", n);
    } else if ((r[0] == 'r' || r[0] == 'R') && isdigit((unsigned char)r[1])) {
        int n = atoi(r + 1);
        if (n == 12) strcpy(base, "oPos");       /* the Xbox reads oPos back as r12 */
        else if (n > 11) { fail(x, "bad temporary %s", r); return -1; }
        else snprintf(base, sizeof base, "r%d", n);
    } else if (r[0] == 'c' && r[1] == '[') {
        /* c[a0.x + N], c[N + a0.x], c[a0.x], c[N] */
        const char *in = r + 2;
        int rel = strstr(in, "a0") != NULL, off = 0, sign = 1;
        for (const char *p = in; *p && *p != ']'; ) {
            if (p[0] == 'a' && p[1] == '0') { p += 2; if (*p == '.') p += 2; continue; }
            if (*p == '+') { sign = 1; p++; continue; }
            if (*p == '-') { sign = -1; p++; continue; }
            if (isdigit((unsigned char)*p)) { off += sign * (int)strtol(p, (char **)&p, 10); continue; }
            if (isspace((unsigned char)*p)) { p++; continue; }
            fail(x, "bad constant index %s", r);
            return -1;
        }
        if (rel) snprintf(base, sizeof base, "c[a0 + %d]", off);
        else if (off >= 0 && off < 96 && x->defined[off])
            snprintf(base, sizeof base, "float4(%.9g, %.9g, %.9g, %.9g)", x->defc[off][0], x->defc[off][1], x->defc[off][2], x->defc[off][3]);
        else snprintf(base, sizeof base, "c[%d]", off);
    } else if (r[0] == 'c' && isdigit((unsigned char)r[1])) {
        int n = atoi(r + 1);
        if (n > 95) { fail(x, "bad constant %s", r); return -1; }
        if (x->defined[n]) snprintf(base, sizeof base, "float4(%.9g, %.9g, %.9g, %.9g)", x->defc[n][0], x->defc[n][1], x->defc[n][2], x->defc[n][3]);
        else snprintf(base, sizeof base, "c[%d]", n);
    } else {
        for (size_t i = 0; i < sizeof OUTPUTS / sizeof OUTPUTS[0]; i++)
            if (!_stricmp(r, OUTPUTS[i])) snprintf(base, sizeof base, "%s", OUTPUTS[i]);
        if (!base[0]) { fail(x, "unknown source register '%s'", r); return -1; }
    }
    snprintf(out, cap, "%s(%s)%s%s", neg ? "-" : "", base, swz[0] ? "." : "", swz);
    return 0;
}

/* the destination register and its write mask */
static int destination(Xs *x, const char *op, char *reg, size_t cap, char *mask, int *is_a0) {
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s", op);
    char *d = trim(tmp), *dot = strchr(d, '.');
    strcpy(mask, "xyzw");
    if (dot) {
        *dot = 0;
        size_t n = 0;
        for (const char *s = dot + 1; *s && n < 4; s++) {
            char c = (char)tolower((unsigned char)*s);
            if (c == 'r') c = 'x'; else if (c == 'g') c = 'y'; else if (c == 'b') c = 'z'; else if (c == 'a') c = 'w';
            if (strchr("xyzw", c)) mask[n++] = c;
        }
        mask[n] = 0;
    }
    *is_a0 = !strcmp(d, "a0");
    if (*is_a0) { snprintf(reg, cap, "a0"); return 0; }
    if ((d[0] == 'r' || d[0] == 'R') && isdigit((unsigned char)d[1])) {
        int n = atoi(d + 1);
        if (n == 12) { snprintf(reg, cap, "oPos"); return 0; }
        if (n > 11) { fail(x, "bad temporary %s", d); return -1; }
        snprintf(reg, cap, "r%d", n);
        return 0;
    }
    for (size_t i = 0; i < sizeof OUTPUTS / sizeof OUTPUTS[0]; i++)
        if (!_stricmp(d, OUTPUTS[i])) { snprintf(reg, cap, "%s", OUTPUTS[i]); return 0; }
    fail(x, "unknown destination register '%s'", d);
    return -1;
}

static void instruction(Xs *x, char *line) {
    char *sp = line;
    while (*sp && !isspace((unsigned char)*sp)) sp++;
    char opname[32];
    snprintf(opname, sizeof opname, "%.*s", (int)(sp - line), line);
    for (char *p = opname; *p; p++) *p = (char)tolower((unsigned char)*p);
    char *ops[4];
    int n = split_operands(sp, ops, 4);

    static const struct { const char *name; int srcs, scalar; } TABLE[] = {
        {"mov", 1, 0}, {"add", 2, 0}, {"sub", 2, 0}, {"mul", 2, 0}, {"mad", 3, 0}, {"dp3", 2, 0}, {"dp4", 2, 0},
        {"min", 2, 0}, {"max", 2, 0}, {"slt", 2, 0}, {"sge", 2, 0}, {"rcp", 1, 1}, {"rsq", 1, 1}, {"expp", 1, 1},
        {"logp", 1, 1}, {"exp", 1, 1}, {"log", 1, 1}, {"frc", 1, 0}, {"lit", 1, 0}, {"dst", 2, 0},
    };
    int k = -1;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) if (!strcmp(opname, TABLE[i].name)) k = (int)i;
    if (k < 0) { fail(x, "unsupported instruction '%s'", opname); return; }
    if (n != TABLE[k].srcs + 1) { fail(x, "'%s' takes %d operands, got %d", opname, TABLE[k].srcs + 1, n); return; }

    char dst[32], mask[8], s[3][256];
    int is_a0;
    if (destination(x, ops[0], dst, sizeof dst, mask, &is_a0)) return;
    for (int i = 0; i < TABLE[k].srcs; i++) if (source(x, ops[i + 1], TABLE[k].scalar, s[i], sizeof s[i])) return;

    const char *A = s[0], *B = s[1], *C = s[2];
    if (!strcmp(opname, "mov")) emit(x, "  t = %s;\n", A);
    else if (!strcmp(opname, "add")) emit(x, "  t = %s + %s;\n", A, B);
    else if (!strcmp(opname, "sub")) emit(x, "  t = %s - %s;\n", A, B);
    else if (!strcmp(opname, "mul")) emit(x, "  t = %s * %s;\n", A, B);
    else if (!strcmp(opname, "mad")) emit(x, "  t = %s * %s + %s;\n", A, B, C);
    else if (!strcmp(opname, "dp3")) emit(x, "  t = dot((%s).xyz, (%s).xyz);\n", A, B);
    else if (!strcmp(opname, "dp4")) emit(x, "  t = dot(%s, %s);\n", A, B);
    else if (!strcmp(opname, "min")) emit(x, "  t = min(%s, %s);\n", A, B);
    else if (!strcmp(opname, "max")) emit(x, "  t = max(%s, %s);\n", A, B);
    else if (!strcmp(opname, "slt")) emit(x, "  t = float4((%s) < (%s));\n", A, B);
    else if (!strcmp(opname, "sge")) emit(x, "  t = float4((%s) >= (%s));\n", A, B);
    else if (!strcmp(opname, "rcp")) emit(x, "  t = 1.0 / (%s).x;\n", A);
    else if (!strcmp(opname, "rsq")) emit(x, "  t = rsqrt(abs((%s).x));\n", A);
    else if (!strcmp(opname, "frc")) emit(x, "  t = frac(%s);\n", A);
    else if (!strcmp(opname, "expp") || !strcmp(opname, "exp"))
        emit(x, "  t.x = (%s).x; t = float4(exp2(floor(t.x)), frac(t.x), exp2(t.x), 1.0);\n", A);
    else if (!strcmp(opname, "logp") || !strcmp(opname, "log"))
        emit(x, "  t.x = abs((%s).x); t.z = log2(t.x); t = float4(floor(t.z), t.x / exp2(floor(t.z)), t.z, 1.0);\n", A);
    else if (!strcmp(opname, "lit"))
        emit(x, "  t = %s; t = float4(1.0, max(t.x, 0.0), (t.x > 0.0) ? pow(max(t.y, 0.0), clamp(t.w, -128.0, 128.0)) : 0.0, 1.0);\n", A);
    else if (!strcmp(opname, "dst")) emit(x, "  t = float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w);\n", A, B, A, B);

    if (is_a0) emit(x, "  a0 = (int)floor(t.x);\n");   /* ARL rounds down (bone indices rely on it) */
    else emit(x, "  %s.%s = t.%s;\n", dst, mask, mask);
}

/* ---- pixel shaders (xps.1.1: NV2A register combiners) ------------------------
   Registers r0 r1 t0..t3 v0 v1 c0..c7 zero, and in xfc fog, prod (E*F) and
   sum (r0 + v1). Source modifiers 1-x, -x, _bias, _bx2, _sat; selectors .a
   .b .rgb; instruction modifiers _x2 _x4 _d2 _sat; register writes clamp to
   [-1, 1] as the combiners do. The final combiner "xfc A,B,C,D,E,F,G" gives
   rgb = A*B + (1-A)*C + D and alpha G; without one the output is r0. Fog is
   left to the device: fog.a reads as 1 (no fog), fog.rgb as 0. */
static int ps_source(Xs *x, const char *op, int alpha_dest, char *out, size_t cap) {
    char tmp[96];
    snprintf(tmp, sizeof tmp, "%s", op);
    char *s = trim(tmp);
    int invert = 0, neg = 0;
    if (s[0] == '1' && s[1] == '-') { invert = 1; s = trim(s + 2); }
    else if (s[0] == '-') { neg = 1; s = trim(s + 1); }
    char *sel = strchr(s, '.');
    char selector[8] = "";
    if (sel) { snprintf(selector, sizeof selector, "%s", sel + 1); *sel = 0; }
    int bias = 0, bx2 = 0, sat = 0;
    for (char *u; (u = strrchr(s, '_')) != NULL; *u = 0) {
        if (!_stricmp(u, "_bias")) bias = 1;
        else if (!_stricmp(u, "_bx2")) bx2 = 1;
        else if (!_stricmp(u, "_sat")) sat = 1;
        else break;
    }
    char base[96];
    if ((s[0] == 'r' && (s[1] == '0' || s[1] == '1') && !s[2]) || (s[0] == 't' && s[1] >= '0' && s[1] <= '3' && !s[2]) ||
        (s[0] == 'v' && (s[1] == '0' || s[1] == '1') && !s[2])) snprintf(base, sizeof base, "%s", s);
    else if (s[0] == 'c' && isdigit((unsigned char)s[1])) {
        int n = atoi(s + 1);
        if (n > 7) { fail(x, "bad pixel shader constant %s", s); return -1; }
        if (x->defined[n]) snprintf(base, sizeof base, "float4(%.9g, %.9g, %.9g, %.9g)", x->defc[n][0], x->defc[n][1], x->defc[n][2], x->defc[n][3]);
        else snprintf(base, sizeof base, "c[%d]", n);
    }
    else if (!strcmp(s, "zero")) strcpy(base, "float4(0, 0, 0, 0)");
    else if (!strcmp(s, "fog")) strcpy(base, "float4(0, 0, 0, 1)");
    else if (!strcmp(s, "prod")) strcpy(base, "prod");
    else if (!strcmp(s, "sum")) strcpy(base, "(r0 + v1)");
    else { fail(x, "unknown pixel shader register '%s'", s); return -1; }
    char e[256];
    snprintf(e, sizeof e, "%s", base);
    if (bias) { char t[256]; snprintf(t, sizeof t, "(%s - 0.5)", e); strcpy(e, t); }
    if (bx2) { char t[256]; snprintf(t, sizeof t, "(2 * %s - 1)", e); strcpy(e, t); }
    if (sat) { char t[256]; snprintf(t, sizeof t, "saturate(%s)", e); strcpy(e, t); }
    const char *swz = "";
    if (!_stricmp(selector, "a") || !_stricmp(selector, "w")) swz = ".aaaa";
    else if (!_stricmp(selector, "b") || !_stricmp(selector, "z")) swz = ".bbbb";
    else if (!_stricmp(selector, "rgb") || !_stricmp(selector, "xyz")) swz = alpha_dest ? ".bbbb" : ".rgbb";  /* alpha reads blue */
    if (invert) snprintf(out, cap, "(1 - (%s)%s)", e, swz);
    else snprintf(out, cap, "%s(%s)%s", neg ? "-" : "", e, swz);
    return 0;
}

static void ps_instruction(Xs *x, char *line) {
    char *sp = line;
    while (*sp && !isspace((unsigned char)*sp)) sp++;
    char opname[32];
    snprintf(opname, sizeof opname, "%.*s", (int)(sp - line), line);
    for (char *p = opname; *p; p++) *p = (char)tolower((unsigned char)*p);
    float scale = 1;
    int sat = 0;
    for (char *u; (u = strrchr(opname, '_')) != NULL; *u = 0) {
        if (!strcmp(u, "_x2")) scale *= 2;
        else if (!strcmp(u, "_x4")) scale *= 4;
        else if (!strcmp(u, "_d2")) scale *= 0.5f;
        else if (!strcmp(u, "_sat")) sat = 1;
        else { fail(x, "unknown instruction modifier '%s'", u); return; }
    }
    char *ops[8];
    int n = split_operands(sp, ops, 8);

    /* texture addressing: the destination is the stage */
    if (!strncmp(opname, "tex", 3)) {
        int stage = (n >= 1 && ops[0][0] == 't') ? ops[0][1] - '0' : -1;
        if (stage < 0 || stage > 3) { fail(x, "bad texture register in %s", opname); return; }
        char src[256] = "";
        if (n >= 2 && ps_source(x, ops[1], 0, src, sizeof src)) return;
        /* NV2A "project2d": s/q, t/q, and for a depth texture the r/q compare;
           texscale: 1/w, 1/h for linear textures (texel coordinates) and, for
           depth textures, r from depth buffer units to 0..1 */
        if (!strcmp(opname, "tex")) {
            x->sampler[stage] = 1;
            emit(x, "  t%d = tex2Dproj(s%d, tc%d * float4(texscale[%d].xyz, 1));\n", stage, stage, stage, stage);
        }
        /* NV2A pass-through: s, t, r and q as a colour, clamped (DX8 on the PC
           gives alpha 1; the shadow receiver fades by its q) */
        else if (!strcmp(opname, "texcoord")) emit(x, "  t%d = saturate(tc%d);\n", stage, stage);
        else if (!strcmp(opname, "texm3x3pad")) {
            if (x->m3_rows > 1) { fail(x, "texm3x3pad without a matrix row free"); return; }
            emit(x, "  m3.%c = dot(tc%d.xyz, (%s).xyz);\n", "xy"[x->m3_rows++], stage, src);
        } else if (!strcmp(opname, "texm3x3vspec") || !strcmp(opname, "texm3x3tex")) {
            if (x->m3_rows != 2) { fail(x, "%s without two texm3x3pad rows", opname); return; }
            x->m3_rows = 0;
            x->sampler[stage] = 2;
            emit(x, "  m3.z = dot(tc%d.xyz, (%s).xyz);\n", stage, src);
            if (opname[7] == 'v')   /* reflect the eye vector (the rows' w) about the normal */
                emit(x, "  e3 = float3(tc%d.w, tc%d.w, tc%d.w); m3 = 2 * m3 * dot(m3, e3) / dot(m3, m3) - e3;\n", stage - 2, stage - 1, stage);
            emit(x, "  t%d = texCUBE(s%d, m3);\n", stage, stage);
        } else fail(x, "unsupported texture instruction '%s'", opname);
        return;
    }

    if (!strcmp(opname, "xfc")) {
        if (n != 7) { fail(x, "xfc takes 7 operands"); return; }
        char s[7][256];
        if (ps_source(x, ops[4], 0, s[4], sizeof s[4]) || ps_source(x, ops[5], 0, s[5], sizeof s[5])) return;
        emit(x, "  prod = %s * %s;\n", s[4], s[5]);
        for (int i = 0; i < 4; i++) if (ps_source(x, ops[i], 0, s[i], sizeof s[i])) return;
        if (ps_source(x, ops[6], 1, s[6], sizeof s[6])) return;
        emit(x, "  fin.rgb = (%s * %s + (1 - %s) * %s + %s).rgb;\n  fin.a = (%s).a;\n", s[0], s[1], s[0], s[2], s[3], s[6]);
        x->final_written = 1;
        x->final_fog = !strncmp(trim(ops[0]), "fog", 3);   /* "xfc fog.a, r0, fog.rgb, ...": the device blends the fog */
        return;
    }

    static const struct { const char *name; int srcs; } TABLE[] = {
        {"mov", 1}, {"add", 2}, {"sub", 2}, {"mul", 2}, {"mad", 3}, {"lrp", 3}, {"dp3", 2}, {"dp4", 2}, {"cnd", 3},
    };
    int k = -1;
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; i++) if (!strcmp(opname, TABLE[i].name)) k = (int)i;
    if (k < 0) { fail(x, "unsupported pixel shader instruction '%s'", opname); return; }
    if (n != TABLE[k].srcs + 1) { fail(x, "'%s' takes %d operands, got %d", opname, TABLE[k].srcs + 1, n); return; }

    char dst[8], mask[8] = "";
    {
        char tmp[64];
        snprintf(tmp, sizeof tmp, "%s", ops[0]);
        char *d = trim(tmp), *dot = strchr(d, '.');
        if (dot) { snprintf(mask, sizeof mask, "%s", dot + 1); *dot = 0; }
        if (!((d[0] == 'r' && (d[1] == '0' || d[1] == '1')) || (d[0] == 't' && d[1] >= '0' && d[1] <= '3') ||
              (d[0] == 'v' && (d[1] == '0' || d[1] == '1'))) || d[2]) { fail(x, "bad destination '%s'", d); return; }
        snprintf(dst, sizeof dst, "%s", d);
    }
    for (char *p = mask; *p; p++) *p = (char)tolower((unsigned char)*p);
    int alpha_only = !strcmp(mask, "a") || !strcmp(mask, "w");
    char s[3][256];
    for (int i = 0; i < TABLE[k].srcs; i++) if (ps_source(x, ops[i + 1], alpha_only, s[i], sizeof s[i])) return;
    const char *A = s[0], *B = s[1], *C = s[2];
    if (!strcmp(opname, "mov")) emit(x, "  res = %s;\n", A);
    else if (!strcmp(opname, "add")) emit(x, "  res = %s + %s;\n", A, B);
    else if (!strcmp(opname, "sub")) emit(x, "  res = %s - %s;\n", A, B);
    else if (!strcmp(opname, "mul")) emit(x, "  res = %s * %s;\n", A, B);
    else if (!strcmp(opname, "mad")) emit(x, "  res = %s * %s + %s;\n", A, B, C);
    else if (!strcmp(opname, "lrp")) emit(x, "  res = %s * %s + (1 - %s) * %s;\n", A, B, A, C);
    else if (!strcmp(opname, "dp3")) emit(x, "  res = dot((%s).rgb, (%s).rgb);\n", A, B);
    else if (!strcmp(opname, "dp4")) emit(x, "  res = dot(%s, %s);\n", A, B);
    else if (!strcmp(opname, "cnd")) emit(x, "  res = ((%s).a > 0.5) ? %s : %s;\n", A, B, C);
    if (scale != 1) emit(x, "  res *= %g;\n", scale);
    emit(x, sat ? "  res = saturate(res);\n" : "  res = clamp(res, -1, 1);\n");
    if (!mask[0] || !strcmp(mask, "rgba") || !strcmp(mask, "xyzw")) emit(x, "  %s = res;\n", dst);
    else if (!strcmp(mask, "rgb") || !strcmp(mask, "xyz")) emit(x, "  %s.rgb = res.rgb;\n", dst);
    else if (alpha_only) emit(x, "  %s.a = res.a;\n", dst);
    else fail(x, "unsupported write mask .%s", mask);
}

static void process_line(Xs *x, const char *raw);

static void expand_macro(Xs *x, const Macro *m, char *args) {
    char *argv[4];
    int argc = split_operands(args, argv, 4);
    if (argc != m->nparams) { fail(x, "macro %s takes %d arguments", m->name, m->nparams); return; }
    if (x->depth > 4) { fail(x, "macros nest too deep"); return; }
    x->depth++;
    for (int i = 0; i < m->nbody && !x->failed; i++) {
        char line[512] = "", *o = line;
        for (const char *p = m->body[i]; *p && o < line + sizeof line - 64; ) {
            if (*p == '%') {
                int done = 0;
                for (int a = 0; a < m->nparams; a++) {
                    size_t len = strlen(m->params[a]);
                    if (!strncmp(p + 1, m->params[a], len) && !is_ident((unsigned char)p[1 + len])) {
                        o += sprintf(o, "%s", argv[a]); p += 1 + len; done = 1; break;
                    }
                }
                if (done) continue;
            }
            *o++ = *p++;
        }
        *o = 0;
        /* %inc(cN) -> c(N+1) */
        for (char *inc; (inc = strstr(line, "%inc(")) != NULL; ) {
            char *close = strchr(inc, ')');
            if (!close) { fail(x, "bad %%inc"); break; }
            char regname[32];
            snprintf(regname, sizeof regname, "%.*s", (int)(close - inc - 5), inc + 5);
            char *rg = trim(regname), *digits = rg;
            while (*digits && !isdigit((unsigned char)*digits)) digits++;
            char repl[48];
            snprintf(repl, sizeof repl, "%.*s%d", (int)(digits - rg), rg, atoi(digits) + 1);
            char rest[512];
            snprintf(rest, sizeof rest, "%s", close + 1);
            snprintf(inc, sizeof line - (inc - line), "%s%s", repl, rest);
        }
        process_line(x, line);
    }
    x->depth--;
}

static void process_line(Xs *x, const char *raw) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", raw);
    char *c = strchr(buf, ';');
    if (c) *c = 0;
    if ((c = strstr(buf, "//")) != NULL) *c = 0;
    char *line = trim(buf);
    if (!*line) return;
    if (!strncmp(line, "vs.", 3) || !strncmp(line, "xvs.", 4) || !strncmp(line, "ps.", 3) || !strncmp(line, "xps.", 4)) return;
    if (!strncmp(line, "#define", 7)) {
        char *p = trim(line + 7), *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        if (x->ndefs >= (int)(sizeof x->defs / sizeof x->defs[0])) { fail(x, "too many #defines"); return; }
        Define *d = &x->defs[x->ndefs++];
        snprintf(d->name, sizeof d->name, "%.*s", (int)(e - p), p);
        snprintf(d->value, sizeof d->value, "%s", trim(e));
        return;
    }
    if (line[0] == '#') return;   /* #pragma: none of the game's shaders uses screenspace */
    substitute_defines(x, line, sizeof buf - (line - buf));
    if (!strncmp(line, "def", 3) && isspace((unsigned char)line[3])) {
        char *ops[5];
        if (split_operands(line + 3, ops, 5) != 5 || ops[0][0] != 'c') { fail(x, "bad def"); return; }
        int n = atoi(ops[0] + 1);
        if (n < 0 || n > 95) { fail(x, "bad def register"); return; }
        for (int i = 0; i < 4; i++) x->defc[n][i] = strtof(ops[i + 1], NULL);
        x->defined[n] = 1;
        return;
    }
    char first[64];
    sscanf(line, "%63s", first);
    for (int i = 0; i < x->nmacros; i++)
        if (!strcmp(first, x->macros[i].name)) { expand_macro(x, &x->macros[i], line + strlen(first)); return; }
    if (x->ps) ps_instruction(x, line);
    else instruction(x, line);
}

static int translate(const char *src, size_t len, int ps, char *out, size_t cap, uint32_t *inputs, int *fog) {
    Xs *x = calloc(1, sizeof *x);
    x->ps = ps;
    char *text = malloc(len + 1);
    memcpy(text, src, len);
    text[len] = 0;
    for (char *p = text; *p; p++) if (*p == '\r') *p = ' ';

    Macro *open = NULL;
    for (char *line = text, *next; line && !x->failed; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        char *t = trim(line);
        if (open) {
            char head[16];
            if (sscanf(t, "%15s", head) == 1 && !strcmp(head, "endm")) { open = NULL; continue; }
            if (open->nbody < 64) open->body[open->nbody++] = t;
            continue;
        }
        if (!strncmp(t, "macro", 5) && isspace((unsigned char)t[5])) {
            if (x->nmacros >= 8) { fail(x, "too many macros"); break; }
            open = &x->macros[x->nmacros++];
            char *p = trim(t + 5), *e = p;
            while (*e && !isspace((unsigned char)*e)) e++;
            snprintf(open->name, sizeof open->name, "%.*s", (int)(e - p), p);
            for (char *tok = strtok(e, " \t,"); tok && open->nparams < 4; tok = strtok(NULL, " \t,"))
                snprintf(open->params[open->nparams++], sizeof open->params[0], "%s", tok);
            continue;
        }
        process_line(x, t);
    }

    int rc = 0;
    if (x->failed) {
        snprintf(out, cap, "%s", x->msg);
        rc = -1;
    } else if (ps) {
        size_t o = 0;
        o += snprintf(out + o, cap - o, "float4 c[8] : register(c0);\nfloat4 texscale[4] : register(c8);\n");
        for (int i = 0; i < 4; i++)
            if (x->sampler[i]) o += snprintf(out + o, cap - o, "%s s%d : register(s%d);\n", x->sampler[i] == 2 ? "samplerCUBE" : "sampler2D", i, i);
        o += snprintf(out + o, cap - o,
                      "float4 main(float4 v0 : COLOR0, float4 v1 : COLOR1, float4 tc0 : TEXCOORD0, float4 tc1 : TEXCOORD1,\n"
                      "            float4 tc2 : TEXCOORD2, float4 tc3 : TEXCOORD3) : COLOR {\n"
                      "  float4 r0 = 0, r1 = 0, t0 = 0, t1 = 0, t2 = 0, t3 = 0, res = 0, prod = 0, fin = 0;\n"
                      "  float3 m3 = 0, e3 = 0;\n");
        if (o + x->body_len + 64 > cap) { snprintf(out, cap, "HLSL too long"); rc = -1; }
        else {
            memcpy(out + o, x->body ? x->body : "", x->body_len);
            o += x->body_len;
            snprintf(out + o, cap - o, "  return saturate(%s);\n}\n", x->final_written ? "fin" : "r0");
        }
    } else {
        size_t o = 0;
        o += snprintf(out + o, cap - o,
                      "float4 c[96] : register(c0);\n"
                      "float4 hostfog : register(c96);\n"
                      "struct VS_OUT { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
                      "  float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; };\n"
                      "VS_OUT main(");
        int first = 1;
        for (int i = 0; i < 16; i++)
            if (x->inputs & (1u << i)) { o += snprintf(out + o, cap - o, "%sfloat4 v%d : TEXCOORD%d", first ? "" : ", ", i, i); first = 0; }
        o += snprintf(out + o, cap - o,
                      ") {\n"
                      "  float4 t = 0, r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0;\n"
                      "  float4 oPos = float4(0, 0, 0, 1), oD0 = 1, oD1 = 0, oT0 = float4(0, 0, 0, 1), oT1 = float4(0, 0, 0, 1),\n"
                      "    oT2 = float4(0, 0, 0, 1), oT3 = float4(0, 0, 0, 1), oFog = 1, oPts = 0, oB0 = 0, oB1 = 0;\n"
                      "  int a0 = 0;\n");
        if (o + x->body_len + 256 > cap) { snprintf(out, cap, "HLSL too long"); rc = -1; }
        else {
            memcpy(out + o, x->body ? x->body : "", x->body_len);
            o += x->body_len;
            snprintf(out + o, cap - o,
                     /* oFog is a fog coordinate on the Xbox, a blend factor in Direct3D 9:
                        apply the FOGTABLEMODE function (hostfog: start, end, density, mode) */
                     "  float fc = abs(oFog.x), fog = 1.0;\n"
                     "  if (hostfog.w == 3) fog = (hostfog.y - fc) / (hostfog.y - hostfog.x);\n"
                     "  else if (hostfog.w == 1) fog = exp(-fc * hostfog.z);\n"
                     "  else if (hostfog.w == 2) fog = exp(-(fc * hostfog.z) * (fc * hostfog.z));\n"
                     "  VS_OUT o; o.pos = oPos; o.d0 = oD0; o.d1 = oD1; o.t0 = oT0; o.t1 = oT1; o.t2 = oT2; o.t3 = oT3; o.fog = saturate(fog);\n"
                     "  return o;\n}\n");
        }
    }
    if (inputs) *inputs = x->inputs;
    if (fog) *fog = !x->final_written || x->final_fog;   /* without xfc the default final combiner fogs */
    free(x->body);
    free(text);
    free(x);
    return rc;
}

int xs_vs_to_hlsl(const char *src, size_t len, char *out, size_t cap, uint32_t *inputs) {
    return translate(src, len, 0, out, cap, inputs, NULL);
}

int xs_ps_to_hlsl(const char *src, size_t len, char *out, size_t cap, int *fog) {
    return translate(src, len, 1, out, cap, NULL, fog);
}

int xs_is_pixel_shader(const char *src, size_t len) {
    while (len && isspace((unsigned char)*src)) { src++; len--; }
    return (len > 3 && !strncmp(src, "ps.", 3)) || (len > 4 && !strncmp(src, "xps.", 4));
}
