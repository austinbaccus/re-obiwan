# Code map

Where the code in `default.xbe` comes from, and how execution gets from the
entry point to the game's frame loop. Function-level detail (15,501 functions after the RTTI pass in classes.md; the
numbers below are from the first pass of 12,860
with address range, section, class and any library name) is in
[functions.csv](functions.csv).

## How functions were identified

Ghidra's XBE loader applies XDK library signatures and Function ID during
analysis. That named:

| Source | Functions named | Where |
|---|---|---|
| D3D8 | 113 | `D3D` section (+147 unnamed helpers there) |
| DSOUND | 138 | `DSOUND` section (+284 unnamed) |
| XAPILIB | 35 | `.text` (23) and `XPP` (12) |
| XGRAPHC | 5 | `XGRPH` section (+288 unnamed) |
| MSVC runtime (LIBC / LIBCPMT / undname) | ~295 | `.text`, mostly `0x002CBD20`..`0x002DDF29` |
| Kernel jump stubs | 9 | `.text` |
| D3DX8 | 0 | `D3DX` section (403 functions, all unnamed) |

The library *sections* are reliable boundaries on their own: everything in
`D3D`, `D3DX`, `XGRPH`, `DSOUND` and `XPP` is XDK code, named or not. Inside
`.text` the boundaries come from the named functions, so they are approximate
where unnamed library helpers sit next to game code.

The `class` column in functions.csv is: `xdk` (library section, or a function
with an XDK namespace), `crt` (named MSVC runtime), `xboxkrnl` (kernel jump
stub), `thunk` (`jmp` to another function), `unknown` (everything else, which is
overwhelmingly game code).

## `.text` (`0x00011000`..`0x00368400`)

| Range | What | Notes |
|---|---|---|
| `0x00011000`..`0x0002A6D5` | game code | contains `main` at `0x000251F0` |
| `0x0002A6D6`..`0x0002E836` | XAPILIB | startup (`entry`, `XapiInitProcess`, `_cinit`, `_rtinit`), threads, launch data, drive mapping, errors; some unmatched XAPI helpers in between |
| `0x0002EB60`..`0x002CBD16` | game code | the Tanis engine and the game: ~10,000 functions, 2.5 MB |
| `0x002CBD20`..`0x002DDF29` | MSVC runtime | LIBCPMT (iostreams, locale, `basic_string`), LIBC (stdio, heap, string, `__CxxFrameHandler`, SEH helpers, 64-bit math), the C++ name undecorator |
| ~`0x002DE752`..`0x002E09FF` | XAPILIB | `XCalculateSignature*`, `GetTimeZoneInformation`, file/time helpers, more kernel jump stubs |
| `0x002E0BB0`..`0x00320A02` | unmatched | ~1,000 functions. At least part is middleware: `0x00303E80` is IJG libjpeg's colorspace setup (`jpeg_default_colorspace` shape). Needs classifying |
| `0x00320A10`..`0x00320EEC` | MSVC runtime | `_strnicmp`, `_itoa`, `setjmp`/`longjmp`, `frexp`, 64-bit shifts |
| `0x00320EEC`..`0x003613B1` | C++ EH funclets | `__ehhandler$...` stubs (`mov eax, FuncInfo; jmp __CxxFrameHandler`) and `__unwindfunclet$...` destructor calls. Ghidra created no functions here (~260 KB): they are reached only through EH tables |
| `0x003613B2`..`0x003683F9` | static initializers | 1,609 small `$E` dynamic-initializer stubs and the destructor thunks they register with `atexit`. Reached only through the `__xc_a` table |

Approximate size split: unclassified `.text` functions ~2.58 MB (mostly game,
plus the unmatched block above), XDK library sections ~0.34 MB plus a few
tens of KB of XAPI in `.text`, CRT ~0.08 MB, EH and initializer stubs ~0.29 MB.

## Code reached only through data

About 16,000 pointers in `.rdata`/`.data` point into executable sections, and
9,275 of them point at addresses Ghidra did not mark as a function start. A
recompiler that discovers code by following calls from the entry point will miss
all of these, so it must seed its work list from:

- **Initializer tables** walked by `_cinit` (`0x0002DE60`): C initializers
  `__xi_a`..`__xi_z` at `0x00425AD0`..`0x00425AE4` (5 slots) and C++ dynamic
  initializers `__xc_a`..`__xc_z` at `0x00422BB0`..`0x00425ACC` (3,015 slots;
  `0` and `-1` entries are skipped). There is also an optional hook pointer at
  `0x00440AF8` called first.
- **C++ EH tables**: every `FuncInfo` (pointed to by an `__ehhandler$` stub)
  lists unwind funclets and catch handlers. The function prologues install the
  stub with `push <stub>; mov eax, fs:[0]; push eax; mov fs:[0], esp`.
- **Vtables**: the image carries MSVC RTTI for 885 classes and structs
  (`.?AV...@@` type descriptors in `.rdata`), so every vtable can be found from
  its `CompleteObjectLocator` and each slot becomes a known function entry.
  Names like `CEntityHuman`, `CTanisMsg<...>`, `CPhysNode`, `CD3DContext` also
  give the class hierarchy for free.
