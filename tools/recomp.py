#!/usr/bin/env python3
"""
recomp.py - static recompiler: Motorola 68000 / 68EC020 machine code -> C

Usage: recomp.py <program.bin> <out_dir> [--prefix NAME] [--cpu 68000|68ec020] [coverage.txt ...]

* Traces code by recursive descent from the exception vectors plus any
  "landing" addresses recorded by the runtime (indirect jump targets).
* Every basic block start becomes a label; code is grouped in 8KB chunks,
  one C function each. Intra-chunk branches are plain gotos.
* Common instructions are translated natively. Anything else is executed
  through the Musashi reference core (rc_interp_fallback) so output is
  always complete and correct.
* Flags use Musashi's internal representation so native code and the
  fallback interpreter share CPU state. Dead flag stores are removed.
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dis as mdis

CHUNK_SHIFT = 13
CHK = '0x80000000u'
ALLF = frozenset('XNZVC')
SZ = {'b': 8, 'w': 16, 'l': 32}
MASK = {'b': '0xffu', 'w': '0xffffu', 'l': '0xffffffffu'}

ROM = b''
CYC = b''
CPU020 = False
TRACE = os.environ.get('RC_TRACE') is not None
PREFIX = 'gunlock'

def r16(a): return (ROM[a] << 8) | ROM[a + 1] if a + 1 < len(ROM) else 0
def r32(a): return (r16(a) << 16) | r16(a + 2)
def s8(v): return v - 0x100 if v & 0x80 else v
def s16(v): return v - 0x10000 if v & 0x8000 else v
def h(v): return '0x%xu' % (v & 0xffffffff)

class Unsupported(Exception): pass

def nflag(sz, r): return {'b': r, 'w': '((%s) >> 8)' % r, 'l': '((%s) >> 24)' % r}[sz]

# ---------------------------------------------------------------- EA
class EA:
    """an effective address; .setup lines compute address (temps), read()/write() give C"""
    def __init__(self, ins, mode, reg, sz, tmp):
        self.mode, self.reg, self.sz = mode, reg, sz
        tmp = 'ea_' + tmp
        self.tmp = tmp
        self.setup = []
        self.addr = None
        self.kind = None
        inc = {'b': 1, 'w': 2, 'l': 4}[sz] if sz else 0
        if mode == 0: self.kind = 'D'
        elif mode == 1: self.kind = 'A'
        elif mode == 2: self.kind = 'M'; self.addr = 'REG_A[%d]' % reg
        elif mode == 3:
            if reg == 7 and sz == 'b': inc = 2
            self.kind = 'M'; self.addr = tmp
            self.setup = ['uint32_t %s = REG_A[%d]; REG_A[%d] += %d;' % (tmp, reg, reg, inc)]
        elif mode == 4:
            if reg == 7 and sz == 'b': inc = 2
            self.kind = 'M'; self.addr = tmp
            self.setup = ['REG_A[%d] -= %d; uint32_t %s = REG_A[%d];' % (reg, inc, tmp, reg)]
        elif mode == 5:
            d = s16(ins.ext16())
            self.kind = 'M'; self.addr = '(REG_A[%d] + (%d))' % (reg, d)
        elif mode == 6:
            self.kind = 'M'; self.addr = self.index(ins, 'REG_A[%d]' % reg)
        elif mode == 7:
            if reg == 0: self.kind = 'M'; self.addr = h(s16(ins.ext16()))
            elif reg == 1: self.kind = 'M'; self.addr = h(ins.ext32())
            elif reg == 2:
                base = ins.pos; d = s16(ins.ext16())
                self.kind = 'M'; self.addr = h(base + d); self.pcrel = True
            elif reg == 3:
                base = ins.pos
                self.kind = 'M'; self.addr = self.index(ins, h(base)); self.pcrel = True
            elif reg == 4:
                self.kind = 'I'
                if sz == 'l': self.imm = ins.ext32()
                elif sz == 'w': self.imm = ins.ext16()
                elif sz == 'b': self.imm = ins.ext16() & 0xff
                else: raise Unsupported('imm nosize')
            else: raise Unsupported('mode7 reg %d' % reg)
    @staticmethod
    def index(ins, base):
        e = ins.ext16()
        if e & 0x0100:
            # 68020 full extension word: only the non memory-indirect forms are translated
            if not CPU020: raise Unsupported('full ext')
            if e & 0x0007: raise Unsupported('memory indirect')
            if e & 0x0008: raise Unsupported('full ext reserved')
            bs, is_ = e & 0x80, e & 0x40
            bdsz = (e >> 4) & 3
            if bdsz == 0: raise Unsupported('full ext bd')
            bd = 0
            ins.eacyc = getattr(ins, 'eacyc', 0) + {1: 0, 2: 2, 3: 6}[bdsz]   # m68ki_ea_idx_cycle_table
            if bdsz == 2: bd = s16(ins.ext16())
            elif bdsz == 3:
                v = ins.ext32(); bd = v - (1 << 32) if v & 0x80000000 else v
            parts = []
            if not bs: parts.append(base)
            if not is_:
                r = (e >> 12) & 15; scale = (e >> 9) & 3
                xr = 'REG_DA[%d]' % r
                x = xr if e & 0x800 else '(uint32_t)(int16_t)%s' % xr
                if scale: x = '(%s << %d)' % (x, scale)
                parts.append(x)
            parts.append('(%d)' % bd)
            return '(' + ' + '.join(parts) + ')'
        scale = (e >> 9) & 3
        if scale and not CPU020: raise Unsupported('scale on 68000')
        r = (e >> 12) & 15
        d = s8(e & 0xff)
        xr = 'REG_DA[%d]' % r
        x = xr if e & 0x800 else '(uint32_t)(int16_t)%s' % xr
        if scale: x = '(%s << %d)' % (x, scale)
        return '(%s + %s + (%d))' % (base, x, d)
    def is_mem(self): return self.kind == 'M'
    def read(self, sz=None):
        sz = sz or self.sz
        if self.kind == 'D': return '(REG_D[%d] & %s)' % (self.reg, MASK[sz]) if sz != 'l' else 'REG_D[%d]' % self.reg
        if self.kind == 'A': return '(REG_A[%d] & %s)' % (self.reg, MASK[sz]) if sz != 'l' else 'REG_A[%d]' % self.reg
        if self.kind == 'I': return h(self.imm)
        return 'rd%d(%s)' % (SZ[sz], self.addr)
    def write(self, v, sz=None):
        sz = sz or self.sz
        if self.kind == 'D':
            r = 'REG_D[%d]' % self.reg
            if sz == 'l': return '%s = %s;' % (r, v)
            m = MASK[sz]
            return '%s = (%s & ~%s) | ((%s) & %s);' % (r, r, m, v, m)
        if self.kind == 'A': return 'REG_A[%d] = %s;' % (self.reg, v)
        if self.kind == 'M': return 'wr%d(%s, %s);' % (SZ[sz], self.addr, v)
        raise Unsupported('write imm')
    def bind(self, tmp):
        """for read-modify-write: make the address a temp so it's computed once"""
        tmp = self.tmp
        if self.kind == 'M' and self.addr != tmp and not self.addr.startswith('0x') and not self.addr.startswith('REG_A['):
            self.setup.append('uint32_t %s = %s;' % (tmp, self.addr))
            self.addr = tmp
        elif self.kind == 'M' and self.addr.startswith('REG_A['):
            self.setup.append('uint32_t %s = %s;' % (tmp, self.addr)); self.addr = tmp

