"""Static recompiler front end: Ghidra p-code -> C.

Every guest function becomes `void f_XXXXXXXX(Ctx *ctx)`. Guest registers
live in ctx->r[], laid out exactly like Ghidra's x86 register space, so each
p-code varnode maps to a fixed byte offset. Guest memory is reached through
MEM(addr). Calls become C calls (the guest return address is still pushed on
the guest stack, so stack layout matches the original), RET becomes `return`,
and computed jumps dispatch over the targets Ghidra recovered.

x87 values are kept as host doubles in the first 8 bytes of each 10-byte ST
slot; only 80-bit loads/stores to memory convert.

Run in the ghidra-mcp container against a copy of the project:

    /opt/pyghidra-mcp/bin/python3 pcode2c.py <project-dir> <out-dir> [select] [hooks.csv]

`hooks.csv` lists `address,name` pairs for functions the host runtime
implements itself as `hook_<name>`; their bodies are not translated.

`select` is `leaf` (default: functions with no calls, computed jumps, user
ops or fs: accesses, for differential testing), `text` (everything in the
.text section: game, XAPI and CRT, but not the D3D/DSOUND/... library
sections), `all`, or a file with one hex address per line. Coverage statistics for every function in the program
are always written to <out-dir>/meta.json.
"""
import collections
import json
import os
import sys

import pyghidra

pyghidra.start()
from ghidra.app.cmd.disassemble import DisassembleCommand  # noqa: E402
from ghidra.base.project import GhidraProject  # noqa: E402
from ghidra.util.task import TaskMonitor  # noqa: E402

PROGRAM = 'default.xbe-076e14'
FS_OFFSET = 0x110
INT_SIZES = (1, 2, 4, 8)
FUNCS_PER_FILE = 200

BINOP = {
    'INT_ADD': '+', 'INT_SUB': '-', 'INT_MULT': '*', 'INT_AND': '&', 'INT_OR': '|', 'INT_XOR': '^',
}
CMP_U = {'INT_EQUAL': '==', 'INT_NOTEQUAL': '!=', 'INT_LESS': '<', 'INT_LESSEQUAL': '<='}
CMP_S = {'INT_SLESS': '<', 'INT_SLESSEQUAL': '<='}
FBIN = {'FLOAT_ADD': '+', 'FLOAT_SUB': '-', 'FLOAT_MULT': '*', 'FLOAT_DIV': '/'}
FCMP = {'FLOAT_EQUAL': '==', 'FLOAT_NOTEQUAL': '!=', 'FLOAT_LESS': '<', 'FLOAT_LESSEQUAL': '<='}
# CALLOTHER ops with a direct C equivalent: name -> (runtime helper, float args?)
USEROPS_FLOAT = {'fsin': 'sin', 'fcos': 'cos', 'fpatan': 'rt_fpatan', 'fptan': 'tan',
                 'f2xm1': 'rt_f2xm1', 'fscale': 'rt_fscale', 'log2': 'log2'}
USEROPS_NOP = {'LOCK', 'UNLOCK', 'ffree'}
# packed-integer user ops: name -> runtime helper taking (out, in0, in1[, imm])
USEROPS_PACKED = {'pmaddwd': 'rt_pmaddwd', 'psubsw': 'rt_psubsw', 'packssdw': 'rt_packssdw',
                  'psllw': 'rt_psllw', 'pshufw': 'rt_pshufw'}
# instructions that only make sense on real hardware: trap if ever reached
USEROPS_TRAP = {'in', 'out', 'cpuid', 'cpuid_basic_info', 'cpuid_Version_info', 'cpuid_cache_tlb_info',
                'cpuid_serial_info', 'cpuid_Deterministic_Cache_Parameters_info',
                'cpuid_MONITOR_MWAIT_Features_info', 'cpuid_Thermal_Power_Management_info',
                'cpuid_Extended_Feature_Enumeration_info', 'cpuid_Direct_Cache_Access_info',
                'cpuid_Architectural_Performance_Monitoring_info', 'cpuid_Extended_Topology_info',
                'cpuid_Processor_Extended_States_info', 'cpuid_Quality_of_Service_info',
                'cpuid_brand_part1_info', 'cpuid_brand_part2_info', 'cpuid_brand_part3_info',
                'invalidInstructionException'}


class Unsupported(Exception):
    pass


def ut(size):
    return f'uint{size * 8}_t'


def st(size):
    return f'int{size * 8}_t'


