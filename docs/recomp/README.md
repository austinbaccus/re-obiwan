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
| [api-surface.md](api-surface.md) | every XDK, CRT and kernel function or global the game touches: the host runtime's job ([csv](api-surface.csv)) |
| [game-structures.md](game-structures.md) | gameplay fields and functions: health, players, damage, camera |
| [classes.md](classes.md) | 885 RTTI types, class hierarchy and 1,187 vtables ([classes.csv](classes.csv), [vtables.csv](vtables.csv)) |
| [functions.csv](functions.csv) | all 15,501 functions in the Ghidra project: range, size, section, class, namespace, name |
| [library-names.csv](library-names.csv) | library functions identified by hand, with evidence |

## Summary

- One fixed-address image at `0x00010000`, no relocations, everything preloaded.
  Entry `0x0002AC74`, game `main` at `0x000251F0`, per-frame tick at
  `0x0021CB90` called from the loop at `0x000256ED`.
- XDK libraries sit in their own sections (`D3D`, `D3DX`, `XGRPH`, `DSOUND`,
  `XPP`); XAPI and the MSVC runtime are in `.text` at known ranges. Ghidra's XBE
  loader named ~600 library functions, enough to define the HLE boundary.
- Game and engine (LucasArts' "Tanis") are ~2.5 MB of x87-heavy, virtual-call-heavy
  MSVC C++ with 885 RTTI types, C++ exceptions (it does throw), and no direct
  hardware access.
- The game reaches the OS almost only through XAPI (2 direct kernel calls), uses
  about 60 D3D8 functions plus direct writes into D3D's render-state arrays, a
  small slice of DirectSound, and plain XInput.
- ~300 KB of `.text` (EH funclets, static initializers) and the first thread
  entry are reachable only through data tables, so code discovery must be
  seeded from those tables, not only from the entry point.

## Regenerating

```sh
# header facts (any machine with Python 3)
python3 tools/xbe_info.py extracted/default.xbe > /tmp/xbe.json

# classes and vtables (Python 3 only)
python3 tools/rtti_vtables.py extracted/default.xbe docs/recomp

# Ghidra steps open the shared project, so stop the MCP server first
docker compose stop ghidra-mcp
docker compose run --rm --no-deps ghidra-mcp bash -c '
  py=/opt/pyghidra-mcp/bin/python3; t=/workspace/tools/ghidra_scripts; d=/workspace/docs/recomp
  $py $t/apply_rtti.py /workspace/ghidra $d &&
  $py $t/export_function_map.py /workspace/ghidra $d/functions.csv &&
  $py $t/export_api_surface.py /workspace/ghidra $d/api-surface.csv'
docker compose start ghidra-mcp
```

functions.csv and api-surface.csv reflect the Ghidra project after
`apply_rtti.py`. Regenerate them as functions get named.
