"""Create functions at the addresses listed in docs/recomp/extra-functions.txt.

A line `table <start> <end>` creates a function at every .text address a
pointer in that range points to (initializer tables, callback tables).

Ghidra only finds code it can reach; thread entry points and callbacks that
are passed as data (the first thread's start routine, XapiThreadStartup)
stay undefined and the recompiler skips them. Writes to the project, so
stop the ghidra-mcp service first (see apply_rtti.py for the command line).

    python3 create_functions.py <project-dir> <extra-functions.txt>
"""
import sys

import pyghidra

pyghidra.start()
from ghidra.app.cmd.disassemble import DisassembleCommand  # noqa: E402
from ghidra.app.cmd.function import CreateFunctionCmd  # noqa: E402
from ghidra.base.project import GhidraProject  # noqa: E402
from ghidra.util.task import TaskMonitor  # noqa: E402

PROGRAM = 'default.xbe-076e14'


def main(project_dir, list_path):
    addrs, tables = [], []
    for line in open(list_path):
        line = line.split('#')[0].strip()
        if line.startswith('table '):
            _, lo, hi = line.split()
            tables.append((int(lo, 16), int(hi, 16)))
        elif line:
            addrs.append(int(line, 16))
    project = GhidraProject.openProject(project_dir, 'obiwan', False)
    program = project.openProgram('/', PROGRAM, False)
    space = program.getAddressFactory().getDefaultAddressSpace()
    fm, listing, memory = program.getFunctionManager(), program.getListing(), program.getMemory()
    text = memory.getBlock('.text')
    # every pointer in a table that lands in .text is a code entry point
    for lo, hi in tables:
        for a in range(lo, hi, 4):
            v = memory.getInt(space.getAddress(a)) & 0xFFFFFFFF
            if text.contains(space.getAddress(v)) and v not in (0, 0xFFFFFFFF):
                addrs.append(v)
    tx = program.startTransaction('Create extra functions')
    created = 0
    try:
        for a in addrs:
            addr = space.getAddress(a)
            if fm.getFunctionAt(addr) is not None:
                continue
            cu = listing.getCodeUnitContaining(addr)
            if cu is not None and cu.getMinAddress() != addr:
                listing.clearCodeUnits(cu.getMinAddress(), cu.getMaxAddress(), False)
            if listing.getInstructionAt(addr) is None:
                DisassembleCommand(addr, None, True).applyTo(program, TaskMonitor.DUMMY)
            if CreateFunctionCmd(addr).applyTo(program, TaskMonitor.DUMMY):
                created += 1
            else:
                print(f'could not create a function at {a:08x}')
    finally:
        program.endTransaction(tx, True)
    project.save(program)
    project.close()
    print(f'created {created} of {len(addrs)}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
