"""Apply recovered RTTI and hand-identified library names to the Ghidra project.

Inputs (produced by tools/rtti_vtables.py and kept in docs/recomp/):
  classes.csv, vtables.csv, library-names.csv

For every vtable this
  - labels it `<Class>::vftable` (or `vftable_<offset>` for secondary vtables
    of classes with multiple inheritance) and types its slots as pointers,
  - disassembles and creates a function at every slot target that has none,
    clearing any overlapping bad disassembly first,
  - names still-unnamed slot functions `<Class>::vf<index>`, using the most
    basic class that has the function in its vtable.
Then it renames the library functions listed in library-names.csv.

Writes to the project, so stop the ghidra-mcp service first:

    docker compose stop ghidra-mcp
    docker compose run --rm --no-deps ghidra-mcp /opt/pyghidra-mcp/bin/python3 \
        /workspace/tools/ghidra_scripts/apply_rtti.py /workspace/ghidra /workspace/docs/recomp
    docker compose start ghidra-mcp
"""
import collections
import csv
import os
import sys

import pyghidra

pyghidra.start()
from ghidra.app.cmd.disassemble import DisassembleCommand  # noqa: E402
from ghidra.app.cmd.function import CreateFunctionCmd  # noqa: E402
from ghidra.app.util import NamespaceUtils  # noqa: E402
from ghidra.base.project import GhidraProject  # noqa: E402
from ghidra.program.model.address import AddressSet  # noqa: E402
from ghidra.program.model.data import PointerDataType  # noqa: E402
from ghidra.program.model.symbol import SourceType  # noqa: E402
from ghidra.util.task import TaskMonitor  # noqa: E402

PROGRAM = 'default.xbe-076e14'
USER = SourceType.USER_DEFINED
MON = TaskMonitor.DUMMY


def clean(name):
    for ch in ' ,<>*&':
        name = name.replace(ch, '_')
    return name


def main(project_dir, docs_dir):
    project = GhidraProject.openProject(project_dir, 'obiwan', False)
    program = project.openProgram('/', PROGRAM, False)
    space = program.getAddressFactory().getDefaultAddressSpace()
    addr = lambda v: space.getAddress(v)
    listing, fm, symtab = program.getListing(), program.getFunctionManager(), program.getSymbolTable()
    glob = program.getGlobalNamespace()

    nbases = {}
    for row in csv.DictReader(open(os.path.join(docs_dir, 'classes.csv'))):
        nbases[row['name']] = len(row['bases'].split())
    vtables = list(csv.DictReader(open(os.path.join(docs_dir, 'vtables.csv'))))

    # owner of each slot target: the class with the fewest bases that uses it
    owners = collections.defaultdict(list)
    for vt in vtables:
        for i, t in enumerate(vt['slot_targets'].split()):
            owners[int(t, 16)].append((nbases.get(vt['class'], 0), vt['class'], i))

    tx = program.startTransaction('Apply RTTI vtables')
    stats = collections.Counter()
    try:
        def ns_for(cls):
            return NamespaceUtils.createNamespaceHierarchy(clean(cls), glob, program, USER)

        for vt in vtables:
            start, n = int(vt['vtable'], 16), int(vt['slots'])
            ns = ns_for(vt['class'])
            label = 'vftable' if vt['offset_in_object'] == '0' else f"vftable_{vt['offset_in_object']}"
            symtab.createLabel(addr(start), label, ns, USER)
            if n:
                listing.clearCodeUnits(addr(start), addr(start + 4 * n - 1), False)
                for i in range(n):
                    listing.createData(addr(start + 4 * i), PointerDataType())
            stats['vtables'] += 1

        for target, uses in owners.items():
            a = addr(target)
            f = fm.getFunctionAt(a)
            if f is None:
                cu = listing.getCodeUnitContaining(a)
                if cu is not None and cu.getMinAddress() != a:
                    listing.clearCodeUnits(cu.getMinAddress(), cu.getMaxAddress(), False)
                    stats['cleared_bad_code'] += 1
                if listing.getInstructionAt(a) is None:
                    DisassembleCommand(a, None, True).applyTo(program, MON)
                existing = fm.getFunctionContaining(a)
                if existing is not None and existing.getEntryPoint() != a:
                    # the slot target is in the middle of a function Ghidra
                    # over-extended: create the new one first, then recreate
                    # the old one so its body stops at the new entry
                    entry, name = existing.getEntryPoint(), existing.getName()
                    fm.removeFunction(entry)
                    CreateFunctionCmd(a).applyTo(program, MON)
                    CreateFunctionCmd(entry).applyTo(program, MON)
                    if fm.getFunctionAt(entry) is not None and not name.startswith('FUN_'):
                        fm.getFunctionAt(entry).setName(name, USER)
                    stats['split_functions'] += 1
                else:
                    CreateFunctionCmd(a).applyTo(program, MON)
                f = fm.getFunctionAt(a)
                stats['created' if f else 'create_failed'] += 1
            if f is None:
                continue
            classes = {c for _, c, _ in uses}
            if f.getName().startswith('FUN_') and len(classes) <= 8:
                _, cls, idx = min(uses)
                f.setParentNamespace(ns_for(cls))
                f.setName(f'vf{idx:02d}', USER)
                stats['named'] += 1

        for row in csv.DictReader(open(os.path.join(docs_dir, 'library-names.csv'))):
            a = addr(int(row['address'], 16))
            f = fm.getFunctionAt(a)
            if f is None:
                CreateFunctionCmd(a).applyTo(program, MON)
                f = fm.getFunctionAt(a)
            if f is None or f.getName(True) == row['name']:
                continue
            ns_name, _, short = row['name'].rpartition('::')
            existing = symtab.getSymbol(short, a, ns_for(ns_name) if ns_name else glob)
            if existing is not None and existing != f.getSymbol():
                existing.delete()  # a secondary label with the wanted name
            if ns_name:
                f.setParentNamespace(ns_for(ns_name))
            f.setName(short, USER)
            f.setComment(f"Identified by hand ({row['library']}): {row['evidence']}")
            stats['library_named'] += 1
    finally:
        program.endTransaction(tx, True)
    project.save(program)
    project.close()
    print(dict(stats))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
