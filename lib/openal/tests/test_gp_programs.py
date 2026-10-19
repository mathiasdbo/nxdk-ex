#!/usr/bin/env python3
"""
The GP programs on the host (tasks phase8-02, phase9-04; EFX_PLAN.md 3.3-3.4,
GP_KERNELS.md): the frame loop (dsp/gp_loop.asm) with its kernels, run in
tools/dsp56sim.py. Every ABI 2 kernel runs relocated (not at P:$100 / X:$E0).

  1  copy program (no kernel): FIFO 0 carries bins 0/1
  2  reverb (dsp/gp_kfreeverb.asm) in a slot: the X words apu_reverb_layout()
     writes for that slot, then frames of a click and noise (with full-scale
     words) on bins 0-3; OUT L/R and FIFO 0 equal apu_reverb.c's reference word
     for word (presets: hall, underwater with its output lowpass)
  3  mailbox: pairs posted while a slot runs land together at the start of the
     next frame, and MB_ACK follows MB_SEQ; an empty post is acknowledged too
  4  burn: X:BURN cycles of nop run (the step count grows by that much)
  5  custom kernel (phase8-04, EFX_PLAN.md 3.6): the example echo
     (dsp/gp_kecho.asm) loaded the way apu_gp.c loads a slot; the echo lands
     exactly DELAY samples late on each ear, feedback repeats it, a DELAY posted
     through the mailbox applies from the next frame, and DELAY is clamped to 32
  6  EFX echo kernel (dsp/gp_kefx_echo.asm)
  7  EFX chorus / flanger kernel (dsp/gp_kefx_chorus.asm)
  8  legacy ABI 1 kernel at P:$100 (FLAGS bit 4), alone
  9  relocations (phase9-04): each ABI 2 image relocated by the loader's rules
     equals its source assembled at those bases; the expression rules
 10  three slots at once (phase9-04): reverb, echo and chorus on their own bin
     pairs in one frame loop; what each adds to OUT L/R equals what it adds
     running alone, word for word

Run by tests/run_tests.py with NXDK_AL_CLANG and NXDK_AL_LIB set (the
reference helper tests/gpsim_reverb_ref.c is linked against the library).
"""
import math
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OPENAL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(OPENAL, 'tools'))
import dsp56asm  # noqa: E402
import dsp56sim  # noqa: E402


def src(name):
    return open(os.path.join(OPENAL, 'dsp', name), encoding='utf-8').read()


LOOP = src('gp_loop.asm')
REVERB = src('gp_kfreeverb.asm')
ECHO = src('gp_kecho.asm')
EFX_ECHO = src('gp_kefx_echo.asm')
EFX_CHORUS = src('gp_kefx_chorus.asm')

CTL_FIFO = 1 | 2 | (1 << 10) | (32 << 14)
COPY_DESC = [0x4000, CTL_FIFO, (32 << 4) | 1, 0x1400, 0, 0, 0]   # apu_gp.c
SCRATCH0 = 0x2000                                               # the kernels' scratch starts here

# Where the tests place the kernels (not the library's first fit, on purpose: nothing at P:$100 / X:$E0)
AT_REVERB = {'p': 0x230, 'x': 0x1A0, 'y': 0}
AT_ECHO = {'p': 0x500, 'x': 0xA60, 'y': 0}
AT_CHORUS = {'p': 0x600, 'x': 0xB20, 'y': 0}
AT_KECHO = {'p': 0x140, 'x': 0x300, 'y': 0}
SCR_ECHO = SCRATCH0 + 0x2D000                                   # after the reverb's lines


def new_gp(kernels=()):
    """The loop with relocated kernels [(source, bases)]; the library's loop X words."""
    gp = dsp56sim.Gp(LOOP, extra=tuple(kernels))
    s = gp.symbols
    for i, v in enumerate(COPY_DESC):
        gp.x[i] = v
        gp.x[s['FIFO2'] + i] = s['OUTL'] if i == 3 else v
    gp.run_until_wait()
    return gp


