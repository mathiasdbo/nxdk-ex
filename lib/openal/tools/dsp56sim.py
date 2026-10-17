#!/usr/bin/env python3
"""
Source-level simulator for the DSP56300 subset the MCPX GP programs of this
library use (the instructions tools/dsp56asm.py assembles), with a model of
what those programs touch on the GP: the frame handshake ($FFFFC4/$FFFFC5),
the DMA engine ($FFFFD4/$FFFFD6: descriptor chains to circular scratch and the
output FIFOs) and the mixbins at X:$1400. Clean-room: arithmetic from
Motorola's public DSP56300 Family Manual, the DMA and frame model from this
library's own console probes (docs/GP_REVERB_PLAN.md, XEMU_VERIFICATION.md 8.1b).

It runs the assembled source line by line (not the encoded words), so it checks
a program's logic and fixed-point arithmetic on the host; encodings are checked
by the assembler and on hardware.

    sim = Gp(program_text)          # or Gp(text, extra=[(origin_text), ...])
    sim.x[addr] = value             # the CPU's X-window writes
    out = sim.frame(bins)           # bins: {n: [32 ints]}; returns FIFO 0 samples of the frame
"""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dsp56asm  # noqa: E402

MASK24 = 0xFFFFFF
MASK56 = (1 << 56) - 1


def sx24(v):
    v &= MASK24
    return v - (1 << 24) if v & 0x800000 else v


def sx56(v):
    v &= MASK56
    return v - (1 << 56) if v & (1 << 55) else v


class SimError(Exception):
    pass


