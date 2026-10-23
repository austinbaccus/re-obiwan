# re-obiwan

Reverse engineering *Star Wars: Obi-Wan* (original Xbox, 2001) from the owner's
own disc image, with the long-term goal of a native Windows 11 build.

## Layout (inside the container, repo is mounted at /workspace)

- `obiwan.iso` — the owner's game image. Never commit it or anything extracted from it.
- `extracted/` — game files from `extract-iso` (`default.xbe` is the executable). Gitignored.
- `ghidra/` — shared Ghidra project `obiwan.gpr`. Gitignored.
- `docker/` — image definition and helper scripts (`extract-iso`, `start-ghidra-mcp`, `analyze-headless`).

## Tools

- The `ghidra` MCP server (pyghidra-mcp, headless) runs in the `ghidra-mcp`
  container against `ghidra/obiwan.gpr`, with the XBE loader installed. Use it for
  decompiling, xrefs, searching, renaming and commenting.
- `analyze-headless` runs Ghidra scripts against the same project. Stop the
  `ghidra-mcp` service first: a Ghidra project can only be opened by one process.
- `extract-xiso` / `extract-iso` for the disc image.
- In MCP calls the binary is named `/default.xbe-076e14`.

## The recompiled game (Windows host, `recomp/`)

- Build from PowerShell (cmake is only on PATH in a VS prompt):
  `cmd /c "\"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat\" amd64_x86 >nul 2>nul && cmake --build build\game --target obiwan 2>&1"`
  then run `build\game\obiwan.exe` (the XBE path is optional; it finds
  `extracted\default.xbe` above itself). The target also builds
  `Obi-Wan.exe`, the windowed double-click launch the owner uses, with
  settings in `build\game\obiwan.ini` and output in `build\game\obiwan.log`.
  After changing a hook file also build `difftest`: its DLL links
  `hooks_*.c` and needs whatever they call.
- `recomp/README.md` explains the layout and lists the `RECOMP_*` switches
  (screenshots, state logs, pixel probe, renderer choice, fixed clock).
- Test runs: `tools\shots.ps1` (scripted input, screenshots, contact sheet on a
  scratch hard disk) and `tools\cmp.ps1` (Direct3D 9 against 11, with a diff).
  `build\launches\*.bin` boot straight into a level via `RECOMP_LAUNCH`, and
  `RECOMP_FIXEDFPS=60` makes a frame number the same moment in every run.
- Windows cannot create threads in the process once the XBE is loaded, so
  anything that needs host threads (Direct3D, XAudio2) starts in `boot.c`
  before `xbe_load`.
- `tools\shots.ps1` kills any running `obiwan.exe` first: don't start test
  runs while the owner is playing (check for both `obiwan` and `Obi-Wan`
  processes). Scripted runs (`RECOMP_INPUT`) ignore real controllers and the
  keyboard.
- The game caps itself at 120 fps (`sys_fpsclamp`); the host paces frames
  instead (`RECOMP_MAXFPS`, 0 for none). With `RECOMP_FIXEDFPS=n` running
  faster than real time, speech and music still play in real time and
  cutscenes wait for them, so add `RECOMP_MAXFPS=n` when comparing game
  time across settings.
- Gameplay structures (health, players, damage, camera) are in
  `docs/recomp/game-structures.md`; keep it current. Debug keys live in
  `recomp/host/debugkeys.c` (F1 restores health).
- Game runs are not repeatable frame for frame even with `RECOMP_FIXEDFPS`
  (guest threads), so compare several runs per setting, or reason from logs
  and probes. `RECOMP_NATIVE` (original x86 for chosen functions),
  `host_watch` (hardware watchpoint), `RECOMP_PROFILE` and the F3 overlay
  are the debugging and performance tools (see the README).
- Temporary probes go into `build\generated\*.c` (never committed); strip
  them before committing a build anyone relies on. A change to `guest.h` or
  a retranslation rebuilds all generated code (about 14 minutes).
- Retranslating: run the README's Docker command with an output folder
  other than `build/generated` (say `build/generated_new`), then swap the
  folders, so a running game and build are not disturbed.
- Level codes: the mission titles are in `extracted\data\dlg\UI.dlg`
  (`MissionNNTitle`) and each `data\scripts\<level>.script` names its own;
  e.g. "Valuable Assistance" is `L05_6`, "City Under Siege" (first Theed
  level, water left of the start) is `L03_6a`, and `L02_1a` is the fogged
  jungle. A launch file for any level is a copy of one in `build\launches`
  with the name at offset 7 replaced.
- `recomp/README.md` is Windows-1252 with CRLF line endings: patch it with a
  byte-level script, not the Edit/Write tools.

## Rules

- Do not add game assets, the XBE, or decompiled game code dumps verbatim to git
  unless the owner asks.