def load_slot(gp, k, n, bases, params=(), scratch=SCRATCH0, extra=()):
    """What apu_gp.c does to start kernel n of the program (gp.ksymbols[n]) in
    slot k: X block zeroed, ABI words (input pair: bins 2+2k / 3+2k), the
    kernel's own words (`extra`, e.g. the reverb's layout), the trampoline, on."""
    s, ks = gp.symbols, gp.ksymbols[n]
    x = bases['x']
    for a in range(x, x + ks['K_XWORDS']):
        gp.x[a] = 0
    gp.x[x] = 0x1400 + 32 * (2 + 2 * k)
    gp.x[x + 1] = scratch
    gp.x[x + 2] = ks['K_SCRATCH_BYTES']
    for i, v in enumerate(params):
        gp.x[x + 8 + i] = v & 0xFFFFFF
    for a, v in extra:
        gp.x[a] = v
    gp.x[s['SLOTX'] + k] = x
    gp.patch_jmp(s['TRAMP%d' % k], bases['p'])
    gp.x[s['FLAGS']] |= 1 << k


def sx(v):
    return v - (1 << 24) if v & 0x800000 else v


def fifo_of(left, right):
    out = []
    for i in range(32):
        out += [sx(left[i]) >> 8, sx(right[i]) >> 8]
    return out


def build_ref(tmp):
    clang = os.environ.get('NXDK_AL_CLANG', 'clang')
    lib = os.environ.get('NXDK_AL_LIB')
    exe = os.path.join(tmp, 'gpsim_reverb_ref.exe')
    cmd = [clang, '-std=c99', '-Wall', '-Werror', '-pedantic', '-I', os.path.join(OPENAL, 'src'),
           '-I', os.path.join(OPENAL, 'include'), os.path.join(HERE, 'gpsim_reverb_ref.c')]
    cmd += [lib] if lib else [os.path.join(OPENAL, 'src', 'apu_reverb.c')]
    if not sys.platform.startswith('win'):
        cmd.append('-lm')
    subprocess.run(cmd + ['-o', exe], check=True)
    return exe


def run_ref(exe, preset, frames, xbase, scratch):
    out = subprocess.run([exe, str(preset), str(frames), str(xbase), str(scratch)], check=True,
                         capture_output=True, text=True).stdout
    words, fr = [], []
    for ln in out.splitlines():
        p = ln.split()
        if not p:
            continue
        if p[0] == 'W':
            words.append((int(p[1], 16), int(p[2], 16)))
        elif p[0] == 'I':
            if p[1] == '0':
                fr.append({'in': {}, 'out': {}})
            fr[-1]['in'][int(p[1])] = [int(v, 16) for v in p[2:]]
        elif p[0] == 'O':
            fr[-1]['out'][int(p[1])] = [int(v, 16) for v in p[2:]]
    return words, fr


def test_copy():
    gp = new_gp()
    left = [(0x123400 + 0x100 * i) & 0xFFFFFF for i in range(32)]
    right = [(0xF00000 - 0x2300 * i) & 0xFFFFFF for i in range(32)]
    for _ in range(3):
        fifo = gp.frame({0: left, 1: right})
        assert fifo == fifo_of(left, right), 'copy program: FIFO is not bins 0/1'
    assert gp.x[gp.symbols['CNT']] == 3, 'frame counter'
    print('[1] copy program: FIFO 0 = bins 0/1, frame counter counts [PASS]')


