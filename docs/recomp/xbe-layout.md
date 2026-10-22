# XBE layout

Facts from the image header of `extracted/default.xbe`, produced by
`tools/xbe_info.py`. All addresses are virtual addresses in the Xbox address
space; the image is linked to load at `0x00010000` and contains no relocations,
so a recompiler can treat every absolute address in the code as final.

## Identity

| Field | Value |
|---|---|
| File size | 4,714,496 bytes |
| MD5 / SHA-256 | `082b940255be5db275961c68c66e5a6a` / `fedbc1c6576a7163303eb9ea79a70cd83fcff2e369995329413fc5fe06ba7e55` |
| Title | Star Wars Obi-Wan |
| Title ID | `0x4156000F` (`AV-015`) |
| Certificate version | 1, disc 0 |
| Game region | `0x4` (rest of world / PAL) |
| Allowed media | `0x2` (DVD-X2) |
| Build key | retail (entry point / thunk XOR keys `0xA8FC57AB` / `0x5B6D40B6`) |
| Link time (PE) | 2002-02-07 01:30:15 UTC |
| Certificate time | 2002-02-12 14:23:09 UTC |
| Original PE | `D:\Tanis\Code\Engine\main\main_xbox___Xbox_QA\main_xbox.exe` |

The developer engine is called **Tanis** (namespace of many classes and the
`CTanis*` RTTI names). The build configuration is "Xbox QA": optimized, but with
assertions left in (hundreds of `if (!cond) int3;` sites in game code).

Because hashes are recorded, docs and addresses in this folder apply to exactly
this image. Other regions/revisions need their own map.

## Image header

| Field | Value |
|---|---|
| Base address | `0x00010000` |
| Size of headers | `0x99C` |
| Size of image | `0x5195E0` (image ends at `0x005295E0`) |
| Entry point | `0x0002AC74` (decoded) |
| Kernel thunk table | `0x003BC220` (decoded; first bytes of `.rdata`) |
| Non-kernel import directory | none |
| TLS directory | `0x003BE7D8` |
| Init flags | `0x1` (mount utility drive) |
| PE stack commit | `0x50000` (320 KiB main thread stack) |
| PE heap reserve / commit | `0x100000` / `0x1000` |

## Sections

XBE section flags: `1` writable, `2` preload, `4` executable, `8` inserted
file, `0x10` head page read-only, `0x20` tail page read-only.

| Section | Flags | VA start | VA end | Virtual size | File offset | Raw size | Contents |
|---|---|---|---|---|---|---|---|
| `.text` | `0x16` X P HeadRO | `0x00011000` | `0x00368400` | `0x357400` | `0x001000` | `0x357400` | game code, XAPI, CRT, EH funclets |
| `D3D` | `0x07` W P X | `0x00368400` | `0x0037B0F4` | `0x12CF4` | `0x359000` | `0xF884` | Direct3D 8 (NV2A push-buffer driver) |
| `D3DX` | `0x07` W P X | `0x0037B100` | `0x00397F70` | `0x1CE70` | `0x369000` | `0x1CE68` | D3DX8 + embedded zlib 1.1.3 / libpng / IJG libjpeg |
| `XGRPH` | `0x07` W P X | `0x00397F80` | `0x003AAC60` | `0x12CE0` | `0x386000` | `0x1206C` | XGraphics (swizzle, surface headers) |
| `DSOUND` | `0x07` W P X | `0x003AAC60` | `0x003B4EF8` | `0xA298` | `0x399000` | `0xA164` | DirectSound (MCPX APU driver) |
| `XPP` | `0x07` W P X | `0x003B4F00` | `0x003BC21C` | `0x731C` | `0x3A4000` | `0x731C` | XAPI peripherals: USB, XInput, memory units |
| `.rdata` | `0x06` P X | `0x003BC220` | `0x00422B88` | `0x66968` | `0x3AC000` | `0x66958` | kernel thunks, constants, vtables, RTTI, strings |
| `.data` | `0x07` W P X | `0x00422BA0` | `0x0051F03C` | `0xFC49C` | `0x413000` | `0x5F91C` | initialized data; `0x9CB80` bytes of BSS after raw data |
| `DOLBY` | `0x26` P X TailRO | `0x0051F040` | `0x00525DD8` | `0x6D98` | `0x473000` | `0x6D84` | audio DSP code loaded by DSOUND (Dolby Digital encoder), **not x86** |
| `$$XTIMAGE` | `0x38` HeadRO TailRO Ins | `0x00525DE0` | `0x005285E0` | `0x2800` | `0x47A000` | `0x2800` | title image (dashboard icon) |
| `$$XSIMAGE` | `0x38` HeadRO TailRO Ins | `0x005285E0` | `0x005295E0` | `0x1000` | `0x47D000` | `0x1000` | save-game image |

Notes for the recompiler:

- The executable bit is set on `.rdata` and `.data` (normal for XBEs). Do not
  treat those sections as code: the few hundred "instructions" Ghidra shows there
  are data decoded as x86. The same holds for `DOLBY`, which is a blob uploaded
  to the audio DSP, and for the two `$$X*IMAGE` sections.
- Every section is `preload`, so the whole image is resident from boot. There are
  no `XeLoadSection` overlays to model, although the game imports
  `XeLoadSection`/`XeUnloadSection`: their only callers are two small XAPI
  wrappers (`0x0002E40B`, `0x0002E42C`, presumably `XLoadSection`/`XFreeSection`), which are
  called only from XAPI drive-mapping code (`0x0002D87C`,
  `XapiMapLetterToDirectory`). Game code never loads or frees a section.
- `.data` has `0x9CB80` bytes of zero-filled tail (BSS), from `0x004824BC` to
  `0x0051F03C`. The globals the main loop uses (`0x004824C0`..`0x004824CC`) sit
  right at the start of it.

## Library versions

The XDK build is **3911** (the December 2001 XDK), with two QFE'd libraries.
Every entry has QFE 1 and the "approved" flag set.

| Library | Version |
|---|---|
| XAPILIB | 1.0.3911 |
| XBOXKRNL | 1.0.3911 |
| LIBC | 1.0.3911 |
| LIBCPMT | 1.0.3911 |
| D3D8 | 1.0.3948 |
| D3DX8 | 1.0.3911 |
| XGRAPHC | 1.0.3911 |
| DSOUND | 1.0.3936 |

These versions are what to feed signature tools (Cxbx-Reloaded / xemu HLE
databases, XbSymbolDatabase, Ghidra FID sets) when matching more of the
library code. No XNET/XONLINE library is linked: the game has no network code.

## TLS

| Field | Value |
|---|---|
| Raw data template | none (start = end = 0) |
| Index variable | `0x00485768` (`_tls_index`) |
| Zero-fill size | 12 bytes |
| Callbacks | none |

So each thread gets 12 zeroed bytes of TLS. Code reaches it through the
`fs`-relative KPCR (`fs:[0x28]` is the current KTHREAD, `fs:[0x4]` the stack
base) plus the index at `0x00485768`; the startup thread sets it up at
`0x0002AC0A`. A recompiler needs a per-thread emulated KPCR/KTHREAD block for
these reads (9,000+ `fs:` accesses in the image, nearly all SEH frame links at
`fs:[0]`).

## Kernel imports

112 imports, all by ordinal, through a table of 4-byte slots at
`0x003BC220`..`0x003BC3DC`. Full list in [kernel-imports.md](kernel-imports.md).