class Gp:
    PERIPH_FRAME = 0xFFFFC4
    PERIPH_STATUS = 0xFFFFC5
    PERIPH_NEXT = 0xFFFFD4
    PERIPH_CTL = 0xFFFFD6

    def __init__(self, text, extra=(), max_steps=200_000):
        self.code = {}          # pc -> (mnemonic, operands, size, line)
        self.symbols = {}
        for src in (text,) + tuple(extra):
            mem, symbols, _, listing = dsp56asm.assemble(src)
            for pc, words, raw in listing:
                _, mn, ops = dsp56asm.parse_line(raw)
                if mn is None or mn.lower() == 'dc':
                    continue
                if pc in self.code:
                    raise SimError('P:$%04X defined twice' % pc)
                self.code[pc] = (mn.lower(), ops or '', len(words), raw.strip(), symbols)
            self.symbols.update(symbols)
        self.x = [0] * 0x2000
        self.y = [0] * 0x800
        self.scratch = {}       # byte address -> 24-bit word (format 2/6 both keep 24 bits)
        self.fifo = [[] for _ in range(4)]
        self.reg = {n: 0 for n in ('x0', 'x1', 'y0', 'y1', 'a', 'b')}
        self.r = [0] * 8
        self.n = [0] * 8
        self.status = 0         # $FFFFC5: bit 1 START_FRAME, bit 7 end of list
        self.next_block = 0
        self.pc = 0
        self.stack = []
        self.loops = []         # [start, last, count]
        self.flags = (False, False, False)   # N, Z, V of the last ALU result
        self.max_steps = max_steps
        self.steps = 0
        self.frame_steps = 0
        self.waiting = False    # parked on the START_FRAME poll

    # ---------------------------------------------------------- memory
    def xread(self, a):
        a &= MASK24
        if a >= 0xFFFFC0:
            return self.periph_read(a)
        if a >= 0x2000:
            raise SimError('X:$%06X out of range' % a)
        return self.x[a]

    def xwrite(self, a, v):
        a &= MASK24
        if a >= 0xFFFFC0:
            return self.periph_write(a, v & MASK24)
        if a >= 0x2000:
            raise SimError('X:$%06X out of range' % a)
        self.x[a] = v & MASK24

    def mem_read(self, sp, a):
        if sp == 'x':
            return self.xread(a)
        if (a & MASK24) >= 0x800:
            raise SimError('Y:$%06X out of range' % a)
        return self.y[a & MASK24]

    def mem_write(self, sp, a, v):
        if sp == 'x':
            return self.xwrite(a, v)
        if (a & MASK24) >= 0x800:
            raise SimError('Y:$%06X out of range' % a)
        self.y[a & MASK24] = v & MASK24

    def periph_read(self, a):
        if a == self.PERIPH_STATUS:
            return self.status
        if a == self.PERIPH_NEXT:
            return self.next_block
        if a == self.PERIPH_CTL:
            return 0
        return 0

    def periph_write(self, a, v):
        if a == self.PERIPH_FRAME:
            pass
        elif a == self.PERIPH_STATUS:
            self.status &= ~(v & 0x82)      # write 1 to clear START_FRAME / end of list
        elif a == self.PERIPH_NEXT:
            self.next_block = v
        elif a == self.PERIPH_CTL:
            if v & 1:
                self.run_dma(self.next_block)
                self.status |= 0x80

    # ---------------------------------------------------------- DMA
    def run_dma(self, d):
        for _ in range(512):
            nxt, ctl, cnt, dsp, off, base, size1 = (self.x[d + i] for i in range(7))
            to_mem = bool(ctl & 2)
            buf = (ctl >> 5) & 0xF
            stride = (ctl >> 14) & 0x3FF
            if ctl & 1:                                 # interleaved: frames, channels
                frames, chans = cnt >> 4, (cnt & 0xF) + 1
                items = [(dsp + c * stride + i) for i in range(frames) for c in range(chans)]
            else:
                step = stride if stride else 0
                items = [dsp + i * step for i in range(cnt)]
            if buf <= 3:                                # output FIFO: 16-bit samples
                if not to_mem:
                    raise SimError('FIFO read DMA not modelled')
                fmt = (ctl >> 10) & 7
                if fmt != 1:
                    raise SimError('FIFO format %d not modelled' % fmt)
                for a in items:
                    self.fifo[buf].append(sx24(self.x[a]) >> 8)
            elif buf == 0xE:                            # circular scratch, byte offsets
                size = size1 + 1
                for i, a in enumerate(items):
                    addr = base + (off + 4 * i) % size
                    if to_mem:
                        self.scratch[addr] = self.x[a]
                    else:
                        self.x[a] = self.scratch.get(addr, 0)
                if ctl & 0x10:
                    self.x[d + 4] = (off + 4 * len(items)) % size
            else:
                raise SimError('DMA buffer %X not modelled' % buf)
            if nxt & 0x4000:
                return
            d = nxt & 0x3FFF
        raise SimError('DMA chain does not end')

    # ---------------------------------------------------------- registers
    def get(self, name):
        """Register as a 24-bit word (accumulators through the limiter)."""
        if name in ('x0', 'x1', 'y0', 'y1'):
            return self.reg[name]
        if name in ('a', 'b'):
            v = self.reg[name]
            if v >= (1 << 47):
                return 0x7FFFFF
            if v < -(1 << 47):
                return 0x800000
            return (v >> 24) & MASK24
        if name in ('a1', 'b1'):
            return (self.reg[name[0]] >> 24) & MASK24
        if name in ('a0', 'b0'):
            return self.reg[name[0]] & MASK24
        if name in ('a2', 'b2'):
            return (self.reg[name[0]] >> 48) & 0xFF
        m = re.fullmatch(r'([rn])([0-7])', name)
        if m:
            return (self.r if m.group(1) == 'r' else self.n)[int(m.group(2))]
        raise SimError('register %s not modelled' % name)

    def put(self, name, v):
        v &= MASK24
        if name in ('x0', 'x1', 'y0', 'y1'):
            self.reg[name] = v
        elif name in ('a', 'b'):
            self.reg[name] = sx24(v) << 24      # A1 = v, A0 = 0, A2 = sign
        elif name in ('a1', 'b1'):
            acc = self.reg[name[0]]
            self.reg[name[0]] = sx56((acc & ~(MASK24 << 24)) | (v << 24))
        elif name in ('a0', 'b0'):
            acc = self.reg[name[0]]
            self.reg[name[0]] = sx56((acc & ~MASK24) | v)
        else:
            m = re.fullmatch(r'([rn])([0-7])', name)
            if not m:
                raise SimError('register %s not modelled' % name)
            (self.r if m.group(1) == 'r' else self.n)[int(m.group(2))] = v

    def operand24(self, s):
        """ALU source operand as a signed 24-bit value placed for the accumulator (x0 etc.)."""
        return sx24(self.get(s))

    def set_acc(self, d, v):
        v = sx56(v)
        self.reg[d] = v
        self.flags = (v < 0, v == 0, False)

    # ---------------------------------------------------------- effective addresses
    def ea(self, text, symbols, writes):
        """-> (address, post-update callable)."""
        e = text.replace(' ', '').lower()
        m = re.fullmatch(r'\(r([0-7])\)', e)
        if m:
            k = int(m.group(1))
            return self.r[k], None
        m = re.fullmatch(r'\(r([0-7])\)([+-])(n[0-7])?', e)
        if m:
            k = int(m.group(1))
            inc = self.n[k] if m.group(3) else 1
            inc = inc if m.group(2) == '+' else -inc
            return self.r[k], (k, inc)
        m = re.fullmatch(r'\(r([0-7])\+n([0-7])\)', e)
        if m:
            k = int(m.group(1))
            return (self.r[k] + self.n[k]) & MASK24, None
        m = re.fullmatch(r'-\(r([0-7])\)', e)
        if m:
            k = int(m.group(1))
            self.r[k] = (self.r[k] - 1) & MASK24
            return self.r[k], None
        return dsp56asm.evaluate(text, symbols, 2) & MASK24, None

    # ---------------------------------------------------------- moves
    def plan_move(self, mv, symbols):
        """Read the source now; return a write to do after the ALU op."""
        src, dst = dsp56asm.split_operands(mv)
        upd = []
        if src.startswith('#'):
            val = dsp56asm.evaluate(src[1:].lstrip('>'), symbols, 2) & MASK24
        else:
            pm = dsp56asm.parse_mem(src)
            if pm:
                a, u = self.ea(pm[1], symbols, False)
                val = self.mem_read(pm[0], a)
                if u:
                    upd.append(u)
            else:
                val = self.get(src.lower())
        pm = dsp56asm.parse_mem(dst)
        if pm:
            a, u = self.ea(pm[1], symbols, True)
            if u:
                upd.append(u)
            return lambda: self.mem_write(pm[0], a, val), upd
        reg = dst.lower()
        return lambda: self.put(reg, val), upd

    def do_moves(self, moves, symbols, alu=None):
        writes, updates = [], []
        for mv in moves:
            w, u = self.plan_move(mv, symbols)
            writes.append(w)
            updates += u
        if alu:
            alu()
        for w in writes:
            w()
        for k, inc in updates:
            self.r[k] = (self.r[k] + inc) & MASK24

    # ---------------------------------------------------------- ALU
    def alu(self, mn, ops):
        ops = [o.lower() for o in dsp56asm.split_operands(ops)]
        if mn in ('clr', 'tst', 'neg', 'abs', 'asl', 'asr', 'not', 'rnd', 'inc', 'dec'):
            d = ops[0]
            v = self.reg[d]
            if mn == 'clr':
                v = 0
            elif mn == 'neg':
                v = -v
            elif mn == 'abs':
                v = abs(v)
            elif mn == 'asl':
                v = v << 1
            elif mn == 'asr':
                v = v >> 1
            elif mn == 'inc':
                v = v + 1
            elif mn == 'dec':
                v = v - 1
            elif mn == 'tst':
                self.flags = (v < 0, v == 0, False)
                return
            else:
                raise SimError('%s not modelled' % mn)
            self.set_acc(d, v)
            return
        if mn in ('mpy', 'mpyr', 'mac', 'macr'):
            s1, s2, d = ops
            neg = s1.startswith('-')
            p = self.operand24(s1.lstrip('-+')) * self.operand24(s2) * 2
            if neg:
                p = -p
            v = p if mn.startswith('mpy') else self.reg[d] + p
            if mn.endswith('r'):
                v = ((v + (1 << 23)) >> 24) << 24
            self.set_acc(d, v)
            return
        if mn in ('tfr', 'add', 'sub', 'cmp', 'cmpm'):
            s, d = ops
            if s.startswith('#'):
                sv = dsp56asm.evaluate(s[1:].lstrip('>'), self.symbols, 2)
                sv = sx24(sv) << 24
            elif s in ('a', 'b'):
                sv = self.reg[s]
            else:
                sv = self.operand24(s) << 24
            if mn == 'tfr':
                self.reg[d] = sx56(sv)          # tfr leaves the condition codes alone
                return
            if mn == 'add':
                self.set_acc(d, self.reg[d] + sv)
            elif mn == 'sub':
                self.set_acc(d, self.reg[d] - sv)
            else:
                r = self.reg[d] - sv if mn == 'cmp' else abs(self.reg[d]) - abs(sv)
                r = sx56(r)
                self.flags = (r < 0, r == 0, False)
            return
        raise SimError('ALU op %s not modelled' % mn)

    def cond(self, cc):
        n, z, v = self.flags
        return {
            'cc': True, 'hs': True, 'cs': False, 'lo': False,
            'ge': n == v, 'lt': n != v, 'ne': not z, 'eq': z, 'pl': not n, 'mi': n,
            'gt': (not z) and n == v, 'le': z or n != v,
        }[cc]

    def count_of(self, op, symbols):
        op = op.strip()
        if op.startswith('#'):
            return dsp56asm.evaluate(op[1:], symbols, 2) & 0xFFFF
        pm = dsp56asm.parse_mem(op)
        if pm:
            a, _ = self.ea(pm[1], symbols, False)
            return self.mem_read(pm[0], a) & 0xFFFF
        return self.get(op.lower()) & 0xFFFF

    # ---------------------------------------------------------- execution
    def step(self):
        if self.pc not in self.code:
            raise SimError('no instruction at P:$%04X' % self.pc)
        mn, ops, size, raw, symbols = self.code[self.pc]
        here = self.pc
        nxt = here + size
        jumped = False
        if mn == 'nop':
            pass
        elif mn == 'move':
            self.do_moves(dsp56asm.split_moves(ops), symbols)
        elif mn == 'movep':
            src, dst = dsp56asm.split_operands(ops)
            if src.startswith('#'):
                val = dsp56asm.evaluate(src[1:], symbols, 2) & MASK24
            else:
                pm = dsp56asm.parse_mem(src)
                val = self.mem_read(pm[0], self.ea(pm[1], symbols, False)[0]) if pm else self.get(src.lower())
            pm = dsp56asm.parse_mem(dst)
            if pm:
                self.mem_write(pm[0], self.ea(pm[1], symbols, True)[0], val)
            else:
                self.put(dst.lower(), val)
        elif mn in ('jclr', 'jset'):
            bit, mem, target = dsp56asm.split_operands(ops)
            pm = dsp56asm.parse_mem(mem)
            v = self.mem_read(pm[0], self.ea(pm[1], symbols, False)[0])
            b = (v >> dsp56asm.evaluate(bit[1:], symbols, 2)) & 1
            if (b == 0) == (mn == 'jclr'):
                t = dsp56asm.evaluate(target, symbols, 2)
                if t == here and mn == 'jclr' and pm[1].lower().startswith('$ffffc5'):
                    self.waiting = True     # the START_FRAME poll: the frame is over
                    return
                nxt, jumped = t, True
        elif mn in ('bset', 'bclr', 'bchg'):
            bit, mem = dsp56asm.split_operands(ops)
            pm = dsp56asm.parse_mem(mem)
            a = self.ea(pm[1], symbols, False)[0]
            m = 1 << dsp56asm.evaluate(bit[1:], symbols, 2)
            v = self.mem_read(pm[0], a)
            v = v | m if mn == 'bset' else (v & ~m if mn == 'bclr' else v ^ m)
            self.mem_write(pm[0], a, v)
        elif mn == 'jmp':
            nxt, jumped = dsp56asm.evaluate(ops, symbols, 2), True
        elif mn == 'jsr':
            self.stack.append(nxt)
            nxt, jumped = dsp56asm.evaluate(ops, symbols, 2), True
        elif mn == 'rts':
            if not self.stack:
                raise SimError('rts with an empty stack at P:$%04X' % here)
            nxt, jumped = self.stack.pop(), True
        elif mn.startswith('j') and mn[1:] in dsp56asm.CC:
            if self.cond(mn[1:]):
                nxt, jumped = dsp56asm.evaluate(ops, symbols, 2), True
        elif mn == 'do':
            cnt_op, end = dsp56asm.split_operands(ops)
            cnt = self.count_of(cnt_op, symbols)
            end_pc = dsp56asm.evaluate(end, symbols, 2)
            if cnt == 0:
                raise SimError('do with a count of 0 at P:$%04X' % here)
            self.loops.append([nxt, end_pc - 1, cnt])
        elif mn == 'rep':
            cnt = self.count_of(ops, symbols)
            if cnt == 0:
                raise SimError('rep with a count of 0 at P:$%04X' % here)
            body = nxt
            for _ in range(cnt):
                self.pc = body
                self.exec_one_no_loop()
            self.pc = body + self.code[body][2]
            self.after(body, self.pc, False)
            return
        elif mn == 'enddo':
            self.loops.pop()
        else:
            parts = ops.split(None, 1)
            alu_ops = parts[0] if parts else ''
            moves = dsp56asm.split_moves(parts[1]) if len(parts) > 1 else []
            self.do_moves(moves, symbols, lambda: self.alu(mn, alu_ops))
        self.pc = nxt
        self.after(here, nxt, jumped)

    def exec_one_no_loop(self):
        """Execute the instruction at pc once (a rep's body); pc stays at it."""
        here = self.pc
        mn, ops, size, raw, symbols = self.code[here]
        saved = self.loops
        self.loops = []
        self.step()
        self.loops = saved
        self.pc = here

    def after(self, here, nxt, jumped):
        self.steps += 1
        self.frame_steps += 1
        if self.frame_steps > self.max_steps:
            raise SimError('more than %d steps in one frame' % self.max_steps)
        # A hardware DO loop ends at its last address (LA); a jump or a
        # subroutine call inside it does not count as reaching LA.
        while self.loops and not jumped and here == self.loops[-1][1]:
            top = self.loops[-1]
            top[2] -= 1
            if top[2] > 0:
                self.pc = top[0]
                return
            self.loops.pop()
            self.pc = here + self.code[here][2]
            return

    def frame(self, bins=None):
        """Run one GP frame: load the mixbins, raise START_FRAME, run until the next poll."""
        for b, data in (bins or {}).items():
            for i in range(32):
                self.x[0x1400 + 32 * b + i] = data[i] & MASK24
        for f in self.fifo:
            f.clear()
        if not self.waiting:
            self.run_until_wait()       # program start: up to the first poll
        self.status |= 0x02
        self.waiting = False
        self.pc += 0                    # resume at the poll, which now falls through
        self.frame_steps = 0
        self.run_until_wait()
        return list(self.fifo[0])

    def run_until_wait(self):
        while not self.waiting:
            self.step()
