#!/usr/bin/env python3
"""
Small two-pass assembler for the DSP56300 subset the MCPX GP programs of this
library use. Encodings follow the instruction formats of Motorola's public
DSP56300 Family Manual; every program is also decoded with xemu's public
interpreter before it runs on a console (see docs/GP_REVERB_PLAN.md).

Usage: dsp56asm.py source.asm -o out.h [--name NAME] [--hex out.hex] [--list out.lst] [--kernel | --image]

Source syntax (one statement per line, ';' starts a comment):
    NAME    equ  expr
            org  p:expr                 ; P memory only
    label:  instruction                 ; the colon is optional in column 0
            dc   expr[,expr...]

Instructions:
    nop  enddo  rts  inc D  dec D                    (D = a | b)
    jmp target   jCC target   jsr target            (CC: cc hs ge ne pl nn ec lc gt cs lo lt eq mi nr es ls le)
    jclr/jset #n,x|y:addr,target                     addr: $FFFFC0..$FFFFFF (pp) or < $40 (aa)
    bset/bclr/bchg #n,x|y:addr | #n,reg
    movep #imm,x|y:pp     movep x|y:pp,reg     movep reg,x|y:pp
    movep x|y:pp,x|y:$abs     movep x|y:$abs,x|y:pp
    do #n,end | do x|y:aa,end | do reg,end           (end = address after the loop's last word)
    rep #n | rep x|y:aa | rep reg
    ori/andi #n,mr|ccr|omr
    add/sub/cmp #imm,D       (6-bit or long; the value lands in D1, as an integer)
    move  <moves>
    ALU   operands  [<moves>]                         (ALU ops below; at most one memory move,
                                                       or one X and one Y move with (Rn) modes)
Moves:
    #imm,reg (always the long 24-bit form)   reg,reg   x|y:ea,reg   reg,x|y:ea
    ea: $abs (< $40 short, else long absolute) | (rn) | (rn)+ | (rn)- | (rn)+n | (rn)-n | (rn+nn) | -(rn)
ALU ops (parallel): clr tst rnd neg abs asl asr not  (D)
    tfr add sub cmp cmpm  (S,D)     or and eor  (x0|x1|y0|y1,D)
    mpy mpyr mac macr  ([-]S1,S2,D)
"""
import argparse
import re
import sys

# ---------------------------------------------------------------- registers

REG5 = {'x0': 0x04, 'x1': 0x05, 'y0': 0x06, 'y1': 0x07, 'a0': 0x08, 'b0': 0x09, 'a2': 0x0A, 'b2': 0x0B,
        'a1': 0x0C, 'b1': 0x0D, 'a': 0x0E, 'b': 0x0F}
for _i in range(8):
    REG5['r%d' % _i] = 0x10 + _i
    REG5['n%d' % _i] = 0x18 + _i
REG6 = dict(REG5)
for _i in range(8):
    REG6['m%d' % _i] = 0x20 + _i
REG6.update({'ep': 0x2A, 'vba': 0x30, 'sc': 0x31, 'sz': 0x38, 'sr': 0x39, 'omr': 0x3A, 'sp': 0x3B,
             'ssh': 0x3C, 'ssl': 0x3D, 'la': 0x3E, 'lc': 0x3F})

CC = {'cc': 0, 'hs': 0, 'ge': 1, 'ne': 2, 'pl': 3, 'nn': 4, 'ec': 5, 'lc': 6, 'gt': 7,
      'cs': 8, 'lo': 8, 'lt': 9, 'eq': 10, 'mi': 11, 'nr': 12, 'es': 13, 'ls': 14, 'le': 15}

# Multiplier operand pairs (QQQ), either order accepted
QQQ = {('x0', 'x0'): 0, ('y0', 'y0'): 1, ('x1', 'x0'): 2, ('y1', 'y0'): 3,
       ('x0', 'y1'): 4, ('y0', 'x0'): 5, ('x1', 'y0'): 6, ('y1', 'x1'): 7}
MUL_OP = {'mpy': 0, 'mpyr': 1, 'mac': 2, 'macr': 3}
# Source field JJJ of the add/sub/tfr/cmp/... rows 0x40-0x7F
JJ = {'x0': 0, 'y0': 1, 'x1': 2, 'y1': 3}
ROW = {'add': 0, 'tfr': 1, 'or': 2, 'eor': 3, 'sub': 4, 'cmp': 5, 'and': 6, 'cmpm': 7}


class AsmError(Exception):
    pass


# ---------------------------------------------------------------- expressions

# Relocatable assembly (ABI 2 kernel images, docs/GP_KERNELS.md): the space
# ('p', 'x' or 'y') each relocatable symbol moves with. None: absolute assembly.
_REL = None
_REL_SHIFTS = (0x2A1, 0x553)     # trial base moves that tell a relocatable expression from a constant


