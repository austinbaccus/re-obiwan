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
    to a fixed location. Inside a function, the general registers (with their
    8- and 16-bit parts), the flags and the x87 stack it uses are C locals,
    loaded from `ctx->r` at entry and written back around calls, returns,
    traps and any op that needs a register's address; integer temporaries are
    locals too. The C compiler then keeps them in host registers and drops
    flag computations nothing reads (about 5x faster than keeping everything
    in `ctx->r`). The small access helpers in `guest.h` are forced inline:
    the generated functions are too large for MSVC to inline them otherwise.
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

- **Renderer** (`host/render*.c`): `hooks_d3d.c` keeps the guest's D3D8
  objects and hands the device state and draws to `render.h`. The default
  backend is Direct3D 11 (`render_d3d11.c`): the game's shaders are its
  source translated to SM4 HLSL (`shader_xlate.c`), and draws that use the
  fixed-function pipeline get shaders generated from the texture stage and
  transform states, cached by state. `RECOMP_RENDERER=d3d9` selects the
  Direct3D 9 backend it was ported from (`render_d3d9.c`), kept as the
  reference; `render_common.c` holds what both share (texture formats and
  swizzling, vertex formats, the Xbox state encodings).

- **Sound** (`host/hooks_dsound.c`, `host/audio.c`): DirectSound is replaced
  at its API too. Each sound buffer is an XAudio2 voice playing a host copy
  of its data (Xbox ADPCM decoded to PCM) with DirectSound's play, stop,
  loop, cursor, volume and frequency semantics, and 3D buffers get distance
  attenuation and a stereo pan. Streams (music, speech) queue the game's
  packets on a voice and report each one done once it has played; the game
  reads them with ReadFileEx, whose completion the host kernel delivers as an
  APC at the thread's next alertable wait, as the Xbox kernel does.
  The engine starts before the XBE is loaded, like the Direct3D device:
  Windows cannot create threads in the process afterwards.

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