class Ins:
    def __init__(self, pc):
        self.pc = pc
        self.op = r16(pc)
        self.pos = pc + 2
        self.length, self.text = mdis.dis(pc)
        self.lines = []          # C lines
        self.fset = frozenset()  # flags written
        self.fuse = frozenset()  # flags read
        self.flaglines = []      # (flag, line) candidates for DCE, kept in lines as markers
        self.kind = 'seq'        # seq | branch | cond | call | ret | jmpind | callind | fallback | stop
        self.target = None
        self.native = False
    def ext16(self):
        v = r16(self.pos); self.pos += 2; return v
    def ext32(self):
        v = r32(self.pos); self.pos += 4; return v

# conditions (Musashi macros)
COND = ['COND_T()', 'COND_F()', 'COND_HI()', 'COND_LS()', 'COND_CC()', 'COND_CS()', 'COND_NE()', 'COND_EQ()',
        'COND_VC()', 'COND_VS()', 'COND_PL()', 'COND_MI()', 'COND_GE()', 'COND_LT()', 'COND_GT()', 'COND_LE()']
CONDUSE = [set(), set(), {'C', 'Z'}, {'C', 'Z'}, {'C'}, {'C'}, {'Z'}, {'Z'}, {'V'}, {'V'}, {'N'}, {'N'},
           {'N', 'V'}, {'N', 'V'}, {'N', 'V', 'Z'}, {'N', 'V', 'Z'}]

def F(ins, flag, expr):
    ins.lines.append(('F', flag, 'FLAG_%s = %s;' % (flag, expr)))

def logic_flags(ins, sz, r):
    F(ins, 'N', nflag(sz, r)); F(ins, 'Z', r); F(ins, 'V', '0'); F(ins, 'C', '0')

def add_flags(ins, sz, s, d, r, x=True):
    F(ins, 'N', nflag(sz, r))
    F(ins, 'V', {'b': 'VFLAG_ADD_8(%s,%s,%s)', 'w': 'VFLAG_ADD_16(%s,%s,%s)', 'l': 'VFLAG_ADD_32(%s,%s,%s)'}[sz] % (s, d, r))
    c = {'b': 'CFLAG_8(%s)' % r, 'w': 'CFLAG_16(%s)' % r, 'l': 'CFLAG_ADD_32(%s,%s,%s)' % (s, d, r)}[sz]
    F(ins, 'C', c)
    if x: F(ins, 'X', c)
    F(ins, 'Z', '((%s) & %s)' % (r, MASK[sz]))

def sub_flags(ins, sz, s, d, r, x=True):
    F(ins, 'N', nflag(sz, r))
    F(ins, 'V', {'b': 'VFLAG_SUB_8(%s,%s,%s)', 'w': 'VFLAG_SUB_16(%s,%s,%s)', 'l': 'VFLAG_SUB_32(%s,%s,%s)'}[sz] % (s, d, r))
    c = {'b': 'CFLAG_8(%s)' % r, 'w': 'CFLAG_16(%s)' % r, 'l': 'CFLAG_SUB_32(%s,%s,%s)' % (s, d, r)}[sz]
    F(ins, 'C', c)
    if x: F(ins, 'X', c)
    F(ins, 'Z', '((%s) & %s)' % (r, MASK[sz]))

def L(ins, *ls): ins.lines.extend(ls)

def sizefield(v): return {0: 'b', 1: 'w', 2: 'l'}.get(v)

def ea_lines(ins, *eas):
    for e in eas: L(ins, *e.setup)