def evaluate(expr, symbols, pass_no, shift=None):
    """shift: {space: n} adds n to every relocatable symbol of that space (reloc_kind)."""
    e = expr.strip()
    if not e:
        raise AsmError('empty expression')
    e = re.sub(r'\$([0-9a-fA-F]+)', lambda m: str(int(m.group(1), 16)), e)
    e = re.sub(r'%([01]+)', lambda m: str(int(m.group(1), 2)), e)

    def sym(m):
        name = m.group(0)
        if name.lower().startswith('0x'):
            return name
        if name in symbols:
            v = symbols[name]
            if shift and _REL:
                v += shift.get(_REL.get(name), 0)
            return str(v)
        if pass_no == 1:
            return '0'
        raise AsmError('undefined symbol %s' % name)
    e = re.sub(r'[A-Za-z_][A-Za-z0-9_]*', sym, e)
    if not re.fullmatch(r'[0-9xXa-fA-F\s+\-*/()<>&|~^]*', e):
        raise AsmError('bad expression %r' % expr)
    e = e.replace('/', '//')
    return int(eval(e, {'__builtins__': {}}, {}))


def reloc_kind(expr, symbols, pass_no):
    """The space an expression moves with: None (a constant), 'p', 'x' or 'y'.
    reloc +- constant is relocatable, reloc - reloc of one space a constant;
    anything else that involves a relocatable symbol is an error."""
    if not _REL:
        return None
    v0 = evaluate(expr, symbols, pass_no)
    kind = None
    for space in 'pxy':
        moves = [evaluate(expr, symbols, pass_no, {space: d}) - v0 for d in _REL_SHIFTS]
        if moves == [0, 0]:
            continue
        if moves != list(_REL_SHIFTS) or kind:
            raise AsmError('%r is not relocatable (only reloc +- constant, or reloc - reloc of one space)' % expr)
        kind = space
    return kind


def is_rel(expr, symbols, pass_no):
    return reloc_kind(expr, symbols, pass_no) is not None


def no_rel(expr, symbols, pass_no, what):
    if is_rel(expr, symbols, pass_no):
        raise AsmError('%s cannot take a relocatable operand (%s)' % (what, expr))


# ---------------------------------------------------------------- operands

def split_operands(s):
    """Split on commas outside parentheses."""
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        elif ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def is_reg(s, table=REG5):
    return s.lower() in table


def parse_mem(s):
    """'x:...' / 'y:...' -> (space, rest) or None."""
    m = re.fullmatch(r'([xyXY]):(.+)', s.strip())
    if not m:
        return None
    return m.group(1).lower(), m.group(2).strip()


def parse_ea(ea, symbols, pass_no):
    """-> ('rn', MMMRRR) | ('abs', addr)"""
    e = ea.replace(' ', '').lower()
    m = re.fullmatch(r'\(r([0-7])\)', e)
    if m:
        return 'rn', (0b100 << 3) | int(m.group(1))
    m = re.fullmatch(r'\(r([0-7])\)\+', e)
    if m:
        return 'rn', (0b011 << 3) | int(m.group(1))
    m = re.fullmatch(r'\(r([0-7])\)-', e)
    if m:
        return 'rn', (0b010 << 3) | int(m.group(1))
    m = re.fullmatch(r'\(r([0-7])\)\+n([0-7])', e)
    if m:
        if m.group(1) != m.group(2):
            raise AsmError('(rn)+nn needs the same register number')
        return 'rn', (0b001 << 3) | int(m.group(1))
    m = re.fullmatch(r'\(r([0-7])\)-n([0-7])', e)
    if m:
        if m.group(1) != m.group(2):
            raise AsmError('(rn)-nn needs the same register number')
        return 'rn', (0b000 << 3) | int(m.group(1))
    m = re.fullmatch(r'\(r([0-7])\+n([0-7])\)', e)
    if m:
        if m.group(1) != m.group(2):
            raise AsmError('(rn+nn) needs the same register number')
        return 'rn', (0b101 << 3) | int(m.group(1))
    m = re.fullmatch(r'-\(r([0-7])\)', e)
    if m:
        return 'rn', (0b111 << 3) | int(m.group(1))
    if is_rel(ea, symbols, pass_no):
        return 'long', evaluate(ea, symbols, pass_no) & 0xFFFFFF   # relocatable: always the extension word
    return 'abs', evaluate(ea, symbols, pass_no) & 0xFFFFFF


# ---------------------------------------------------------------- parallel ALU byte

