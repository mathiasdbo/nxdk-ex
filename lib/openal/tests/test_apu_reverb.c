#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "apu_reverb.h"
#include "apu_gp_kfreeverb.h"   /* dsp/gp_kfreeverb.asm through tools/dsp56asm.py */
#include "apu_gp_loop.h"        /* dsp/gp_loop.asm */

/*
 * Reverb on the GP (apu_reverb.c): the X-memory layout dsp/gp_reverb.asm
 * reads (chains, line buffers, tables) and the host reference of its
 * arithmetic. The reference matched the program run in xemu's DSP
 * interpreter bit for bit (impulse and sustained input); here it checks the
 * acoustics: delays, decay time, damping and dry pass-through.
 */

#define XB  0x1A0u      /* the reverb slot's X block (anywhere: the kernel is relocatable) */
#define SCR 0x40000u    /* and its scratch */

static uint32_t find(const apu_reverb_word_t *w, uint32_t n, uint32_t addr) {
    uint32_t i, found = 0xFFFFFFFFu;
    for (i = 0; i < n; i++) {
        if (w[i].addr == addr) {
            assert(found == 0xFFFFFFFFu);   /* each word once */
            found = w[i].value;
        }
    }
    assert(found != 0xFFFFFFFFu);
    return found;
}

/* Energy of the left output in [from, to) samples after an impulse in the send */
static double run_energy(const apu_reverb_params_t *p, uint32_t frames, uint32_t from, uint32_t to, double *peak_out,
                         uint32_t *first_out) {
    apu_reverb_ref_t r;
    double e = 0.0, peak = 0.0;
    uint32_t f, k, first = 0xFFFFFFFFu;
    assert(apu_reverb_ref_init(&r, p) == 0);
    for (f = 0; f < frames; f++) {
        int32_t bins[4][32], out[2][32];
        memset(bins, 0, sizeof(bins));
        if (f == 0) bins[2][0] = 0x400000;
        apu_reverb_ref_frame(&r, bins, out);
        for (k = 0; k < 32u; k++) {
            uint32_t t = f * 32u + k;
            double v = out[0][k] / 8388608.0;
            if (out[0][k] && first == 0xFFFFFFFFu) first = t;
            if (t >= from && t < to) e += v * v;
            if (fabs(v) > peak) peak = fabs(v);
        }
    }
    apu_reverb_ref_free(&r);
    if (peak_out) *peak_out = peak;
    if (first_out) *first_out = first;
    return e;
}

