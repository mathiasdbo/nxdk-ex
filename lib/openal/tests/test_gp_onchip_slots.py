#!/usr/bin/env python3
"""
The on-chip EFX effects' GP kernels beside each other (task phase9-07,
docs/SOFT_GAP_PLAN.md; GP_KERNELS.md), in tools/dsp56sim.py:

  1  relocations: each image relocated by the loader's rules equals its source
     assembled at those bases (as tests/test_gp_programs.py [9] for the others)
  2  the reverb and three on-chip kernels (equalizer, distortion, autowah: the
     heaviest), one per slot, in one frame loop; then four on-chip kernels
     (ring modulator, distortion, equalizer, compressor): what each adds to
     OUT L / OUT R equals what it adds running alone, word for word, and the
     instructions a frame together

The words are the library's (tests/gpsim_onchip_words.c). The attach rules
for the same sets (the shared budgets) are tests/test_efx_multi.c [6].
"""
import os
import random
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True                                   # no __pycache__ in the source tree
import test_gp_programs as gpt  # noqa: E402
from test_gp_programs import dsp56asm, sx  # noqa: E402
from test_gp_onchip import build_words_helper, lib_words, src  # noqa: E402
from test_gp_onchip_env import autowah_words, compressor_words  # noqa: E402

KERNELS = {k: src('gp_kefx_%s.asm' % k) for k in ('ringmod', 'distortion', 'equalizer', 'compressor', 'autowah')}


def test_relocations():
    kinds = {}
    for name, text in sorted(KERNELS.items()):
        img, code, sym = dsp56asm.image_words(text)
        relocs, code2 = gpt.image_relocs(img)
        assert code2 == code and img[2] == 0 and img[3] == 0 and img[4] == sym['K_XWORDS'] and img[5] == 0
        for b in ({'p': 0x100, 'x': 0xE0, 'y': 0}, {'p': 0x7FF - len(code), 'x': 0xBFF - sym['K_XWORDS'], 'y': 0},
                  {'p': 0x3A1, 'x': 0x2C5, 'y': 0x123}):
            mem = dsp56asm.assemble(text, b)[0]
            want = [mem.get(b['p'] + i, 0) for i in range(len(code))]
            got = dsp56asm.relocate(code, relocs, b['p'], b['x'], b['y'])
            if got != want:
                i = next(i for i in range(len(code)) if got[i] != want[i])
                raise AssertionError('%s at %s: word %d relocated %06x, assembled %06x' % (name, b, i, got[i], want[i]))
        for _, kd in relocs:
            kinds[kd] = kinds.get(kd, 0) + 1
        print('    %-10s %3d P words, X block $%02X, no Y, no scratch, no descriptors, %d relocs'
              % (name, len(code), sym['K_XWORDS'], len(relocs)))
    print('[1] relocations: 5 ABI 2 images relocated = assembled at those bases (kinds %s) [PASS]'
          % sorted(kinds.items()))


def run_together(plan, frames, ins):
    """plan: [(source, bases, words, scratch, extra X words)] in slot order. -> OUT L/R per frame, steps."""
    gp = gpt.new_gp([(p[0], p[1]) for p in plan])
    for k, p in enumerate(plan):
        gpt.load_slot(gp, k, k, p[1], p[2], p[3], p[4])
    outs, steps = [], 0
    for f in range(frames):
        gp.frame(ins[f])
        o = gp.symbols['OUTL']
        outs.append([sx(v) for v in gp.x[o:o + 64]])
        steps = max(steps, gp.frame_steps)
    assert gp.x[gp.symbols['TMOUT']] == 0
    return outs, steps