def alu_byte(mn, ops):
    mn = mn.lower()
    ops = [o.lower() for o in ops]

    def acc(r):
        if r not in ('a', 'b'):
            raise AsmError('%s: destination must be a or b' % mn)
        return 1 if r == 'b' else 0
    one = {'clr': 0x13, 'tst': 0x03, 'rnd': 0x11, 'neg': 0x36, 'abs': 0x26, 'asl': 0x32, 'asr': 0x22,
           'not': 0x17, 'lsl': 0x33, 'lsr': 0x23, 'rol': 0x37, 'ror': 0x27}
    if mn in one:
        if len(ops) != 1:
            raise AsmError('%s takes one operand' % mn)
        return one[mn] | (acc(ops[0]) << 3)
    if mn in MUL_OP:
        if len(ops) != 3:
            raise AsmError('%s takes S1,S2,D' % mn)
        neg = ops[0].startswith('-')
        s1 = ops[0].lstrip('+-')
        pair = (s1, ops[1])
        if pair not in QQQ:
            pair = (ops[1], s1)
            if pair not in QQQ:
                raise AsmError('%s: no multiplier pair %s,%s' % (mn, s1, ops[1]))
        return 0x80 | (QQQ[pair] << 4) | (acc(ops[2]) << 3) | (int(neg) << 2) | MUL_OP[mn]
    if mn in ROW:
        if len(ops) != 2:
            raise AsmError('%s takes S,D' % mn)
        s, d = ops
        dd = acc(d)
        if s in JJ:
            return 0x40 | (JJ[s] << 4) | (dd << 3) | ROW[mn]
        if s in ('a', 'b'):
            if s == d:
                raise AsmError('%s %s,%s is not encodable' % (mn, s, d))
            # rows 0x00-0x1F: the other accumulator as source
            table = {'tfr': 0x01, 'cmp': 0x05, 'cmpm': 0x07, 'add': 0x10, 'sub': 0x14}
            if mn not in table:
                raise AsmError('%s %s,%s is not encodable' % (mn, s, d))
            return table[mn] | (dd << 3)
        if s in ('x', 'y') and mn in ('add', 'sub'):
            return (0x20 if s == 'x' else 0x30) | (dd << 3) | (0 if mn == 'add' else 4)
        raise AsmError('%s: bad source %s' % (mn, s))
    raise AsmError('unknown ALU op %s' % mn)


ALU_OPS = {'clr', 'tst', 'rnd', 'neg', 'abs', 'asl', 'asr', 'not', 'lsl', 'lsr', 'rol', 'ror',
           'tfr', 'add', 'sub', 'cmp', 'cmpm', 'or', 'and', 'eor', 'mpy', 'mpyr', 'mac', 'macr'}


# ---------------------------------------------------------------- parallel moves

def encode_moves(moves, alu, symbols, pass_no):
    """moves: list of 'src,dst' strings. Returns list of words."""
    if not moves:
        return [0x200000 | alu]
    parsed = []
    for mv in moves:
        p = split_operands(mv)
        if len(p) != 2:
            raise AsmError('bad move %r' % mv)
        parsed.append((p[0].strip(), p[1].strip()))

    if len(parsed) == 2:
        return encode_xy(parsed, alu, symbols, pass_no)

    src, dst = parsed[0]
    # immediate -> register (long form)
    if src.startswith('#'):
        d = dst.lower()
        if d not in REG5:
            raise AsmError('bad immediate destination %s' % dst)
        r = REG5[d]
        reloc_kind(src[1:], symbols, pass_no)              # only reloc +- constant
        val = evaluate(src[1:], symbols, pass_no) & 0xFFFFFF
        return [0x400000 | ((r >> 3) << 20) | ((r & 7) << 16) | (1 << 15) | (1 << 14) | (0b110100 << 8) | alu, val]
    # register -> register
    if is_reg(src) and is_reg(dst):
        s, d = REG5[src.lower()], REG5[dst.lower()]
        return [0x200000 | (s << 13) | (d << 8) | alu]
    # memory <-> register
    ms, md = parse_mem(src), parse_mem(dst)
    if ms and is_reg(dst):
        space, ea, reg, w = ms[0], ms[1], REG5[dst.lower()], 1
    elif md and is_reg(src):
        space, ea, reg, w = md[0], md[1], REG5[src.lower()], 0
    else:
        raise AsmError('bad move %s,%s' % (src, dst))
    kind, v = parse_ea(ea, symbols, pass_no)
    base = 0x400000 | ((reg >> 3) << 20) | ((1 if space == 'y' else 0) << 19) | ((reg & 7) << 16) | (w << 15) | alu
    if kind == 'rn':
        return [base | (1 << 14) | (v << 8)]
    if kind == 'abs' and v < 0x40 and not ea.strip().startswith('>'):
        return [base | (v << 8)]
    return [base | (1 << 14) | (0b110000 << 8), v]


