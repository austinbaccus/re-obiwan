"""Export the function map of the analyzed XBE as CSV (pyghidra, headless).

Writes one row per function Ghidra knows about: address range, size, name,
the library namespace the XBE loader's signatures assigned (D3D8, DSOUND,
XAPILIB, ...), the section it lives in and a coarse classification. No code
is exported, only addresses and names.

The ghidra-mcp service keeps the project locked, so run this against a copy:

    docker compose exec ghidra-mcp bash -c '
        rm -rf /tmp/gcopy && mkdir /tmp/gcopy &&
        cp -r /workspace/ghidra/obiwan.gpr /workspace/ghidra/obiwan.rep /tmp/gcopy/ &&
        find /tmp/gcopy -name "*.lock*" -delete &&
        /opt/pyghidra-mcp/bin/python3 /workspace/tools/ghidra_scripts/export_function_map.py \
            /tmp/gcopy /workspace/docs/recomp/functions.csv'
"""
import csv
import sys

import pyghidra

pyghidra.start()
from ghidra.base.project import GhidraProject  # noqa: E402

PROGRAM = 'default.xbe-076e14'


def classify(f, section):
    ns = f.getParentNamespace()
    name = f.getName()
    if not ns.isGlobal():
        return 'xboxkrnl' if ns.getName() == 'xboxkrnl.exe' else 'xdk'
    if f.isThunk() and name.startswith('thunk_FUN_'):
        return 'thunk'
    if not name.startswith('FUN_'):
        return 'crt'  # FID / analysis-named MSVC runtime (LIBC, LIBCPMT, undname)
    if section in ('D3D', 'D3DX', 'XGRPH', 'DSOUND', 'XPP'):
        return 'xdk'  # unnamed helper inside a library section
    return 'unknown'


def main(project_dir, out_path):
    project = GhidraProject.openProject(project_dir, 'obiwan', False)
    program = project.openProgram('/', PROGRAM, True)
    memory = program.getMemory()
    with open(out_path, 'w', newline='') as out:
        w = csv.writer(out, lineterminator='\n')
        w.writerow(['start', 'end', 'size', 'section', 'class', 'namespace', 'name'])
        for f in program.getFunctionManager().getFunctions(True):
            body = f.getBody()
            entry = f.getEntryPoint()
            section = memory.getBlock(entry).getName()
            ns = f.getParentNamespace()
            w.writerow([
                f'0x{entry.getOffset():08x}', f'0x{body.getMaxAddress().getOffset():08x}',
                body.getNumAddresses(), section, classify(f, section),
                '' if ns.isGlobal() else ns.getName(True), f.getName(),
            ])
    project.close()


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
