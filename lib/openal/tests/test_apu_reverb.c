#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "apu_reverb.h"
#include "apu_gp_reverb.h"   /* dsp/gp_reverb.asm through tools/dsp56asm.py */

/*
 * Reverb on the GP (apu_reverb.c): the X-memory layout dsp/gp_reverb.asm
 * reads (chains, line buffers, tables) and the host reference of its
 * arithmetic. The reference matched the program run in xemu's DSP
 * interpreter bit for bit (impulse and sustained input); here it checks the
 * acoustics: delays, decay time, damping and dry pass-through.
 */

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
        int32_t bins[3][32], out[2][32];
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
    n = apu_reverb_layout(&p, w);
    assert(n <= APU_REVERB_MAX_WORDS);
    apu_reverb_line_lengths(&p, len);
    assert(len[0] == 960u);                                   /* 20 ms pre-delay */
    for (k = 0; k < APU_REVERB_LINES; k++) {
        uint32_t r = 0x100u + 16u * k, wd = r + 8u, off, size, off2, size2;
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
        assert(find(w, n, wd + 0u) == ((k + 1u < APU_REVERB_LINES) ? wd + 16u : 0x08u));   /* writes end at the FIFO */
        assert(find(w, n, r + 1u) == (0x9D0u | (1u << 14)));  /* dsp_step 1 (console: rounds 1-2) */
        assert(find(w, n, wd + 1u) == (0x9D2u | (1u << 14)));
        assert(find(w, n, r + 2u) == 32u && find(w, n, wd + 2u) == 32u);
        assert(find(w, n, r + 3u) == 0x300u + 64u * k && find(w, n, wd + 3u) == 0x320u + 64u * k);
        assert(find(w, n, wd + 4u) - find(w, n, r + 4u) == 4u * len[k]);   /* the delay, in bytes */
        assert(find(w, n, r + 5u) == off && find(w, n, wd + 6u) == size - 1u);
    }
    assert(find(w, n, 0x08u) == 0x4000u && find(w, n, 0x0Bu) == 0x200u);
    assert(find(w, n, 0x0Au) == ((32u << 4) | 1u));
    assert(apu_reverb_scratch_bytes() + APU_REVERB_SCRATCH_BASE <= 2u * 1024u * 1024u);
    assert(GP_REVERB_WORDS <= 0x800u);
    assert(GP_REVERB_CTAB == 0x28u && GP_REVERB_ATAB == 0x58u && GP_REVERB_RCHAIN == 0x100u);
    assert(GP_REVERB_FLAGS == 0x11u && GP_REVERB_CNT == 0x10u);
    printf("    -> 13 lines, chains end at EOL and at the FIFO descriptor, buffers apart [PASS]\n");

    printf("[2] Coefficients...\n");
    assert(apu_reverb_q23(0.5f) == 0x400000u && apu_reverb_q23(-1.0f) == 0x800000u);
    assert(apu_reverb_q23(1.0f) == 0x7FFFFFu && apu_reverb_q23(-2.0f) == 0x800000u);
    {
        uint32_t g_short = find(w, n, 0x28u + 3u), g_long = find(w, n, 0x28u + 6u * 7u + 3u);
        assert(g_short > g_long && g_short < 0x7FFFFFu);      /* a longer comb decays more per pass */
        p.decay_time = 3.0f;
        n = apu_reverb_coefficients(&p, w);
        assert(find(w, n, 0x28u + 3u) > g_short);              /* longer decay, more feedback */
        p.hf_ratio = 1.0f;
        n = apu_reverb_coefficients(&p, w);
        assert(find(w, n, 0x28u + 4u) == 0);                  /* no damping */
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
            int32_t bins[3][32], out[2][32];
            for (i = 0; i < 32u; i++) {
                bins[0][i] = (int32_t)((f * 32u + i) * 977u % 0x7FFFFu) - 0x40000;
                bins[1][i] = -bins[0][i];
                bins[2][i] = 0;
            }
            apu_reverb_ref_frame(&r, bins, out);
            for (i = 0; i < 32u; i++) {
                assert(out[0][i] == bins[0][i] && out[1][i] == bins[1][i]);   /* no send: dry passes exactly */
            }
        }
        apu_reverb_ref_free(&r);
    }
    printf("    -> with no send the output is the dry mix, bit for bit [PASS]\n");

    printf("=== All GP Reverb Tests Passed Successfully! ===\n");
    return 0;
}