def encode_xy(parsed, alu, symbols, pass_no):
    """X:Y dual move: 1wmmeeff WrrMMRRR. X uses r0-r3 or r4-r7, Y the other bank."""
    xm = ym = None
    for src, dst in parsed:
        ms, md = parse_mem(src), parse_mem(dst)
        if ms:
            ent = (ms[0], ms[1], dst.lower(), 1)
        elif md:
            ent = (md[0], md[1], src.lower(), 0)
        else:
            raise AsmError('X:Y move needs memory operands')
        if ent[0] == 'x':
            xm = ent
        else:
            ym = ent
    if not xm or not ym:
        raise AsmError('X:Y move needs one x: and one y: operand')
    xr = {'x0': 0, 'x1': 1, 'a': 2, 'b': 3}
    yr = {'y0': 0, 'y1': 1, 'a': 2, 'b': 3}
    if xm[2] not in xr or ym[2] not in yr:
        raise AsmError('X:Y move: x register must be x0/x1/a/b, y register y0/y1/a/b')

    def mode(ea):
        e = ea.replace(' ', '').lower()
        m = re.fullmatch(r'\(r([0-7])\)(\+n[0-7]|\+|-)?', e)
        if not m:
            raise AsmError('X:Y move: unsupported ea %s' % ea)
        r = int(m.group(1))
        suf = m.group(2) or ''
        mm = {'': 0b00, '+n': 0b01, '-': 0b10, '+': 0b11}
        key = '+n' if suf.startswith('+n') else suf
        if key == '+n' and int(suf[2]) != r:
            raise AsmError('(rn)+nn needs the same register number')
        # MM: 00 (Rn), 01 (Rn)+Nn, 10 (Rn)-, 11 (Rn)+
        mm = {'': 0b00, '+n': 0b01, '-': 0b10, '+': 0b11}[key]
        return r, mm
    xreg, xmm = mode(xm[1])
    yreg, ymm = mode(ym[1])
    if (xreg < 4) == (yreg < 4):
        raise AsmError('X:Y move: x and y must use registers from different banks (r0-r3 / r4-r7)')
    word = 0x800000
    word |= ym[3] << 22               # w: y direction (1 = read memory)
    word |= ymm << 20                 # mm: y mode
    word |= xr[xm[2]] << 18           # ee
    word |= yr[ym[2]] << 16           # ff
    word |= xm[3] << 15               # W: x direction
    word |= (yreg & 3) << 13          # rr
    word |= xmm << 11                 # MM
    word |= xreg << 8                 # RRR (bit 2 selects the bank)
    return [word | alu]


# ---------------------------------------------------------------- non-parallel

def space_bit(sp):
    return 1 if sp == 'y' else 0


def mem_addr(op, symbols, pass_no):
    m = parse_mem(op)
    if not m:
        raise AsmError('expected x: or y: operand, got %s' % op)
    return m[0], evaluate(m[1], symbols, pass_no) & 0xFFFFFF


def imm(op, symbols, pass_no):
    if not op.startswith('#'):
        raise AsmError('expected #immediate, got %s' % op)
    return evaluate(op[1:], symbols, pass_no)


def short_addr(op, symbols, pass_no, what):
    """An x:/y: operand of a form with no extension word: never relocatable."""
    m = parse_mem(op)
    if m:
        no_rel(m[1], symbols, pass_no, what)
    return mem_addr(op, symbols, pass_no)


def bit_no(op, symbols, pass_no):
    """Bit number of jclr/jset/bset/bclr. The DSP56300 field is 5 bits, but xemu's
    templates only decode 0-15, so larger numbers are refused."""
    no_rel(op[1:], symbols, pass_no, 'a bit number')
    n = imm(op, symbols, pass_no)
    if not 0 <= n < 16:
        raise AsmError('bit number %d: only 0-15 (xemu decodes 4 bits)' % n)
    return n


