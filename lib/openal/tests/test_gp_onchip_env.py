#!/usr/bin/env python3
"""
The on-chip EFX effects that follow an envelope (task phase9-07,
docs/SOFT_GAP_PLAN.md; GP_KERNELS.md): their GP kernels in tools/dsp56sim.py
against OpenAL Soft's algorithms in double-precision floating point, on the
library's own words, as tests/test_gp_onchip.py does for the filters.

  1  compressor (dsp/gp_kefx_compressor.asm)
  2  autowah (dsp/gp_kefx_autowah.asm)

Errors are in LSB of 24 bits on OUT L / OUT R (256 make one 16-bit LSB).
"""
import math
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True                                   # no __pycache__ in the source tree
from test_gp_onchip import (LIB, build_words_helper, clip, lib_words, lsb_err, q, q23, run_wet,  # noqa: E402
                            signal, src, sx)

EFX_COMPRESSOR = src('gp_kefx_compressor.asm')
EFX_AUTOWAH = src('gp_kefx_autowah.asm')


# ------------------------------------------------------------ 1 compressor

def compressor_words(on, slot_gain=1.0):
    """The library's words (al_efx.c compressor_params): OpenAL Soft's fixed rates at 48 kHz."""
    att = (2.0 / 0.5) ** (1.0 / (48000 * 0.1))
    rel = (0.5 / 2.0) ** (1.0 / (48000 * 0.2))
    return [on, q23(att - 1.0), q23(rel), q23(slot_gain)]


def compressor_model(words, left, right, n):
    """OpenAL Soft's compressor with the kernel's words (its 1 is 1 - 2^-23 here, as on the GP)."""
    one = 0x7FFFFF / 0x800000
    on, a, r, g = words[0], 1.0 + q(words[1]), q(words[2]), q(words[3])
    env, out = one, ([], [])
    for t in range(n):
        amp = min(max(abs(clip(q(left[t]) + q(right[t]))), 0.5), one) if on else one
        if amp > env:
            env = min(env * a, amp)
        elif amp < env:
            env = max(env * r, amp)
        for c, b in ((0, left), (1, right)):
            out[c].append(clip(2 * q(b[t]) / env * g))
    return out


