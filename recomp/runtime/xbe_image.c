#include <stdio.h>
#include <stdlib.h>

#include "guest.h"
#include "xbe_image.h"

#define RETAIL_ENTRY_KEY 0xA8FC57ABu
#define RETAIL_THUNK_KEY 0x5B6D40B6u

int xbe_load(const char *path, XbeInfo *info) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 0; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n);
    if (fread(file, 1, n, f) != (size_t)n) { fprintf(stderr, "short read on %s\n", path); return 0; }
    fclose(f);
    if (memcmp(file, "XBEH", 4) != 0) { fprintf(stderr, "%s is not an XBE\n", path); return 0; }

    info->base = RD32(file + 0x104);
    info->size = RD32(file + 0x10c);
    info->entry = RD32(file + 0x128) ^ RETAIL_ENTRY_KEY;
    info->kernel_thunk = RD32(file + 0x158) ^ RETAIL_THUNK_KEY;
    info->tls_dir = RD32(file + 0x12c);
    info->num_sections = RD32(file + 0x11c);
    info->section_headers = RD32(file + 0x120);

    uint8_t *image = MEM(info->base);
    memset(image, 0, info->size);
    memcpy(image, file, RD32(file + 0x108));
    uint32_t sec = info->section_headers - info->base;
    for (uint32_t i = 0; i < info->num_sections; i++) {
        const uint8_t *s = file + sec + i * 56;
        uint32_t va = RD32(s + 4), raw = RD32(s + 12), rsize = RD32(s + 16);
        memcpy(image + (va - info->base), file + raw, rsize);
    }
    free(file);
    return 1;
}