def encode(mn, operands, pc, symbols, pass_no):
    mn_l = mn.lower()
    ops = split_operands(operands) if operands else []

    if mn_l == 'nop':
        return [0x000000]
    if mn_l == 'enddo':
        return [0x00008C]
    if mn_l == 'rts':
        return [0x00000C]
    if mn_l in ('inc', 'dec'):
        d = ops[0].lower()
        return [(0x000008 if mn_l == 'inc' else 0x00000A) | (1 if d == 'b' else 0)]
    if mn_l in ('jmp', 'jsr'):
        if reloc_kind(ops[0], symbols, pass_no) not in (None, 'p'):
            raise AsmError('%s: the target must be in P' % mn)
        t = evaluate(ops[0], symbols, pass_no)
        if t < 0x1000:
            return [(0x0C0000 if mn_l == 'jmp' else 0x0D0000) | t]
        return [0x0AF080 if mn_l == 'jmp' else 0x0BF080, t & 0xFFFFFF]
    if mn_l.startswith('j') and mn_l[1:] in CC:
        if reloc_kind(ops[0], symbols, pass_no) not in (None, 'p'):
            raise AsmError('%s: the target must be in P' % mn)
        t = evaluate(ops[0], symbols, pass_no)
        if t >= 0x1000:
            raise AsmError('%s target beyond 12 bits' % mn)
        return [0x0E0000 | (CC[mn_l[1:]] << 12) | t]
    if mn_l in ('jclr', 'jset'):
        n = bit_no(ops[0], symbols, pass_no)
        target = evaluate(ops[2], symbols, pass_no)
        set_bit = 0x20 if mn_l == 'jset' else 0
        if is_reg(ops[1], REG6):
            return [0x0AC000 | (REG6[ops[1].lower()] << 8) | set_bit | n, target]
        sp, a = short_addr(ops[1], symbols, pass_no, mn)
        if a >= 0xFFFFC0:
            return [0x0A8080 | ((a & 0x3F) << 8) | (space_bit(sp) << 6) | set_bit | n, target]
        if a < 0x40:
            return [0x0A0080 | (a << 8) | (space_bit(sp) << 6) | set_bit | n, target]
        raise AsmError('%s: address must be pp or < $40' % mn)
    if mn_l in ('bset', 'bclr', 'bchg'):
        n = bit_no(ops[0], symbols, pass_no)
        if is_reg(ops[1], REG6):
            low = {'bclr': 0x40, 'bset': 0x60, 'bchg': 0x40}[mn_l]
            base = 0x0BC000 if mn_l == 'bchg' else 0x0AC000
            return [base | (REG6[ops[1].lower()] << 8) | low | n]
        sp, a = short_addr(ops[1], symbols, pass_no, mn)
        base = 0x0B0000 if mn_l == 'bchg' else 0x0A0000
        bit = 0x20 if mn_l == 'bset' else 0
        if a >= 0xFFFFC0:
            return [base | 0x8000 | ((a & 0x3F) << 8) | (space_bit(sp) << 6) | bit | n]
        if a < 0x40:
            return [base | (a << 8) | (space_bit(sp) << 6) | bit | n]
        raise AsmError('%s: address must be pp or < $40' % mn)
    if mn_l == 'movep':
        src, dst = ops
        mdst, msrc = parse_mem(dst), parse_mem(src)
        # #imm -> pp
        if src.startswith('#'):
            sp, a = mem_addr(dst, symbols, pass_no)
            if a < 0xFFFFC0:
                raise AsmError('movep: destination must be a peripheral')
            reloc_kind(src[1:], symbols, pass_no)
            v = imm(src, symbols, pass_no) & 0xFFFFFF
            return [0x084080 | (space_bit(sp) << 16) | (1 << 15) | (0b110100 << 8) | (a & 0x3F), v]
        # pp <-> register:  0000100sW1dddddd00pppppp  (W=1: register -> pp)
        if is_reg(dst, REG6) and msrc:
            no_rel(msrc[1], symbols, pass_no, 'movep pp')
            sp, a = msrc[0], evaluate(msrc[1], symbols, pass_no) & 0xFFFFFF
            return [0x084000 | (space_bit(sp) << 16) | (REG6[dst.lower()] << 8) | (a & 0x3F)]
        if is_reg(src, REG6) and mdst:
            no_rel(mdst[1], symbols, pass_no, 'movep pp')
            sp, a = mdst[0], evaluate(mdst[1], symbols, pass_no) & 0xFFFFFF
            return [0x084000 | (space_bit(sp) << 16) | (1 << 15) | (REG6[src.lower()] << 8) | (a & 0x3F)]
        # pp <-> absolute memory: 0000100sW1MMMRRR1Spppppp (W=1: ea -> pp)
        sps, a_s = mem_addr(src, symbols, pass_no)
        spd, a_d = mem_addr(dst, symbols, pass_no)
        if a_d >= 0xFFFFC0 and a_s < 0xFFFFC0:
            return [0x084080 | (space_bit(spd) << 16) | (1 << 15) | (0b110000 << 8) | (space_bit(sps) << 6) |
                    (a_d & 0x3F), a_s]
        if a_s >= 0xFFFFC0 and a_d < 0xFFFFC0:
            return [0x084080 | (space_bit(sps) << 16) | (0b110000 << 8) | (space_bit(spd) << 6) | (a_s & 0x3F), a_d]
        raise AsmError('unsupported movep form')
    if mn_l in ('do', 'rep'):
        last = None
        if mn_l == 'do':
            end = evaluate(ops[1], symbols, pass_no)
            last = (end - 1) & 0xFFFFFF
        src = ops[0]
        tail = 0x00 if mn_l == 'do' else 0x20
        if src.startswith('#'):
            no_rel(src[1:], symbols, pass_no, mn)
            n = imm(src, symbols, pass_no)
            if not 0 < n < 0x1000:
                raise AsmError('%s count must be 1..4095' % mn)
            w = 0x060080 | ((n & 0xFF) << 8) | (n >> 8) | (0x00 if mn_l == 'do' else 0x20)
            return [w, last] if last is not None else [w]
        if is_reg(src, REG6):
            w = 0x06C000 | (REG6[src.lower()] << 8) | tail
            return [w, last] if last is not None else [w]
        sp, a = short_addr(src, symbols, pass_no, mn)
        if a >= 0x40:
            raise AsmError('%s: memory count must be at an address < $40' % mn)
        w = 0x060000 | (a << 8) | (space_bit(sp) << 6) | tail
        return [w, last] if last is not None else [w]
    if mn_l in ('ori', 'andi'):
        no_rel(ops[0][1:], symbols, pass_no, mn)
        n = imm(ops[0], symbols, pass_no)
        ee = {'mr': 0, 'ccr': 1, 'com': 1, 'omr': 2, 'eom': 2}[ops[1].lower()]
        return [((n & 0xFF) << 8) | (0xF8 if mn_l == 'ori' else 0xB8) | ee]
    if mn_l in ('add', 'sub', 'cmp') and ops and ops[0].startswith('#'):
        v = imm(ops[0], symbols, pass_no)
        d = 1 if ops[1].lower() == 'b' else 0
        code = {'add': 0, 'sub': 4, 'cmp': 5}[mn_l]
        if 0 <= v < 0x40 and not ops[0].startswith('#>') and not is_rel(ops[0][1:], symbols, pass_no):
            return [0x014080 | (v << 8) | (d << 3) | code]
        return [0x0140C0 | (d << 3) | code, v & 0xFFFFFF]
    if mn_l in ('move', 'movec'):
        p = split_operands(operands)
        ctrl = [o for o in p if o.lower() in REG6 and o.lower() not in REG5]
        if ctrl or mn_l == 'movec':
            return encode_movec(p, symbols, pass_no)
        return encode_moves(split_moves(operands), 0x00, symbols, pass_no)
    if mn_l in ALU_OPS:
        fields = operands.split()
        if not fields:
            raise AsmError('%s needs operands' % mn)
        alu = alu_byte(mn_l, split_operands(fields[0]))
        return encode_moves(fields[1:], alu, symbols, pass_no)
    raise AsmError('unknown instruction %s' % mn)