def test_compressor():
    n = 32 * 400
    # a loud passage, then a quiet one: the gain falls toward 1, then rises toward 2
    left = [(q23((0.4 if t < n // 2 else 0.06) * math.sin(2 * math.pi * 300 * t / 48000)) & 0xFFFFFF) for t in range(n)]
    right = signal(n, 41, 0.1)
    worst, steps = 0, 0
    for on, g in ((1, 1.0), (1, 0.5), (0, 1.0)):
        words = lib_words('compressor', (on, g), compressor_words)
        ol, orr, st = run_wet(EFX_COMPRESSOR, words, left, right, n)
        ml, mr = compressor_model(words, left, right, n)
        err = max(lsb_err(ol, ml), lsb_err(orr, mr))
        print('    compressor on %d gain %.1f: %d LSB (24-bit), %d instructions' % (on, g, err, st))
        assert err <= 4 * 256, 'compressor %s: %d LSB (24-bit) from the model' % ((on, g), err)
        worst, steps = max(worst, err), max(steps, st)
        if on:   # it did compress: the quiet half comes out louder than its input
            k = n - 100
            assert abs(ol[k]) > 1.9 * g * abs(2 * sx(left[k])), (ol[k], sx(left[k]))
    print("[1] EFX compressor: envelope, attack / release, on / off against OpenAL Soft's algorithm, within "
          '%.2f LSB of 16 bits; %d instructions per frame [PASS]' % (worst / 256.0, steps))

# ------------------------------------------------------------ 2 autowah

def autowah_words(attack, release, resonance, peak, slot_gain=1.0):
    """The library's words (al_efx.c autowah_params): OpenAL Soft's derived values."""
    res = math.sqrt(math.log10(resonance) * 10.0 / 3.0)
    return [q23(1.0 - math.exp(-1.0 / (attack * 48000))),
            q23(1.0 - math.exp(-1.0 / (min(max(release, 0.001), 1.0) * 48000))),
            q23((1.0 - math.log10(peak / 31621.0)) / 16), q23(16 * (2500.0 - 20.0) / 48000), q23(20.0 / 48000),
            q23(res / 4), q23(1.0 / res), q23(slot_gain)]


def autowah_model(words, left, right, n):
    """OpenAL Soft's autowah with the kernel's words; y limited where the kernel limits it."""
    att, rel, pk, bw, fmin = 1 - q(words[0]), 1 - q(words[1]), 16 * q(words[2]), q(words[3]) / 16, q(words[4])
    res, ires, g = 4 * q(words[5]), q(words[6]), q(words[7])
    env, z = 0.0, [[0.0, 0.0], [0.0, 0.0]]
    out = ([], [])
    for t in range(n):
        s = pk * abs(clip(q(left[t]) + q(right[t])))
        a = att if s > env else rel
        env = s + a * (env - s)
        w0 = min(bw * env + fmin, 0.46) * 2 * math.pi
        cw, al = math.cos(w0), math.sin(w0) / 10.0
        a0 = 1 + al * ires
        b0, b1, b2, a1, a2 = (1 + al * res) / a0, -2 * cw / a0, (1 - al * res) / a0, -2 * cw / a0, (1 - al * ires) / a0
        for c, b in ((0, left), (1, right)):
            u = clip(2 * q(b[t]))
            y = clip(u * b0 + z[c][0])
            z[c][0] = u * b1 - y * a1 + z[c][1]
            z[c][1] = u * b2 - y * a2
            out[c].append(g * y)
    return out


def f32(v):
    return struct.unpack('<f', struct.pack('<f', v))[0]


def autowah_float32(words, left, right, n):
    """OpenAL Soft's autowah as it runs, in single precision (every operation rounded to a float): the
    yardstick for how far from the double-precision model a correct implementation lands."""
    att, rel, pk = f32(1 - q(words[0])), f32(1 - q(words[1])), f32(16 * q(words[2]))
    bw, fmin, res = f32(q(words[3]) / 16), f32(q(words[4])), f32(4 * q(words[5]))
    env, z, two_pi = 0.0, [[0.0, 0.0], [0.0, 0.0]], f32(2 * math.pi)
    out = ([], [])
    for t in range(n):
        s = f32(pk * f32(abs(clip(q(left[t]) + q(right[t])))))
        a = att if s > env else rel
        env = f32(s + f32(a * f32(env - s)))
        w0 = f32(f32(min(f32(f32(bw * env) + fmin), f32(0.46))) * two_pi)
        cw, al = f32(math.cos(w0)), f32(f32(math.sin(w0)) / 10.0)
        a0 = f32(1 + f32(al / res))
        b0, b1 = f32(f32(1 + f32(al * res)) / a0), f32(f32(-2 * cw) / a0)
        b2, a2 = f32(f32(1 - f32(al * res)) / a0), f32(f32(1 - f32(al / res)) / a0)
        for c, b in ((0, left), (1, right)):
            u = f32(clip(2 * q(b[t])))
            y = f32(f32(u * b0) + z[c][0])
            z[c][0] = f32(f32(f32(u * b1) - f32(y * b1)) + z[c][1])
            z[c][1] = f32(f32(u * b2) - f32(y * a2))
            out[c].append(y * q(words[7]))
    return out


def test_autowah():
    n = 32 * 150
    # plucks: a decaying tone every 50 ms, so the envelope attacks and releases
    left = [(q23(0.3 * math.exp(-(t % 2400) / 600.0) * math.sin(2 * math.pi * 220 * t / 48000)) & 0xFFFFFF)
            for t in range(n)]
    right = signal(n, 51, 0.05)
    worst, steps = 0, 0
    for args in ((0.06, 0.06, 1000.0, 11.22), (0.0001, 0.5, 2.0, 31621.0), (0.01, 0.001, 100.0, 1.0),
                 (1.0, 1.0, 10.0, 0.00003), (0.06, 0.06, 1000.0, 11.22, 0.4)):
        words = lib_words('autowah', args, autowah_words)
        ol, orr, st = run_wet(EFX_AUTOWAH, words, left, right, n)
        ml, mr = autowah_model(words, left, right, n)
        err = max(lsb_err(ol, ml), lsb_err(orr, mr))
        print('    autowah %s: %d LSB (24-bit), %d instructions' % (args, err, st))
        assert err <= 4 * 256, 'autowah %s: %d LSB (24-bit) from the model' % (args, err)
        worst, steps = max(worst, err), max(steps, st)
        if len(args) == 4 and args[2] == 1000.0:
            sl, sr = autowah_float32(words, left, right, n)
            soft = max(lsb_err([v * 0x800000 for v in sl], ml), lsb_err([v * 0x800000 for v in sr], mr))
            print('    (OpenAL Soft in single precision: %d LSB from the same model)' % soft)
    print("[2] EFX autowah: envelope, attack / release, the moving peak per channel against OpenAL Soft's "
          'algorithm, within %.2f LSB of 16 bits (its words move every sample; OpenAL Soft\'s own float '
          'arithmetic lands as far: see above); %d instructions per frame [PASS]' % (worst / 256.0, steps))


def main():
    with tempfile.TemporaryDirectory(prefix='gponchip_') as tmp:
        build_words_helper(tmp)
        test_compressor()
        test_autowah()
    print('    (%d sets of words: the library\'s = this file\'s derivation, within 1 LSB a word)' % LIB['sets'])
    print('=== on-chip EFX effects (envelopes) on the host: all passed ===')


if __name__ == '__main__':
    main()
