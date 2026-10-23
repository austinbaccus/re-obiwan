/* Translate shader source files with shader_xlate.c and compile the HLSL
   with d3dcompiler_47, to check the translator against the game's shaders
   offline:  xs_check.exe [-v] vs_*.txt   (-v prints the HLSL) */
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
    int verbose = 0, bad = 0, total = 0;
    static char hlsl[1 << 18];
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
        FILE *f = fopen(argv[i], "rb");
        if (!f) { fprintf(stderr, "%s: cannot open\n", argv[i]); bad++; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *src = malloc(n);
        fread(src, 1, n, f);
        fclose(f);
        total++;
        uint32_t inputs = 0;
        int ps = xs_is_pixel_shader(src, n);
        if (ps ? xs_ps_to_hlsl(src, n, hlsl, sizeof hlsl) : xs_vs_to_hlsl(src, n, hlsl, sizeof hlsl, &inputs)) {
            printf("%s: translate: %s\n", argv[i], hlsl);
            bad++;
        } else {
            if (verbose) printf("---- %s\n%s", argv[i], hlsl);
            ID3DBlob *code = NULL, *errors = NULL;
            HRESULT hr = compile(hlsl, strlen(hlsl), argv[i], NULL, NULL, "main", ps ? "ps_2_0" : "vs_2_0", 0, 0, &code, &errors);
            if (FAILED(hr)) {
                printf("%s: compile: %s\n", argv[i], errors ? (const char *)ID3D10Blob_GetBufferPointer(errors) : "?");
                bad++;
            } else {
                printf("%s: ok, inputs %04x, %u bytes\n", argv[i], inputs, (unsigned)ID3D10Blob_GetBufferSize(code));
            }
            if (code) ID3D10Blob_Release(code);
            if (errors) ID3D10Blob_Release(errors);
        }
        free(src);
    }
    printf("%d of %d failed\n", bad, total);
    return bad != 0;
}