def check_additive(name, plan, frames, seed):
    """Each slot k on its own bins 2+2k / 3+2k; together = the sum of alone, word for word."""
    rng = random.Random(seed)
    ins = [{b: [rng.randint(-0x80000, 0x80000) & 0xFFFFFF for _ in range(32)] for b in range(10)}
           for _ in range(frames)]
    together, steps = run_together(plan, frames, ins)
    alone = []
    for k in range(len(plan)):
        # alone, in its own slot k (the others' slots empty)
        gp = gpt.new_gp([(plan[k][0], plan[k][1])])
        gpt.load_slot(gp, k, 0, plan[k][1], plan[k][2], plan[k][3], plan[k][4])
        outs, st = [], 0
        for f in range(frames):
            gp.frame(ins[f])
            o = gp.symbols['OUTL']
            outs.append([sx(v) for v in gp.x[o:o + 64]])
            st = max(st, gp.frame_steps)
        alone.append((outs, st))
    dry = [[sx(v) for v in ins[f][0] + ins[f][1]] for f in range(frames)]
    for f in range(frames):
        for i in range(64):
            wet = [alone[k][0][f][i] - dry[f][i] for k in range(len(plan))]
            if together[f][i] - dry[f][i] != sum(wet):
                raise AssertionError('%s frame %d word %d: together %d, alone %s'
                                     % (name, f, i, together[f][i] - dry[f][i], wet))
    for k in range(len(plan)):
        assert any(any(alone[k][0][f][i] != dry[f][i] for i in range(64)) for f in range(frames)), (name, k)
    return steps, [a[1] for a in alone]


def test_together(exe):
    frames = 80                                                  # the reverb's first output is that late
    words, _ = gpt.run_ref(exe, 3, 1, gpt.AT_REVERB['x'], gpt.SCRATCH0)
    eq = lib_words('equalizer', (2.0, 150, 0.5, 700, 0.7, 3.0, 2500, 0.4, 0.5, 9000))
    dist = lib_words('distortion', (0.4, 0.3, 9000, 2500, 2500))
    wah = lib_words('autowah', (0.02, 0.1, 400.0, 30.0), autowah_words)
    plan = [(gpt.REVERB, gpt.AT_REVERB, [], gpt.SCRATCH0, words),
            (KERNELS['equalizer'], {'p': 0x300, 'x': 0xA60, 'y': 0}, eq, 0, []),
            (KERNELS['distortion'], {'p': 0x380, 'x': 0xAA4, 'y': 0}, dist, 0, []),
            (KERNELS['autowah'], {'p': 0x440, 'x': 0xAD7, 'y': 0}, wah, 0, [])]
    steps, alone = check_additive('reverb + 3', plan, frames, 7)
    print('[2] the reverb, the equalizer (4 bands), the distortion and the autowah in slots 0-3: each one\'s output '
          '= alone, word for word over %d frames; %d instructions per frame together (alone %s)'
          % (frames, steps, ' / '.join(str(a) for a in alone)))
    rm = lib_words('ringmod', (440, 800, 0))
    comp = lib_words('compressor', (1, 0.8), compressor_words)
    plan = [(KERNELS['ringmod'], {'p': 0x100, 'x': 0x0E0, 'y': 0}, rm, 0, []),
            (KERNELS['distortion'], {'p': 0x1B0, 'x': 0x140, 'y': 0}, dist, 0, []),
            (KERNELS['equalizer'], {'p': 0x260, 'x': 0x180, 'y': 0}, eq, 0, []),
            (KERNELS['compressor'], {'p': 0x2C0, 'x': 0x1D0, 'y': 0}, comp, 0, [])]
    steps, alone = check_additive('4 on-chip', plan, 40, 8)
    print('    the ring modulator, the distortion, the equalizer and the compressor in slots 0-3: the same; '
          '%d instructions per frame together (alone %s), over 40 frames [PASS]'
          % (steps, ' / '.join(str(a) for a in alone)))


def main():
    test_relocations()
    with tempfile.TemporaryDirectory(prefix='gponchip_') as tmp:
        build_words_helper(tmp)
        test_together(gpt.build_ref(tmp))
    print('=== on-chip EFX effects beside each other on the host: all passed ===')


if __name__ == '__main__':
    main()