# ---------------------------------------------------------------- translate
def translate(ins):
    op = ins.op
    hi = op >> 12
    mode, reg = (op >> 3) & 7, op & 7
    rx = (op >> 9) & 7
    if hi in (1, 2, 3):
        sz = {1: 'b', 3: 'w', 2: 'l'}[hi]
        src = EA(ins, mode, reg, sz, 's')
        dmode = (op >> 6) & 7
        if dmode == 1:
            if sz == 'b': raise Unsupported('movea.b')
            L(ins, '{'); ea_lines(ins, src)
            v = src.read()
            L(ins, 'REG_A[%d] = %s;' % (rx, v if sz == 'l' else '(uint32_t)(int16_t)%s' % v), '}')
            return
        dst = EA(ins, dmode, rx, sz, 'd')
        if dst.kind in ('A', 'I'): raise Unsupported('move dst')
        L(ins, '{'); ea_lines(ins, src)
        L(ins, 'uint32_t r = %s;' % src.read())
        ea_lines(ins, dst)
        L(ins, dst.write('r'))
        logic_flags(ins, sz, 'r'); L(ins, '}')
        return
    if hi == 7:
        if op & 0x100: raise Unsupported('moveq')
        v = s8(op & 0xff) & 0xffffffff
        L(ins, 'REG_D[%d] = %s;' % (rx, h(v)))
        F(ins, 'N', h(v >> 24)); F(ins, 'Z', h(v)); F(ins, 'V', '0'); F(ins, 'C', '0')
        return
    if hi == 6:
        cc = (op >> 8) & 15
        d = s8(op & 0xff)
        ins.notake = -2
        if d == 0: d = s16(ins.ext16()); ins.notake = 0 if CPU020 else 2
        elif (op & 0xff) == 0xff:
            if not CPU020: raise Unsupported('bcc.l')
            v = ins.ext32(); d = v - (1 << 32) if v & 0x80000000 else v; ins.notake = 0
        tgt = (ins.pc + 2 + d) & 0xffffff
        ins.target = tgt
        if cc == 0: ins.kind = 'branch'
        elif cc == 1:
            ins.kind = 'call'
            L(ins, 'REG_A[7] -= 4; wr32(REG_A[7], %s);' % h(ins.pc + ins.length))
        else:
            ins.kind = 'cond'; ins.cond = COND[cc]; ins.fuse = frozenset(CONDUSE[cc])
        return
    if hi == 5:
        sz = sizefield((op >> 6) & 3)
        if sz is None:
            cc = (op >> 8) & 15
            if mode == 1:   # dbcc
                d = s16(ins.ext16())
                ins.target = (ins.pc + 2 + d) & 0xffffff
                ins.kind = 'dbcc'; ins.cond = COND[cc]; ins.fuse = frozenset(CONDUSE[cc]); ins.dreg = reg
                return
            if mode == 7 and reg >= 2: raise Unsupported('trapcc')
            dst = EA(ins, mode, reg, 'b', 'd')
            if dst.kind in ('A', 'I'): raise Unsupported('scc')
            ins.fuse = frozenset(CONDUSE[cc])
            if dst.kind == 'D':
                L(ins, 'if (%s) { REG_D[%d] |= 0xffu;%s } else REG_D[%d] &= 0xffffff00u;' % (COND[cc], reg, '' if CPU020 else ' g_cycles += 2;', reg)); return
            L(ins, '{'); ea_lines(ins, dst); L(ins, dst.write('(%s ? 0xffu : 0u)' % COND[cc]), '}')
            return
        q = (op >> 9) & 7 or 8
        sub = op & 0x100
        dst = EA(ins, mode, reg, sz, 'd')
        if dst.kind == 'A':
            L(ins, 'REG_A[%d] %s= %d;' % (reg, '-' if sub else '+', q)); return
        if dst.kind == 'I': raise Unsupported('addq imm')
        dst.bind('d')
        L(ins, '{'); ea_lines(ins, dst)
        L(ins, 'uint32_t s = %d, dv = %s;' % (q, dst.read()))
        L(ins, 'uint32_t r = %s;' % ('dv - s' if sub else 's + dv'))
        L(ins, dst.write('r'))
        (sub_flags if sub else add_flags)(ins, sz, 's', 'dv', 'r')
        L(ins, '}')
        return
    if hi == 0:
        if op & 0x100 or (op & 0xff00) == 0x0800:
            # bit ops
            typ = (op >> 6) & 3
            if op & 0x100:
                if mode == 1: raise Unsupported('movep')
                bit = 'REG_D[%d]' % rx
            else:
                bit = h(ins.ext16() & 0xff)
            if mode == 0:
                L(ins, '{ uint32_t m = 1u << (%s & 31);' % bit)
                F(ins, 'Z', 'REG_D[%d] & m' % reg)
                if typ == 1: L(ins, 'REG_D[%d] ^= m;' % reg)
                elif typ == 2: L(ins, 'REG_D[%d] &= ~m;' % reg)
                elif typ == 3: L(ins, 'REG_D[%d] |= m;' % reg)
                L(ins, '}')
                return
            dst = EA(ins, mode, reg, 'b', 'd')
            if dst.kind in ('A',): raise Unsupported('bit A')
            if dst.kind == 'I':
                if typ != 0: raise Unsupported('bit imm')
            else: dst.bind('d')
            L(ins, '{'); ea_lines(ins, dst)
            L(ins, 'uint32_t m = 1u << (%s & 7); uint32_t dv = %s;' % (bit, dst.read()))
            F(ins, 'Z', 'dv & m')
            if typ == 1: L(ins, dst.write('dv ^ m'))
            elif typ == 2: L(ins, dst.write('dv & ~m'))
            elif typ == 3: L(ins, dst.write('dv | m'))
            L(ins, '}')
            return
        sub = (op >> 9) & 7
        sz = sizefield((op >> 6) & 3)
        if sz is None or sub in (7,): raise Unsupported('imm op')
        if mode == 7 and reg == 4: raise Unsupported('to ccr/sr')
        immv = EA(ins, 7, 4, sz, 'i').imm
        dst = EA(ins, mode, reg, sz, 'd')
        if dst.kind in ('A', 'I'): raise Unsupported('imm dst')
        dst.bind('d')
        L(ins, '{'); ea_lines(ins, dst)
        L(ins, 'uint32_t s = %s, dv = %s;' % (h(immv), dst.read()))
        if sub == 0: L(ins, 'uint32_t r = dv | s;', dst.write('r')); logic_flags(ins, sz, 'r')
        elif sub == 1: L(ins, 'uint32_t r = dv & s;', dst.write('r')); logic_flags(ins, sz, 'r')
        elif sub == 5: L(ins, 'uint32_t r = dv ^ s;', dst.write('r')); logic_flags(ins, sz, 'r')
        elif sub == 2: L(ins, 'uint32_t r = dv - s;', dst.write('r')); sub_flags(ins, sz, 's', 'dv', 'r')
        elif sub == 3: L(ins, 'uint32_t r = s + dv;', dst.write('r')); add_flags(ins, sz, 's', 'dv', 'r')
        elif sub == 6: L(ins, 'uint32_t r = dv - s;'); sub_flags(ins, sz, 's', 'dv', 'r', x=False)
        else: raise Unsupported('imm sub')
        L(ins, '}')
        return
    if hi == 4:
        if op == 0x4e71: return
        if op == 0x4e75:
            ins.kind = 'ret'; L(ins, 'uint32_t t = rd32(REG_A[7]); REG_A[7] += 4;'); return
        if (op & 0xf1c0) == 0x41c0:  # lea
            src = EA(ins, mode, reg, None, 's')
            if src.kind != 'M' or mode in (3, 4): raise Unsupported('lea')
            L(ins, 'REG_A[%d] = %s;' % (rx, src.addr)); return
        if (op & 0xffc0) in (0x4e80, 0x4ec0):  # jsr/jmp
            src = EA(ins, mode, reg, None, 's')
            if src.kind != 'M' or mode in (3, 4): raise Unsupported('jmp')
            call = (op & 0xffc0) == 0x4e80
            if src.addr.startswith('0x'):
                ins.target = int(src.addr.rstrip('u'), 16) & 0xffffff
                ins.kind = 'call' if call else 'branch'
                if call: L(ins, 'REG_A[7] -= 4; wr32(REG_A[7], %s);' % h(ins.pc + ins.length))
            else:
                ins.kind = 'callind' if call else 'jmpind'
                L(ins, 'uint32_t t = %s;' % src.addr)
                if call: L(ins, 'REG_A[7] -= 4; wr32(REG_A[7], %s);' % h(ins.pc + ins.length))
            return
        if (op & 0xffc0) == 0x4840:
            if mode == 0:
                L(ins, '{ uint32_t r = (REG_D[%d] << 16) | (REG_D[%d] >> 16); REG_D[%d] = r;' % (reg, reg, reg))
                logic_flags(ins, 'l', 'r'); L(ins, '}'); return
            src = EA(ins, mode, reg, None, 's')
            if src.kind != 'M' or mode in (3, 4): raise Unsupported('pea')
            L(ins, '{ uint32_t a = %s; REG_A[7] -= 4; wr32(REG_A[7], a); }' % src.addr); return
        if (op & 0xffb8) == 0x4880:  # ext
            if op & 0x40:
                L(ins, '{ uint32_t r = (uint32_t)(int16_t)REG_D[%d]; REG_D[%d] = r;' % (reg, reg)); logic_flags(ins, 'l', 'r')
            else:
                L(ins, '{ uint32_t r = (uint32_t)(int8_t)REG_D[%d] & 0xffff; REG_D[%d] = (REG_D[%d] & 0xffff0000u) | r;' % (reg, reg, reg)); logic_flags(ins, 'w', 'r')
            L(ins, '}'); return
        if (op & 0xfb80) == 0x4880:  # movem
            return movem(ins)
        if (op & 0xfff8) == 0x4e50:  # link
            if reg == 7: raise Unsupported('link a7')
            d = s16(ins.ext16())
            L(ins, 'REG_A[7] -= 4; wr32(REG_A[7], REG_A[%d]); REG_A[%d] = REG_A[7]; REG_A[7] += %d;' % (reg, reg, d)); return
        if (op & 0xfff8) == 0x4e58:  # unlk
            if reg == 7: raise Unsupported('unlk a7')
            L(ins, 'REG_A[7] = REG_A[%d]; REG_A[%d] = rd32(REG_A[7]); REG_A[7] += 4;' % (reg, reg)); return
        sub = (op >> 8) & 0xf
        sz = sizefield((op >> 6) & 3)
        if sz and sub in (2, 4, 6, 0xa):
            dst = EA(ins, mode, reg, sz, 'd')
            if dst.kind in ('A', 'I'): raise Unsupported('unary')
            if sub == 2:  # clr
                L(ins, '{'); ea_lines(ins, dst); L(ins, dst.write('0'), '}')
                F(ins, 'N', '0'); F(ins, 'Z', '0'); F(ins, 'V', '0'); F(ins, 'C', '0'); return
            dst.bind('d')
            L(ins, '{'); ea_lines(ins, dst); L(ins, 'uint32_t dv = %s;' % dst.read())
            if sub == 0xa: logic_flags(ins, sz, 'dv')
            elif sub == 6: L(ins, 'uint32_t r = ~dv & %s;' % MASK[sz], dst.write('r')); logic_flags(ins, sz, 'r')
            elif sub == 4:
                L(ins, 'uint32_t r = 0u - dv;', dst.write('r'))
                F(ins, 'N', nflag(sz, 'r'))
                c = {'b': 'CFLAG_8(r)', 'w': 'CFLAG_16(r)', 'l': 'CFLAG_SUB_32(dv, 0, r)'}[sz]
                F(ins, 'C', c); F(ins, 'X', c)
                F(ins, 'V', {'b': '(dv & r)', 'w': '((dv & r) >> 8)', 'l': '((dv & r) >> 24)'}[sz])
                F(ins, 'Z', '(r & %s)' % MASK[sz])
            L(ins, '}'); return
        raise Unsupported('line4')
    if hi in (8, 9, 0xb, 0xc, 0xd):
        opm = (op >> 6) & 7
        if opm in (3, 7):
            if hi in (9, 0xb, 0xd):  # adda/suba/cmpa
                sz = 'w' if opm == 3 else 'l'
                src = EA(ins, mode, reg, sz, 's')
                L(ins, '{'); ea_lines(ins, src)
                v = src.read() if sz == 'l' else '(uint32_t)(int16_t)%s' % src.read()
                L(ins, 'uint32_t s = %s;' % v)
                if hi == 0xd: L(ins, 'REG_A[%d] += s;' % rx)
                elif hi == 9: L(ins, 'REG_A[%d] -= s;' % rx)
                else:
                    L(ins, 'uint32_t dv = REG_A[%d], r = dv - s;' % rx); sub_flags(ins, 'l', 's', 'dv', 'r', x=False)
                L(ins, '}'); return
            if hi == 0xc:  # mulu/muls
                src = EA(ins, mode, reg, 'w', 's')
                if src.kind == 'A': raise Unsupported('mul An')
                L(ins, '{'); ea_lines(ins, src)
                L(ins, 'uint32_t sv = %s;' % src.read())
                cm = '' if CPU020 else ' g_cycles += rc_mulu_cyc(sv);'
                cs = '' if CPU020 else ' g_cycles += rc_muls_cyc(sv);'
                if opm == 3: L(ins, 'uint32_t r = (REG_D[%d] & 0xffffu) * sv;%s' % (rx, cm))
                else: L(ins, 'uint32_t r = (uint32_t)((int32_t)(int16_t)REG_D[%d] * (int32_t)(int16_t)sv);%s' % (rx, cs))
                L(ins, 'REG_D[%d] = r;' % rx); logic_flags(ins, 'l', 'r'); L(ins, '}'); return
            raise Unsupported('div')
        sz = sizefield(opm & 3)
        to_ea = opm >= 4
        if to_ea and mode in (0, 1) and hi != 0xb:
            if hi == 0xc and opm in (5, 6) and mode in (0, 1) or (hi == 0xc and opm == 5):
                return exg(ins)
            raise Unsupported('x/bcd')
        if hi == 0xb and to_ea and mode == 1: raise Unsupported('cmpm')
        if hi == 0xb and to_ea:  # eor
            dst = EA(ins, mode, reg, sz, 'd')
            if dst.kind in ('A', 'I'): raise Unsupported('eor')
            dst.bind('d')
            L(ins, '{'); ea_lines(ins, dst)
            L(ins, 'uint32_t r = (%s ^ REG_D[%d]) & %s;' % (dst.read(), rx, MASK[sz]), dst.write('r'))
            logic_flags(ins, sz, 'r'); L(ins, '}'); return
        if not to_ea:
            src = EA(ins, mode, reg, sz, 's')
            if src.kind == 'A' and sz == 'b': raise Unsupported('An.b')
            dst = EA(ins, 0, rx, sz, 'd')
            L(ins, '{'); ea_lines(ins, src)
            L(ins, 'uint32_t s = %s, dv = %s;' % (src.read(), dst.read()))
        else:
            dst = EA(ins, mode, reg, sz, 'd')
            if dst.kind in ('A', 'I'): raise Unsupported('to ea')
            dst.bind('d')
            L(ins, '{'); ea_lines(ins, dst)
            L(ins, 'uint32_t s = %s, dv = %s;' % (EA(ins, 0, rx, sz, 's').read(), dst.read()))
        if hi == 8: L(ins, 'uint32_t r = dv | s;', dst.write('r')); logic_flags(ins, sz, 'r')
        elif hi == 0xc: L(ins, 'uint32_t r = dv & s;', dst.write('r')); logic_flags(ins, sz, 'r')
        elif hi == 0xd: L(ins, 'uint32_t r = s + dv;', dst.write('r')); add_flags(ins, sz, 's', 'dv', 'r')
        elif hi == 9: L(ins, 'uint32_t r = dv - s;', dst.write('r')); sub_flags(ins, sz, 's', 'dv', 'r')
        elif hi == 0xb: L(ins, 'uint32_t r = dv - s;'); sub_flags(ins, sz, 's', 'dv', 'r', x=False)
        L(ins, '}'); return
    if hi == 0xe:
        sz = sizefield((op >> 6) & 3)
        if sz is None: raise Unsupported('mem shift')
        if op & 0x20: raise Unsupported('reg-count shift')
        return shift_imm(ins, sz)
    raise Unsupported('line %x' % hi)

