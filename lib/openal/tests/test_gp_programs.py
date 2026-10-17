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
    print('[3] mailbox: pairs land together at the next frame, MB_ACK follows MB_SEQ, empty posts too [PASS]')
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
    print('=== GP programs on the host: all passed ===')


if __name__ == '__main__':
    main()
