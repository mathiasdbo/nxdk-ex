#!/usr/bin/env python3
"""
The default GP program on the host (task phase8-02, EFX_PLAN.md 3.3-3.4): the
frame loop (dsp/gp_loop.asm) with kernel 0 (dsp/gp_kfreeverb.asm), run in
tools/dsp56sim.py, against apu_reverb.c's host reference.

  1  copy program (kernel off): FIFO 0 carries bins 0/1
  2  reverb: the X words apu_reverb_layout() writes, then frames of a click and
     noise (with full-scale words) on bins 0-3; OUT L/R and FIFO 0 equal the
     reference word for word (presets: hall, underwater with its output lowpass)
  3  mailbox: pairs posted while the kernel runs land together at the start of
     the next frame, and MB_ACK follows MB_SEQ; an empty post is acknowledged too
  4  burn: X:BURN cycles of nop run (the step count grows by that much)
  5  custom kernel (phase8-04, EFX_PLAN.md 3.6): the example echo
     (dsp/gp_kecho.asm) loaded the way apu_gp.c loads a custom kernel; the
     echo lands exactly DELAY samples late on each ear, feedback repeats it,
     a DELAY posted through the mailbox applies from the next frame, and
     DELAY is clamped to 32

Run by tests/run_tests.py with NXDK_AL_CLANG and NXDK_AL_LIB set (the
reference helper tests/gpsim_reverb_ref.c is linked against the library).
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
OPENAL = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(OPENAL, 'tools'))
import dsp56sim  # noqa: E402

LOOP = open(os.path.join(OPENAL, 'dsp', 'gp_loop.asm'), encoding='utf-8').read()
KERNEL = open(os.path.join(OPENAL, 'dsp', 'gp_kfreeverb.asm'), encoding='utf-8').read()
ECHO = open(os.path.join(OPENAL, 'dsp', 'gp_kecho.asm'), encoding='utf-8').read()

COPY_DESC = [0x4000, 1 | 2 | (1 << 10) | (32 << 14), (32 << 4) | 1, 0x1400, 0, 0, 0]   # apu_gp.c


def new_gp():
    gp = dsp56sim.Gp(LOOP, extra=(KERNEL,))
    for i, v in enumerate(COPY_DESC):
        gp.x[i] = v
    gp.run_until_wait()
    return gp


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


def parse_ref(text):
    words, frames = [], []
    for ln in text.splitlines():
        p = ln.split()
        if not p:
            continue
        if p[0] == 'W':
            words.append((int(p[1], 16), int(p[2], 16)))
        elif p[0] == 'I':
            if p[1] == '0':
                frames.append({'in': {}, 'out': {}})
            frames[-1]['in'][int(p[1])] = [int(v, 16) for v in p[2:]]
        elif p[0] == 'O':
            frames[-1]['out'][int(p[1])] = [int(v, 16) for v in p[2:]]
    return words, frames


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
    out = subprocess.run([exe, str(preset), str(frames)], check=True, capture_output=True, text=True).stdout
    words, fr = parse_ref(out)
    gp = new_gp()
    for a, v in words:
        gp.x[a] = v
    gp.x[gp.symbols['FLAGS']] = 1
    for f, frame in enumerate(fr):
        fifo = gp.frame(frame['in'])
        outl, outr = gp.symbols['OUTL'], gp.symbols['OUTR']
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
    gp.x[s['FLAGS']] = 1
    gp.x[s['K_RHEAD']] = 0                       # no kernel chains: an empty kernel run
    gp.x[s['K_WHEAD']] = s['FIFO2']
    for i, v in enumerate([0x4000, 1 | 2 | (1 << 10) | (32 << 14), (32 << 4) | 1, s['OUTL'], 0, 0, 0]):
        gp.x[s['FIFO2'] + i] = v
    # an "rts" kernel: the loop's dry path alone
    gp.code[s['KERNEL']] = ('rts', '', 1, 'rts', s)
    left = [0x010000 * (i + 1) & 0xFFFFFF for i in range(32)]
    right = [0] * 32
    fifo = gp.frame({0: left, 1: right})
    assert fifo == fifo_of(left, right), 'dry path with an empty kernel'
    # post three pairs; nothing may change before the next frame starts
    pairs = [(0x200, 0x111111), (0x201, 0x222222), (0x7FF, 0x333333)]
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
    gp.x[s['MBOX'] + 1] = 0
    gp.x[s['MB_SEQ']] = 4                         # the lowpass off again
    gp.frame({0: left, 1: right})
    # a post held until frame MB_AT (the frame counter, mod 2^24), across the wrap too
    for cnt, at in ((40, 43), (0xFFFFFE, 1)):
        gp.x[s['CNT']] = cnt
        gp.x[s['MBOX']] = 0x300
        gp.x[s['MBOX'] + 1] = at + 7
        gp.x[s['MB_N']] = 1
        gp.x[s['MB_AT']] = at
        gp.x[s['MB_SEQ']] += 1
        frames = 0
        while gp.x[0x300] != at + 7:
            assert gp.x[s['MB_ACK']] != gp.x[s['MB_SEQ']]
            gp.frame({0: left, 1: right})
            frames += 1
            assert frames < 8
        # applied at the start of frame `at`: CNT has counted that frame once
        assert (gp.x[s['CNT']] - 1) & 0xFFFFFF == at and gp.x[s['MB_ACK']] == gp.x[s['MB_SEQ']], (cnt, at, frames)
    print('[3] mailbox: pairs land together at the next frame, MB_ACK follows MB_SEQ, empty posts too, '
          'held until MB_AT (across the wrap) [PASS]')
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


def load_custom(gp, params, scratch_bytes):
    """What apu_gp.c does for a custom kernel: kernel X zeroed, ABI words, chain heads, kernel on."""
    s = gp.symbols
    for a in range(s['KX_BASE'], 0xC00):
        gp.x[a] = 0
    gp.x[s['KIN']] = 0x1400 + 32 * 2
    gp.x[s['KSCR']] = 0x2000
    gp.x[s['KSCRN']] = scratch_bytes
    for i, v in enumerate(params):
        gp.x[s['KPARAM'] + i] = v & 0xFFFFFF
    gp.x[s['K_RHEAD']] = 0
    gp.x[s['K_WHEAD']] = s['FIFO2']
    for i, v in enumerate([0x4000, 1 | 2 | (1 << 10) | (32 << 14), (32 << 4) | 1, s['OUTL'], 0, 0, 0]):
        gp.x[s['FIFO2'] + i] = v
    gp.x[s['FLAGS']] = 1


def run_echo(gp, frames, impulses):
    """impulses: {(bin, absolute sample): value}; returns the FIFO's left and right streams (16-bit)."""
    left, right = [], []
    for f in range(frames):
        bins = {b: [0] * 32 for b in range(4)}
        for (b, t), v in impulses.items():
            if t // 32 == f:
                bins[b][t % 32] = v
        fifo = gp.frame(bins)
        left += fifo[0::2]
        right += fifo[1::2]
    return left, right


