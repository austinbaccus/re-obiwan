#!/usr/bin/env python3
"""Recover classes, inheritance and vtables from MSVC RTTI in an XBE.

Usage: python3 tools/rtti_vtables.py extracted/default.xbe <out-dir>

Writes <out-dir>/classes.csv and <out-dir>/vtables.csv. Only addresses and
the type names the compiler stored in the binary are written.

MSVC (7.x, x86) layout used here:
  TypeDescriptor       +0 type_info vftable, +4 spare, +8 mangled name ".?AV...@@"
  CompleteObjectLocator +0 signature (0), +4 offset of this vtable in the
                       object, +8 ctor-disp offset, +C TypeDescriptor*,
                       +10 ClassHierarchyDescriptor*
  ClassHierarchyDescriptor +0 signature, +4 attributes (1 = multiple
                       inheritance, 2 = virtual), +8 base count, +C BaseClassArray*
  BaseClassDescriptor  +0 TypeDescriptor*, +4 contained bases, +8 mdisp,
                       +C pdisp, +10 vdisp, +14 attributes
  vtable               dword before slot 0 points at the CompleteObjectLocator
"""
import csv
import os
import re
import struct
import sys


def simple_name(mangled):
    """'.?AVCZoneFace@CZone@@' -> 'CZone::CZoneFace'; templates keep their mangled form."""
    body = mangled[4:-2]
    if body.startswith('?$') or '?' in body:
        return body
    return '::'.join(reversed(body.split('@')))


class Image:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        u32 = lambda o: struct.unpack_from('<I', self.data, o)[0]
        base = u32(0x104)
        hdr = lambda va: va - base
        self.sections = []
        for i in range(u32(0x11C)):
            o = hdr(u32(0x120)) + i * 56
            flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<6I', self.data, o)
            n = hdr(name_va)
            name = self.data[n:self.data.index(b'\0', n)].decode()
            self.sections.append((name, va, rsize, raw, flags))
        self.code = [(va, va + rsize) for name, va, rsize, raw, flags in self.sections
                     if name in ('.text', 'D3D', 'D3DX', 'XGRPH', 'DSOUND', 'XPP')]

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def raw(self, va):
        for name, sva, size, raw, flags in self.sections:
            if sva <= va < sva + size:
                return raw + va - sva
        return None

    def u32(self, va):
        o = self.raw(va)
        return None if o is None else struct.unpack_from('<I', self.data, o)[0]

    def is_code(self, va):
        return any(lo <= va < hi for lo, hi in self.code)

    def plausible_entry(self, va):
        """MSVC pads functions to 16 bytes here; library code at least follows a ret or padding."""
        if va % 16 == 0:
            return True
        o = self.raw(va)
        return self.data[o - 1] in (0x90, 0xCC, 0xC3) or self.data[o - 3] == 0xC2

    def dwords(self, section):
        name, va, size, raw, flags = self.section(section)
        for i in range(0, size - 3, 4):
            yield va + i, struct.unpack_from('<I', self.data, raw + i)[0]


def main(path, out_dir):
    img = Image(path)

    # TypeDescriptors: mangled names in .data, the descriptor starts 8 bytes earlier.
    tds = {}
    for name, sva, size, raw, flags in img.sections:
        if name not in ('.data', '.rdata'):
            continue
        blob = img.data[raw:raw + size]
        for m in re.finditer(rb'\.\?A[VU][^\0]+@@\0', blob):
            td = sva + m.start() - 8
            tds[td] = m.group(0)[:-1].decode('latin1')

    # CompleteObjectLocators and their hierarchy descriptors live in .rdata.
    cols = {}
    for va, v in img.dwords('.rdata'):
        if v in tds and va % 4 == 0:
            col = va - 12
            if img.u32(col) != 0:
                continue
            chd = img.u32(col + 16)
            if chd is None or img.raw(chd) is None or img.u32(chd + 12) is None:
                continue
            cols[col] = (img.u32(col + 4), img.u32(col + 8), v, chd)

    classes = {}
    for col, (offset, cd, td, chd) in cols.items():
        if td in classes:
            continue
        attrs, nbases, bca = img.u32(chd + 4), img.u32(chd + 8), img.u32(chd + 12)
        bases = []
        for i in range(1, nbases):  # entry 0 is the class itself
            bcd = img.u32(bca + 4 * i)
            btd = img.u32(bcd)
            bases.append((tds.get(btd, f'?{btd:08x}'), img.u32(bcd + 8), img.u32(bcd + 4)))
        classes[td] = (attrs, bases)

    # vtables: a dword in .rdata pointing at a COL, followed by code pointers.
    col_refs = {va: v for va, v in img.dwords('.rdata') if v in cols}
    vtables = []
    for meta, col in sorted(col_refs.items()):
        start = meta + 4
        slots = []
        va = start
        while True:
            v = img.u32(va)
            if v is None or not img.is_code(v) or not img.plausible_entry(v) or (va in col_refs and va != meta):
                break
            slots.append(v)
            va += 4
            if va in col_refs:
                break
        offset, cd, td, chd = cols[col]
        vtables.append((start, tds[td], offset, col, slots))

    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, 'classes.csv'), 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['type_descriptor', 'name', 'mangled', 'kind', 'inheritance', 'bases'])
        for td, mangled in sorted(tds.items(), key=lambda t: simple_name(t[1]).lower()):
            attrs, bases = classes.get(td, (None, []))
            inh = '' if attrs is None else ('virtual' if attrs & 2 else 'multiple' if attrs & 1 else 'single')
            w.writerow([f'0x{td:08x}', simple_name(mangled), mangled,
                        'struct' if mangled[3] == 'U' else 'class', inh,
                        ' '.join(f'{simple_name(b)}@{off}' for b, off, n in bases)])
    with open(os.path.join(out_dir, 'vtables.csv'), 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(['vtable', 'class', 'offset_in_object', 'locator', 'slots', 'slot_targets'])
        for start, mangled, offset, col, slots in vtables:
            w.writerow([f'0x{start:08x}', simple_name(mangled), offset, f'0x{col:08x}', len(slots),
                        ' '.join(f'0x{s:08x}' for s in slots)])
    print(f'{len(tds)} type descriptors, {len(classes)} classes with locators, {len(vtables)} vtables, '
          f'{sum(len(v[4]) for v in vtables)} slots, {len({s for v in vtables for s in v[4]})} distinct targets')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