def exg(ins):
    op = ins.op; rx = (op >> 9) & 7; ry = op & 7
    m = op & 0x1f8
    if m == 0x140: a, b = 'REG_D[%d]' % rx, 'REG_D[%d]' % ry
    elif m == 0x148: a, b = 'REG_A[%d]' % rx, 'REG_A[%d]' % ry
    elif m == 0x188: a, b = 'REG_D[%d]' % rx, 'REG_A[%d]' % ry
    else: raise Unsupported('exg')
    L(ins, '{ uint32_t t = %s; %s = %s; %s = t; }' % (a, a, b, b))

def movem(ins):
    op = ins.op
    tomem = not (op & 0x400)
    sz = 'l' if op & 0x40 else 'w'
    mask = ins.ext16()
    mode, reg = (op >> 3) & 7, op & 7
    n = 4 if sz == 'l' else 2
    ins.extra = bin(mask).count('1') * (4 if CPU020 else (8 if sz == 'l' else 4))
    L(ins, '{')
    if tomem:
        if mode == 4:
            L(ins, 'uint32_t ea = REG_A[%d];' % reg)
            for i in range(16):
                if mask & (1 << i):
                    L(ins, 'ea -= %d; wr%d(ea, REG_DA[%d]%s);' % (n, SZ[sz], 15 - i, '' if sz == 'l' else ' & 0xffff'))
            L(ins, 'REG_A[%d] = ea;' % reg)
        else:
            e = EA(ins, mode, reg, None, 'ea')
            if e.kind != 'M' or mode == 3 or getattr(e, 'pcrel', False): raise Unsupported('movem')
            L(ins, 'uint32_t ea = %s;' % e.addr)
            for i in range(16):
                if mask & (1 << i):
                    L(ins, 'wr%d(ea, REG_DA[%d]%s); ea += %d;' % (SZ[sz], i, '' if sz == 'l' else ' & 0xffff', n))
    else:
        if mode == 3: L(ins, 'uint32_t ea = REG_A[%d];' % reg)
        else:
            e = EA(ins, mode, reg, None, 'ea')
            if e.kind != 'M' or mode == 4: raise Unsupported('movem')
            L(ins, 'uint32_t ea = %s;' % e.addr)
        for i in range(16):
            if mask & (1 << i):
                v = 'rd32(ea)' if sz == 'l' else '(uint32_t)(int16_t)rd16(ea)'
                L(ins, 'REG_DA[%d] = %s; ea += %d;' % (i, v, n))
        if mode == 3: L(ins, 'REG_A[%d] = ea;' % reg)
    L(ins, '}')

