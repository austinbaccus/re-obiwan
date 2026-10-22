# Notes for a static recompiler

What the map implies for translating `default.xbe` (32-bit x86, Pentium III
733 MHz) into a native Windows 11 program. Numbers come from a full pass over
Ghidra's listing (1,079,975 instructions).

## Recompile, replace, or drop

| Part | Range | Plan |
|---|---|---|
| Game + Tanis engine | most of `.text` | **recompile** |
| C++ EH funclets, static initializers | `0x00320EEC`..`0x003683F9` | **recompile** (seeded from tables, see code-map.md) |
| MSVC runtime | `0x002CBD20`..`0x002DDF29`, `0x00320A10`..`0x00320EEC` | recompile as-is at first (it is plain x86 with no OS calls beyond XAPI), or map the common functions (`malloc`, `memcpy`, `_ftol`, string, stdio) to host CRT later |
| XAPILIB | `.text` `0x0002A6D6`..`0x0002E836`, `~0x002DE752`..`0x002E09FF`, `XPP` | **replace** with a host implementation at the function level (files, threads, launch data, input, memory units, save games) |
| D3D8 | `D3D` | **replace**: high-level Direct3D 8 on D3D12/Vulkan, intercepting at the named `D3DDevice_*` / `D3DResource_*` / `D3DTexture_*` entry points. The library writes NV2A push buffers directly, so recompiling it would mean emulating the GPU |
| XGraphics | `XGRPH` | **replace** (swizzle/unswizzle helpers are simple to reimplement) |
| D3DX8 | `D3DX` | **replace** with host equivalents, or recompile if it turns out to be pure math. It embeds zlib 1.1.3, libpng and IJG libjpeg, which can be linked from source instead |
| DirectSound | `DSOUND` | **replace** with a host audio backend at the `IDirectSound*` / `CMcpx*` API level. The library drives the MCPX APU through MMIO (`0xFE800000`..) |
| `DOLBY` | `DOLBY` | **drop**: APU DSP code, never executed by the CPU |
| Kernel | thunk table `0x003BC220` | **implement** only what remains reachable after the above are replaced |

The cleanest boundary is the set of named XDK entry points: every call from
game code into a library section, or into a named XAPI function, becomes a
call into the host runtime. Enumerate those call sites from the call graph
before writing any HLE code; that list is the real API surface.

## Instruction-level hazards

| Hazard | Count | Where | Consequence |
|---|---|---|---|
| x87 floating point | ~129,000 | 125k in `.text` | The game does nearly all float math on x87. Plain `float`/`double` in C gives slightly different results from 80-bit intermediate precision; the FPU control word is set at startup (`__ctrlfp` is linked), which decides precision and rounding. Start with host `double` and compare against an emulator only where physics or demo playback diverge (attract-mode demos replay recorded `dt` and input, so they are a good determinism test) |
| SSE | ~450 | mostly `D3D`, `XGRPH`, plus 253 in `.text` | small; Pentium III SSE1 only |
| MMX | ~1,270 | almost all in `D3DX` (1,198) | replaced with the library |
| Indirect calls | 11,528 | 10,637 in `.text` | overwhelmingly C++ virtual calls (`call [reg+off]`). Needs a runtime lookup from guest address to host function for any target not proven statically; the class hierarchy and vtables recovered from RTTI (classes.md) bound the possible targets of most of them |
| Indirect jumps | 878 | 854 in `.text` | switch tables (inline in `.text`) plus jumps through the kernel thunk table |
| `fs:` accesses | ~9,000 | everywhere | SEH frame chain at `fs:[0]` in almost every function with C++ objects on the stack; TLS through `fs:[0x28]`/`fs:[0x4]`. Needs a per-thread emulated KPCR block |
| `int3` | ~900 | game code | QA-build asserts; map to a fatal error with the guest address |
| `rdtsc` | 2 | `.text` | map to a host timer scaled to 733 MHz |
| `setjmp`/`longjmp` | 3 + 3 calls | `D3DX` only | libpng/libjpeg error paths; gone if D3DX is replaced |
| privileged / port I/O | handful | `D3D` only (and false decodes in data) | gone with D3D replacement |

## C++ exceptions and SEH

`__CxxFrameHandler` (`0x002CF914`) is referenced by about 1,800 `__ehhandler$`
stubs, so EH state is maintained in most game functions. The game also
**throws**: `_CxxThrowException` (`0x002CFD19`) has 160 call sites in game code,
and `typeid` (660) and `dynamic_cast` (311) are used heavily (api-surface.md).
So EH cannot be treated as dead code. Options:

1. Keep the guest model: emulate the `fs:[0]` registration chain and run the
   original `__CxxFrameHandler`/`RtlUnwind` path (recompiled CRT code plus a host
   `RaiseException`/`RtlUnwind`). Faithful, no need to understand each
   function, and the unwind funclets are already known entry points.
2. Translate each guest `try` region to host C++ `try`/`catch` or setjmp-style
   unwinding. Faster at run time but needs every FuncInfo decoded and mapped.