def encode_movec(p, symbols, pass_no):
    """MOVEC to or from a control register (m0-m7, sr, omr, la, lc, ...)."""
    if len(p) != 2:
        raise AsmError('movec takes S,D')
    src, dst = p[0].strip(), p[1].strip()

    def ctrl(r):
        c = REG6.get(r.lower())
        if c is None or c < 0x20:
            return None
        return c & 0x1F
    if src.startswith('#'):
        c = ctrl(dst)
        if c is None:
            raise AsmError('movec: %s is not a control register' % dst)
        v = imm(src, symbols, pass_no) & 0xFFFFFF
        return [0x050020 | (1 << 15) | (1 << 14) | (0b110100 << 8) | c, v]
    cs, cd = ctrl(src), ctrl(dst)
    if cs is not None and is_reg(dst, REG6):
        # control register -> register:  00000100W1eeeeee101ddddd, W=0
        return [0x0440A0 | (REG6[dst.lower()] << 8) | cs]
    if cd is not None and is_reg(src, REG6):
        return [0x0440A0 | (1 << 15) | (REG6[src.lower()] << 8) | cd]
    ms, md = parse_mem(src), parse_mem(dst)
    if ms and cd is not None:
        space, ea, c, w = ms[0], ms[1], cd, 1
    elif md and cs is not None:
        space, ea, c, w = md[0], md[1], cs, 0
    else:
        raise AsmError('unsupported movec %s,%s' % (src, dst))
    kind, v = parse_ea(ea, symbols, pass_no)
    base = 0x050020 | (w << 15) | (space_bit(space) << 6) | c
    if kind == 'rn':
        return [base | (1 << 14) | (v << 8)]
    if kind == 'abs' and v < 0x40:
        return [base | (v << 8)]
    return [base | (1 << 14) | (0b110000 << 8), v]


def split_moves(s):
    return s.split() if s else []


# ---------------------------------------------------------------- driver

def parse_line(line):
    line = line.split(';', 1)[0].rstrip()
    if not line.strip():
        return None, None, None
    label = None
    if not line[0].isspace():
        first, _, rest = line.partition(' ')
        if '\t' in first:
            first, _, r2 = first.partition('\t')
            rest = r2 + ' ' + rest
        label = first.rstrip(':')
        line = rest
    parts = line.strip().split(None, 1)
    if not parts:
        return label, None, None
    return label, parts[0], parts[1] if len(parts) > 1 else ''


def assemble(text, bases=None):
    """Two passes over the source. bases None: absolute assembly. bases {'p', 'x',
    'y'}: a relocatable kernel (ABI 2) placed at those bases: KX and KY are
    predefined as the X and Y bases, `org p:n` means P base + n, and every label
    moves with P."""
    global _REL
    lines = text.splitlines()
    symbols = {}
    exported = []
    _REL = {} if bases is not None else None
    try:
        for pass_no in (1, 2):
            pc = 0
            mem = {}
            listing = []
            if bases is not None:
                symbols['KX'], symbols['KY'] = bases['x'], bases['y']
                _REL['KX'], _REL['KY'] = 'x', 'y'
            for no, raw in enumerate(lines, 1):
                try:
                    label, mn, operands = parse_line(raw)
                    if mn and mn.lower() == 'equ':
                        if bases is not None and label in ('KX', 'KY'):
                            raise AsmError('%s is predefined' % label)
                        v = evaluate(operands, symbols, pass_no)
                        symbols[label] = v
                        if _REL is not None:
                            kind = reloc_kind(operands, symbols, pass_no)
                            if kind:
                                _REL[label] = kind
                            else:
                                _REL.pop(label, None)
                        if pass_no == 1:
                            exported.append(label)
                        continue
                    if label:
                        if pass_no == 1:
                            if label in symbols:
                                raise AsmError('duplicate label %s' % label)
                            exported.append(label)
                        symbols[label] = pc
                        if _REL is not None:
                            _REL[label] = 'p'
                    if not mn:
                        continue
                    if mn.lower() == 'org':
                        m = re.fullmatch(r'[pP]:(.+)', operands.strip())
                        if not m:
                            raise AsmError('org supports p: only')
                        if _REL is not None:
                            no_rel(m.group(1), symbols, pass_no, 'org')
                        pc = evaluate(m.group(1), symbols, pass_no) + (bases['p'] if bases is not None else 0)
                        if label:
                            symbols[label] = pc
                        continue
                    if mn.lower() == 'dc':
                        words = [evaluate(x, symbols, pass_no) & 0xFFFFFF for x in split_operands(operands)]
                        for x in split_operands(operands):
                            reloc_kind(x, symbols, pass_no)      # only reloc +- constant
                    else:
                        words = encode(mn, operands, pc, symbols, pass_no)
                    listing.append((pc, words, raw.rstrip()))
                    for w in words:
                        if pc in mem:
                            raise AsmError('P:$%04X written twice' % pc)
                        mem[pc] = w & 0xFFFFFF
                        pc += 1
                except AsmError as e:
                    raise AsmError('line %d: %s\n    %s' % (no, e, raw.strip()))
    finally:
        _REL = None
    return mem, symbols, exported, listing