- **Thread and callback entry points** passed to `CreateThread`,
  `XRegisterThreadNotifyRoutine`, `atexit`, DSOUND/D3D callbacks, etc. The very
  first one is `0x0002AC05` (below), which Ghidra also did not make a function.
- **Switch tables**: 878 computed jumps; MSVC places the jump tables inline in
  `.text` after the function, so they must be decoded and excluded from code.

## Startup path

```
entry                    0x0002AC74   XBE entry point (retail XOR-decoded)
 ├─ sets up TLS sizes, _tls_index
 ├─ CreateThread(start = 0x0002AC05)        XAPI main thread, default stack
 │    0x0002AC05:
 │     ├─ XapiInitProcess   0x0002DD51      heap, D: and utility drive mapping
 │     ├─ TLS block init via fs: (0x0002AC0A..0x0002AC4B)
 │     ├─ _rtinit           0x0002DEB8
 │     ├─ _cinit            0x0002DE60      runs __xi / __xc tables
 │     ├─ main(0, 0, 0)     0x000251F0      game
 │     └─ XapiBootToDash(1, 1, 0)           if main returns
 └─ NtClose(thread)        0x0002DEE2       entry thread then returns
```

## `main` (`0x000251F0`)

1. CRT/engine setup (`0x002D0440`), `XInitDevices` with the gamepad device
   table, platform init (`0x0002AB32`).
2. Allocates the top-level singletons through a global allocator object
   (`PTR_DAT_004291F8`, called through its vtable) and stores them in `.bss`
   globals:
   - `0x004824C4`: the game/application object (built by `0x00227340`); its
     flags at `+0x74`, `+0x75`, `+0x80`, `+0x81` select attract-demo playback,
     front-end, and "launched from another XBE" modes.
   - `0x004824CC` (also copied to `0x004B0428`): object from `0x00025070`.
   - `0x004824C8`: object from `0x000CBE30` (has a per-frame virtual at `+4`).
3. `XGetLaunchInfo`: if the XBE was relaunched with launch data, it restores
   settings from it and can jump straight into a level (`"mp_"` prefix check,
   `"PlayDemo"`, `"demo1"`), otherwise it starts the front end.
4. Subsystem init: `0x002116A0`, `0x002298B0`, `0x00207170`; front end
   (`0x000A3580`, which also loads `start.swf`).
5. **Main loop at `0x000256ED`:**

   ```
   while (Game_Tick(app)) {            // 0x0021CB90, __fastcall, ECX = app
       if (!Pump()) Pump();            // 0x002081D0 -> 0x002C4620, calls 0x0002A9F0
       if (app->flag75) obj_4824C8->vtbl[1]();
       clear a 1 KiB stack buffer
   }
   ```

6. Teardown: destroys the singletons through their vtables (`vtbl[0]` is the
   scalar deleting destructor), then `XLaunchNewImageA` (back to the dashboard
   or into another XBE).

## Per-frame tick (`0x0021CB90`)

One call is one frame. In order:

- `0x000E51C0`: frame begin.
- Time: reads the clock twice through `0x000DB8E0` (static-initialized once,
  guarded by bit 0 of `0x004F34B0`), computes `dt` as a float from the previous
  frame's timestamp at `0x004F34A8` (a `double`), and **clamps `dt` to 0.1 s**.
  Demo playback (`+0x74`) records `dt`; replay (`+0x75`) substitutes the
  recorded one (`0x00207450` / `0x002074A0`).
- Simulation, each taking `dt`: `0x001804D0`, then (unless paused, `+0x80`)
  `0x0024E2F0` and `0x001793C0`, then `0x00158340`, `0x000A41C0`.
- Demo/attract handling and front-end flow (`0x000CA090`, `0x00209A60`,
  `0x0020EDA0`, `0x000C9E30`, `0x002144D0`, timeout check against
  `*(float*)(0x0049C7A4 + 0x138)` which is the game clock).
- `0x000CA0C0`, then `0x0021C4E0(dt)` (likely render), `0x000DDCE0` (unless
  paused), `0x000E5480` (frame end / present).
- Returns `app->+0x3C == 0`; setting that byte ends the loop.

Function purposes past "called from the tick with `dt`" are inferred from call
order and arguments only; none have been opened yet. They are the obvious next
targets for naming.

## Other observations

- `int3` after a failed check appears throughout game code (QA build asserts).
  Treat them as a trap/abort in the recompiled output.
- `rdtsc` appears twice in game code (`0x000E5220`, `0x0033F132`); the latter
  is in the EH/initializer area and still needs checking. Map it to a host
  counter scaled to 733 MHz.
- Game code has no other privileged instructions. The `in`/`out`/`wbinvd`
  instructions are all inside `D3D` (hardware access) or are false
  disassembly in data sections. One `in` at `0x0014FD17` is a Ghidra
  overlapping-decode error: the bytes are a `call` at `0x0014FD16`.
- Immediate references to NV2A registers (`0xFD000000`..) appear twice in
  game code and to MCPX APU registers (`0xFE800000`..) only in `DSOUND`; the
  80 game-code immediates in the `0x80000000` range are contiguous-memory
  (physical-mapped) addresses, usually texture or push-buffer memory handled
  through D3D. Each needs a look, but the game appears to touch hardware only
  through the XDK libraries.