def test_reverb(exe, preset, frames):
    words, fr = run_ref(exe, preset, frames, AT_REVERB['x'], SCRATCH0)
    gp = new_gp([(REVERB, AT_REVERB)])
    load_slot(gp, 0, 0, AT_REVERB, extra=words)
    gp.x[gp.symbols['FLAGS']] |= 0x20                # the reverb's output lowpass
    outl, outr = gp.symbols['OUTL'], gp.symbols['OUTR']
    for f, frame in enumerate(fr):
        fifo = gp.frame(frame['in'])
        for ch, base in ((0, outl), (1, outr)):
            got = gp.x[base:base + 32]
            if got != frame['out'][ch]:
                i = next(i for i in range(32) if got[i] != frame['out'][ch][i])
                raise AssertionError('preset %d frame %d ch %d sample %d: GP %06x, reference %06x'
                                     % (preset, f, ch, i, got[i], frame['out'][ch][i]))
        assert fifo == fifo_of(frame['out'][0], frame['out'][1]), 'preset %d frame %d: FIFO' % (preset, f)
    assert gp.x[gp.symbols['TMOUT']] == 0, 'a DMA wait timed out'
    return gp.frame_steps


def test_mailbox():
    gp = new_gp()
    s = gp.symbols
    # slot 0 on with no kernel (its trampoline still at the empty one) and no chains
    gp.x[s['SLOTX']] = 0x200
    gp.x[s['FLAGS']] = 1 | 0x20
    left = [0x010000 * (i + 1) & 0xFFFFFF for i in range(32)]
    right = [0] * 32
    fifo = gp.frame({0: left, 1: right})
    assert fifo == fifo_of(left, right), 'dry path with an empty kernel'
    # post three pairs; nothing may change before the next frame starts
    pairs = [(0x300, 0x111111), (0x301, 0x222222), (0x7FF, 0x333333)]
    for i, (a, v) in enumerate(pairs):
        gp.x[s['MBOX'] + 2 * i] = a
        gp.x[s['MBOX'] + 2 * i + 1] = v
    gp.x[s['MB_N']] = len(pairs)
    gp.x[s['MB_SEQ']] = 1
    assert all(gp.x[a] == 0 for a, _ in pairs)
    gp.frame({0: left, 1: right})
    assert all(gp.x[a] == v for a, v in pairs), 'mailbox pairs not applied in one frame'
    assert gp.x[s['MB_ACK']] == 1
    # an empty post (MB_N = 0) is acknowledged without touching memory
    gp.x[s['MB_N']] = 0
    gp.x[s['MB_SEQ']] = 2
    gp.frame({0: left, 1: right})
    assert gp.x[s['MB_ACK']] == 2
    # the output lowpass coefficient arrives through the mailbox too: it dulls the next frame
    gp.x[s['MBOX']] = s['OUTLP']
    gp.x[s['MBOX'] + 1] = 0x700000
    gp.x[s['MB_N']] = 1
    gp.x[s['MB_SEQ']] = 3
    step = [0x400000] * 32
    fifo = gp.frame({0: step, 1: step})
    # both stages start from the last frame's 0x200000: 0x240000, then 0x208000 (p = 0.875)
    assert gp.x[s['MB_ACK']] == 3 and fifo[0] == 0x208000 >> 8, 'output lowpass from the mailbox: %x' % fifo[0]
    # FLAGS bit 5 off: no lowpass
    gp.x[s['FLAGS']] = 1
    fifo = gp.frame({0: step, 1: step})
    assert fifo[0] == 0x400000 >> 8, 'lowpass off: %x' % fifo[0]
    print('[3] mailbox: pairs land together at the next frame, MB_ACK follows MB_SEQ, empty posts too; '
          'the output lowpass only with FLAGS bit 5 [PASS]')
    return gp


def test_burn(gp):
    s = gp.symbols
    gp.x[s['MB_SEQ']] = gp.x[s['MB_ACK']]
    gp.frame()
    base = gp.frame_steps
    gp.x[s['BURN']] = 500
    gp.frame()
    assert gp.frame_steps == base + 500 + 1, 'burn: %d steps, wanted %d' % (gp.frame_steps, base + 501)
    print('[4] burn: X:BURN nops per frame [PASS]')


