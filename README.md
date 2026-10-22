# re-obiwan

Reverse engineering my own copy of *Star Wars: Obi-Wan* (original Xbox) with
Ghidra, working towards a native Windows 11 build.

This repo holds the tooling and, later, the reconstructed source. It does not
contain the game: you need your own disc image.

## What's in the environment

Everything runs in Docker so the setup is identical on any machine:

| Tool | Version | Purpose |
| --- | --- | --- |
| [Ghidra](https://github.com/NationalSecurityAgency/ghidra) | 12.1.4 | Disassembler / decompiler (headless in Docker) |
| [ghidra-xbe](https://github.com/XboxDev/ghidra-xbe) | build-202610161023 | Loads Xbox `.xbe` executables and recovers XDK library symbols |
| [extract-xiso](https://github.com/XboxDev/extract-xiso) | build-202609111233 | Extracts files from the Xbox disc image |
| [pyghidra-mcp](https://github.com/clearbluejar/pyghidra-mcp) | 0.2.7 | Headless Ghidra MCP server, so Claude can drive Ghidra |
| [Claude Code](https://docs.claude.com/en/docs/claude-code) | 2.1.307 | AI assistant inside the container (installed from npm on Node.js 22.23.4) |
| Eclipse Temurin JDK | 21.0.12.1 | Java runtime Ghidra needs |

Versions are pinned in [`docker/Dockerfile`](docker/Dockerfile); downloads are
checked against SHA-256 hashes.

Two containers are defined in [`compose.yaml`](compose.yaml):

- **`ghidra-mcp`** runs pyghidra-mcp on port 8000 against the shared Ghidra
  project in `ghidra/obiwan.gpr`. Once `extracted/default.xbe` exists it is
  imported and analyzed automatically.
- **`re`** is your interactive shell, with Claude Code, Ghidra's command-line
  tools and extract-xiso. Claude reaches Ghidra through [`.mcp.json`](.mcp.json).

The repo folder is mounted into both at `/workspace`, so the ISO, the extracted
files and the Ghidra project all stay on your Windows drive and are visible to
Windows tools too.

```
X:\source\repos\re-obiwan\
├── obiwan.iso        # your disc image (gitignored)
├── extracted\        # files from the ISO, incl. default.xbe (gitignored)
├── ghidra\           # shared Ghidra project obiwan.gpr (gitignored)
├── docker\           # Dockerfile + helper scripts
├── compose.yaml
└── .mcp.json
```

## Setup on Windows 11

### 1. Install Docker Desktop

1. Install [Docker Desktop](https://docs.docker.com/desktop/setup/install/windows-install/)
   and keep the default **WSL 2 backend**. Reboot if asked.
2. Start Docker Desktop. In **Settings → Resources** give it at least 8 GB of
   memory; Ghidra analysis is memory-hungry.
3. Check it works in PowerShell: `docker run --rm hello-world`

### 2. Put the game files in place

Copy your disc image to `X:\source\repos\re-obiwan\obiwan.iso`, and the files
you extracted from it (the folder containing `default.xbe`) somewhere inside the
repo folder, for example `X:\source\repos\re-obiwan\extracted\`. Both are
gitignored (`*.iso`, `*.xbe` and `extracted/`), so they will never be committed.

The MCP server looks for `extracted/default.xbe` first and otherwise for the
first `default.xbe` up to three folders deep. If yours is somewhere else, create
a `.env` file next to `compose.yaml`:

```
OBIWAN_XBE=/workspace/path/to/default.xbe
```

Haven't extracted the ISO yet? The container has extract-xiso: run
`docker compose run --rm re extract-iso` and it writes everything to `extracted/`.

### 3. Build and start

In PowerShell:

```powershell
cd X:\source\repos\re-obiwan
docker compose build                 # first build downloads ~700 MB
docker compose up -d ghidra-mcp      # starts the Ghidra MCP server
docker compose logs -f ghidra-mcp    # watch it import default.xbe and analyze; Ctrl+C to stop watching
```

The first analysis of `default.xbe` takes a few minutes. The log says which
`default.xbe` it picked up.

### 4. Use Claude in the container

```powershell
docker compose run --rm re
```

```bash
claude            # first run: log in with your Claude account (or set ANTHROPIC_API_KEY)
```

Run `/mcp` inside Claude to confirm the `ghidra` server is connected. Your login
is kept in a Docker volume, so you only log in once.

Claude Code running on Windows (outside Docker) can use the same server: the
port is published on `127.0.0.1:8000`, and the `re` container shares the
server's network, so `.mcp.json` points at `http://localhost:8000/mcp` in both
places. (The server only accepts `localhost` as the host name; that is MCP's
protection against DNS rebinding, so don't point clients at `ghidra-mcp:8000`.)

### 5. Ghidra GUI on Windows (optional, for browsing by hand)

Ghidra's window can't easily be shown from inside Docker, so for the visual
side install the same version on Windows:

1. Install a 64-bit JDK 21, e.g. [Eclipse Temurin 21](https://adoptium.net/temurin/releases/?version=21).
2. Download [Ghidra 12.1.4](https://github.com/NationalSecurityAgency/ghidra/releases/tag/Ghidra_12.1.4_build)
   and unzip it, e.g. to `C:\Tools\ghidra_12.1.4_PUBLIC`.
3. Download the matching XBE loader,
   [`ghidra_12.1.4_PUBLIC_20261016_ghidra-xbe.zip`](https://github.com/XboxDev/ghidra-xbe/releases/tag/build-202610161023).
   Start Ghidra with `ghidraRun.bat`, choose **File → Install Extensions**, click
   **+**, select the zip, and restart Ghidra.
4. **File → Open Project** → `X:\source\repos\re-obiwan\ghidra\obiwan.gpr`.

A Ghidra project can only be open in one place at a time. Stop the server
first (`docker compose stop ghidra-mcp`) before opening it in the GUI, and close
the GUI before starting the server again. Keep the Windows Ghidra version equal
to the one in the Dockerfile so both can read the project.

### 6. xemu for comparing against the original (optional)

Install [xemu](https://xemu.app/), the original Xbox emulator. It needs an Xbox
BIOS, MCPX boot ROM and HDD image dumped from your own console; see the
[xemu setup guide](https://xemu.app/docs/required-files/). xemu loads
`obiwan.iso` directly and is our reference for how the game should behave,
and its GDB stub lets us step through the original code alongside Ghidra.

## Day-to-day commands

| Command | Where | What it does |
| --- | --- | --- |
| `docker compose up -d ghidra-mcp` | PowerShell | Start the Ghidra MCP server |
| `docker compose stop ghidra-mcp` | PowerShell | Stop it (needed before using the GUI) |
| `docker compose run --rm re` | PowerShell | Open a shell with Claude Code and the tools |
| `extract-iso` | `re` shell | Extract `obiwan.iso` into `extracted/` (only if you haven't already) |
| `analyze-headless -process default.xbe -postScript X.java` | `re` shell | Run a Ghidra script on the project (stop `ghidra-mcp` first) |
| `claude` | `re` shell | Start Claude Code |

## Troubleshooting

- **Scripts fail with `bad interpreter` or `$'\r'`**: the repo was checked out
  with Windows line endings. `.gitattributes` prevents this for new checkouts;
  for an existing one run `git rm --cached -r . && git reset --hard`.
- **Ghidra is slow**: files on `X:\` are shared into Docker over a network
  filesystem. If analysis is painfully slow, clone the repo inside WSL
  (`\\wsl$\Ubuntu\home\<you>\re-obiwan`) and run compose from there.
- **`project is locked`**: the project is open in the GUI or in another
  container. Close it there first.
- **`/mcp` in Claude says `HTTP 421`**: the client is reaching the server by a
  host name other than `localhost`. Use `http://localhost:8000/mcp` (what the
  repo's `.mcp.json` has) and make sure the `ghidra-mcp` service is running.

## What comes next

The binary map for the static recompiler (sections, kernel imports, library
ranges, entry point and main loop, recompilation hazards) is in
[docs/recomp](docs/recomp/README.md).

This environment only covers the analysis side. The rebuilt game will be
compiled and run on Windows itself, because it has to replace the Xbox's
Direct3D 8, audio and controller APIs with PC equivalents. That toolchain will be
added once there is code to build.