IMAGE_ORIGIN = 0x100      # ABI 1: dsp/gp_loop.asm KERNEL
IMAGE_MAGIC = 0x47504B    # 'GPK' (AL_XBOX_GP_IMAGE_MAGIC)
IMAGE_ABI = 2
RELOC_KINDS = {'p': 0, 'p12': 1, 'x': 2, 'y': 3}   # reloc entry bits 13:12
ABI2_XWORDS_MIN = 0x28    # the ABI words of a slot's X block (KIN .. KPARAM + 31)


def is_relocatable_source(text):
    """An ABI 2 kernel declares K_XWORDS (docs/GP_KERNELS.md)."""
    return re.search(r'^K_XWORDS\s+equ\b', text, re.M | re.I) is not None


def _is_dc(raw):
    _, mn, _ = parse_line(raw)
    return mn is not None and mn.lower() == 'dc'


def relocations(text):
    """Assemble a relocatable kernel at base 0 and find, by moving each base,
    which code words move with which space: [(offset, kind)], plus the base-0
    assembly. Opcode words move in their 12-bit address field (a short jmp,
    jsr or jCC into P); extension words move whole."""
    zero = {'p': 0, 'x': 0, 'y': 0}
    mem0, symbols, exported, listing = assemble(text, zero)
    if not mem0 or min(mem0) != 0:
        raise AsmError('a relocatable kernel starts at org p:0')
    first = {pc for pc, _, raw in listing if not _is_dc(raw)}
    found = {}
    for space in 'pxy':
        for d in _REL_SHIFTS:
            mem = assemble(text, dict(zero, **{space: d}))[0]
            if len(mem) != len(mem0):
                raise AsmError('the code size depends on the %s base' % space.upper())
            for i in mem0:
                w0, w = mem0[i], mem[(i + d) if space == 'p' else i]
                if w == w0:
                    continue
                if i in first:
                    ok = space == 'p' and (w ^ w0) & ~0xFFF == 0 and (w & 0xFFF) - (w0 & 0xFFF) == d
                    kind = 'p12'
                else:
                    ok = (w - w0) & 0xFFFFFF == d
                    kind = space
                if not ok or found.get(i, kind) != kind:
                    raise AsmError('P:$%04X: a word that does not relocate cleanly' % i)
                found[i] = kind
    return sorted((i, RELOC_KINDS[k]) for i, k in found.items()), mem0, symbols, exported, listing


def relocate(code, relocs, pbase, xbase, ybase):
    """What the library's loader does (apu_gp.c): add each base to its words."""
    out = list(code)
    for off, kind in relocs:
        w = out[off]
        if kind == 0:
            w = (w + pbase) & 0xFFFFFF
        elif kind == 1:
            w = (w & ~0xFFF) | ((w + pbase) & 0xFFF)
        elif kind == 2:
            w = (w + xbase) & 0xFFFFFF
        else:
            w = (w + ybase) & 0xFFFFFF
        out[off] = w
    return out