class FunctionTranslator:
    def __init__(self, program, func, known):
        self.program = program
        self.lang = program.getLanguage()
        self.func = func
        self.known = known          # addresses of all functions being emitted
        self.entry = func.getEntryPoint().getOffset()
        self.lines = []
        self.labels_used = set()
        self.features = collections.Counter()
        self.uniq = {}              # unique offset -> compact offset
        self.uniq_size = 0
        self.needs_dispatch = False  # function has computed jumps: emit one shared switch

    # ---- varnodes -------------------------------------------------------
    def space(self, v):
        return v.getAddress().getAddressSpace().getName()

    def map_unique(self, v):
        off, size = v.getOffset(), v.getSize()
        return self.ublock[off]

    def ptr(self, v):
        sp, off = self.space(v), v.getOffset()
        if sp == 'register':
            if off == FS_OFFSET:
                self.features['fs'] += 1
            return f'(ctx->r + 0x{off:x})'
        if sp == 'unique':
            return f'(U + {self.map_unique(v)})'
        if sp == 'ram':
            return f'MEM(0x{off:08x}u)'
        raise Unsupported(f'pointer to {sp}')

    def rd(self, v):
        size = v.getSize()
        if self.space(v) == 'const':
            val = v.getOffset() & ((1 << (8 * size)) - 1) if size <= 8 else v.getOffset()
            return f'(({ut(size)})0x{val:x}ull)' if size in INT_SIZES else self.unsup(f'const size {size}')
        if size not in INT_SIZES:
            raise Unsupported(f'int read of size {size}')
        return f'RD{size * 8}({self.ptr(v)})'

    def wr(self, v, expr):
        size = v.getSize()
        if size not in INT_SIZES:
            raise Unsupported(f'int write of size {size}')
        self.emit(f'WR{size * 8}({self.ptr(v)}, ({ut(size)})({expr}));')

    def rdf(self, v):
        size = v.getSize()
        if self.space(v) == 'const':
            bits = v.getOffset()
            return {4: f'bits_f32(0x{bits:x}u)', 8: f'bits_f64(0x{bits:x}ull)'}.get(size) or self.unsup('fconst')
        if size == 4:
            return f'RDF32({self.ptr(v)})'
        if size in (8, 10):
            return f'RDF64({self.ptr(v)})'
        raise Unsupported(f'float size {size}')

    def wrf(self, v, expr):
        size = v.getSize()
        if size == 4:
            self.emit(f'WRF32({self.ptr(v)}, (float)({expr}));')
        elif size == 8:
            self.emit(f'WRF64({self.ptr(v)}, (double)({expr}));')
        elif size == 10:
            self.emit(f'WRF80SLOT({self.ptr(v)}, (double)({expr}));')
        else:
            raise Unsupported(f'float write size {size}')

    def wide_src(self, v, n):
        if self.space(v) == 'const':
            if v.getOffset() != 0:
                raise Unsupported('wide non-zero const')
            return 'g_zero16'
        return self.ptr(v)

    def signed(self, v):
        return f'(({st(v.getSize())}){self.rd(v)})'

    def unsup(self, what):
        raise Unsupported(what)

    def emit(self, s):
        self.lines.append('    ' + s)

    # ---- control flow ---------------------------------------------------
    def in_func(self, addr):
        return self.body.contains(self.space_.getAddress(addr))

    def goto_addr(self, addr):
        if addr in self.insn_addrs:
            self.labels_used.add(f'L_{addr:08x}')
            return f'goto L_{addr:08x};'
        self.features['tailjump'] += 1
        return f'{{ {self.call_expr(addr)} return; }}'

    def call_expr(self, addr):
        # RECOMP_TRACE_CALLS=2 logs direct calls too (see rt_trace_call)
        pre = f'if (g_trace_calls > 1) rt_trace_call(ctx, 0x{addr:08x}u); '
        if addr in self.known:
            return f'{pre}f_{addr:08x}(ctx);'
        return f'rt_call(ctx, 0x{addr:08x}u);'

    # ---- translation ----------------------------------------------------
    def collect_uniques(self):
        spans = []
        for ins in self.insns:
            for op in ins.getPcode():
                for v in list(op.getInputs()) + ([op.getOutput()] if op.getOutput() else []):
                    if v.isUnique():
                        spans.append((v.getOffset(), v.getOffset() + v.getSize()))
        spans.sort()
        self.ublock = {}
        merged = []
        for lo, hi in spans:
            if merged and lo < merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], hi)
            else:
                merged.append([lo, hi])
        compact = 0
        starts = {}
        for lo, hi in merged:
            starts[lo] = (compact, hi)
            compact += (hi - lo + 7) & ~7
        self.uniq_size = max(compact, 8)
        # map every unique offset used to its compact position
        mlist = [(lo, starts[lo][0], starts[lo][1]) for lo, _ in merged]
        import bisect
        los = [m[0] for m in mlist]
        for lo, hi in spans:
            i = bisect.bisect_right(los, lo) - 1
            blo, bcompact, _ = mlist[i]
            self.ublock[lo] = bcompact + (lo - blo)

    def collect_insns(self):
        """Walk the control flow from the entry instead of trusting Ghidra's body.

        MSVC shares tail blocks between functions and Ghidra assigns each block
        to one owner, so a function can jump into a block that is not in its
        body and is not a function entry. Following jumps and fall-throughs
        from the entry (stopping at other functions' entries, which become
        tail calls) gives every block this function can reach.
        """
        listing, fm = self.program.getListing(), self.program.getFunctionManager()
        memory = self.program.getMemory()

        def ins_at(addr):
            # flow can land on bytes Ghidra never disassembled; do it now (the
            # project is a scratch copy opened for writing and never saved)
            ins = listing.getInstructionAt(addr)
            if ins is None:
                block = memory.getBlock(addr)
                if block is not None and block.isExecute():
                    # flow reaches these bytes, so they are code: a mis-decoded
                    # instruction may start inside the real one, or Ghidra typed
                    # them as data (a lone shared `ret`); clear and disassemble
                    data = listing.getDefinedDataContaining(addr)
                    if data is not None:
                        listing.clearCodeUnits(data.getMinAddress(), data.getMaxAddress(), False)
                    else:
                        listing.clearCodeUnits(addr, addr.add(15), False)
                    DisassembleCommand(addr, None, True).applyTo(self.program, TaskMonitor.DUMMY)
                    ins = listing.getInstructionAt(addr)
                    if ins is not None:
                        self.features['redisassembled'] += 1
            return ins

        start = ins_at(self.space_.getAddress(self.entry))
        if start is None:
            return []
        # Ghidra's body also seeds the walk: a block reached only through a
        # jump table Ghidra resolved partially has no incoming flow we can
        # follow, but it is still part of the function.
        seen, order, work = set(), [], [start]
        for ins in listing.getInstructions(self.func.getBody(), True):
            if fm.getFunctionAt(ins.getAddress()) is None or ins.getAddress().getOffset() == self.entry:
                work.append(ins)
        while work:
            ins = work.pop()
            a = ins.getAddress().getOffset()
            if a in seen:
                continue
            seen.add(a)
            order.append(ins)
            ft = ins.getFallThrough()
            if ft is not None:
                nxt = ins_at(ft)
                if nxt is not None:
                    work.append(nxt)
            flow = ins.getFlowType()
            if flow.isJump():
                targets = [t for t in ins.getFlows()]
                if flow.isComputed():
                    targets += self.read_jump_table(ins)
                for t in targets:
                    to = t.getOffset()
                    if to == self.entry or fm.getFunctionAt(t) is not None:
                        continue  # a jump to a function entry stays a tail call
                    nxt = ins_at(t)
                    if nxt is not None:
                        work.append(nxt)
        order.sort(key=lambda i: i.getAddress().getOffset())
        return order

    def read_jump_table(self, ins):
        """Targets of `jmp dword ptr [reg*4 + table]` read from the table itself.

        Ghidra's switch recovery sometimes resolves only part of a table (or
        none); the entries that point back into this function's neighbourhood
        are taken as cases. Stops at the first entry that does not.
        """
        memory = self.program.getMemory()
        base = None
        for i in range(ins.getNumOperands()):
            for obj in ins.getOpObjects(i):
                if hasattr(obj, 'getUnsignedValue') and obj.getUnsignedValue() > 0x10000:
                    base = obj.getUnsignedValue()
        if base is None or '*0x4' not in str(ins):
            return []
        # any entry inside the section that holds this function counts: bodies
        # of the game's biggest functions are split across several Ghidra
        # functions, so a tighter window misses cases
        block = memory.getBlock(self.space_.getAddress(self.entry))
        lo, hi = block.getStart().getOffset(), block.getEnd().getOffset() + 1
        out, misses = [], 0
        for n in range(1024):
            try:
                v = memory.getInt(self.space_.getAddress(base + 4 * n)) & 0xFFFFFFFF
            except Exception:  # noqa: BLE001
                break
            if not (lo <= v < hi):
                misses += 1
                if misses > 2:
                    break
                continue
            out.append(self.space_.getAddress(v))
        if out:
            self.features['jumptable_read'] += 1
        return out

    def translate(self):
        listing = self.program.getListing()
        self.body = self.func.getBody()
        self.space_ = self.program.getAddressFactory().getDefaultAddressSpace()
        self.insns = self.collect_insns()
        self.insn_addrs = {i.getAddress().getOffset() for i in self.insns}
        self.labels_used.add(f'L_{self.entry:08x}')
        self.collect_uniques()
        for idx, ins in enumerate(self.insns):
            a = ins.getAddress().getOffset()
            self.lines.append(f'L_{a:08x}: /* {ins} */')
            ops = list(ins.getPcode())
            rel_targets = set()
            for i, op in enumerate(ops):
                if op.getMnemonic() in ('BRANCH', 'CBRANCH') and self.space(op.getInput(0)) == 'const':
                    rel_targets.add(i + self.sext(op.getInput(0)))
            for i, op in enumerate(ops):
                if i in rel_targets:
                    self.lines.append(f'L_{a:08x}_{i}:;')
                self.op(ins, a, i, op, ops)
            if len(ops) in rel_targets:
                self.lines.append(f'L_{a:08x}_{len(ops)}:;')
            # explicit fall-through when the next emitted instruction is not the next in memory
            ft = ins.getFallThrough()
            if ft is not None:
                nxt = self.insns[idx + 1].getAddress().getOffset() if idx + 1 < len(self.insns) else None
                if ft.getOffset() != nxt:
                    self.emit(self.goto_addr(ft.getOffset()))
            self.features['insns'] += 1
        return self.render()

    def sext(self, v):
        x = v.getOffset() & 0xFFFFFFFF
        return x - (1 << 32) if x & 0x80000000 else x

    def rel_label(self, a, i, off):
        t = i + off
        self.labels_used.add(f'L_{a:08x}_{t}')
        return f'L_{a:08x}_{t}'

    def op(self, ins, a, i, op, ops):
        m = op.getMnemonic()
        o = op.getOutput()
        ins_ = list(op.getInputs())
        self.features['op_' + m] += 1
        if m == 'COPY':
            if o.getSize() in INT_SIZES:
                self.wr(o, self.rd(ins_[0]))
            else:
                self.emit(f'memcpy({self.ptr(o)}, {self.ptr(ins_[0])}, {o.getSize()});')
        elif m == 'LOAD':
            addr = self.rd(ins_[1])
            n = o.getSize()
            if n in INT_SIZES:
                self.wr(o, f'RD{n * 8}(MEM({addr}))')
            elif n == 10:
                self.emit(f'WRF80SLOT({self.ptr(o)}, f80_to_f64(MEM({addr})));')
            else:
                self.emit(f'memcpy({self.ptr(o)}, MEM({addr}), {n});')
        elif m == 'STORE':
            addr, val = self.rd(ins_[1]), ins_[2]
            n = val.getSize()
            if n in INT_SIZES:
                self.emit(f'WR{n * 8}(MEM({addr}), {self.rd(val)});')
            elif n == 10:
                self.emit(f'f64_to_f80(MEM({addr}), {self.rdf(val)});')
            else:
                src = self.ptr(val) if self.space(val) != 'const' else self.unsup('const store')
                self.emit(f'memcpy(MEM({addr}), {src}, {n});')
        elif m == 'BRANCH':
            t = ins_[0]
            if self.space(t) == 'const':
                self.emit(f'goto {self.rel_label(a, i, self.sext(t))};')
            else:
                self.emit(self.goto_addr(t.getOffset()))
        elif m == 'CBRANCH':
            t, c = ins_
            if self.space(t) == 'const':
                self.emit(f'if ({self.rd(c)}) goto {self.rel_label(a, i, self.sext(t))};')
            else:
                self.emit(f'if ({self.rd(c)}) {self.goto_addr(t.getOffset())}')
        elif m == 'BRANCHIND':
            # Ghidra's jump-table recovery can miss cases, so the switch covers
            # every instruction of this function; only external targets fall back
            self.features['branchind'] += 1
            self.needs_dispatch = True
            self.emit(f'{{ t_dispatch = {self.rd(ins_[0])}; goto L_dispatch; }}')
        elif m == 'CALL':
            self.features['call'] += 1
            self.emit(self.call_expr(ins_[0].getOffset()))
        elif m == 'CALLIND':
            self.features['callind'] += 1
            self.emit(f'rt_call(ctx, {self.rd(ins_[0])});')
        elif m == 'RETURN':
            self.emit('return;')
        elif m == 'CALLOTHER':
            self.userop(a, op, o, ins_)
        elif m in BINOP:
            n = o.getSize()
            if n in INT_SIZES:
                self.wr(o, f'({ut(n)})({self.rd(ins_[0])} {BINOP[m]} {self.rd(ins_[1])})')
            elif m in ('INT_AND', 'INT_OR', 'INT_XOR'):
                fn = {'INT_AND': 'and', 'INT_OR': 'or', 'INT_XOR': 'xor'}[m]
                self.emit(f'wide_{fn}({self.ptr(o)}, {self.wide_src(ins_[0], n)}, {self.wide_src(ins_[1], n)}, {n});')
            else:
                raise Unsupported(f'{m} of size {n}')
        elif m in CMP_U:
            self.wr(o, f'{self.rd(ins_[0])} {CMP_U[m]} {self.rd(ins_[1])}')
        elif m in CMP_S:
            self.wr(o, f'{self.signed(ins_[0])} {CMP_S[m]} {self.signed(ins_[1])}')
        elif m in ('INT_DIV', 'INT_REM', 'INT_SDIV', 'INT_SREM'):
            n = o.getSize()
            fn = {'INT_DIV': 'udiv', 'INT_REM': 'urem', 'INT_SDIV': 'sdiv', 'INT_SREM': 'srem'}[m]
            self.wr(o, f'rt_{fn}{n * 8}(ctx, {self.rd(ins_[0])}, {self.rd(ins_[1])})')
        elif m == 'INT_NEGATE':
            self.wr(o, f'~{self.rd(ins_[0])}')
        elif m == 'INT_2COMP':
            self.wr(o, f'-{self.rd(ins_[0])}')
        elif m == 'INT_ZEXT':
            if o.getSize() not in INT_SIZES:
                self.emit(f'memset({self.ptr(o)}, 0, {o.getSize()}); '
                          f'memcpy({self.ptr(o)}, {self.ptr(ins_[0])}, {ins_[0].getSize()});')
            else:
                self.wr(o, self.rd(ins_[0]))
        elif m == 'INT_SEXT':
            self.wr(o, f'({st(o.getSize())}){self.signed(ins_[0])}')
        elif m == 'INT_CARRY':
            n = ins_[0].getSize()
            self.wr(o, f'carry{n * 8}({self.rd(ins_[0])}, {self.rd(ins_[1])})')
        elif m == 'INT_SCARRY':
            n = ins_[0].getSize()
            self.wr(o, f'scarry{n * 8}({self.rd(ins_[0])}, {self.rd(ins_[1])})')
        elif m == 'INT_SBORROW':
            n = ins_[0].getSize()
            self.wr(o, f'sborrow{n * 8}({self.rd(ins_[0])}, {self.rd(ins_[1])})')
        elif m in ('INT_LEFT', 'INT_RIGHT', 'INT_SRIGHT'):
            n = ins_[0].getSize()
            fn = {'INT_LEFT': 'shl', 'INT_RIGHT': 'shr', 'INT_SRIGHT': 'sar'}[m]
            self.wr(o, f'{fn}{n * 8}({self.rd(ins_[0])}, (uint64_t){self.rd(ins_[1])})')
        elif m == 'BOOL_NEGATE':
            self.wr(o, f'!{self.rd(ins_[0])}')
        elif m in ('BOOL_AND', 'BOOL_OR', 'BOOL_XOR'):
            sym = {'BOOL_AND': '&', 'BOOL_OR': '|', 'BOOL_XOR': '^'}[m]
            self.wr(o, f'({self.rd(ins_[0])} {sym} {self.rd(ins_[1])}) & 1')
        elif m == 'POPCOUNT':
            self.wr(o, f'popcount64((uint64_t){self.rd(ins_[0])})')
        elif m == 'SUBPIECE':
            src, c = ins_[0], ins_[1].getOffset()
            if src.getSize() in INT_SIZES and o.getSize() in INT_SIZES:
                self.wr(o, f'({self.rd(src)} >> {8 * c})' if c else self.rd(src))
            else:
                self.emit(f'memcpy({self.ptr(o)}, {self.ptr(src)} + {c}, {o.getSize()});')
        elif m == 'PIECE':
            hi, lo = ins_
            if o.getSize() in INT_SIZES:
                self.wr(o, f'((({ut(o.getSize())}){self.rd(hi)}) << {8 * lo.getSize()}) | {self.rd(lo)}')
            else:
                self.emit(f'memcpy({self.ptr(o)}, {self.ptr(lo)}, {lo.getSize()}); '
                          f'memcpy({self.ptr(o)} + {lo.getSize()}, {self.ptr(hi)}, {hi.getSize()});')
        elif m in FBIN:
            self.wrf(o, f'{self.rdf(ins_[0])} {FBIN[m]} {self.rdf(ins_[1])}')
        elif m in FCMP:
            self.wr(o, f'{self.rdf(ins_[0])} {FCMP[m]} {self.rdf(ins_[1])}')
        elif m == 'FLOAT_NEG':
            self.wrf(o, f'-{self.rdf(ins_[0])}')
        elif m == 'FLOAT_ABS':
            self.wrf(o, f'fabs({self.rdf(ins_[0])})')
        elif m == 'FLOAT_SQRT':
            self.wrf(o, f'sqrt({self.rdf(ins_[0])})')
        elif m == 'FLOAT_NAN':
            self.wr(o, f'isnan({self.rdf(ins_[0])})')
        elif m == 'INT2FLOAT':
            self.wrf(o, f'(double){self.signed(ins_[0])}')
        elif m == 'FLOAT2FLOAT':
            self.wrf(o, self.rdf(ins_[0]))
        elif m == 'TRUNC':
            self.wr(o, f'f2i{o.getSize() * 8}({self.rdf(ins_[0])})')
        elif m == 'ROUND':
            self.wrf(o, f'nearbyint({self.rdf(ins_[0])})')
        elif m == 'FLOAT_FLOOR':
            self.wrf(o, f'floor({self.rdf(ins_[0])})')
        elif m == 'FLOAT_CEIL':
            self.wrf(o, f'ceil({self.rdf(ins_[0])})')
        else:
            raise Unsupported(f'op {m}')

    def userop(self, a, op, o, ins_):
        name = self.lang.getUserDefinedOpName(int(ins_[0].getOffset()))
        self.features['userop_' + name] += 1
        args = ins_[1:]
        if name in USEROPS_NOP:
            return
        if name in USEROPS_FLOAT and o is not None:
            fargs = ', '.join(self.rdf(x) for x in args)
            self.wrf(o, f'{USEROPS_FLOAT[name]}({fargs})')
        elif name == 'swi':
            self.emit(f'rt_interrupt(ctx, 0x{a:08x}u, {self.rd(args[0])});')
        elif name == 'rdtsc' and o is not None:
            self.wr(o, 'rt_rdtsc(ctx)')
        elif name in USEROPS_PACKED and o is not None:
            # packed sources are pointers; a shift count or shuffle immediate is a value
            def packed_arg(x, as_value):
                if self.space(x) == 'const':
                    return f'(uint64_t)0x{x.getOffset():x}ull'
                return f'RD64({self.ptr(x)})' if as_value else self.ptr(x)
            value_last = name in ('psllw', 'pshufw')
            parts = [packed_arg(x, value_last and i == len(args) - 1) for i, x in enumerate(args)]
            self.emit(f'{USEROPS_PACKED[name]}({self.ptr(o)}, {", ".join(parts)});')
        else:
            # anything else has no host equivalent (privileged instructions,
            # or padding decoded as code): trap if it is ever reached
            self.features['userop_trap_' + name] += 1
            self.emit(f'rt_trap(ctx, TRAP_UNSUPPORTED, 0x{a:08x}u); return;')

    def render(self):
        # instructions are emitted in address order, and a function body can
        # contain blocks below its entry point (shared tails, adjustor thunks),
        # so always start by jumping to the entry
        out = [f'void f_{self.entry:08x}(Ctx *ctx) {{ /* {self.func.getName(True)} */',
               f'    uint8_t U[{self.uniq_size}];']
        if self.needs_dispatch:
            out.append('    uint32_t t_dispatch;')
            # every instruction is a possible target of a computed jump whose
            # table was only partly recovered; one switch per function keeps
            # the generated code a manageable size
            for t in sorted(self.insn_addrs):
                self.labels_used.add(f'L_{t:08x}')
        if self.entry in self.insn_addrs:
            out.append(f'    goto L_{self.entry:08x};')
        else:  # Ghidra has no instruction at the entry (a stray 1-byte function)
            out.append(f'    rt_trap(ctx, TRAP_MISSING_FUNCTION, 0x{self.entry:08x}u);')
        for line in self.lines:
            if line.endswith(':;') and line.split(':')[0] not in self.labels_used:
                continue
            if line.startswith('L_') and ': /*' in line:
                label = line.split(':')[0]
                if label not in self.labels_used:
                    out.append('    /* ' + line[line.index('/*') + 3:])
                    continue
            out.append(line)
        if self.needs_dispatch:
            out.append('L_dispatch:')
            out.append('    switch (t_dispatch) {')
            for t in sorted(self.insn_addrs):
                out.append(f'    case 0x{t:08x}u: goto L_{t:08x};')
            out.append('    default: rt_jump(ctx, t_dispatch); return;')
            out.append('    }')
        out.append('}')
        return '\n'.join(out)


