#!/usr/bin/env python3
"""Dump the structural metadata of an XBE as JSON.

Usage: python3 tools/xbe_info.py extracted/default.xbe > xbe.json

Reads only headers and tables (image header, certificate, section headers,
library versions, TLS directory, kernel thunk table). No code or assets are
copied out, so the output is safe to keep next to the docs.
"""
import json
import struct
import sys

# Entry point / kernel thunk addresses are XOR-scrambled with a key that
# depends on whether the XBE was built for retail or debug kits.
KEYS = {
    'retail': (0xA8FC57AB, 0x5B6D40B6),
    'debug': (0x94859D4B, 0xEFB1F152),
}

SECTION_FLAGS = {
    0x01: 'writable',
    0x02: 'preload',
    0x04: 'executable',
    0x08: 'inserted_file',
    0x10: 'head_page_ro',
    0x20: 'tail_page_ro',
}


def main(path):
    data = open(path, 'rb').read()

    def u32(off):
        return struct.unpack_from('<I', data, off)[0]

    if data[:4] != b'XBEH':
        sys.exit(f'{path}: not an XBE')
    base = u32(0x104)

    def hdr(va):
        return va - base  # the image header is mapped at the base address

    def cstr(va):
        o = hdr(va)
        return data[o:data.index(b'\0', o)].decode('latin1')

    def wstr(va):
        o, s = hdr(va), b''
        while data[o:o + 2] != b'\0\0':
            s += data[o:o + 2]
            o += 2
        return s.decode('utf-16le')

    fields = [
        ('base', 0x104), ('size_of_headers', 0x108), ('size_of_image', 0x10C),
        ('size_of_image_header', 0x110), ('timedate', 0x114), ('certificate', 0x118),
        ('num_sections', 0x11C), ('section_headers', 0x120), ('init_flags', 0x124),
        ('entry_point_encoded', 0x128), ('tls_directory', 0x12C), ('pe_stack_commit', 0x130),
        ('pe_heap_reserve', 0x134), ('pe_heap_commit', 0x138), ('pe_base', 0x13C),
        ('pe_size_of_image', 0x140), ('pe_checksum', 0x144), ('pe_timedate', 0x148),
        ('debug_pathname', 0x14C), ('debug_filename', 0x150), ('debug_unicode_filename', 0x154),
        ('kernel_thunk_encoded', 0x158), ('non_kernel_import_dir', 0x15C),
        ('num_library_versions', 0x160), ('library_versions', 0x164),
        ('kernel_library_version', 0x168), ('xapi_library_version', 0x16C),
        ('logo_bitmap', 0x170), ('logo_bitmap_size', 0x174),
    ]
    h = {name: u32(off) for name, off in fields}

    for kind, (ep_key, thunk_key) in KEYS.items():
        ep = h['entry_point_encoded'] ^ ep_key
        thunk = h['kernel_thunk_encoded'] ^ thunk_key
        if base <= ep < base + h['size_of_image'] and base <= thunk < base + h['size_of_image']:
            h['build'] = kind
            h['entry_point'] = ep
            h['kernel_thunk'] = thunk
            break
    h['debug_pathname_str'] = cstr(h['debug_pathname'])
    h['debug_unicode_filename_str'] = wstr(h['debug_unicode_filename'])

    c = hdr(h['certificate'])
    cert = {
        'size': u32(c), 'timedate': u32(c + 4), 'title_id': u32(c + 8),
        'title_name': data[c + 0xC:c + 0xC + 80].decode('utf-16le').rstrip('\0'),
        'allowed_media': u32(c + 0x9C), 'game_region': u32(c + 0xA0),
        'game_ratings': u32(c + 0xA4), 'disk_number': u32(c + 0xA8), 'version': u32(c + 0xAC),
    }

    sections = []
    for i in range(h['num_sections']):
        o = hdr(h['section_headers']) + i * 56
        flags, va, vsize, raw, rsize, name_va = struct.unpack_from('<6I', data, o)
        sections.append({
            'name': cstr(name_va), 'flags': flags,
            'flag_names': [n for bit, n in SECTION_FLAGS.items() if flags & bit],
            'va': va, 'vsize': vsize, 'raw': raw, 'rsize': rsize,
        })

    def read_va(va):
        for s in sections:
            if s['va'] <= va < s['va'] + s['rsize']:
                return u32(s['raw'] + va - s['va'])
        return None

    libs = []
    for i in range(h['num_library_versions']):
        o = hdr(h['library_versions']) + i * 16
        major, minor, build, flags = struct.unpack_from('<4H', data, o + 8)
        libs.append({
            'name': data[o:o + 8].rstrip(b'\0').decode(),
            'version': f'{major}.{minor}.{build}',
            'qfe': flags & 0x1FFF, 'approved': (flags >> 13) & 3, 'debug': bool(flags >> 15),
        })

    tls = None
    if h['tls_directory']:
        t = h['tls_directory']
        tls = {
            'raw_data_start': read_va(t), 'raw_data_end': read_va(t + 4),
            'index_address': read_va(t + 8), 'callbacks': read_va(t + 12),
            'zero_fill_size': read_va(t + 16), 'characteristics': read_va(t + 20),
        }

    thunks = []
    slot = h.get('kernel_thunk')
    while slot is not None:
        v = read_va(slot)
        if not v:
            break
        thunks.append({'slot': slot, 'ordinal': v & 0x7FFFFFFF})
        slot += 4

    json.dump({'header': h, 'certificate': cert, 'sections': sections,
               'library_versions': libs, 'tls': tls, 'kernel_thunks': thunks},
              sys.stdout, indent=1)


if __name__ == '__main__':
    main(sys.argv[1])
