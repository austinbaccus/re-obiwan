# Binary map for static recompilation

Step 1 of the native Windows port: a map of `default.xbe` (Star Wars: Obi-Wan,
PAL, XDK 3911, MD5 `082b940255be5db275961c68c66e5a6a`) written for building a
static recompiler. It contains addresses, names and structure only; no game
code, data or assets.

| Document | Contents |
|---|---|
| [xbe-layout.md](xbe-layout.md) | header, certificate, sections, library versions, TLS |
| [kernel-imports.md](kernel-imports.md) | the 112 `xboxkrnl.exe` imports and their thunk slots |
| [code-map.md](code-map.md) | what each part of `.text` is, code reached only via tables, entry point → `main` → frame loop |
| [recompiler-notes.md](recompiler-notes.md) | what to recompile vs. replace, instruction hazards, EH, memory, threads, next steps |
| [functions.csv](functions.csv) | all 12,860 functions Ghidra found: range, size, section, class, library namespace, name |

## Summary

- One fixed-address image at `0x00010000`, no relocations, everything preloaded.
  Entry `0x0002AC74`, game `main` at `0x000251F0`, per-frame tick at
  `0x0021CB90` called from the loop at `0x000256ED`.
- XDK libraries sit in their own sections (`D3D`, `D3DX`, `XGRPH`, `DSOUND`,
  `XPP`); XAPI and the MSVC runtime are in `.text` at known ranges. Ghidra's XBE
  loader named ~600 library functions, enough to define the HLE boundary.
- Game and engine (LucasArts' "Tanis") are ~2.5 MB of x87-heavy, virtual-call-heavy
  MSVC C++ with 885 RTTI types, C++ EH everywhere, and no direct hardware access.
- ~300 KB of `.text` (EH funclets, static initializers) and the first thread
  entry are reachable only through data tables, so code discovery must be
  seeded from those tables, not only from the entry point.

## Regenerating

```sh
# header facts (any machine with Python 3)
python3 tools/xbe_info.py extracted/default.xbe > /tmp/xbe.json

# function map: needs the analyzed Ghidra project; see the script's docstring
# for running it against a copy while the ghidra-mcp service is up
docker compose exec ghidra-mcp /opt/pyghidra-mcp/bin/python3 \
    /workspace/tools/ghidra_scripts/export_function_map.py <project-copy> docs/recomp/functions.csv
```

functions.csv reflects the Ghidra project as analyzed on 2026-10-22 with no
manual renames. Regenerate it as functions get named.
