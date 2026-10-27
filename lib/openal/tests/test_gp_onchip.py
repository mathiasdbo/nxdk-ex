#!/usr/bin/env python3
"""
The on-chip EFX effects' GP kernels on the host (task phase9-07,
docs/SOFT_GAP_PLAN.md; GP_KERNELS.md): each kernel relocated into a slot of
the frame loop (dsp/gp_loop.asm) in tools/dsp56sim.py, against OpenAL Soft's
algorithm for that effect in double-precision floating point, fed the
kernel's own parameter words (so the comparison measures the kernel's
fixed-point arithmetic and approximations, not the words' rounding). The words
are the library's (al_efx.c, through tests/gpsim_onchip_words.c), and each set
is checked against this file's own derivation from the EFX values, within 1
LSB a word; tests/test_efx_*.c check them against the EFX values too.

  1  ring modulator (dsp/gp_kefx_ringmod.asm)
  2  distortion (dsp/gp_kefx_distortion.asm)
  3  equalizer (dsp/gp_kefx_equalizer.asm)

The compressor and the autowah are in tests/test_gp_onchip_env.py, several
kernels at once and the relocations in tests/test_gp_onchip_slots.py (each
file stays well inside run_tests.py's time limit).

Errors are in LSB of 24 bits on OUT L / OUT R; the codec gets the top 16 bits
(256 of them make one 16-bit LSB).
"""
import glob
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True                                   # no __pycache__ in the source tree
import test_gp_programs as gpt  # noqa: E402
from test_gp_programs import sx, kernel_gp, src  # noqa: E402

EFX_RINGMOD = src('gp_kefx_ringmod.asm')
EFX_DISTORTION = src('gp_kefx_distortion.asm')
EFX_EQUALIZER = src('gp_kefx_equalizer.asm')

AT_FX = {'p': 0x380, 'x': 0x9B0, 'y': 0}                            # anywhere, as for the others


# ------------------------------------------------------------ OpenAL Soft's biquad design (BiquadFilter::setParams)

def rcpq_from_slope(gain, slope):
    return math.sqrt((gain + 1.0 / gain) * (1.0 / slope - 1.0) + 2.0)


def rcpq_from_bandwidth(f0norm, bw):
    w0 = 2.0 * math.pi * f0norm
    return 2.0 * math.sinh(math.log(2.0) / 2.0 * bw * w0 / math.sin(w0))


def biquad(kind, f0norm, gain, rcpq):
    """(b0, b1, b2, a1, a2), normalised by a0."""
    w0 = 2.0 * math.pi * f0norm
    sw, cw = math.sin(w0), math.cos(w0)
    al = sw / 2.0 * rcpq
    g = gain
    if kind == 'highshelf':
        s = 2.0 * math.sqrt(g) * al
        b = [g * ((g + 1) + (g - 1) * cw + s), -2 * g * ((g - 1) + (g + 1) * cw), g * ((g + 1) + (g - 1) * cw - s)]
        a = [(g + 1) - (g - 1) * cw + s, 2 * ((g - 1) - (g + 1) * cw), (g + 1) - (g - 1) * cw - s]
    elif kind == 'lowshelf':
        s = 2.0 * math.sqrt(g) * al
        b = [g * ((g + 1) - (g - 1) * cw + s), 2 * g * ((g - 1) - (g + 1) * cw), g * ((g + 1) - (g - 1) * cw - s)]
        a = [(g + 1) + (g - 1) * cw + s, -2 * ((g - 1) + (g + 1) * cw), (g + 1) + (g - 1) * cw - s]
    elif kind == 'peaking':
        b = [1 + al * g, -2 * cw, 1 - al * g]
        a = [1 + al / g, -2 * cw, 1 - al / g]
    elif kind == 'lowpass':
        b = [(1 - cw) / 2, 1 - cw, (1 - cw) / 2]
        a = [1 + al, -2 * cw, 1 - al]
    elif kind == 'highpass':
        b = [(1 + cw) / 2, -(1 + cw), (1 + cw) / 2]
        a = [1 + al, -2 * cw, 1 - al]
    else:                                                       # bandpass
        b = [al, 0.0, -al]
        a = [1 + al, -2 * cw, 1 - al]
    return [b[0] / a[0], b[1] / a[0], b[2] / a[0], a[1] / a[0], a[2] / a[0]]


def q23(v):
    return max(-0x800000, min(0x7FFFFF, int(math.floor(v * 0x800000 + 0.5))))


def q(w):
    return sx(w & 0xFFFFFF) / 0x800000