def hits(stream):
    return [(t, v) for t, v in enumerate(stream) if v]


def test_echo():
    gp = dsp56sim.Gp(LOOP, extra=(ECHO,))
    for i, v in enumerate(COPY_DESC):
        gp.x[i] = v
    gp.run_until_wait()
    s = gp.symbols
    # delay 100, no feedback, wet 0.5: a 0.5 click on bin 2 at sample 5, on bin 3 at 40
    load_custom(gp, [100, 0, 0x400000], s['K_SCRATCH_BYTES'])
    left, right = run_echo(gp, 12, {(2, 5): 0x400000, (3, 40): 0x400000})
    assert hits(left) == [(105, 0x2000)], 'echo left: %s' % hits(left)
    assert hits(right) == [(140, 0x2000)], 'echo right: %s' % hits(right)
    # feedback 0.5: the echo repeats every DELAY samples at half the level
    gp = dsp56sim.Gp(LOOP, extra=(ECHO,))
    for i, v in enumerate(COPY_DESC):
        gp.x[i] = v
    gp.run_until_wait()
    load_custom(gp, [100, 0x400000, 0x400000], s['K_SCRATCH_BYTES'])
    left, _ = run_echo(gp, 12, {(2, 5): 0x400000})
    assert hits(left) == [(105, 0x2000), (205, 0x1000), (305, 0x800)], 'feedback: %s' % hits(left)
    # a new delay through the mailbox at frame 12 (sample 384); a click at 400 echoes 37 later
    gp.x[s['MBOX']] = s['KPARAM']
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
    assert gp.x[s['TMOUT']] == 0, 'a DMA wait timed out'
    print('[5] custom kernel (echo): exact delay per ear, feedback, live delay via the mailbox, clamp; '
          '%d instructions per frame [PASS]' % gp.frame_steps)


def main():
    test_copy()
    with tempfile.TemporaryDirectory(prefix='gpsim_') as tmp:
        exe = build_ref(tmp)
        steps = test_reverb(exe, 3, 160)
        test_reverb(exe, 7, 120)
    print('[2] reverb kernel in the loop = host reference, word for word (hall 160 frames, underwater 120), '
          '%d instructions per frame [PASS]' % steps)
    gp = test_mailbox()
    test_burn(gp)
    test_echo()
    print('=== GP programs on the host: all passed ===')


if __name__ == '__main__':
    main()