Start with option 1. The RTTI that `catch` matching, `typeid` and
`dynamic_cast` read stays at its original address in the guest image, so it
works unchanged (classes.md).

Option 1 is what the prototype does. The CRT's handlers run as recompiled
code; the host kernel supplies `RtlRaiseException` (walk the `fs:[0]` chain,
call each handler as a cdecl routine) and `RtlUnwind` (call the handlers with
`EXCEPTION_UNWINDING` up to the target frame, then continue at the caller's
return point). Two things make the continuations work:

- Functions that register a handler, take the address of one of their own
  instructions, or call `setjmp` are emitted *resumable*: `ctx->resume_ip`
  enters them at any instruction through their dispatch switch (about 1,900
  functions). A call to an address inside such a function is a catch or
  filter funclet and runs the same way. `docs/recomp/resumable-functions.txt`
  adds any the detection misses; `rt_resume` names the function when it is
  asked to enter one that is not.
- Every `ret` records the return address in `ctx->ret_ip`. `rt_resume(ip)`
  runs the function containing `ip` from there and, when it returns, keeps
  going in the caller at `ret_ip`, and so on up the guest stack until the
  return address is one the host pushed (`0xDEAD0000 | depth`), at which point
  it `longjmp`s to that host entry (`rt_host_call`). The host frames of the
  throwing path are simply abandoned until then, so each throw leaves its
  host stack depth behind until the thread (or callback) ends; threads have
  64 MB host stacks to absorb that.

## Memory model

- The image is fixed at `0x00010000`..`0x005295E0` with no relocations, so the
  simplest correct layout reserves that exact range in the host process (or
  adds a constant base to every guest address). Windows normally leaves the
  low 64 KB unmapped and the range above it is free in a 32-bit or
  large-address-aware process; for a 64-bit host, a 4 GB guest window with a
  base pointer is the usual design.
- Xbox user space is 64 MB of RAM, with contiguous memory also visible at
  `0x80000000` (physical mirror) for GPU/APU buffers. Game code has ~80
  immediates in that range; they need to land in the same window.
- Heaps come from `NtAllocateVirtualMemory`, `MmAllocateContiguousMemory[Ex]`
  and `ExAllocatePool*` (via XAPI and the CRT). Implement them over the guest
  window rather than the host heap so guest pointers stay 32-bit.

## Threads and timing

- The game itself runs on the XAPI main thread created at `0x0002AC74`. D3D8,
  DSOUND and the USB stack create more threads, DPCs and interrupt handlers;
  those go away with HLE replacements. Check whether game code calls
  `CreateThread` itself (for streaming, `__AudioStreamList.txt` suggests
  streamed audio) before assuming a single-threaded game.
- The frame tick clamps `dt` to 0.1 s and is variable-step, so running faster
  than 60 Hz on PC should mostly work; anything that assumes 30/60 Hz will show
  up in the per-subsystem tick functions listed in code-map.md.

## Prototype results (2026-10-22)

`recomp/` holds the translator, runtime and differential test described in
`recomp/README.md`. Over the whole image (15,501 functions plus 3,015
initializer entries added later):

- every function translates; the output builds with MSVC for x86 and x64;
- the differential test over all 13,938 `.text` functions, 3 random states
  each: 4,595 functions match the original on every run that executed
  (registers, x87 result, all of memory), 9,239 fault natively on every run
  (wild pointers from random inputs, so untestable this way), 41 reach
  library code that was not recompiled, 11 time out;
- 52 functions differ, all explained: `FNSTSW` results (Ghidra models only
  C0-C3 of the x87 status word, not TOP or the sticky exception flags, and
  the game only tests C0/C2/C3), `FNSTCW` reserved bit 6, functions that
  read their own return address (`_alloca_probe`, `__SEH_prolog/epilog`),
  and two one-bit float rounding differences from 80-bit intermediates.

The recompiled XAPI runs unchanged on the host kernel in `recomp/host`:
`XapiInitProcess` (heap, D:/T:/U:/Z: drive mapping, title metadata, cache
partition) and the CRT and C++ static initializers complete.

## Suggested next steps

Done: the API surface ([api-surface.md](api-surface.md)) and the RTTI classes
and vtables ([classes.md](classes.md)), applied to the Ghidra project.

1. Check whether `CD3DContext`/`CDrawContext` (87 virtual methods) own most of
   the D3D8 calls. If so, the renderer boundary can be that class rather than
   D3D8 itself.
2. Trace DirectSound buffer creation and use through the DSOUND COM vtables
   to finish the audio surface, and identify the game's `CreateThread` target.
3. Classify the unmatched block `0x002E0BB0`..`0x00320A02` (game vs. libjpeg or
   other middleware).
4. Teach Ghidra about the EH funclets and `$E` initializers so the function
   list covers all of `.text`.
5. Pick the recompiler architecture (one C function per guest function with a
   guest register file, as in XenonRecomp / N64Recomp, is the proven shape for
   this size of binary) and prototype it on a small leaf subsystem.