def shift_imm(ins, sz):
    op = ins.op
    cnt = (op >> 9) & 7 or 8
    typ = (op >> 3) & 3
    left = op & 0x100
    r = op & 7
    bits = SZ[sz]; m = MASK[sz]
    typ0 = (ins.op >> 3) & 3
    # Musashi (the reference core): on 020 only rotates add count cycles; as/ls add none
    ins.extra = (cnt if typ0 >= 2 else 0) if CPU020 else 2 * cnt
    L(ins, '{ uint32_t src = REG_D[%d] & %s;' % (r, m))
    def store(res): L(ins, (EA(ins, 0, r, sz, 'd').write(res)))
    if typ == 1:  # ls
        if left:
            L(ins, 'uint32_t res = (src << %d) & %s;' % (cnt, m)); store('res')
            c = {'b': 'src << %d' % cnt, 'w': 'src >> %d' % (8 - cnt), 'l': 'src >> %d' % (24 - cnt)}[sz]
            if sz == 'l' and cnt > 24: c = 'src << %d' % (cnt - 24)
            F(ins, 'N', nflag(sz, 'res')); F(ins, 'Z', 'res'); F(ins, 'X', c); F(ins, 'C', c); F(ins, 'V', '0')
        else:
            L(ins, 'uint32_t res = src >> %d;' % cnt); store('res')
            c = 'src << %d' % (9 - cnt) if cnt <= 9 else 'src >> %d' % (cnt - 9)
            F(ins, 'N', '0'); F(ins, 'Z', 'res'); F(ins, 'X', c); F(ins, 'C', c); F(ins, 'V', '0')
    elif typ == 0:  # as
        if left:
            L(ins, 'uint32_t res = (src << %d) & %s;' % (cnt, m)); store('res')
            c = {'b': 'src << %d' % cnt, 'w': 'src >> %d' % (8 - cnt), 'l': 'src >> %d' % (24 - cnt)}[sz]
            if sz == 'l' and cnt > 24: c = 'src << %d' % (cnt - 24)
            # V: set if any of the top cnt+1 bits differ
            tm = ((1 << (cnt + 1)) - 1) << (bits - cnt - 1) if cnt < bits else (1 << bits) - 1
            tm &= (1 << bits) - 1
            F(ins, 'N', nflag(sz, 'res')); F(ins, 'Z', 'res'); F(ins, 'X', c); F(ins, 'C', c)
            if cnt >= bits: F(ins, 'V', '(src ? 0x80 : 0)')
            else: F(ins, 'V', '(((src & %s) == 0 || (src & %s) == %s) ? 0 : 0x80)' % (h(tm), h(tm), h(tm)))
        else:
            sign = 1 << (bits - 1)
            fill = (((1 << cnt) - 1) << (bits - cnt)) & ((1 << bits) - 1)
            L(ins, 'uint32_t res = (src >> %d) | ((src & %s) ? %s : 0u);' % (cnt, h(sign), h(fill))); store('res')
            c = 'src << %d' % (9 - cnt) if cnt <= 9 else 'src >> %d' % (cnt - 9)
            F(ins, 'N', nflag(sz, 'res')); F(ins, 'Z', 'res'); F(ins, 'X', c); F(ins, 'C', c); F(ins, 'V', '0')
    elif typ == 3:  # ro
        s = cnt % bits
        if left:
            L(ins, 'uint32_t res = ((src << %d) | (src >> %d)) & %s;' % (s, bits - s, m) if s else 'uint32_t res = src;')
            c = '(src >> %d) << 8 & 0x100' % (bits - cnt) if cnt <= bits else '0'
        else:
            L(ins, 'uint32_t res = ((src >> %d) | (src << %d)) & %s;' % (s, bits - s, m) if s else 'uint32_t res = src;')
            c = '((src >> %d) & 1) << 8' % (cnt - 1)
        store('res')
        F(ins, 'N', nflag(sz, 'res')); F(ins, 'Z', 'res'); F(ins, 'C', c); F(ins, 'V', '0')
    else:
        raise Unsupported('roxl')
    L(ins, '}')

