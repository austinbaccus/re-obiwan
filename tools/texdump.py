"""Render raw guest texture dumps (RECOMP_DUMPTEX) several ways to see how
they are laid out: python -I tools/texdump.py build/tex_*_WxH.raw

Writes <name>.linear.png, <name>.morton.png (Xbox swizzle undone) and
<name>.l8.png (the bytes as an 8-bit image four times as wide)."""
import os
import sys

from PIL import Image


def masks(w, h):
    mx = my = 0
    i = j = 1
    while i < w or i < h:
        if i < w:
            mx |= j
            j <<= 1
        if i < h:
            my |= j
            j <<= 1
        i <<= 1
    return mx, my


def unswizzle(data, w, h, bpp):
    mx, my = masks(w, h)
    out = bytearray(w * h * bpp)
    y_bits = 0
    for y in range(h):
        x_bits = 0
        row = y * w * bpp
        for x in range(w):
            src = (x_bits | y_bits) * bpp
            out[row + x * bpp:row + x * bpp + bpp] = data[src:src + bpp]
            x_bits = (x_bits - mx) & mx
        y_bits = (y_bits - my) & my
    return bytes(out)


def main(paths):
    for p in paths:
        base = os.path.basename(p)[:-4]
        w, h = (int(x) for x in base.rsplit('_', 1)[1].split('x'))
        fmt = int(base.split('_')[2], 16)
        data = open(p, 'rb').read()
        bpp = 4 if fmt in (0x06, 0x07, 0x12, 0x1E) else 2 if fmt in (0x02, 0x03, 0x04, 0x05, 0x10, 0x11, 0x1A, 0x1C, 0x1D, 0x20) else 1
        if bpp != 4:
            print(p, 'bpp', bpp, 'skipped')
            continue
        for name, buf in (('linear', data), ('morton', unswizzle(data, w, h, bpp))):
            img = Image.frombytes('RGBA', (w, h), bytes(b for i in range(0, w * h * 4, 4) for b in (buf[i + 2], buf[i + 1], buf[i], 255)))
            img.save(p[:-4] + '.' + name + '.png')
        Image.frombytes('L', (w * 4, h), data[:w * h * 4]).save(p[:-4] + '.l8.png')
        print(p, w, h)


if __name__ == '__main__':
    main(sys.argv[1:])
