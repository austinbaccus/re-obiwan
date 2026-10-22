# Static recompiler (prototype)

Turns the game's x86 code into C that builds with MSVC, and checks the
result against the original machine code.

```
default.xbe ──Ghidra analysis──▶ p-code per instruction ──pcode2c.py──▶ build/generated/*.c
                                                                              │
                         recomp/runtime (CPU state, memory, dispatch) ◀───────┤
                                                                              ▼
                         recomp/tests/difftest: original x86 vs recompiled C, same inputs
```

Nothing generated is committed: `build/` is in .gitignore because the output
is a translation of the game's code.

## How it works

- **Translator** (`translator/pcode2c.py`, runs with pyghidra in the
  `ghidra-mcp` container). Ghidra already lifts every x86 instruction to
  p-code, a small register-transfer language with exact flag semantics. The
  translator emits one C function per guest function: `void f_XXXXXXXX(Ctx *ctx)`.
  - Registers are bytes in `ctx->r`, at the offsets of Ghidra's x86 register
    space, so any p-code varnode (including partial registers and flags) maps
    to a fixed location. Temporaries become a per-function local buffer.
  - Memory is `MEM(addr)`: `g_membase + addr`. The image keeps its original
    addresses.
  - `CALL` becomes a C call; the guest return address is still pushed on the
    guest stack so stack layout and `[esp+n]` argument access match the
    original exactly. `RET` becomes `return`.
  - Jumps inside the function become `goto`; jumps out of it are tail calls.
    Computed jumps switch over the targets Ghidra recovered and fall back to
    the runtime.
  - x87 values are host doubles stored in the 10-byte ST slots; only 80-bit
    memory loads and stores convert. x87 transcendental ops map to libm.
- **Runtime** (`runtime/`): the `Ctx` layout, memory and flag helpers,
  `rt_call` (address → recompiled function lookup), traps for missing
  functions, `int3` and divide errors.
- **Differential test** (`tests/`): a 32-bit Windows harness that maps the
  XBE at its real addresses, runs a function's original x86 code natively and
  its recompiled C from the same random starting state (registers, stack,
  a scratch area full of small ints, floats and pointers), and compares all
  eight registers, the x87 result and every byte of the image, stack and
  scratch memory. Each function runs in its own process with a timeout.
  `loader.c` explains how the harness gets the XBE's address range from
  Windows.

- **Host runtime** (`host/`): what the recompiled game runs on. `kernel.c`
  implements the xboxkrnl.exe functions the image imports (memory arena,
  files and the object namespace with `D:`/`T:`/`U:`/`Z:`, threads with the
  Xbox TLS layout, events, timers, SHA-1, time) and patches the kernel thunk
  table so `call [slot]` reaches them. The recompiled XAPI runs unchanged on
  top of that. `hooks.csv` lists guest functions replaced by host code
  (`hooks_xapi.c`: XInput, the exit paths); the translator emits a wrapper
  for each instead of its body. `boot.c` loads the XBE, runs `entry`, and
  prints a symbolized stack on a crash (host symbols are guest addresses).
  `obiwan.exe` is the same fixed-address loader as the test harness, grown
  to cover all 64 MB of guest RAM.

## Running it

```powershell
# 1. Translate (inside Docker; reads a copy of the Ghidra project)
docker compose exec ghidra-mcp bash -c '
  rm -rf /tmp/gcopy && mkdir /tmp/gcopy &&
  cp -r /workspace/ghidra/obiwan.gpr /workspace/ghidra/obiwan.rep /tmp/gcopy/ &&
  find /tmp/gcopy -name "*.lock*" -delete &&
  /opt/pyghidra-mcp/bin/python3 /workspace/recomp/translator/pcode2c.py /tmp/gcopy /workspace/build/generated all /workspace/recomp/host/hooks.csv 12'

# 2. Build (Windows, Visual Studio 2022 with C++ tools)
.\recomp\build.ps1

# 3. Test: every function, 3 random starting states each
.\build\x86\difftest.exe extracted\default.xbe 3 > build\difftest.jsonl
.\build\x86\difftest.exe extracted\default.xbe --one 002e0936 1   # one function, seed 1

# 4. Boot the game (translate with `all` and host/hooks.csv first)
.\build\x86\obiwan.exe extracted\default.xbe --trace       # RECOMP_TRACE_CALLS=1 logs indirect calls too
```

The game reads its files from the folder that holds `default.xbe` and keeps
its "hard disk" (save games, title metadata, cache partition) in `xbox_hdd`
next to the executable. Environment switches for unattended runs:

- `RECOMP_STALL=seconds`: when no frame has been presented for that long,
  print every guest thread's guest and host stack (three samples, then exit).
- `RECOMP_INPUT="300:START,700:A"`: press a gamepad button for 10 frames at
  those frame numbers (the attract loop only ends on START).
- `RECOMP_LAUNCH=path`: boot with saved launch data instead of a cold boot.
  When the game relaunches itself (`XLaunchNewImageA`) the host saves the
  data as `launch.bin`, restarts, and keeps it as `launch.last.bin`.
- `RECOMP_TRACE=1`, `RECOMP_TRACE_CALLS=1|2`: log kernel calls and hooks;
  indirect (or all) guest calls. About 100� slower.

The translator's third argument picks what to emit: `leaf` (functions
with no calls, computed jumps, special instructions or `fs:` accesses: the
ones the harness can test in isolation), `all`, or a file of addresses. The
fourth is `host/hooks.csv`, the fifth the number of worker processes: each
works on its own copy of the scratch project (about 350 MB and a JVM each),
and 12 of them turn the 25-minute single-process run into 9 minutes.
`build/generated/meta.json` always lists every function with its translation
status and features.

## Status

See the PR description / `docs/recomp/recompiler-notes.md` for the current
numbers. Known gaps, roughly in the order they will matter:

- Non-leaf functions are translated but not yet tested: they need the
  harness to provide callees (recompiled ones, or traps) and `fs:` (the SEH
  chain and TLS).
- C++ exceptions and SEH run the original CRT handlers; see "C++ exceptions
  and SEH" in `docs/recomp/recompiler-notes.md` for how continuations are
  entered (`rt_resume`, resumable functions).
- 10 functions use instructions the translator does not handle yet (port
  I/O, `cpuid` and a few MMX ops), all in library or startup code.
- x87 is modelled with 64-bit doubles. The harness runs the original code
  with the FPU in 53-bit precision mode, which is what makes results match;
  80-bit precision differences on the real console are not modelled.
- Tricks that rely on the real return address (`_alloca_probe`,
  `__SEH_prolog`, code that pops its own return address) need special
  handling in the runtime.
- Generated code is correct but naive: every register access goes through
  `ctx->r`. Register caching and flag elimination come later.