# ---------------------------------------------------------------- classification of fallbacks
def classify_fallback(op):
    if (op & 0xf000) == 0x6000: return 'flow'
    if (op & 0xf0f8) == 0x50c8: return 'flow'
    if (op & 0xffc0) in (0x4ec0, 0x4e80): return 'flow'
    if op in (0x4e70, 0x4e72, 0x4e73, 0x4e74, 0x4e75, 0x4e76, 0x4e77): return 'flow'
    if (op & 0xfff8) == 0x50f8 and (op & 7) >= 2: return 'flow'
    if (op & 0xfff0) == 0x4e40: return 'flow'
    if (op & 0xffc0) == 0x46c0 or op in (0x027c, 0x007c, 0x0a7c): return 'flow'
    return 'seq'

def is_check_op(op): return classify_fallback(op) == 'flow'

def decode(pc):
    ins = Ins(pc)
    if not mdis.valid(ins.op):
        ins.kind = 'stop'; ins.native = False; return ins
    try:
        translate(ins)
        if ins.pos - ins.pc != ins.length:
            raise Unsupported('length mismatch %d vs %d' % (ins.pos - ins.pc, ins.length))
        ins.native = True
        if not ins.fset:
            ins.fset = frozenset(l[1] for l in ins.lines if isinstance(l, tuple))
    except Unsupported as e:
        ins.native = False
        ins.lines = []
        ins.kind = 'fallback'
        ins.why = str(e)
        ins.target = None
        # control flow of the fallback for tracing
        op = ins.op
        if op in (0x4e73, 0x4e74, 0x4e75, 0x4e77): ins.fkind = 'ret'
        elif (op & 0xffc0) == 0x4ec0: ins.fkind = 'jmpind'
        elif (op & 0xffc0) == 0x4e80: ins.fkind = 'callind'
        elif (op & 0xf000) == 0x6000 or (op & 0xf0f8) == 0x50c8: ins.fkind = 'jmpind'
        else: ins.fkind = 'seq'
    return ins

