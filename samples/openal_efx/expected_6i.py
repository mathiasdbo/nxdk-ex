#!/usr/bin/env python3
"""
Where samples/openal_efx's sections 6i (the on-chip effects) and 6j get the
numbers they check against: each library kernel run in the host simulator
(lib/openal/tools/dsp56sim.py, the frame loop plus the kernel) on the
library's words (the tests' own derivations, equal to al_efx.c's within 1 LSB a
word), fed the bins the sample's sound produces, and measured the way the
sample measures it (Goertzel over 960-sample blocks of the top 16 bits).

The tests (lib/openal/tests/test_gp_onchip*.py) put the kernels within 3.3 LSB
of 16 bits of OpenAL Soft's algorithm in double precision, so these numbers
are also the floating-point model's to far better than the tolerances.

How a sound reaches a kernel (lib/openal/docs/EFX_PLAN.md 3.2, console 2.1): a
mono voice at gain 1 with send 1 puts half of its 16-bit sample, as a 24-bit
word, on each of the slot's two bins (bin = pcm * 128); the dry voice,
centre-panned, gives each ear 0.7071 of it. The sample reads that dry level
first and quotes every wet level against it, so the expectations here are
20 log10(wet amplitude / (0.70711 * pcm amplitude)).

Tolerances: the level the VP hands the kernel is the one thing the simulator
cannot know to better than the 1/64 dB volume steps and the pan law, so each
level-dependent check (distortion, compressor, autowah) is also run here with
the input 0.5 dB louder and quieter; the sample's tolerance covers that spread
plus 0.5 dB (16-bit output, the codec path).

    python samples/openal_efx/expected_6i.py      (about a minute)
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.join(HERE, '..', '..', 'lib', 'openal', 'tests')
sys.path.insert(0, TESTS)
sys.dont_write_bytecode = True
import test_gp_onchip as oc          # noqa: E402
import test_gp_onchip_env as env     # noqa: E402

FS = 48000
DRY = 0.70711                       # the dry, centre-panned ear against the sample


def bins_of(pcm):
    """A mono voice's 16-bit samples as the two bins' 24-bit words (half the send each)."""
    w = [(int(v) * 128) & 0xFFFFFF for v in pcm]
    return w, w


def amp(x, hz, start, count):
    """The sample's tone amplitude: Goertzel power averaged over 960-sample blocks, as an amplitude."""
    c = 2.0 * math.cos(2 * math.pi * hz / FS)
    acc, blocks = 0.0, count // 960
    for b in range(blocks):
        s1 = s2 = 0.0
        for i in range(960):
            s = x[start + b * 960 + i] + c * s1 - s2
            s2, s1 = s1, s
        acc += s1 * s1 + s2 * s2 - c * s1 * s2
    return 2.0 * math.sqrt(acc / blocks) / 960.0


def db(v):
    return 20.0 * math.log10(v) if v > 0 else -999.0


def tone(freqs, amp_pcm, n, scale=1.0):
    return [int(round(scale * sum(amp_pcm * math.sin(2 * math.pi * f * t / FS) for f in freqs))) for t in range(n)]


def run(source, words, pcm):
    left, right = bins_of(pcm)
    ol, _, steps = oc.run_wet(source, words, left, right, len(pcm))
    return [v >> 8 for v in ol], steps          # the codec's 16 bits (an arithmetic shift, as the mix descriptor)


SETTLE = 14400                      # 0.3 s, then 0.4 s measured (the sample: run_for(300), capture(400))
MEAS = 19200


def ringmod():
    a = 8000
    pcm = tone([1000], a, SETTLE + MEAS)
    out, st = run(oc.EFX_RINGMOD, oc.ringmod_words(300.0, 0.0, 0), pcm)
    ref = DRY * a
    r = [db(amp(out, f, SETTLE, MEAS) / ref) for f in (700, 1300, 1000)]
    print('ring modulator 1 kHz (pcm %d) x 300 Hz sine, highpass off: 700 Hz %.2f, 1300 Hz %.2f, 1 kHz %.1f dB '
          'against the dry ear; %d instructions' % (a, r[0], r[1], r[2], st))


def distortion(scale=1.0):
    a = 8000
    pcm = tone([1000], a, SETTLE + MEAS, scale)
    words = oc.distortion_words(0.5, 0.5, 8000.0, 3000.0, 6000.0)
    out, st = run(oc.EFX_DISTORTION, words, pcm)
    h = [amp(out, f, SETTLE, MEAS) for f in (1000, 3000, 5000)]
    return db(h[0] / (DRY * a)), db(h[1] / h[0]), db(h[2] / h[0]), st