def load_hooks(path):
    """hooks.csv: address,name -> functions the host runtime implements as hook_<name>."""
    hooks = {}
    if path and os.path.exists(path):
        for line in open(path):
            line = line.split('#')[0].strip()
            if line:
                addr, name = [x.strip() for x in line.split(',')[:2]]
                hooks[int(addr, 16)] = name
    return hooks


def main(project_dir, out_dir, select='leaf', hooks_path=None):
    hooks = load_hooks(hooks_path)
    project = GhidraProject.openProject(project_dir, 'obiwan', False)
    program = project.openProgram('/', PROGRAM, False)
    tx = program.startTransaction('translate (scratch, never saved)')
    fm, memory = program.getFunctionManager(), program.getMemory()

    # Ghidra's CreateFunctionCmd does not create callees, so code reached only
    # from functions added later (initializers, vtable slots) can be called
    # directly without being a function. Close over direct-call targets on
    # this scratch copy before translating.
    from ghidra.app.cmd.function import CreateFunctionCmd
    listing = program.getListing()
    code_blocks = [b for b in memory.getBlocks() if b.isExecute() and b.getName() not in ('.rdata', '.data', 'DOLBY')]
    created = 0
    space = program.getAddressFactory().getDefaultAddressSpace()

    def looks_like_entry(addr):
        """Code address not inside a function whose preceding bytes end a function
        (ret, int3, nop padding, jmp): safe to make a function without splitting one."""
        if fm.getFunctionContaining(addr) is not None or not any(b.contains(addr) for b in code_blocks):
            return False
        prev = memory.getByte(addr.subtract(1)) & 0xFF
        prev3 = memory.getByte(addr.subtract(3)) & 0xFF
        return prev in (0xC3, 0xC2, 0xCC, 0x90, 0xE9, 0xEB) or prev3 == 0xC2

    # Code pointers in data: vtables without RTTI (XDK libraries, and game
    # classes without a locator), callback tables. Every pointer into code
    # that is (or can become) an instruction start is made a function.
    # Splitting a real function this way is harmless: the control-flow walk
    # follows fall-through into the new entry and turns jumps to it into
    # tail calls, and make_function never clears existing code or data.
    pointer_targets = set()
    for b in memory.getBlocks():
        if b.getName() not in ('.rdata', '.data'):
            continue
        a_, end = b.getStart(), b.getEnd()
        while a_.add(3).compareTo(end) <= 0:
            v = memory.getInt(a_) & 0xFFFFFFFF
            if 0x11000 <= v < 0x3bc220:
                t = space.getAddress(v)
                if fm.getFunctionAt(t) is None and any(cb.contains(t) for cb in code_blocks):
                    pointer_targets.add(t)
            a_ = a_.add(4)
    def make_function(addr):
        """Create a function at addr if the bytes there are an instruction or
        undefined; never disturb defined data (jump tables) or other functions."""
        if fm.getFunctionAt(addr) is not None:
            return False
        if listing.getInstructionAt(addr) is None:
            if listing.getDefinedDataContaining(addr) is not None:
                return False  # jump tables and other data stay untouched
            inside = listing.getInstructionContaining(addr)
            if inside is not None:
                # a pointer into the middle of an instruction means Ghidra decoded
                # from the wrong byte: clear just that instruction and redo it
                listing.clearCodeUnits(inside.getMinAddress(), inside.getMaxAddress(), False)
            DisassembleCommand(addr, None, True).applyTo(program, TaskMonitor.DUMMY)
            if listing.getInstructionAt(addr) is None:
                return False
        return bool(CreateFunctionCmd(addr).applyTo(program, TaskMonitor.DUMMY))

    for addr in pointer_targets:
        if make_function(addr):
            created += 1
    print(f'created {created} functions for code pointers in data', file=sys.stderr)

    def call_target_closure():
        made_total = 0
        for _round in range(8):
            missing = set()
            for ins in listing.getInstructions(True):
                if ins.getFlowType().isCall():
                    for flow in ins.getFlows():
                        if fm.getFunctionAt(flow) is None and any(b.contains(flow) for b in code_blocks):
                            missing.add(flow)
            made = sum(1 for addr in missing if make_function(addr))
            made_total += made
            if not made:
                break
        return made_total

    created += call_target_closure()
    if created:
        print(f'created {created} functions for direct-call targets', file=sys.stderr)

    funcs = [f for f in fm.getFunctions(True)
             if not f.isExternal() and memory.getBlock(f.getEntryPoint()).isExecute()]

    explicit = None
    if select not in ('leaf', 'all', 'text'):
        explicit = {int(x, 16) for x in open(select).read().split()}

    # First pass: translate everything (with an empty `known` set) to gather
    # coverage and features, then emit the selected set. Translating
    # disassembles code on demand, which can expose calls to functions that
    # did not exist yet, so repeat the closure and the pass until it settles.
    meta, ok = {}, {}
    for _round in range(5):
        # every function is walked again each round: a walk can grow once code
        # it reaches has been disassembled, and the emission pass must see the
        # same listing this pass saw
        meta, ok = {}, {}
        for f in funcs:
            a = f.getEntryPoint().getOffset()
            t = FunctionTranslator(program, f, set())
            try:
                code = t.translate()
                status = 'ok'
            except Unsupported as e:
                code, status = None, f'unsupported: {e}'
            except Exception as e:  # noqa: BLE001 - keep going and report
                code, status = None, f'error: {e}'
            meta[a] = {'name': f.getName(True), 'status': status, 'block': memory.getBlock(f.getEntryPoint()).getName(),
                       'features': dict(t.features)}
            if code is not None:
                ok[a] = f
        more = call_target_closure()
        if not more:
            break
        print(f'round {_round + 1}: {more} more functions found by disassembling on demand', file=sys.stderr)
        funcs = [f for f in fm.getFunctions(True)
                 if not f.isExternal() and memory.getBlock(f.getEntryPoint()).isExecute()]

    def is_leaf(a):
        ft = meta[a]['features']
        return (meta[a]['status'] == 'ok' and meta[a]['block'] == '.text' and not ft.get('call')
                and not ft.get('callind') and not ft.get('branchind') and not ft.get('fs')
                and not ft.get('tailjump') and not any(k.startswith('userop_') for k in ft))

    if select == 'all':
        chosen = sorted(ok)
    elif select == 'text':
        chosen = sorted(a for a in ok if meta[a]['block'] == '.text')
    elif select == 'leaf':
        chosen = sorted(a for a in ok if is_leaf(a))
    else:
        chosen = sorted(a for a in explicit if a in ok)
    chosen = sorted(set(chosen) | set(hooks))
    known = set(chosen)
    for a, name in hooks.items():
        meta.setdefault(a, {'name': name, 'status': 'hook', 'block': '', 'features': {}})

    os.makedirs(out_dir, exist_ok=True)
    for old in os.listdir(out_dir):
        if old.startswith('funcs_') and old.endswith('.c'):
            os.remove(os.path.join(out_dir, old))
    for n in range(0, len(chosen), FUNCS_PER_FILE):
        part = chosen[n:n + FUNCS_PER_FILE]
        with open(os.path.join(out_dir, f'funcs_{n // FUNCS_PER_FILE:04d}.c'), 'w') as out:
            out.write('/* Generated by recomp/translator/pcode2c.py from the game executable. Do not commit. */\n')
            out.write('#include "guest.h"\n#include "funcs.h"\n\n')
            for a in part:
                if a in hooks:
                    # the host runtime provides this one; keep the symbol so direct calls link
                    out.write(f'void f_{a:08x}(Ctx *ctx) {{ hook_{hooks[a]}(ctx); }}' + '\n\n')
                    continue
                try:
                    code = FunctionTranslator(program, ok[a], known).translate()
                except Exception as e:  # noqa: BLE001 - keep the build consistent
                    print(f'emit failed for {a:08x}: {e}', file=sys.stderr)
                    meta[a]['status'] = f'emit failed: {e}'
                    code = (f'void f_{a:08x}(Ctx *ctx) {{ /* {meta[a]["name"]}: {e} */' + '\n'
                            + f'    rt_trap(ctx, TRAP_UNSUPPORTED, 0x{a:08x}u);' + '\n}')
                out.write(code + '\n\n')

    def purge_bytes(f):
        """Bytes a function pops on return (stdcall `ret n`), or 0; None if it has no function.

        Uses the control-flow walk, since the `ret` may sit in a shared epilogue
        outside Ghidra's body for the function."""
        if f is None:
            return None
        seen = set()
        while f is not None and f.getEntryPoint().getOffset() not in seen:
            seen.add(f.getEntryPoint().getOffset())
            t = FunctionTranslator(program, f, set())
            t.space_ = program.getAddressFactory().getDefaultAddressSpace()
            insns = t.collect_insns()
            for ins in insns:
                if ins.getMnemonicString().upper() == 'RET':
                    return ins.getNumOperands() and int(ins.getScalar(0).getUnsignedValue()) or 0
            # no ret of its own: a thunk that tail-jumps into another function
            nxt = None
            for ins in insns:
                if ins.getFlowType().isJump():
                    for flow in ins.getFlows():
                        if fm.getFunctionAt(flow) is not None:
                            nxt = fm.getFunctionAt(flow)
            f = nxt
        return 0

    with open(os.path.join(out_dir, 'funcs.h'), 'w') as out:
        out.write('#pragma once\n#include "guest.h"\n')
        for a in chosen:
            out.write(f'void f_{a:08x}(Ctx *ctx);\n')
        for a, name in sorted(hooks.items()):
            out.write(f'void hook_{name}(Ctx *ctx); /* 0x{a:08x} */\n')
            pb = purge_bytes(ok.get(a))
            if pb is not None:
                out.write(f'#define HOOKARGS_{name} {pb // 4} /* stdcall arguments popped by the original */\n')
    with open(os.path.join(out_dir, 'functab.c'), 'w') as out:
        out.write('#include "guest.h"\n#include "funcs.h"\n\nconst FuncEntry g_functab[] = {\n')
        for a in chosen:
            out.write(f'    {{0x{a:08x}u, f_{a:08x}, "{meta[a]["name"]}"}},\n')
        out.write('};\nconst unsigned g_functab_count = sizeof(g_functab) / sizeof(g_functab[0]);\n')
    statuses = collections.Counter(m['status'] for m in meta.values())
    summary = {'functions': len(meta), 'translated': len(ok), 'emitted': len(chosen), 'hooked': len(hooks),
               'leaf_candidates': sum(1 for a in ok if is_leaf(a)), 'status': statuses.most_common()}
    json.dump({'summary': summary, 'functions': {f'0x{a:08x}': m for a, m in meta.items()}},
              open(os.path.join(out_dir, 'meta.json'), 'w'), indent=1)
    print(json.dumps(summary, indent=1))
    program.endTransaction(tx, False)
    project.close()


if __name__ == '__main__':
    main(*sys.argv[1:])
