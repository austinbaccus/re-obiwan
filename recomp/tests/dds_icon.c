/* Make the program icon from one of the game's own textures at build time,
   so nothing from the disc is committed:  dds_icon.exe in.dds out.ico
   Reads a DXT1/3/5 texture, crops it to its opaque part (the HUD portraits
   are circles with a transparent border) and writes 32-bit icon images at
   the sizes Windows asks for. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int SIZES[] = {16, 20, 24, 32, 40, 48, 64};
#define NSIZES (int)(sizeof SIZES / sizeof SIZES[0])

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v & 0xffff); wr16(p + 2, v >> 16); }

static void rgb565(uint16_t c, float *o) {
    o[0] = (c >> 11) * 255.0f / 31; o[1] = (c >> 5 & 63) * 255.0f / 63; o[2] = (c & 31) * 255.0f / 31;
}

/* One 4x4 block into rgba (straight alpha, 0..255), row stride w pixels */
static void decode_block(const uint8_t *b, int fourcc, float *rgba, int w) {
    float a[16], pal[4][4];
    const uint8_t *col = b;
    if (fourcc == '1') {
        for (int i = 0; i < 16; i++) a[i] = 255;
    } else if (fourcc == '3') {
        for (int i = 0; i < 16; i++) a[i] = (b[i / 2] >> (i & 1) * 4 & 15) * 17.0f;
        col = b + 8;
    } else {
        float a0 = b[0], a1 = b[1], ap[8] = {a0, a1};
        for (int i = 2; i < 8; i++) ap[i] = a0 > a1 ? ((8 - i) * a0 + (i - 1) * a1) / 7 : i < 6 ? ((6 - i) * a0 + (i - 1) * a1) / 5 : i == 6 ? 0 : 255;
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++) bits |= (uint64_t)b[2 + i] << 8 * i;
        for (int i = 0; i < 16; i++) a[i] = ap[bits >> 3 * i & 7];
        col = b + 8;
    }
    uint16_t c0 = rd16(col), c1 = rd16(col + 2);
    rgb565(c0, pal[0]); rgb565(c1, pal[1]);
    int four = fourcc != '1' || c0 > c1; /* DXT1 with c0 <= c1: three colours and transparent */
    for (int k = 0; k < 3; k++) {
        pal[2][k] = four ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2;
        pal[3][k] = four ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0;
    }
    uint32_t idx = rd32(col + 4);
    for (int i = 0; i < 16; i++) {
        int s = idx >> 2 * i & 3;
        float *o = rgba + ((i / 4) * w + i % 4) * 4;
        memcpy(o, pal[s], 3 * sizeof(float));
        o[3] = !four && s == 3 ? 0 : a[i];
    }
}

/* Weight of a source pixel d pixels from the sample point: a tent, widened
   to r source pixels when shrinking */
