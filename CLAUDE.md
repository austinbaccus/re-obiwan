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
  then run `build\game\obiwan.exe extracted\default.xbe`.
- `recomp/README.md` explains the layout and lists the `RECOMP_*` switches
  (screenshots, state logs, pixel probe, renderer choice, fixed clock).
- Test runs: `tools\shots.ps1` (scripted input, screenshots, contact sheet on a
  scratch hard disk) and `tools\cmp.ps1` (Direct3D 9 against 11, with a diff).
  `build\launches\*.bin` boot straight into a level via `RECOMP_LAUNCH`, and
  `RECOMP_FIXEDFPS=60` makes a frame number the same moment in every run.
- Windows cannot create threads in the process once the XBE is loaded, so
  anything that needs host threads (Direct3D, XAudio2) starts in `boot.c`
  before `xbe_load`.
- `recomp/README.md` is Windows-1252 with CRLF line endings: patch it with a
  byte-level script, not the Edit/Write tools.

## Rules

- Do not add game assets, the XBE, or decompiled game code dumps verbatim to git
  unless the owner asks.