def bq_words(c, scale):
    """A kernel's 5 words for a biquad: b0 b1 b2 -a1 -a2, times scale (1/2 or 1/4)."""
    return [q23(c[0] * scale), q23(c[1] * scale), q23(c[2] * scale), q23(-c[3] * scale), q23(-c[4] * scale)]


def bq_of_words(w, scale):
    """The biquad the words hold (what the kernel runs): (b0, b1, b2, a1, a2)."""
    v = [q(x) / scale for x in w]
    return [v[0], v[1], v[2], -v[3], -v[4]]


class Df1:
    """A biquad as the kernels run it (direct form 1, the output limited to full scale), in floating
    point; while nothing reaches full scale it is OpenAL Soft's filter (its transposed form 2) exactly."""
    def __init__(self, c):
        self.c, self.x1, self.x2, self.y1, self.y2 = c, 0.0, 0.0, 0.0, 0.0

    def __call__(self, x):
        b0, b1, b2, a1, a2 = self.c
        y = clip(b0 * x + b1 * self.x1 + b2 * self.x2 - a1 * self.y1 - a2 * self.y2)
        self.x2, self.x1, self.y2, self.y1 = self.x1, x, self.y1, y
        return y


class Df1Exact:
    """The kernels' biquad (their `bq`) word for word: direct form 1, the sum times 2 (`shifts` 1:
    words halved) or 4, plus the last rounding residual, rounded (convergent), limited.
    Input and output are 24-bit words."""
    def __init__(self, words, shifts):
        self.w = [sx(v & 0xFFFFFF) for v in words]
        self.shifts, self.h, self.e = shifts, [0, 0, 0, 0], 0

    def __call__(self, x):
        x = sx(x & 0xFFFFFF)
        b0, b1, b2, na1, na2 = self.w
        x1, x2, y1, y2 = self.h
        acc = 2 * (b0 * x + b1 * x1 + b2 * x2 + na1 * y1 + na2 * y2)
        acc = (acc << self.shifts) + 2 * self.e
        r = acc - (acc & 0xFFFFFF)
        if (acc & 0xFFFFFF) > 0x800000 or ((acc & 0xFFFFFF) == 0x800000 and (r >> 24) & 1):
            r += 1 << 24
        self.e = (acc - r) >> 1
        y = max(-0x800000, min(0x7FFFFF, r >> 24))
        self.h = [x, x1, y, y1]
        return y & 0xFFFFFF


class Tdf2:
    """OpenAL Soft's biquad processing (transposed direct form 2)."""
    def __init__(self, c):
        self.c, self.z1, self.z2 = c, 0.0, 0.0

    def __call__(self, x):
        b0, b1, b2, a1, a2 = self.c
        y = x * b0 + self.z1
        self.z1 = x * b1 - y * a1 + self.z2
        self.z2 = x * b2 - y * a2
        return y


def clip(v):
    return max(-1.0, min(v, 0x7FFFFF / 0x800000))


# ------------------------------------------------------------ running a kernel

