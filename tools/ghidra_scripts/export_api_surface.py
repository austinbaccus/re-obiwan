"""List every reference from game code into XDK libraries, the CRT and the kernel.

This is the surface a static recompiler's host runtime has to provide. For each
library address the game reaches it writes one row: the target, the function
that contains it, its section and category, how it is reached (call, jump,
read, write, address taken) and how many sites / distinct game functions do so.

"Game code" is every instruction in .text outside the XAPI and CRT ranges
documented in docs/recomp/code-map.md, so the C++ EH funclets and static
initializers count as game code too.

Run against a copy of the project (see export_function_map.py):

    /opt/pyghidra-mcp/bin/python3 export_api_surface.py <project-dir> <out.csv>
"""
import collections
import csv
import sys

import pyghidra

pyghidra.start()
from ghidra.base.project import GhidraProject  # noqa: E402

PROGRAM = 'default.xbe-076e14'

# Library ranges inside .text (inclusive start, exclusive end).
TEXT_LIBS = [
    (0x0002A6D6, 0x0002EB60, 'XAPILIB'),
    (0x002CBD20, 0x002DE752, 'CRT'),
    (0x002DE752, 0x002E0A00, 'XAPILIB'),
    (0x00320A10, 0x00320EEC, 'CRT'),
]
LIB_SECTIONS = {'D3D': 'D3D8', 'D3DX': 'D3DX8', 'XGRPH': 'XGRAPHC', 'DSOUND': 'DSOUND', 'XPP': 'XAPILIB'}
THUNK_START, THUNK_END = 0x003BC220, 0x003BC3E0


def category(addr, block_name):
    if THUNK_START <= addr < THUNK_END:
        return 'xboxkrnl'
    if block_name in LIB_SECTIONS:
        return LIB_SECTIONS[block_name]
    if block_name == '.text':
        for lo, hi, name in TEXT_LIBS:
            if lo <= addr < hi:
                return name
    return None


def ref_kind(ref):
    t = ref.getReferenceType()
    if t.isCall():
        return 'call'
    if t.isJump():
        return 'jump'
    if t.isWrite():
        return 'write'
    if t.isRead():
        return 'read'
    return 'addr'


def main(project_dir, out_path):
    project = GhidraProject.openProject(project_dir, 'obiwan', False)
    program = project.openProgram('/', PROGRAM, True)
    memory, listing = program.getMemory(), program.getListing()
    fm, symtab = program.getFunctionManager(), program.getSymbolTable()
    text = memory.getBlock('.text')

    sites = collections.defaultdict(collections.Counter)   # target -> kind -> count
    callers = collections.defaultdict(set)                 # target -> game functions
    for ins in listing.getInstructions(text.getStart(), True):
        a = ins.getAddress()
        if not text.contains(a):
            break
        if category(a.getOffset(), '.text') is not None:
            continue  # library code calling library code
        caller = fm.getFunctionContaining(a)
        caller_name = caller.getName() if caller else f'nofunc_{a.getOffset():08x}'
        for ref in ins.getReferencesFrom():
            to = ref.getToAddress()
            if not to.isMemoryAddress():
                continue
            block = memory.getBlock(to)
            if block is None:
                continue
            cat = category(to.getOffset(), block.getName())
            if cat is None:
                continue
            sites[to.getOffset()][ref_kind(ref)] += 1
            callers[to.getOffset()].add(caller_name)

    rows = []
    for addr, kinds in sites.items():
        a = program.getAddressFactory().getDefaultAddressSpace().getAddress(addr)
        block = memory.getBlock(a).getName()
        cat = category(addr, block)
        if cat == 'xboxkrnl':
            syms = [s for s in symtab.getSymbols(a)]
            ext = [r.getToAddress() for r in program.getReferenceManager().getReferencesFrom(a)]
            name = syms[0].getName() if syms else ''
            if ext:
                es = symtab.getPrimarySymbol(ext[0])
                if es is not None:
                    name = es.getName()
            func, ns = name, 'xboxkrnl.exe'
        else:
            f = fm.getFunctionContaining(a)
            if f is not None:
                func = f.getName()
                ns = '' if f.getParentNamespace().isGlobal() else f.getParentNamespace().getName(True)
                if f.getEntryPoint().getOffset() != addr:
                    func += f'+0x{addr - f.getEntryPoint().getOffset():x}'
            else:
                s = symtab.getPrimarySymbol(a)
                func, ns = (s.getName() if s else ''), ''
        rows.append([
            f'0x{addr:08x}', cat, block, ns, func,
            sum(kinds.values()), len(callers[addr]),
            ' '.join(f'{k}:{v}' for k, v in sorted(kinds.items())),
        ])
    rows.sort(key=lambda r: (r[1], -r[6], r[0]))
    with open(out_path, 'w', newline='') as out:
        w = csv.writer(out, lineterminator='\n')
        w.writerow(['target', 'library', 'section', 'namespace', 'name', 'sites', 'game_functions', 'kinds'])
        w.writerows(rows)
    project.close()


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