def run_echo(gp, frames, impulses):
    """impulses: {(bin, absolute sample): value}; returns the FIFO's left and right streams (16-bit)."""
    left, right = [], []
    for f in range(frames):
        bins = {b: [0] * 32 for b in range(10)}
        for (b, t), v in impulses.items():
            if t // 32 == f:
                bins[b][t % 32] = v
        fifo = gp.frame(bins)
        left += fifo[0::2]
        right += fifo[1::2]
    return left, right


def hits(stream):
    return [(t, v) for t, v in enumerate(stream) if v]


def kernel_gp(source, bases, params, k=0):
    """The loop with one relocated kernel, started in slot k (input bins 2+2k / 3+2k)."""
    gp = new_gp([(source, bases)])
    load_slot(gp, k, 0, bases, params)
    return gp


def test_echo():
    # delay 100, no feedback, wet 0.5: a 0.5 click on bin 2 at sample 5, on bin 3 at 40
    gp = kernel_gp(ECHO, AT_KECHO, [100, 0, 0x400000])
    left, right = run_echo(gp, 12, {(2, 5): 0x400000, (3, 40): 0x400000})
    assert hits(left) == [(105, 0x2000)], 'echo left: %s' % hits(left)
    assert hits(right) == [(140, 0x2000)], 'echo right: %s' % hits(right)
    # feedback 0.5: the echo repeats every DELAY samples at half the level
    gp = kernel_gp(ECHO, AT_KECHO, [100, 0x400000, 0x400000])
    s, ks = gp.symbols, gp.ksymbols[0]
    left, _ = run_echo(gp, 12, {(2, 5): 0x400000})
    assert hits(left) == [(105, 0x2000), (205, 0x1000), (305, 0x800)], 'feedback: %s' % hits(left)
    # a new delay through the mailbox at frame 12 (sample 384); a click at 400 echoes 37 later
    gp.x[s['MBOX']] = ks['KPARAM']
    gp.x[s['MBOX'] + 1] = 37
    gp.x[s['MB_N']] = 1
    gp.x[s['MB_SEQ']] = 1
    left, _ = run_echo(gp, 6, {(2, 400 - 384): 0x400000})
    tail = [(t + 384, v) for t, v in hits(left)]
    assert (437, 0x2000) in tail and all(t != 500 for t, _ in tail), 'live delay change: %s' % tail
    # below the minimum: 32 samples
    gp.x[s['MBOX'] + 1] = 3
    gp.x[s['MB_SEQ']] = 2
    left, _ = run_echo(gp, 8, {(2, 100): 0x400000})
    assert (132, 0x2000) in hits(left), 'delay clamp: %s' % hits(left)
    # in slot 3 it reads bins 8/9
    gp = kernel_gp(ECHO, AT_KECHO, [100, 0, 0x400000], k=3)
    left, right = run_echo(gp, 6, {(2, 5): 0x400000, (8, 10): 0x400000})
    assert hits(left) == [(110, 0x2000)] and hits(right) == [], 'slot 3: %s' % hits(left)
    assert gp.x[s['TMOUT']] == 0, 'a DMA wait timed out'
    print('[5] custom kernel (echo): exact delay per ear, feedback, live delay via the mailbox, clamp, '
          'slot 3 on bins 8/9; %d instructions per frame [PASS]' % gp.frame_steps)