static float tent(float d, float r) { d = fabsf(d) / r; return d < 1 ? 1 - d : 0; }

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: dds_icon in.dds out.ico\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "dds_icon: cannot open %s\n", argv[1]); return 1; }
    static uint8_t dds[1 << 20];
    size_t len = fread(dds, 1, sizeof dds, f);
    fclose(f);
    int w = len >= 128 ? (int)rd32(dds + 16) : 0, h = len >= 128 ? (int)rd32(dds + 12) : 0;
    uint32_t fourcc = len >= 128 ? rd32(dds + 84) : 0;
    int kind = (fourcc & 0xffffff) == ('D' | 'X' << 8 | 'T' << 16) ? (int)(fourcc >> 24) : 0;
    size_t block = kind == '1' ? 8 : 16;
    if (memcmp(dds, "DDS ", 4) || (kind != '1' && kind != '3' && kind != '5') || w < 4 || h < 4 || w > 1024 || h > 1024 ||
        128 + (size_t)((w + 3) / 4) * ((h + 3) / 4) * block > len) {
        fprintf(stderr, "dds_icon: %s is not a DXT1/3/5 texture\n", argv[1]);
        return 1;
    }
    w &= ~3; h &= ~3;
    float *img = malloc(sizeof(float) * 4 * w * h);
    for (int by = 0; by < h / 4; by++)
        for (int bx = 0; bx < w / 4; bx++)
            decode_block(dds + 128 + (by * ((w + 3) / 4) + bx) * block, kind, img + (by * 4 * w + bx * 4) * 4, w);

    /* The opaque part, made square around its centre, with a pixel to spare */
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (img[(y * w + x) * 4 + 3] > 0) {
                if (x < x0) x0 = x; if (x > x1) x1 = x;
                if (y < y0) y0 = y; if (y > y1) y1 = y;
            }
    if (x1 < 0) { x0 = y0 = 0; x1 = w - 1; y1 = h - 1; }
    float side = (float)((x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0) + 3);
    float cx = (x0 + x1 + 1) / 2.0f, cy = (y0 + y1 + 1) / 2.0f;
    for (int i = 0; i < w * h; i++) /* premultiply, so clear pixels add no colour */
        for (int k = 0; k < 3; k++) img[i * 4 + k] *= img[i * 4 + 3] / 255;

    size_t total = 6 + 16 * NSIZES;
    for (int s = 0; s < NSIZES; s++) total += 40 + SIZES[s] * SIZES[s] * 4 + ((SIZES[s] + 31) / 32 * 4) * SIZES[s];
    uint8_t *ico = calloc(1, total);
    wr16(ico + 2, 1); wr16(ico + 4, NSIZES);
    size_t at = 6 + 16 * NSIZES;
    for (int s = 0; s < NSIZES; s++) {
        int n = SIZES[s], mask_row = (n + 31) / 32 * 4;
        uint32_t bytes = 40 + n * n * 4 + mask_row * n;
        uint8_t *e = ico + 6 + 16 * s, *bmp = ico + at;
        e[0] = e[1] = (uint8_t)n;
        wr16(e + 4, 1); wr16(e + 6, 32); wr32(e + 8, bytes); wr32(e + 12, (uint32_t)at);
        wr32(bmp, 40); wr32(bmp + 4, n); wr32(bmp + 8, 2 * n); /* colour and mask, stacked */
        wr16(bmp + 12, 1); wr16(bmp + 14, 32); wr32(bmp + 20, n * n * 4 + mask_row * n);
        float scale = side / n, r = scale > 1 ? scale : 1;
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++) {
                float sx = cx - side / 2 + (x + 0.5f) * scale - 0.5f, sy = cy - side / 2 + (y + 0.5f) * scale - 0.5f;
                float acc[4] = {0}, wsum = 0;
                for (int v = (int)floorf(sy - r); v <= (int)ceilf(sy + r); v++)
                    for (int u = (int)floorf(sx - r); u <= (int)ceilf(sx + r); u++) {
                        float wt = tent(u - sx, r) * tent(v - sy, r);
                        if (wt <= 0) continue;
                        wsum += wt;
                        if (u < 0 || v < 0 || u >= w || v >= h) continue; /* outside the texture: clear */
                        for (int k = 0; k < 4; k++) acc[k] += wt * img[(v * w + u) * 4 + k];
                    }
                float alpha = wsum > 0 ? acc[3] / wsum : 0;
                uint8_t *p = bmp + 40 + ((n - 1 - y) * n + x) * 4; /* rows bottom up, BGRA */
                for (int k = 0; k < 3; k++) {
                    float c = alpha > 0 ? acc[k] / wsum * 255 / alpha : 0;
                    p[2 - k] = (uint8_t)(c < 0 ? 0 : c > 255 ? 255 : c + 0.5f);
                }
                p[3] = (uint8_t)(alpha > 255 ? 255 : alpha + 0.5f);
                if (!p[3]) bmp[40 + n * n * 4 + (n - 1 - y) * mask_row + x / 8] |= (uint8_t)(0x80 >> (x & 7));
            }
        at += bytes;
    }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(ico, 1, total, f) != total) { fprintf(stderr, "dds_icon: cannot write %s\n", argv[2]); return 1; }
    fclose(f);
    return 0;
}
