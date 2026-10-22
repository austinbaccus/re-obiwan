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

## Rules

- Do not add game assets, the XBE, or decompiled game code dumps verbatim to git
  unless the owner asks.
