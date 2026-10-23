/* Translate shader source files with shader_xlate.c and compile the HLSL
   with d3dcompiler_47, to check the translator against the game's shaders
   offline:  xs_check.exe [-v] [-sm4] vs_*.txt   (-v prints the HLSL; -sm4
   checks the Direct3D 11 target, pixel shaders also with every stage
   sampling a depth texture) */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host/shader_xlate.h"

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                   ID3DBlob **, ID3DBlob **);

int main(int argc, char **argv) {
    HMODULE dll = LoadLibraryA("d3dcompiler_47.dll");
    CompileFn compile = dll ? (CompileFn)GetProcAddress(dll, "D3DCompile") : NULL;
    if (!compile) { fprintf(stderr, "d3dcompiler_47.dll not found\n"); return 2; }
    int verbose = 0, sm4 = 0, bad = 0, total = 0;
    static char hlsl[1 << 18];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
        if (!strcmp(argv[i], "-sm4")) { sm4 = 1; continue; }
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "%s: cannot open\n", argv[i]); bad++; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *src = malloc(n);
        fread(src, 1, n, f);
        fclose(f);
        int ps = xs_is_pixel_shader(src, n);
        for (int depth = 0; depth < (ps && sm4 ? 2 : 1); depth++) {
            total++;
            uint32_t inputs = 0;
            int fog = 0;
            int target = sm4 ? XS_SM4 : XS_SM2;
            if (ps ? xs_ps_to_hlsl(src, n, target, depth ? 0xf : 0, hlsl, sizeof hlsl, &fog) : xs_vs_to_hlsl(src, n, target, hlsl, sizeof hlsl, &inputs)) {
                printf("%s: translate: %s\n", argv[i], hlsl);
                bad++;
                break;
            }
            if (verbose) printf("---- %s%s\n%s", argv[i], depth ? " (depth stages)" : "", hlsl);
            ID3DBlob *code = NULL, *errors = NULL;
            const char *profile = sm4 ? (ps ? "ps_4_0" : "vs_4_0") : (ps ? "ps_2_0" : "vs_2_0");
            HRESULT hr = compile(hlsl, strlen(hlsl), argv[i], NULL, NULL, "main", profile, 0, 0, &code, &errors);
            if (FAILED(hr)) {
                printf("%s%s: compile: %s\n", argv[i], depth ? " (depth stages)" : "", errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
                bad++;
            } else {
                if (ps) printf("%s%s: ok, %s, %u bytes\n", argv[i], depth ? " (depth stages)" : "", fog ? "fogged" : "no fog", (unsigned)ID3D10Blob_GetBufferSize(code));
                else printf("%s: ok, inputs %04x, %u bytes\n", argv[i], inputs, (unsigned)ID3D10Blob_GetBufferSize(code));
            }
            if (code) ID3D10Blob_Release(code);
            if (errors) ID3D10Blob_Release(errors);
        }
        free(src);
    }
    printf("%d of %d failed\n", bad, total);
    return bad != 0;
}