# 4. Boot the game (translate with `all` and host/hooks.csv first). It has its own
#    build tree, build\game (the obiwan.exe in build\x86 goes stale); configure it
#    once from an x64_x86 VS prompt:
#      cmake -S recomp -B build\game -G Ninja -DCMAKE_C_COMPILER=cl -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build\game --target obiwan
.\build\game\obiwan.exe extracted\default.xbe   # a 640x480 window; a controller, or Esc = START, Enter = A, arrows = stick
```

Without a path, `obiwan.exe` finds `extracted\default.xbe` (or
`default.xbe`) in its own folder or the folders above it. `build\game` also
gets `Obi-Wan.exe`, the same game built as a windowed program for a
double-click: no console, and instead of environment variables it reads
`obiwan.ini` next to it (`RECOMP_*=value` lines, and `xbe=path` relative to
that folder; written as a commented template on the first start). A setting
already in the environment wins over the file, and `obiwan.exe` ignores the
file. Its output goes to `obiwan.log` next to it (appended to when the game
relaunches into a level), and start-up errors and crashes also show in a
message box.

Controllers: Windows XInput pads 1..4 are Xbox ports 1..4 and can be plugged
in or out while the game runs. A, B, X and Y are the analog face buttons
(fully pressed), LB is White, RB is Black, the triggers and sticks pass
through with a radial dead zone (`RECOMP_DEADZONE=0.15`, a fraction of full
travel), and rumble reaches the pad (`RECOMP_NORUMBLE=1` turns it off).

The game reads its files from the folder that holds `default.xbe` and keeps
its "hard disk" (save games, title metadata, cache partition) in `xbox_hdd`
next to the executable. Environment switches for unattended runs:

- `RECOMP_PROFILE=seconds`: after that long, sample every guest thread's
  host instruction pointer about once a millisecond for ten seconds and
  print the 40 functions found running most often (self time).
- `RECOMP_STALL=seconds`: when no frame has been presented for that long,
  print every guest thread's guest and host stack (three samples, then exit).
- `RECOMP_INPUT="300:START,700:A,1950:LSUP:900"`: press a gamepad button for
  10 frames (or the given count) at those frame numbers (the attract loop
  only ends on START); LSUP/LSDOWN/LSLEFT/LSRIGHT push the left stick, and
  entries can overlap (LSUP with LSRIGHT steers diagonally). A scripted run
  ignores real controllers and the keyboard, so playing at the same time
  cannot change a test. Otherwise the keyboard stands in for the pad while
  the game window has focus: Esc = START, Enter = A, Space = B,
  Backspace = BACK, arrows = left stick.
- Scripts for relaunched runs: `RECOMP_INPUT` and `RECOMP_SHOT` take
  `this run|next run|...`; each relaunch gets what follows the first `|`,
  and its shots are named `build\shot_<run>_<frame>.bmp`.
- `RECOMP_HDD=dir`: use another emulated hard disk than `xbox_hdd` next to
  the executable (a scratch copy for test runs; the real one keeps saves).
- `RECOMP_LAUNCH=path`: boot with saved launch data instead of a cold boot.
  When the game relaunches itself (`XLaunchNewImageA`) the host saves the
  data as `launch.bin`, restarts, and keeps it as `launch.last.bin`.
- `RECOMP_SHOT="900,1200"`: save the back buffer as `build\shot_<frame>.bmp`
  at those frames (works with the window hidden or covered).
- `RECOMP_RES=1920x1440`: render at that size (Direct3D 11 only). The game
  still draws for its 640x480 back buffer; the host's back buffer, depth
  buffer and back-buffer viewports are scaled, so 3D, text and the HUD are
  all drawn at the higher resolution. Off-screen targets (shadow buffers)
  keep their sizes. Screenshots are saved at the rendered size.
  A size wider than 4:3 (`5120x1440`) is widescreen: the cameras keep their
  vertical field of view and see further to the sides (`widescreen.c`
  widens the angle every camera sets, so culling follows), and the 2D
  layer (HUD, text, menus) stays 4:3 in the middle, clipped to it.
  Untextured draws over the whole screen (fades, flashes, cinema bars)
  still cover the full width.
- F1: every player's health back to full, for testing (a dead player stays
  dead). The fields it uses are in `docs/recomp/game-structures.md`.
- F3 (or `RECOMP_FPS=1` from the start): a performance overlay in the top
  left corner (Direct3D 11): frames per second, frame time and the worst
  one, the CPU time of the thread that presents, and where a frame goes:
  game code (the rest), draw submission with texture uploads and shader
  compiles, and present. `RECOMP_FPSLOG=1` also prints it to stderr
  twice a second.
- `RECOMP_FULLSCREEN=1`: a borderless window covering the primary monitor,
  with the picture centred pixel for pixel and black around it (Alt+F4
  quits).
- `RECOMP_FIXEDFPS=60`: the guest's clocks advance 1/60 s per presented
  frame (and with the host's while nothing is presented, as when loading),
  so a frame number shows the same moment however fast the host runs.
  Comparing renderers: run the same script with `RECOMP_RENDERER=d3d9` and
  without, and diff the shots (levels match; the intro, which waits on
  loading threads, does not quite).
- `RECOMP_RENDERER=d3d9`: the Direct3D 9 reference renderer.
  `RECOMP_D3DDEBUG=1`: Direct3D 11 debug layer, its messages on stderr
  (needs the Graphics Tools optional feature). `RECOMP_SHADERLOG=1`: print
  each generated fixed-function shader. `RECOMP_NOVSYNC=1`: present
  without waiting for vertical sync.
- `RECOMP_MAXFPS=n`: at most n frames a second (0: no limit). The game
  limits itself to 120 (the `sys_fpsclamp` cvar) by spinning at the end of
  each frame; the host replaces that with sleeping until the next frame is
  due, at 120 unless this says otherwise. Vsync still applies on top.
- `RECOMP_STATELOG=frame`: print the device state (blending, stage
  operations, where the first vertex lands on screen) behind every draw of
  that frame.
- `RECOMP_SHADOWLOG=frame`: for the draws of that frame that sample a shadow
  buffer, the light projection constants and where the vertices land in it.
  `RECOMP_NOSHADOW=1` leaves those draws out; `RECOMP_SHADOWDRAWS=a-b` keeps
  only the a-th to b-th of each frame.
- `RECOMP_VSLOG=1`: log shader assembly, vertex declarations, the vertex
  shader constants set (each register/count once) and stream strides.
- `RECOMP_DSLOG=1`: log sound buffers as they are created and played, and
  each DirectSound interface method the game calls (once). `RECOMP_NOAUDIO=1`
  runs silent.
- `RECOMP_RSLOG=1`: log each change of a deferred render state (D3D8 3948
  indices 82..115, read from `D3D_g_RenderState` before each draw).
- `RECOMP_FOG=start,end,RRGGBB`: force linear fog on every draw, to check the
  fog path in levels that do not enable fog themselves (draws multiplied into
  the scene, such as soot decals, fade to white instead).
- `RECOMP_PROBE=frame,x,y`: after each draw of that frame, report when the
  back buffer pixel at (x, y) changes and what the draw was (which draw
  paints an artefact).
- `RECOMP_DUMPTEX=1` or a guest texture address: write uploaded textures as
  `build\tex_*.bmp` plus the raw guest bytes.
- Level select for tests: copy a `launch.last.bin` from a run that relaunched
  into a level, replace the level name at offset 7 (`L01_1`, `L02_1a`,
  `L03_2a`, `L04_3a`, `L05_1`, ...) and boot it with `RECOMP_LAUNCH`. The rest
  of the data still describes the original level, so some levels misbehave
  (L02_1a fails its mission at once).
- `RECOMP_TRACE=1`, `RECOMP_TRACE_CALLS=1|2`: log kernel calls and hooks;
  indirect (or all) guest calls. About 100� slower.
- `RECOMP_NATIVE=117010,0325a0`: run those guest functions, and everything
  they call, as the original x86 code instead of the recompiled C (the XBE
  sits at its own addresses, so it runs on the host CPU against the same
  memory). Kernel calls from native code are forwarded to the host kernel;
  a hooked library function (D3D, DirectSound, XAPI) reached natively is
  not. For narrowing a behaviour difference down to a mistranslated
  function. Runs of the game are not repeatable frame for frame (guest
  threads), so compare several runs per setting.
- `host_watch(addr, len)` (boot.c), called from a temporary probe in the
  generated code: a hardware write watchpoint that prints the recompiled
  instruction, guest registers and guest return addresses of each write
  (`RECOMP_WATCHMAX` hits, default 100).

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
- x87 rounding (FRNDINT, FIST/FISTP) follows the rounding control the guest
  loads with FLDCW, and SSE conversions the one in MXCSR. The CRT's `ceil`
  and `floor` switch modes around FRNDINT; with round-to-nearest, `ceil(0.5)`
  was 0, and the physics step could not halve its time step, so walking
  into a wall immobilized and killed the player (#51).
- Tricks that rely on the real return address (`_alloca_probe`,
  `__SEH_prolog`, code that pops its own return address) need special
  handling in the runtime.
- Indirect calls still go through `rt_call`'s lookup; calls to proven
  virtual-call targets could be direct.