# ---------------------------------------------------------------- trace
def trace(seeds, limit):
    insts = {}
    starts = set()
    work = list(seeds)
    for s in seeds: starts.add(s)
    while work:
        pc = work.pop()
        while True:
            if pc in insts or pc >= limit or pc & 1: break
            ins = decode(pc)
            insts[pc] = ins
            nxt = pc + ins.length
            k = ins.kind
            if k == 'stop': break
            if k == 'fallback':
                fk = ins.fkind
                if fk == 'ret' or fk == 'jmpind': break
                if is_check_op(ins.op): starts.add(nxt)
                pc = nxt; continue
            if k in ('branch',):
                starts.add(ins.target); work.append(ins.target); break
            if k in ('cond', 'dbcc', 'call'):
                starts.add(ins.target); work.append(ins.target); starts.add(nxt); pc = nxt; continue
            if k == 'callind': starts.add(nxt); pc = nxt; continue
            if k in ('ret', 'jmpind'): break
            pc = nxt
    return insts, starts

# ---------------------------------------------------------------- flag DCE
def flag_dce(seq):
    """seq: list of Ins in straight-line order (fallthrough). removes dead flag stores"""
    live = set(ALLF)
    for ins in reversed(seq):
        if not ins.native or ins.kind != 'seq':
            live = set(ALLF)
            if ins.native:
                live |= ins.fuse
            continue
        new = []
        for l in reversed(ins.lines):
            if isinstance(l, tuple):
                if l[1] in live: new.append(l[2])
            else: new.append(l)
        new.reverse()
        ins.out = new
        wrote = set(l[1] for l in ins.lines if isinstance(l, tuple))
        live = (live - wrote) | set(ins.fuse)

# ---------------------------------------------------------------- emit
def chunk_of(a): return a >> CHUNK_SHIFT