def test_efx_echo():
    half = 0x400000

    def gp_with(params):
        return kernel_gp(EFX_ECHO, AT_ECHO, params)
    # two taps, hard spread (tap 1 left, tap 2 right), no feedback, no damping
    gp = gp_with([100, 250, 0, 0, half, 0, 0, half])
    left, right = run_echo(gp, 14, {(2, 5): half})
    assert hits(left) == [(105, 0x2000)], 'tap 1: %s' % hits(left)
    assert hits(right) == [(255, 0x2000)], 'tap 2: %s' % hits(right)
    # the right bin is summed in too
    gp = gp_with([100, 250, 0, 0, half, 0, 0, half])
    left, _ = run_echo(gp, 6, {(3, 5): half})
    assert hits(left) == [(105, 0x2000)], 'right bin: %s' % hits(left)
    # feedback 0.5: tap 2 goes back into the line; its copy shows on both taps
    gp = gp_with([100, 250, half, 0, half, 0, 0, half])
    left, right = run_echo(gp, 24, {(2, 5): half})
    assert hits(left) == [(105, 0x2000), (355, 0x1000), (605, 0x800)], 'feedback left: %s' % hits(left)
    assert hits(right) == [(255, 0x2000), (505, 0x1000), (755, 0x800)], 'feedback right: %s' % hits(right)
    # damping pole 0.5 with feedback 0.5: the first echo of each tap is undamped,
    # the fed-back copy is lowpassed, (1 - p) p^n, so the repeats get darker
    gp = gp_with([100, 250, half, half, half, 0, 0, half])
    left, right = run_echo(gp, 24, {(2, 5): half})
    h, hr = dict(hits(left)), dict(hits(right))
    assert h.get(105) == 0x2000 and hr.get(255) == 0x2000, 'damping, first echo: %s' % hits(left)[:4]
    assert h.get(355) == 0x800 and h.get(356) == 0x400 and h.get(357) == 0x200, 'damping, repeat: %s' % hits(left)[:5]
    # no feedback: damping leaves the taps alone
    gp = gp_with([100, 250, 0, half, half, 0, 0, 0])
    left, _ = run_echo(gp, 6, {(2, 5): half})
    assert hits(left) == [(105, 0x2000)], 'damping, no feedback: %s' % hits(left)
    # delays below 32 samples are clamped (a tap must lie in written blocks)
    gp = gp_with([3, 3, 0, 0, half, 0, 0, 0])
    left, _ = run_echo(gp, 6, {(2, 40): half})
    assert hits(left) == [(72, 0x2000)], 'clamp: %s' % hits(left)
    assert gp.x[gp.symbols['TMOUT']] == 0, 'a DMA wait timed out'
    print('[6] EFX echo kernel: two taps, spread, both bins summed, feedback through tap 2, damping, clamp; '
          '%d instructions per frame [PASS]' % gp.frame_steps)


def chorus_words(delay, depth, fb, rate, phase, wave):
    """The library's words (al_efx.c chorus_params): delays in samples, rate in Hz, phase in degrees."""
    q = lambda v: max(-0x800000, min(0x7FFFFF, int(round(v * 0x800000))))
    return [q(delay / 2048.0), q(depth * delay / 2048.0), q(fb), q(2.0 * rate / 48000.0),
            q(phase / 180.0), wave]


def chorus_model(words, m, n):
    """The kernel's algorithm in floating point: the left and right outputs (16-bit) for input m."""
    D, DD, fb = words[0] * 2048.0 / 0x800000, words[1] * 2048.0 / 0x800000, words[2] / 0x800000
    line, w, ph = [0.0] * 2048, 0, 0
    left, right = [], []

    def wave(raw):
        p = sx(raw) / 0x800000
        if words[5]:
            return 1.0 - 2.0 * abs(p)
        y = 4.0 * p * (1.0 - abs(p))
        return y + 0.225 * (y * abs(y) - y)

    def tap(raw):
        d = max(0.0, D + DD * wave(raw))
        i = int(d)
        f = d - i
        s0, s1 = line[(w - i) % 2048], line[(w - i - 1) % 2048]
        return s0 + f * (s1 - s0)

    for t in range(n):
        x = m[t] / 0x800000
        line[w] = x
        ph = (ph + words[3]) & 0xFFFFFF
        tl, tr = tap(ph), tap((ph + words[4]) & 0xFFFFFF)
        line[w] = x + fb * (tl + tr) / 2.0
        left.append(int(tl * 32768.0))
        right.append(int(tr * 32768.0))
        w = (w + 1) % 2048
    return left, right