def image_words(text):
    """An alXboxEffectProgram image -> (words, code, symbols). ABI 2 (the source
    declares K_XWORDS): magic, 2, scratch bytes, descriptors, X words, Y words,
    reloc count, the relocs, then the code assembled at P:0 with KX = KY = 0.
    ABI 1: magic, 1, scratch bytes, descriptors, then the code for P:$100."""
    if is_relocatable_source(text):
        relocs, mem, symbols, _, _ = relocations(text)
        missing = [k for k in ('K_SCRATCH_BYTES', 'K_DESCRIPTORS', 'K_XWORDS', 'K_YWORDS') if k not in symbols]
        if missing:
            raise AsmError('an ABI 2 kernel defines K_SCRATCH_BYTES, K_DESCRIPTORS, K_XWORDS and K_YWORDS')
        if symbols['K_XWORDS'] < ABI2_XWORDS_MIN:
            raise AsmError('K_XWORDS counts the ABI words too: at least $%X' % ABI2_XWORDS_MIN)
        code = [mem.get(i, 0) for i in range(max(mem) + 1)]
        if len(code) > 0x1000:
            raise AsmError('code beyond the 12-bit reloc offsets')
        header = [IMAGE_MAGIC, 2, symbols['K_SCRATCH_BYTES'] & 0xFFFFFF, symbols['K_DESCRIPTORS'] & 0xFFFFFF,
                  symbols['K_XWORDS'] & 0xFFFFFF, symbols['K_YWORDS'] & 0xFFFFFF, len(relocs)]
        return header + [off | (kind << 12) for off, kind in relocs] + code, code, symbols
    mem, symbols, _, _ = assemble(text)
    missing = [k for k in ('K_SCRATCH_BYTES', 'K_DESCRIPTORS') if k not in symbols]
    if not mem or min(mem) != IMAGE_ORIGIN or missing:
        raise AsmError('a custom kernel starts at P:$%X and defines K_SCRATCH_BYTES and K_DESCRIPTORS '
                       '(or is relocatable: K_XWORDS, org p:0)' % IMAGE_ORIGIN)
    code = [mem.get(IMAGE_ORIGIN + i, 0) for i in range(max(mem) + 1 - IMAGE_ORIGIN)]
    header = [IMAGE_MAGIC, 1, symbols['K_SCRATCH_BYTES'] & 0xFFFFFF, symbols['K_DESCRIPTORS'] & 0xFFFFFF]
    return header + code, code, symbols


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('source')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--name', default=None)
    ap.add_argument('--hex')
    ap.add_argument('--list')
    ap.add_argument('--kernel', action='store_true', help='the image starts at its first org (NAME_ORIGIN)')
    ap.add_argument('--image', action='store_true',
                    help='a kernel image for alXboxEffectProgram (docs/GP_KERNELS.md): relocatable (ABI 2) if '
                         'the source defines K_XWORDS, else ABI 1 at P:$100; emits NAME_image[] (header + code)')
    args = ap.parse_args()
    with open(args.source, encoding='utf-8') as f:
        text = f.read()
    rel = args.image and is_relocatable_source(text)
    try:
        if rel:
            _, mem, symbols, exported, listing = relocations(text)
        else:
            mem, symbols, exported, listing = assemble(text)
        image = image_words(text)[0] if args.image else None
    except AsmError as e:
        print('%s: %s' % (args.source, e), file=sys.stderr)
        return 1
    if not mem:
        print('no code', file=sys.stderr)
        return 1
    origin = min(mem)
    if not args.image and origin != 0 and not args.kernel:
        print('program must start at P:0 (a kernel image takes --kernel)', file=sys.stderr)
        return 1
    size = max(mem) + 1 - origin
    words = [mem.get(origin + i, 0) for i in range(size)]
    name = args.name or re.sub(r'\W', '_', args.source.replace('\\', '/').split('/')[-1].rsplit('.', 1)[0])
    up = name.upper()
    src_name = args.source.replace('\\', '/').split('/')[-1]
    out = ['/* Generated by lib/openal/tools/dsp56asm.py from %s; do not edit. */' % src_name,
           '#ifndef %s_H' % up, '#define %s_H' % up, '', '#include <stdint.h>', '',
           '#define %s_WORDS %du' % (up, size)]
    if origin:
        out.append('#define %s_ORIGIN 0x%Xu' % (up, origin))
    if rel:
        out.append('/* relocatable (ABI 2): symbols in X, Y and P are offsets from the slot\'s bases */')
    seen = {}
    for s in exported:
        if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', s):
            if seen.setdefault(s.upper(), s) != s:
                print('%s: %s and %s are the same in the header' % (args.source, seen[s.upper()], s), file=sys.stderr)
                return 1
            out.append('#define %s_%s 0x%Xu' % (up, s.upper(), symbols[s] & 0xFFFFFF))
    if args.image:
        words = image
        out.append('#define %s_IMAGE_WORDS %du' % (up, len(words)))
        out += ['', 'static const uint32_t %s_image[%s_IMAGE_WORDS] = {' % (name, up)]
    else:
        out += ['', 'static const uint32_t %s_words[%s_WORDS] = {' % (name, up)]
    for i in range(0, len(words), 8):
        out.append('    ' + ' '.join('0x%06Xu,' % w for w in words[i:i + 8]))
    out += ['};', '', '#endif', '']
    with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    if args.hex:
        with open(args.hex, 'w', newline='\n') as f:
            f.write('\n'.join('%06X' % w for w in words) + '\n')
    if args.list:
        with open(args.list, 'w', newline='\n') as f:
            for pc, ws, raw in listing:
                f.write('P:%04X  %-16s %s\n' % (pc, ' '.join('%06X' % w for w in ws), raw))
    return 0


if __name__ == '__main__':
    sys.exit(main())