def emit(insts, starts, outdir):
    os.makedirs(outdir, exist_ok=True)
    addrs = sorted(insts)
    for a in addrs:   # sequential flow crossing a chunk boundary needs an entry label
        n = a + insts[a].length
        if n in insts and chunk_of(n) != chunk_of(a): starts.add(n)
    chunks = {}
    for a in addrs: chunks.setdefault(chunk_of(a), []).append(a)
    # straight-line sequences for DCE
    seq = []
    for i, a in enumerate(addrs):
        ins = insts[a]
        if a in starts and seq: flag_dce(seq); seq = []
        seq.append(ins)
        nxt = a + ins.length
        if ins.kind not in ('seq',) or nxt not in insts:
            flag_dce(seq); seq = []
    if seq: flag_dce(seq)
    stats = {'native': 0, 'fallback': 0}
    why = {}
    files = []
    for c, alist in sorted(chunks.items()):
        out = []
        fn = 'rc_chunk_%04x' % c
        out.append('uint32_t %s(uint32_t pc) {' % fn)
        out.append('  switch (pc) {')
        for a in alist:
            if a in starts: out.append('  case 0x%06x: goto L_%06x;' % (a, a))
        out.append('  default: return rc_interp_fallback(pc, 1);')
        out.append('  }')
        prev_falls = False
        prev_next = None
        for a in alist:
            ins = insts[a]
            if prev_falls and prev_next != a:
                out.append('  ' + jump(prev_next, c, insts, starts, False))
            if a in starts: out.append('L_%06x:' % a)
            elif prev_next != a:
                pass  # unreachable label-less code; still emitted (overlap)
            out.append('  /* %06x: %s */' % (a, ins.text))
            if TRACE: out.append('  rc_trace(0x%06x);' % a)
            nxt = a + ins.length
            cyc = CYC[ins.op] + getattr(ins, 'extra', 0) + getattr(ins, 'eacyc', 0)
            falls = True
            if not ins.native:
                stats['fallback'] += 1
                why[getattr(ins, 'why', 'invalid')] = why.get(getattr(ins, 'why', 'invalid'), 0) + 1
                if ins.kind == 'stop' and not mdis.valid(ins.op):
                    out.append('  return rc_interp_fallback(0x%06x, 0);' % a); falls = False
                elif is_check_op(ins.op) or ins.fkind != 'seq':
                    out.append('  return rc_interp_fallback(0x%06x, 0);' % a); falls = False
                else:
                    out.append('  { uint32_t r = rc_interp_fallback(0x%06x, 0); if (r != 0x%06xu) return r; }' % (a, nxt))
            else:
                stats['native'] += 1
                body = getattr(ins, 'out', None)
                if body is None: body = [l[2] if isinstance(l, tuple) else l for l in ins.lines]
                k = ins.kind
                if k == 'seq':
                    out.extend('  ' + l for l in body)
                    out.append('  g_cycles += %d;' % cyc)
                elif k == 'branch':
                    out.extend('  ' + l for l in body)
                    out.append('  g_cycles += %d;' % cyc)
                    out.append('  ' + jump(ins.target, c, insts, starts, True)); falls = False
                elif k == 'call':
                    out.extend('  ' + l for l in body)
                    out.append('  g_cycles += %d;' % cyc)
                    out.append('  ' + jump(ins.target, c, insts, starts, True)); falls = False
                elif k == 'cond':
                    out.append('  g_cycles += %d;' % cyc)
                    out.append('  if (%s) { %s }' % (ins.cond, jump(ins.target, c, insts, starts, True)))
                    out.append('  g_cycles += %d;' % ins.notake)
                    out.append('  if (g_cycles >= g_next_event) return 0x%06xu | %s;' % (nxt, CHK))
                elif k == 'dbcc':
                    out.append('  g_cycles += %d;' % cyc)
                    noexp, exp = (0, 4) if CPU020 else (-2, 2)
                    out.append('  if (!%s) { uint32_t r = (REG_D[%d] - 1) & 0xffffu; REG_D[%d] = (REG_D[%d] & 0xffff0000u) | r;'
                               ' if (r != 0xffffu) { g_cycles += %d; %s } g_cycles += %d; }' % (ins.cond, ins.dreg, ins.dreg, ins.dreg, noexp, jump(ins.target, c, insts, starts, True), exp))
                    out.append('  if (g_cycles >= g_next_event) return 0x%06xu | %s;' % (nxt, CHK))
                elif k in ('ret', 'jmpind', 'callind'):
                    out.append('  {')
                    out.extend('  ' + l for l in body)
                    out.append('  g_cycles += %d;' % cyc)
                    out.append('  return (t & 0xffffffu) | %s; }' % CHK); falls = False
            prev_falls = falls
            prev_next = nxt
        if prev_falls: out.append('  ' + jump(prev_next, c, insts, starts, False))
        out.append('}')
        files.append((c, fn, out))
    # write files (group chunks into a few translation units)
    groups = {}
    for c, fn, out in files: groups.setdefault(c >> 3, []).append((c, fn, out))
    names = []
    for g, lst in sorted(groups.items()):
        name = 'rc_%s_%02x.c' % (PREFIX, g)
        names.append(name)
        with open(os.path.join(outdir, name), 'w') as f:
            f.write('/* generated by tools/recomp.py - do not edit */\n#include "rc.h"\n\n')
            for c, fn, out in lst:
                f.write('\n'.join(out)); f.write('\n\n')
    with open(os.path.join(outdir, 'rc_table.c'), 'w') as f:
        f.write('/* generated by tools/recomp.py */\n#include "rc.h"\n')
        for c, fn, out in files: f.write('uint32_t %s(uint32_t pc);\n' % fn)
        f.write('rc_chunk_fn rc_chunk_table[2048] = {\n')
        for c, fn, out in files: f.write('  [0x%x] = %s,\n' % (c, fn))
        f.write('};\n')
        f.write('const uint32_t rc_stats_native = %d, rc_stats_fallback = %d;\n' % (stats['native'], stats['fallback']))
    with open(os.path.join(outdir, 'sources.txt'), 'w') as f:
        f.write('\n'.join(names + ['rc_table.c']) + '\n')
    return stats, why

def jump(t, c, insts, starts, check):
    """C statement transferring control to t (with interrupt check point if check)"""
    pre = 'if (g_cycles >= g_next_event) return 0x%06xu | %s; ' % (t, CHK) if check else ''
    if chunk_of(t) == c and t in insts and t in starts:
        return pre + 'goto L_%06x;' % t
    return 'return 0x%06xu%s;' % (t, (' | ' + CHK) if check else '')

def main():
    global ROM, CYC, CPU020, PREFIX
    args = sys.argv[1:]
    covs = []
    prog, outdir = args[0], args[1]
    i = 2
    while i < len(args):
        if args[i] == '--prefix': PREFIX = args[i + 1]; i += 2
        elif args[i] == '--cpu': CPU020 = args[i + 1].lower() != '68000'; i += 2
        else: covs.append(args[i]); i += 1
    ROM = open(prog, 'rb').read()
    here = os.path.dirname(os.path.abspath(__file__))
    CYC = open(os.path.join(here, 'cycles020.bin' if CPU020 else 'cycles68k.bin'), 'rb').read()
    mdis.load(ROM, 2 if CPU020 else 0)
    seeds = set()
    for v in range(1, 64):
        a = r32(v * 4)
        if 0 < a < len(ROM) and not a & 1: seeds.add(a)
    for cov in covs:
        if os.path.exists(cov):
            for line in open(cov):
                line = line.strip()
                if line:
                    a = int(line, 16)
                    if a < len(ROM): seeds.add(a)
    insts, starts = trace(seeds, len(ROM))
    stats, why = emit(insts, starts, outdir)
    print('instructions: %d (native %d, fallback %d), block starts %d' % (len(insts), stats['native'], stats['fallback'], len(starts)))
    for k, v in sorted(why.items(), key=lambda x: -x[1])[:25]: print('  fallback %-28s %d' % (k, v))

if __name__ == '__main__':
    main()