def test_efx_chorus():
    half = 0x400000

    def run(words, frames, impulses):
        gp = kernel_gp(EFX_CHORUS, AT_CHORUS, words)
        gp.y = [0x123456] * 0x800          # Y RAM is not cleared by the loader: the kernel must
        return gp, run_echo(gp, frames, impulses)

    # a whole-sample delay, no LFO: both taps 100 samples late, at the input's level
    gp, (left, right) = run(chorus_words(100, 0, 0, 0, 0, 1), 6, {(2, 5): half})
    assert hits(left) == [(105, 0x4000)] and hits(right) == [(105, 0x4000)], 'delay: %s %s' % (hits(left), hits(right))
    # the right bin is summed in too
    _, (left, _) = run(chorus_words(100, 0, 0, 0, 0, 1), 6, {(3, 5): half})
    assert hits(left) == [(105, 0x4000)], 'right bin: %s' % hits(left)
    # half a sample: linear interpolation over two samples
    _, (left, _) = run(chorus_words(100.5, 0, 0, 0, 0, 1), 6, {(2, 5): half})
    assert hits(left) == [(105, 0x2000), (106, 0x2000)], 'fraction: %s' % hits(left)
    # no delay (a flanger sweeping through zero): the same sample
    _, (left, _) = run(chorus_words(0, 0, 0, 0, 0, 1), 2, {(2, 5): half})
    assert hits(left) == [(5, 0x4000)], 'zero delay: %s' % hits(left)
    # feedback 0.5 from the taps' average back into the line
    _, (left, _) = run(chorus_words(100, 0, 0.5, 0, 0, 1), 12, {(2, 5): half})
    assert hits(left) == [(105, 0x4000), (205, 0x2000), (305, 0x1000)], 'feedback: %s' % hits(left)

    # a 440 Hz tone through the moving taps, against the model, both waveforms
    n = 32 * 160
    tone = [int(0x200000 * math.sin(2 * math.pi * 440 * t / 48000)) for t in range(n)]
    for wave, phase, fb in ((0, 90, 0.25), (1, -90, -0.5)):
        words = chorus_words(480, 0.5, fb, 10, phase, wave)
        gp = kernel_gp(EFX_CHORUS, AT_CHORUS, words)
        left, right = [], []
        for f in range(n // 32):
            fifo = gp.frame({0: [0] * 32, 1: [0] * 32, 2: tone[32 * f:32 * f + 32], 3: [0] * 32})
            left += fifo[0::2]
            right += fifo[1::2]
        ml, mr = chorus_model(words, tone, n)
        err = max(max(abs(a - b) for a, b in zip(left, ml)), max(abs(a - b) for a, b in zip(right, mr)))
        assert err <= 4, 'wave %d: %d LSB from the model' % (wave, err)
        assert max(left) > 0x1800 and left != right, 'wave %d: no output, or no PHASE' % wave
    # the sine's approximation against the real one
    worst = 0.0
    for k in range(-1024, 1024):
        p = k / 1024.0
        y = 4.0 * p * (1.0 - abs(p))
        worst = max(worst, abs(y + 0.225 * (y * abs(y) - y) - math.sin(math.pi * p)))
    assert worst < 0.0011, worst
    assert gp.x[gp.symbols['TMOUT']] == 0, 'a DMA wait timed out'
    print('[7] EFX chorus / flanger kernel: delay, both bins, interpolation, zero delay, feedback, sine and '
          'triangle LFOs against the model (sine within %.4f); %d instructions per frame [PASS]'
          % (worst, gp.frame_steps))


LEGACY = """
K_SCRATCH_BYTES equ 0
K_DESCRIPTORS   equ 0
        org     p:$100
k       move    x:$E0,r0                ; KIN
        move    #$20,r1                 ; OUT L += bin L / 2
        move    #$400000,y0
        do      #32,k_end
        move    x:(r0)+,x0
        move    x:(r1),a
        mac     x0,y0,a
        move    a,x:(r1)+
k_end
        rts
"""


def test_legacy():
    img = dsp56asm.image_words(LEGACY)[0]
    assert img[:4] == [dsp56asm.IMAGE_MAGIC, 1, 0, 0], 'ABI 1 header: %s' % img[:4]
    gp = dsp56sim.Gp(LOOP, extra=(LEGACY,))
    s = gp.symbols
    for i, v in enumerate(COPY_DESC):
        gp.x[i] = v
        gp.x[s['FIFO2'] + i] = s['OUTL'] if i == 3 else v
    gp.run_until_wait()
    gp.x[s['KIN']] = 0x1400 + 32 * 2
    gp.x[s['K_RHEAD']] = 0
    gp.x[s['K_WHEAD']] = s['FIFO2']
    gp.x[s['FLAGS']] = 0x10
    left, _ = run_echo(gp, 2, {(2, 7): 0x400000, (0, 40): 0x100000})
    assert hits(left) == [(7, 0x2000), (40, 0x1000)], 'legacy kernel: %s' % hits(left)
    print('[8] legacy ABI 1 kernel at P:$100: header, its chains at $19 / $1A, alone (FLAGS bit 4) [PASS]')


def image_relocs(img):
    """(relocs, code) of an ABI 2 image: the header the library parses."""
    assert img[0] == dsp56asm.IMAGE_MAGIC and img[1] == 2
    n = img[6]
    relocs = [(w & 0xFFF, (w >> 12) & 3) for w in img[7:7 + n]]
    return relocs, img[7 + n:]


def test_relocations():
    kinds = {}
    for name, text in (('reverb', REVERB), ('efx echo', EFX_ECHO), ('chorus', EFX_CHORUS), ('echo', ECHO)):
        img, code, sym = dsp56asm.image_words(text)
        relocs, code2 = image_relocs(img)
        assert code2 == code and img[4] == sym['K_XWORDS'] and img[5] == sym['K_YWORDS']
        for b in ({'p': 0x100, 'x': 0xE0, 'y': 0}, {'p': 0x7FF - len(code), 'x': 0xBFF - sym['K_XWORDS'], 'y': 0},
                  {'p': 0x3A1, 'x': 0x2C5, 'y': 0x800 - sym['K_YWORDS']}):
            mem = dsp56asm.assemble(text, b)[0]
            want = [mem.get(b['p'] + i, 0) for i in range(len(code))]
            got = dsp56asm.relocate(code, relocs, b['p'], b['x'], b['y'])
            if got != want:
                i = next(i for i in range(len(code)) if got[i] != want[i])
                raise AssertionError('%s at %s: word %d relocated %06x, assembled %06x' % (name, b, i, got[i], want[i]))
        for _, kd in relocs:
            kinds[kd] = kinds.get(kd, 0) + 1
    assert all(kinds.get(kd) for kd in (0, 1, 2, 3)), 'every reloc kind exercised: %s' % kinds
    # the expression rules: reloc +- constant, reloc - reloc of one space; nothing else
    base = """
K_SCRATCH_BYTES equ 0
K_DESCRIPTORS   equ 0
K_XWORDS        equ $30
K_YWORDS        equ 0
A       equ     KX+$28
LEN     equ     A-KX
        org     p:0
k       move    #LEN,x0
        %s
        rts
"""
    for good in ('move #A-4,r0', 'move x:A+1,a', 'move #KY+3,r1', 'move #k+1,r2', 'jsr k', 'add #LEN,a',
                 'move #(A+2)-(KX+1),x0'):
        dsp56asm.image_words(base % good)
    for bad in ('move #KX+KX,r0', 'move #KX*2,r0', 'move #KX+KY,r0', 'move #KX&$FF,r0', 'move #k+KX,r0',
                'jclr #1,x:KX+1,k', 'do x:KX,k', 'rep #KX', 'bset #1,x:KX', 'jmp KX', 'move #-KX,r0'):
        try:
            dsp56asm.image_words(base % bad)
        except dsp56asm.AsmError:
            continue
        raise AssertionError('accepted %r' % bad)
    # a relocatable operand always takes the long form: the size never depends on the bases
    short = dsp56asm.assemble('        org p:0\n        move x:KX+3,a\n        move x:3,a\n',
                              {'p': 0, 'x': 0, 'y': 0})[3]
    assert [len(w) for _, w, _ in short] == [2, 1], short
    print('[9] relocations: 4 ABI 2 images relocated = assembled at those bases (P, 12-bit P, X, Y kinds: %s); '
          'reloc +- const and reloc - reloc only; long forms [PASS]' % sorted(kinds.items()))


def test_three_slots(exe):
    """Reverb (slot 0, bins 2/3), echo (slot 1, bins 4/5) and chorus (slot 2,
    bins 6/7) in one loop: OUT L/R minus the dry bins equals the sum of what each
    kernel adds running alone, word for word (no level here comes near full scale)."""
    frames = 80
    rng = random.Random(9)
    ins = [{b: [rng.randint(-0x100000, 0x100000) & 0xFFFFFF for _ in range(32)] for b in range(8)}
           for _ in range(frames)]
    for f in (0, 30):
        for b in (2, 4, 6):
            ins[f][b][3] = 0x300000                      # clicks the echoes are easy to see in
    words, _ = run_ref(exe, 3, 1, AT_REVERB['x'], SCRATCH0)
    echo = [100, 250, 0x400000, 0x200000, 0x5A8279, 0x5A8279, 0x5A8279, 0x5A8279]
    chorus = chorus_words(480, 0.5, 0.25, 10, 90, 0)
    plan = [(REVERB, AT_REVERB, [], SCRATCH0, words), (EFX_ECHO, AT_ECHO, echo, SCR_ECHO, []),
            (EFX_CHORUS, AT_CHORUS, chorus, SCRATCH0, [])]

    def run(which):
        gp = new_gp([(plan[n][0], plan[n][1]) for n in which])
        for i, n in enumerate(which):
            load_slot(gp, n, i, plan[n][1], plan[n][2], plan[n][3], plan[n][4])
        outs, fifo = [], []
        for f in range(frames):
            fifo.append(gp.frame(ins[f]))
            o = gp.symbols['OUTL']
            outs.append([sx(v) for v in gp.x[o:o + 64]])
        assert gp.x[gp.symbols['TMOUT']] == 0
        return outs, fifo, gp.frame_steps

    together, fifo, steps = run([0, 1, 2])
    alone = [run([n]) for n in range(3)]
    dry = [[sx(v) for v in ins[f][0] + ins[f][1]] for f in range(frames)]
    for f in range(frames):
        for i in range(64):
            wet = [alone[n][0][f][i] - dry[f][i] for n in range(3)]
            got = together[f][i] - dry[f][i]
            if got != sum(wet):
                raise AssertionError('frame %d word %d: together %d, alone %s' % (f, i, got, wet))
        assert fifo[f] == [v >> 8 for p in zip(together[f][:32], together[f][32:]) for v in p]
    assert all(any(any(alone[n][0][f][i] != dry[f][i] for i in range(64)) for f in range(frames)) for n in range(3))
    print('[10] three slots (reverb, echo, chorus) at once: each one\'s output = alone, word for word over %d frames; '
          '%d instructions per frame (alone: %d / %d / %d) [PASS]'
          % (frames, steps, alone[0][2], alone[1][2], alone[2][2]))


def main():
    test_copy()
    with tempfile.TemporaryDirectory(prefix='gpsim_') as tmp:
        exe = build_ref(tmp)
        steps = test_reverb(exe, 3, 160)
        test_reverb(exe, 7, 120)
        print('[2] reverb kernel in a slot = host reference, word for word (hall 160 frames, underwater 120), '
              '%d instructions per frame [PASS]' % steps)
        gp = test_mailbox()
        test_burn(gp)
        test_echo()
        test_efx_echo()
        test_efx_chorus()
        test_legacy()
        test_relocations()
        test_three_slots(exe)
    print('=== GP programs on the host: all passed ===')


if __name__ == '__main__':
    main()