def equalizer():
    a = 3000
    freqs = (100, 500, 3000, 12000)
    pcm = tone(freqs, a, SETTLE + MEAS)
    base = (1.0, 200.0, 1.0, 500.0, 1.0, 1.0, 3000.0, 1.0, 1.0, 6000.0)
    flat_out, st0 = run(oc.EFX_EQUALIZER, oc.equalizer_words(*base), pcm)
    flat = [amp(flat_out, f, SETTLE, MEAS) for f in freqs]
    print('equalizer flat: %s dB against the dry ear (%d instructions)'
          % (', '.join('%.2f' % db(v / (DRY * a)) for v in flat), st0))
    for band, idx in (('low', 0), ('mid1', 2), ('mid2', 5), ('high', 8)):
        args = list(base)
        args[idx] = 3.981
        out, st = run(oc.EFX_EQUALIZER, oc.equalizer_words(*args), pcm)
        g = [db(amp(out, f, SETTLE, MEAS) / flat[k]) for k, f in enumerate(freqs)]
        print('equalizer %-4s +12 dB: 100 Hz %6.2f, 500 Hz %6.2f, 3 kHz %6.2f, 12 kHz %6.2f dB against flat; '
              '%d instructions' % (band, g[0], g[1], g[2], g[3], st))


def compressor(scale=1.0):
    loud, quiet = 29000, 3000
    res = {}
    for name, a in (('loud', loud), ('quiet', quiet)):
        pcm = tone([1000], a, 2 * SETTLE + MEAS, scale)   # 0.6 s settle: the release from 1 to 0.5 takes 0.1 s
        for on in (1, 0):
            out, st = run(env.EFX_COMPRESSOR, env.compressor_words(on, 0.5), pcm)
            res[(name, on)] = amp(out, 1000, 2 * SETTLE, MEAS)
    return (db(res[('loud', 1)] / res[('loud', 0)]), db(res[('quiet', 1)] / res[('quiet', 0)]),
            db(res[('loud', 1)] / res[('quiet', 1)]), db(res[('loud', 0)] / res[('quiet', 0)]), st)


AW_LOUD, AW_QUIET, AW_HARMONICS = 700, 140, 48
AW_SEG = 28800                      # 0.6 s loud, then 0.6 s quiet
AW_WIN = ((12000, 14400), (40800, 14400))   # 0.25 .. 0.55 s and 0.85 .. 1.15 s after the onset


def comb(b, t):
    """100 Hz .. 4.8 kHz, every 100 Hz, Schroeder's phases (a low crest factor): the sample's make_comb."""
    return b * sum(math.sin(2 * math.pi * 100 * k * t / FS + math.pi * k * k / AW_HARMONICS)
                   for k in range(1, AW_HARMONICS + 1))


def peak_hz(out, start, count):
    """The sample's estimate of where the autowah's peak sits: the loudest harmonic, refined by a parabola
    through its level and its neighbours' (in dB)."""
    a = [amp(out, 100 * k, start, count) for k in range(1, AW_HARMONICS + 1)]
    i = max(range(AW_HARMONICS), key=lambda j: a[j])
    d = 0.0
    if 0 < i < AW_HARMONICS - 1:
        l, m, r = (db(a[j]) for j in (i - 1, i, i + 1))
        d = 0.5 * (l - r) / (l - 2 * m + r)
    return 100.0 * (i + 1 + d)


def autowah(scale=1.0):
    pcm = [int(math.floor(scale * comb(AW_LOUD, t) + 0.5)) for t in range(AW_SEG)] +           [int(math.floor(scale * comb(AW_QUIET, t) + 0.5)) for t in range(AW_SEG)]
    words = env.autowah_words(0.06, 0.06, 100.0, 0.00003)
    out, st = run(env.EFX_AUTOWAH, words, pcm)
    fl, fq = (peak_hz(out, start, count) for start, count in AW_WIN)
    return fl, fq, (fl - 20.0) / (fq - 20.0), st


def main():
    ringmod()
    for s in (1.0, 10 ** (0.5 / 20), 10 ** (-0.5 / 20)):
        h1, h3, h5, st = distortion(s)
        print('distortion EDGE 0.5 GAIN 0.5 LP 8 kHz EQ 3 kHz / 6 kHz, 1 kHz pcm 8000 x %.3f: H1 %.2f dB against the '
              'dry ear, H3 %.2f, H5 %.2f dB against H1; %d instructions' % (s, h1, h3, h5, st))
    equalizer()
    for s in (1.0, 10 ** (0.5 / 20), 10 ** (-0.5 / 20)):
        lo, qu, on, off, st = compressor(s)
        print('compressor (slot gain 0.5), 1 kHz pcm 29000 / 3000 x %.3f: loud on-off %.2f, quiet on-off %.2f, '
              'loud-quiet on %.2f, off %.2f dB; %d instructions' % (s, lo, qu, on, off, st))
    for s in (1.0, 10 ** (0.5 / 20), 10 ** (-0.5 / 20)):
        fl, fq, ratio, st = autowah(s)
        print('autowah RESONANCE 100, PEAK_GAIN 0.00003, comb pcm %d then %d a harmonic, x %.3f: peak at %.0f Hz '
              '(loud), %.0f Hz (quiet), (f - 20 Hz) ratio %.3f (the input level ratio, 5); %d instructions'
              % (AW_LOUD, AW_QUIET, s, fl, fq, ratio, st))


if __name__ == '__main__':
    main()
