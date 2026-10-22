"""Create functions at the addresses listed in docs/recomp/extra-functions.txt.

A line `table <start> <end>` creates a function at every code address a
pointer in that range points to (initializer tables, callback tables), and
`scan immediates` does the same for code addresses used as instruction
immediates (function pointers taken by value).

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
    addrs, tables, scan_immediates = [], [], False
    for line in open(list_path):
        line = line.split('#')[0].strip()
        if line.startswith('table '):
            _, lo, hi = line.split()
            tables.append((int(lo, 16), int(hi, 16)))
        elif line == 'scan immediates':
            scan_immediates = True
        elif line:
            addrs.append(int(line, 16))
    project = GhidraProject.openProject(project_dir, 'obiwan', False)
    program = project.openProgram('/', PROGRAM, False)
    space = program.getAddressFactory().getDefaultAddressSpace()
    fm, listing, memory = program.getFunctionManager(), program.getListing(), program.getMemory()
    # every pointer in a table that lands in executable code is an entry point
    code_blocks = [b for b in memory.getBlocks() if b.isExecute() and b.getName() not in ('.rdata', '.data', 'DOLBY')]
    for lo, hi in tables:
        for a in range(lo, hi, 4):
            v = memory.getInt(space.getAddress(a)) & 0xFFFFFFFF
            if v not in (0, 0xFFFFFFFF) and any(b.contains(space.getAddress(v)) for b in code_blocks):
                addrs.append(v)
    if scan_immediates:
        # Function pointers taken by value (`mov reg, imm`, `push imm`): accept an
        # address in code that is not already inside a function and whose
        # preceding byte ends a function (ret, jmp, int3 or nop padding), so a
        # stray constant cannot split a real function.
        ends = {0xC3, 0xC2, 0xCC, 0x90, 0xE9, 0xEB, 0x00}
        found = 0
        for ins in listing.getInstructions(True):
            for i in range(ins.getNumOperands()):
                for obj in ins.getOpObjects(i):
                    if not hasattr(obj, 'getUnsignedValue'):
                        continue
                    v = obj.getUnsignedValue()
                    if v < 0x1000 or v > 0xFFFFFFFF:
                        continue
                    t = space.getAddress(v)
                    if not any(b.contains(t) for b in code_blocks) or fm.getFunctionContaining(t) is not None:
                        continue
                    prev = memory.getByte(t.subtract(1)) & 0xFF
                    # ret imm16 is 3 bytes: the byte before is part of the immediate
                    prev3 = memory.getByte(t.subtract(3)) & 0xFF
                    if prev in ends or prev3 == 0xC2:
                        addrs.append(v)
                        found += 1
        print(f'immediates: {found} candidate entry points')

    tx = program.startTransaction('Create extra functions')
    created = 0
    try:
        pending = list(addrs)
        seen = set()
        while pending:
            a = pending.pop()
            if a in seen:
                continue
            seen.add(a)
            addr = space.getAddress(a)
            if fm.getFunctionAt(addr) is None:
                cu = listing.getCodeUnitContaining(addr)
                if cu is not None and cu.getMinAddress() != addr:
                    listing.clearCodeUnits(cu.getMinAddress(), cu.getMaxAddress(), False)
                if listing.getInstructionAt(addr) is None:
                    DisassembleCommand(addr, None, True).applyTo(program, TaskMonitor.DUMMY)
                if CreateFunctionCmd(addr).applyTo(program, TaskMonitor.DUMMY):
                    created += 1
                else:
                    print(f'could not create a function at {a:08x}')
                    continue
            # CreateFunctionCmd does not create callees: follow direct calls to closure
            f = fm.getFunctionAt(addr)
            for ins in listing.getInstructions(f.getBody(), True):
                if ins.getFlowType().isCall():
                    for flow in ins.getFlows():
                        if fm.getFunctionAt(flow) is None and any(b.contains(flow) for b in code_blocks):
                            pending.append(flow.getOffset())
    finally:
        program.endTransaction(tx, True)
    project.save(program)
    project.close()
    print(f'created {created} of {len(addrs)}')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