int main(void) {
    apu_reverb_params_t p;
    apu_reverb_word_t w[APU_REVERB_MAX_WORDS];
    uint32_t n, k, len[APU_REVERB_LINES];

    printf("=== GP reverb ===\n");
    printf("[1] Layout...\n");
    apu_reverb_default_params(&p);
    n = apu_reverb_layout(&p, XB, SCR, w);                    /* the slot's X block and scratch */
    assert(n <= APU_REVERB_MAX_WORDS);
    apu_reverb_line_lengths(&p, len);
    assert(len[0] == 960u);                                   /* 20 ms pre-delay */
    for (k = 0; k < APU_REVERB_LINES; k++) {
        uint32_t r = XB + GP_KFREEVERB_RCHAIN + 16u * k, wd = r + 8u, off, size, off2, size2;
        apu_reverb_line_buffer(k, &off, &size);
        assert(len[k] >= APU_REVERB_MIN_PRE);                 /* never reads the frame it writes */
        assert(size % 4096u == 0 && size >= 4u * (len[k] + 64u));
        assert(off >= APU_REVERB_SCRATCH_BASE);
        if (k + 1u < APU_REVERB_LINES) {
            apu_reverb_line_buffer(k + 1u, &off2, &size2);
            assert(off2 == off + size);                       /* no overlap */
        }
        assert(off + size <= APU_REVERB_SCRATCH_BASE + apu_reverb_scratch_bytes());
        assert(find(w, n, r + 0u) == ((k + 1u < APU_REVERB_LINES) ? r + 16u : 0x4000u));
        assert(find(w, n, wd + 0u) == ((k + 1u < APU_REVERB_LINES) ? wd + 16u : 0x4000u));   /* both end at EOL */
        assert(find(w, n, r + 1u) == (0x9D0u | (1u << 14)));  /* dsp_step 1 (console: rounds 1-2) */
        assert(find(w, n, wd + 1u) == (0x9D2u | (1u << 14)));
        assert(find(w, n, r + 2u) == 32u && find(w, n, wd + 2u) == 32u);
        assert(find(w, n, r + 3u) == XB + GP_KFREEVERB_PRE_IN + 64u * k);
        assert(find(w, n, wd + 3u) == XB + GP_KFREEVERB_PRE_OUT + 64u * k);
        assert(find(w, n, wd + 4u) - find(w, n, r + 4u) == 4u * len[k]);   /* the delay, in bytes */
        assert(find(w, n, r + 5u) == off - APU_REVERB_SCRATCH_BASE + SCR && find(w, n, wd + 6u) == size - 1u);
    }
    /* chain heads in the slot's X block (ABI 2: KX + 3, KX + 4); KIN and the loop's descriptors are the loader's */
    assert(find(w, n, XB + 3u) == XB + GP_KFREEVERB_RCHAIN && find(w, n, XB + 4u) == XB + GP_KFREEVERB_WCHAIN);
    for (k = 0; k < n; k++) {
        assert(w[k].addr != XB + GP_KFREEVERB_KIN && w[k].addr >= GP_LOOP_OUTLP);
        assert(w[k].addr < GP_LOOP_OUTST + 4u || (w[k].addr >= XB && w[k].addr < XB + GP_KFREEVERB_K_XWORDS));
    }
    assert(apu_reverb_scratch_bytes() + APU_REVERB_SCRATCH_BASE <= 2u * 1024u * 1024u);
    assert(GP_KFREEVERB_K_SCRATCH_BYTES == apu_reverb_scratch_bytes());   /* the image header says the same */
    /* The kernel's X words (offsets from its X block) as apu_reverb.c lays them out */
    assert(GP_LOOP_FIFO2 == 0x08u && GP_LOOP_OUTL == 0x20u && GP_LOOP_OUTR == 0x40u);
    assert(GP_LOOP_OUTLP == 0x1Bu && GP_LOOP_OUTST == 0x1Cu && GP_LOOP_OUTST + 4u <= GP_LOOP_OUTL);
    assert(GP_KFREEVERB_KIN == 0u && GP_KFREEVERB_INGAIN == 0x28u && GP_KFREEVERB_WET == 0x2Bu);
    assert(GP_KFREEVERB_APG == 0x2Cu && GP_KFREEVERB_INST == 0x2Au && GP_KFREEVERB_INLP == 0x29u);
    assert(GP_KFREEVERB_CTAB == 0x30u && GP_KFREEVERB_ATAB == 0x90u && GP_KFREEVERB_RCHAIN == 0xF0u);
    assert(GP_KFREEVERB_ACCL == 0xB0u && GP_KFREEVERB_ACCR == 0xD0u && GP_KFREEVERB_PRE_IN == 0x280u);
    assert(GP_KFREEVERB_WCHAIN == GP_KFREEVERB_RCHAIN + 8u && GP_KFREEVERB_PRE_OUT == GP_KFREEVERB_PRE_IN + 32u);
    assert(GP_KFREEVERB_OUTL == GP_LOOP_OUTL);
    assert(GP_KFREEVERB_PRE_OUT + 64u * (APU_REVERB_LINES - 1u) + 32u == GP_KFREEVERB_K_XWORDS);   /* the X block's end */
    assert(GP_KFREEVERB_WCHAIN + 16u * (APU_REVERB_LINES - 1u) + 7u <= GP_KFREEVERB_PRE_IN);   /* descriptors below the blocks */
    assert(GP_KFREEVERB_K_DESCRIPTORS == 2u * APU_REVERB_LINES && GP_KFREEVERB_K_DESCRIPTORS <= 60u);
    assert(GP_LOOP_FLAGS == 0x11u && GP_LOOP_CNT == 0x10u);
    printf("    -> 25 lines in the slot's X block and scratch, both chains end at EOL, buffers apart [PASS]\n");

    printf("[2] Coefficients...\n");
    assert(apu_reverb_q23(0.5f) == 0x400000u && apu_reverb_q23(-1.0f) == 0x800000u);
    assert(apu_reverb_q23(1.0f) == 0x7FFFFFu && apu_reverb_q23(-2.0f) == 0x800000u);
    {
        uint32_t g_short = find(w, n, XB + GP_KFREEVERB_CTAB + 3u), g_long = find(w, n, XB + GP_KFREEVERB_CTAB + 6u * 15u + 3u);
        assert(g_short > g_long && g_short < 0x7FFFFFu);      /* a longer comb decays more per pass */
        p.decay_time = 3.0f;
        n = apu_reverb_coefficients(&p, XB, w);
        assert(find(w, n, XB + GP_KFREEVERB_CTAB + 3u) > g_short);              /* longer decay, more feedback */
        p.hf_ratio = 1.0f;
        n = apu_reverb_coefficients(&p, XB, w);
        assert(find(w, n, XB + GP_KFREEVERB_CTAB + 4u) == 0);                  /* no damping */
    }
    printf("    -> feedback follows the decay time, damping the HF ratio [PASS]\n");

    printf("[3] Reference: impulse response...\n");
    {
        double e_early, e_late, peak;
        uint32_t first;
        apu_reverb_default_params(&p);
        p.decay_time = 1.0f;
        p.input_gain = 1.0f;     /* level well above the 24-bit floor */
        p.hf_ratio = 1.0f;       /* no damping: the decay time holds at all frequencies */
        e_early = run_energy(&p, 1500u, 4800u, 9600u, &peak, &first);          /* 0.1-0.2 s */
        e_late = run_energy(&p, 1500u, 4800u + 24000u, 9600u + 24000u, NULL, NULL);   /* 0.6-0.7 s */
        /* the first output: pre-delay + the shortest comb (allpasses pass their input at once) */
        assert(first == 960u + 1215u);
        assert(peak > 0.01 && peak < 1.0);
        {
            double db = 10.0 * log10(e_late / e_early);
            printf("    0.5 s later: %.1f dB (T60 1.0 s, -30 expected)\n", db);
            assert(db < -24.0 && db > -36.0);
        }
        p.decay_time = 3.0f;
        e_early = run_energy(&p, 1500u, 4800u, 9600u, NULL, NULL);
        e_late = run_energy(&p, 1500u, 4800u + 24000u, 9600u + 24000u, NULL, NULL);
        printf("    0.5 s later: %.1f dB (T60 3.0 s, -10 expected)\n", 10.0 * log10(e_late / e_early));
        assert(10.0 * log10(e_late / e_early) < -6.0 && 10.0 * log10(e_late / e_early) > -14.0);
    }
    printf("    -> echoes start after pre-delay + shortest comb, decay follows T60 [PASS]\n");

    printf("[4] Reference: dry signal and silence...\n");
    {
        apu_reverb_ref_t r;
        uint32_t f, i;
        apu_reverb_default_params(&p);
        assert(apu_reverb_ref_init(&r, &p) == 0);
        for (f = 0; f < 200u; f++) {
            int32_t bins[4][32], out[2][32];
            for (i = 0; i < 32u; i++) {
                bins[0][i] = (int32_t)((f * 32u + i) * 977u % 0x7FFFFu) - 0x40000;
                bins[1][i] = -bins[0][i];
                bins[2][i] = bins[3][i] = 0;
            }
            apu_reverb_ref_frame(&r, bins, out);
            for (i = 0; i < 32u; i++) {
                assert(out[0][i] == bins[0][i] && out[1][i] == bins[1][i]);   /* no send: dry passes exactly */
            }
        }
        apu_reverb_ref_free(&r);
    }
    printf("    -> with no send the output is the dry mix, bit for bit [PASS]\n");

    printf("[5] Output lowpass (underwater)...\n");
    {
        apu_reverb_ref_t r;
        uint32_t f, i;
        double e_hi = 0.0, e_lo = 0.0, e_hi_in = 0.0, e_lo_in = 0.0;
        assert(apu_reverb_preset(7, &p) == 0 && p.output_lowpass > 0.9f);
        p.gain = 0.0f;                                      /* the dry path alone */
        assert(apu_reverb_ref_init(&r, &p) == 0);
        for (f = 0; f < 300u; f++) {
            int32_t bins[4][32], out[2][32];
            for (i = 0; i < 32u; i++) {
                uint32_t t = f * 32u + i;
                int32_t lo = (int32_t)(0x200000 * sin(2.0 * 3.14159265 * 100.0 * t / 48000.0));
                int32_t hi = (t & 1u) ? 0x100000 : -0x100000;   /* 24 kHz */
                bins[0][i] = lo;                            /* left: 100 Hz */
                bins[1][i] = hi;                            /* right: Nyquist */
                bins[2][i] = bins[3][i] = 0;
                if (f >= 100u) {
                    e_lo_in += (double)lo * lo;
                    e_hi_in += (double)hi * hi;
                }
            }
            apu_reverb_ref_frame(&r, bins, out);
            for (i = 0; i < 32u && f >= 100u; i++) {
                e_lo += (double)out[0][i] * out[0][i];
                e_hi += (double)out[1][i] * out[1][i];
            }
        }
        apu_reverb_ref_free(&r);
        printf("    100 Hz: %.1f dB, 24 kHz: %.1f dB\n", 10.0 * log10(e_lo / e_lo_in), 10.0 * log10(e_hi / e_hi_in));
        assert(10.0 * log10(e_lo / e_lo_in) > -3.0);         /* the body passes */
        assert(10.0 * log10(e_hi / e_hi_in) < -40.0);        /* the top is gone */
    }
    printf("    -> the whole mix is muffled, the low end kept [PASS]\n");

    printf("=== All GP Reverb Tests Passed Successfully! ===\n");
    return 0;
}