def run_wet(source, words, left, right, n, at=AT_FX):
    """The kernel in slot 0 on bins 2 / 3 (`left`, `right`: 24-bit words), the dry bins silent:
    what it adds to OUT L / OUT R (signed 24-bit) sample by sample, and the simulator."""
    gp = kernel_gp(source, at, words)
    gp.y = [0x5A5A5A] * 0x800                 # Y RAM is not cleared by the loader
    o = gp.symbols['OUTL']
    ol, orr, steps = [], [], 0
    for f in range(n // 32):
        gp.frame({0: [0] * 32, 1: [0] * 32, 2: left[32 * f:32 * f + 32], 3: right[32 * f:32 * f + 32]})
        ol += [sx(v) for v in gp.x[o:o + 32]]
        orr += [sx(v) for v in gp.x[o + 32:o + 64]]
        steps = max(steps, gp.frame_steps)
    assert gp.x[gp.symbols['TMOUT']] == 0, 'a DMA wait timed out'
    return ol, orr, steps


def lsb_err(got, model):
    """The largest difference, in 24-bit LSB, between kernel words and a floating-point model."""
    return max(abs(g - m * 0x800000) for g, m in zip(got, model))


def signal(n, seed, level=0.2, tone=440.0):
    """Bin words: two tones (one with a slow swell) and some noise, about `level` at its peaks."""
    rng = random.Random(seed)
    out = []
    for t in range(n):
        v = level * (0.6 * math.sin(2 * math.pi * tone * t / 48000) * (0.6 + 0.4 * math.sin(2 * math.pi * 2 * t / 48000))
                     + 0.25 * math.sin(2 * math.pi * 3150 * t / 48000) + 0.15 * (rng.random() * 2 - 1))
        out.append(q23(v) & 0xFFFFFF)
    return out


# ------------------------------------------------------------ the library's words

LIB = {'exe': None, 'sets': 0}
TYPES = {'ringmod': 0x9, 'distortion': 0x3, 'equalizer': 0xC, 'compressor': 0xB, 'autowah': 0xA}


def build_words_helper(tmp):
    """tests/gpsim_onchip_words.c against the library run_tests.py built (or the sources)."""
    clang = os.environ.get('NXDK_AL_CLANG', 'clang')
    lib = os.environ.get('NXDK_AL_LIB')
    exe = os.path.join(tmp, 'gpsim_onchip_words.exe')
    cmd = [clang, '-std=c99', '-Wall', '-Werror', '-pedantic', '-I', os.path.join(gpt.OPENAL, 'src'),
           '-I', os.path.join(gpt.OPENAL, 'include'), os.path.join(HERE, 'gpsim_onchip_words.c')]
    cmd += [lib] if lib else sorted(glob.glob(os.path.join(gpt.OPENAL, 'src', '*.c')))
    if not sys.platform.startswith('win'):
        cmd.append('-lm')
    subprocess.run(cmd + ['-o', exe], check=True)
    LIB['exe'] = exe


def f32(v):
    """A value as the API's ALfloat holds it."""
    return struct.unpack('<f', struct.pack('<f', float(v)))[0]


def lib_words(kind, args, derive=None):
    """The words the library gives a slot holding the effect `kind` with these EFX values (the
    arguments of this file's <kind>_words, the slot's gain last where it has one), checked against
    `derive` (this file's <kind>_words by default) within 1 LSB each."""
    n_float = {'ringmod': 2, 'distortion': 5, 'equalizer': 10, 'compressor': 0, 'autowah': 4}[kind]
    spec = ['f:%d:%r' % (i + 1, f32(args[i])) for i in range(n_float)]
    rest = list(args[n_float:])
    if kind == 'ringmod':
        spec.append('i:3:%d' % rest.pop(0))
    elif kind == 'compressor':
        spec.append('i:1:%d' % rest.pop(0))
    if rest:
        spec.append('g:%r' % f32(rest[0]))
    out = subprocess.run([LIB['exe'], str(TYPES[kind])] + spec, check=True, capture_output=True, text=True).stdout
    got = [int(w, 16) for w in out.split()]
    a = [f32(v) if i < n_float or i >= len(args) - len(rest) else v for i, v in enumerate(args)]
    mine = (derive or globals()[kind + '_words'])(*a)
    mine = [w & 0xFFFFFF for w in mine] + [0] * (32 - len(mine))
    bad = [i for i in range(32) if abs(sx(got[i]) - sx(mine[i])) > 1]
    assert not bad, '%s %s: the library\'s words %s, this file\'s %s' % (
        kind, args, [hex(got[i]) for i in bad], [hex(mine[i]) for i in bad])
    LIB['sets'] += 1
    return got


# ------------------------------------------------------------ 1 ring modulator

def ringmod_words(freq, cutoff, wave):
    """The library's words (al_efx.c ringmod_params)."""
    step = min(int(math.floor(freq / 48000.0 * (1 << 24) + 0.5)), (1 << 24) - 1)
    hp = [q23(0.5), 0, 0, 0, 0] if cutoff < 1.0 else \
        bq_words(biquad('highpass', min(cutoff / 48000.0, 0.49), 1.0, rcpq_from_slope(1.0, 0.75)), 0.5)
    return [step, 3 if step == 0 else wave] + hp


def ringmod_model(words, left, right, n):
    """OpenAL Soft's modulator with the kernel's words: each channel highpassed, times the waveform."""
    step, wave = words[0], words[1]
    idx, w = 0, []
    for _ in range(n):
        idx = (idx + step) & 0xFFFFFF
        w.append([math.sin(idx * 2 * math.pi / (1 << 24)), idx * 2.0 / (1 << 24) - 1.0,
                  1.0 if idx & 0x800000 else -1.0, 1.0][wave])
    out = []
    for b in (left, right):
        hp = Df1(bq_of_words(words[2:7], 0.5))
        out.append([hp(clip(2 * q(b[t]))) * w[t] for t in range(n)])
    return out


def test_ringmod():
    n = 32 * 120
    left, right = signal(n, 11), signal(n, 12, 0.15, 660.0)
    worst, steps = 0, 0
    for freq, cutoff, wave in ((440, 800, 0), (1234.5, 150, 0), (90, 2000, 1), (3000, 800, 2), (0, 800, 0),
                               (440, 0, 0), (8000, 24000, 0), (17, 5000, 0)):
        words = lib_words('ringmod', (freq, cutoff, wave))
        ol, orr, st = run_wet(EFX_RINGMOD, words, left, right, n)
        ml, mr = ringmod_model(words, left, right, n)
        err = max(lsb_err(ol, ml), lsb_err(orr, mr))
        print('    ring modulator %s: %d LSB (24-bit), %d instructions' % ((freq, cutoff, wave), err, st))
        assert err <= 4 * 256, 'ring modulator %s: %d LSB (24-bit) from the model' % ((freq, cutoff, wave), err)
        worst, steps = max(worst, err), max(steps, st)
    print('[1] EFX ring modulator: sine / sawtooth / square / none, highpass, both channels against OpenAL '
          'Soft\'s algorithm in floating point, within %.2f LSB of 16 bits; %d instructions per frame [PASS]'
          % (worst / 256.0, steps))


# ------------------------------------------------------------ 2 distortion

def distortion_words(edge, gain, lp_cutoff, eq_center, eq_bw, slot_gain=1.0):
    """The library's words (al_efx.c distortion_params): the filters at 48 kHz with the Q
    OpenAL Soft's 4x-oversampled design gets; the folded waveshaper's d."""
    e = min(math.sin(math.pi / 2 * edge), 0.99)
    d = ((1 - e) / (1 + e)) ** 3
    rq = rcpq_from_bandwidth(lp_cutoff / 48000.0 / 4, (lp_cutoff / 2) / (lp_cutoff * 0.67))
    lp = bq_words(biquad('lowpass', min(lp_cutoff / 48000.0, 0.45), 1.0, rq), 0.5)
    rq = min(rcpq_from_bandwidth(eq_center / 48000.0 / 4, min(eq_bw / (eq_center * 0.67), 20.0)), 20.0)
    c = biquad('bandpass', min(eq_center / 48000.0, 0.45), 1.0, rq)
    g = gain * slot_gain
    bp = bq_words([c[0] * g, c[1] * g, c[2] * g, c[3], c[4]], 0.5)
    sd = q23(d)
    return lp + bp + [sd, min(q23(1 - d), 0x7FFFFF - sd)]


def soft_shaper(x, edge):
    """OpenAL Soft's three waveshaper stages, as it writes them."""
    e = min(math.sin(math.pi / 2 * edge), 0.99)
    fc = 2 * e / (1 - e)
    x = (1 + fc) * x / (1 + fc * abs(x))
    x = (1 + fc) * x / (1 + fc * abs(x)) * -1
    return (1 + fc) * x / (1 + fc * abs(x))


def distortion_model(words, left, right, n, exact_lp=False):
    """OpenAL Soft's distortion (at 48 kHz) with the kernel's words: lowpass, waveshaper, bandpass, gain.
    exact_lp: the lowpass as the kernel computes it, word for word (for edges near 1, where the shaper's
    slope at 0 reaches 2^23: one LSB of v at a zero crossing is then a full-scale step of its output)."""
    lp, bp = Df1(bq_of_words(words[0:5], 0.5)), Df1(bq_of_words(words[5:10], 0.5))
    lpx = Df1Exact(words[0:5], 1)
    d, d1 = q(words[10]), q(words[11])
    out = []
    for t in range(n):
        m = clip(q(left[t]) + q(right[t]))
        v = q(lpx(q23(m))) if exact_lp else lp(m)
        u = abs(v)
        s = clip(u / (d + d1 * u))
        out.append(bp(-s if v >= 0 else s))
    return out, out


def test_distortion():
    # the three stages compose to one: x / (d + (1 - d) |x|), negated, d = ((1 - e) / (1 + e))^3
    for edge in (0.0, 0.2, 0.5, 0.8, 1.0):
        e = min(math.sin(math.pi / 2 * edge), 0.99)
        d = ((1 - e) / (1 + e)) ** 3
        for k in range(-200, 201):
            x = k / 200.0
            assert abs(soft_shaper(x, edge) + x / (d + (1 - d) * abs(x))) < 1e-9, (edge, x)
    n = 32 * 100
    left, right = signal(n, 21, 0.25), signal(n, 22, 0.2, 700.0)
    worst, steps = 0, 0
    for args in ((0.2, 1.0, 8000, 3600, 3600), (0.2, 0.05, 8000, 3600, 3600), (0.0, 1.0, 8000, 3600, 3600),
                 (0.5, 1.0, 4000, 1000, 500), (0.8, 1.0, 8000, 3600, 3600), (0.9, 1.0, 24000, 200, 80),
                 (1.0, 1.0, 80, 24000, 24000), (0.35, 0.05, 12000, 6000, 2000)):
        words = lib_words('distortion', args)
        ol, orr, st = run_wet(EFX_DISTORTION, words, left, right, n)
        ml, mr = distortion_model(words, left, right, n, args[0] >= 0.9)
        err = max(lsb_err(ol, ml), lsb_err(orr, mr))
        print('    distortion %s: %d LSB (24-bit), %d instructions' % (args, err, st))
        assert ol == orr, 'distortion: the same on both sides'
        assert err <= 4 * 256, 'distortion %s: %d LSB (24-bit) from the model' % (args, err)
        worst, steps = max(worst, err), max(steps, st)
    print('[2] EFX distortion: lowpass, the folded waveshaper (= OpenAL Soft\'s three stages), bandpass, gain, '
          'within %.2f LSB of 16 bits of the floating-point model; %d instructions per frame [PASS]'
          % (worst / 256.0, steps))


# ------------------------------------------------------------ 3 equalizer

def equalizer_stages(lg, lc, m1g, m1c, m1w, m2g, m2c, m2w, hg, hc):
    """OpenAL Soft's four filters (gains are the shelf / peak's own: their square root goes to the
    design), the bands at gain 1 left out."""
    out = []
    if lg != 1.0:
        g = math.sqrt(lg)
        out.append(biquad('lowshelf', lc / 48000.0, g, rcpq_from_slope(g, 0.75)))
    for gain, c, w in ((m1g, m1c, m1w), (m2g, m2c, m2w)):
        if gain != 1.0:
            out.append(biquad('peaking', c / 48000.0, math.sqrt(gain), rcpq_from_bandwidth(c / 48000.0, w)))
    if hg != 1.0:
        g = math.sqrt(hg)
        out.append(biquad('highshelf', hc / 48000.0, g, rcpq_from_slope(g, 0.75)))
    return out


def equalizer_words(*args):
    """The library's words (al_efx.c equalizer_params)."""
    st = equalizer_stages(*args)
    return [len(st)] + [w for c in st for w in bq_words(c, 0.25)]


def equalizer_model(words, left, right, n):
    out = []
    for b in (left, right):
        st = [Df1(bq_of_words(words[1 + 5 * k:6 + 5 * k], 0.25)) for k in range(words[0])]
        o = []
        for t in range(n):
            v = clip(2 * q(b[t]))
            for f in st:
                v = f(v)
            o.append(v)
        out.append(o)
    return out


def test_equalizer():
    n = 32 * 100
    left, right = signal(n, 31, 0.1), signal(n, 32, 0.08, 120.0)
    worst, steps, flat = 0, 0, 0
    for args in ((1.0, 200, 1.0, 500, 1.0, 1.0, 3000, 1.0, 1.0, 6000),          # the defaults: no stage
                 (2.0, 200, 1.0, 500, 1.0, 1.0, 3000, 1.0, 1.0, 6000),
                 (7.943, 50, 0.126, 500, 1.0, 3.0, 3000, 0.3, 0.5, 6000),
                 (0.126, 800, 7.943, 3000, 0.01, 0.2, 8000, 1.0, 7.943, 16000),
                 (0.5, 120, 2.0, 200, 0.5, 0.5, 1000, 0.05, 2.0, 4000)):
        words = lib_words('equalizer', args)
        ol, orr, st = run_wet(EFX_EQUALIZER, words, left, right, n)
        ml, mr = equalizer_model(words, left, right, n)
        err = max(lsb_err(ol, ml), lsb_err(orr, mr))
        print('    equalizer %s (%d stages): %d LSB (24-bit), %d instructions' % (args, words[0], err, st))
        assert err <= 4 * 256, 'equalizer %s: %d LSB (24-bit) from the model' % (args, err)
        if words[0] == 0:
            flat = st
        worst, steps = max(worst, err), max(steps, st)
    print("[3] EFX equalizer: low shelf, two peaks, high shelf per channel against OpenAL Soft's filters, "
          'within %.2f LSB of 16 bits; %d instructions per frame with four bands (%d with none) [PASS]'
          % (worst / 256.0, steps, flat))

def main():
    with tempfile.TemporaryDirectory(prefix='gponchip_') as tmp:
        build_words_helper(tmp)
        test_ringmod()
        test_distortion()
        test_equalizer()
    print('    (%d sets of words: the library\'s = this file\'s derivation, within 1 LSB a word)' % LIB['sets'])
    print('=== on-chip EFX effects (filters) on the host: all passed ===')


if __name__ == '__main__':
    main()
