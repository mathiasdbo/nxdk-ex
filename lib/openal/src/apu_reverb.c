#include "apu_reverb.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Program addresses (dsp/gp_reverb.asm) */
#define X_INGAIN   0x20u
#define X_INLP     0x21u
#define X_INST     0x22u
#define X_WET      0x23u
#define X_APG      0x24u
#define X_CTAB     0x28u
#define X_ATAB     0x58u
#define X_FIFO2    0x08u
#define X_RCHAIN   0x100u
#define X_OUTL     0x200u
#define X_ACCL     0x240u
#define X_ACCR     0x260u
#define X_IN(k)    (0x300u + 64u * (k))
#define X_OUT(k)   (0x320u + 64u * (k))

#define EOL        0x004000u
#define STEP1      (1u << 14)
#define CTL_R      (0x0009D0u | STEP1)   /* memory -> DSP, write-back, circular scratch, 24-bit in bits 31:8 */
#define CTL_W      (0x0009D2u | STEP1)   /* DSP -> memory, the same */
#define CTL_FIFO   (1u | 2u | (1u << 10) | (32u << 14))   /* interleaved, to FIFO 0, 16-bit, channel stride 32 */

/* Delays in samples at 48 kHz: combs left then right, allpasses left then right.
 * Mutually prime-ish lengths in the usual Schroeder/Moorer range (25-37 ms
 * combs, 5-13 ms allpasses), the right ear offset from the left. */
static const uint16_t k_comb_len[APU_REVERB_COMBS] = { 1215, 1390, 1548, 1695, 1293, 1476, 1623, 1760 };
static const uint16_t k_ap_len[APU_REVERB_ALLPASSES] = { 605, 371, 480, 245 };

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

uint32_t apu_reverb_q23(float v) {
    double s = floor((double)v * 8388608.0 + 0.5);
    if (s > 8388607.0) s = 8388607.0;
    if (s < -8388608.0) s = -8388608.0;
    return (uint32_t)(int32_t)s & 0xFFFFFFu;
}

void apu_reverb_default_params(apu_reverb_params_t *p) {
    p->decay_time = 1.5f;
    p->hf_ratio = 0.6f;
    p->gain = 0.5f;
    p->pre_delay = 0.02f;
    p->diffusion = 0.6f;
    p->input_gain = 0.12f;
    p->input_lowpass = 0.3f;
}

static uint32_t pre_samples(const apu_reverb_params_t *p) {
    uint32_t n = (uint32_t)(clampf(p->pre_delay, 0.0f, 0.1f) * (float)APU_REVERB_RATE + 0.5f);
    if (n < APU_REVERB_MIN_PRE) n = APU_REVERB_MIN_PRE;
    if (n > APU_REVERB_MAX_PRE) n = APU_REVERB_MAX_PRE;
    return n;
}

void apu_reverb_line_lengths(const apu_reverb_params_t *p, uint32_t len[APU_REVERB_LINES]) {
    uint32_t i;
    len[0] = pre_samples(p);
    for (i = 0; i < APU_REVERB_COMBS; i++) len[1u + i] = k_comb_len[i];
    for (i = 0; i < APU_REVERB_ALLPASSES; i++) len[1u + APU_REVERB_COMBS + i] = k_ap_len[i];
}

/* A line's buffer holds its delay plus two blocks, rounded up to 4 KiB */
static uint32_t buffer_bytes(uint32_t k) {
    uint32_t n = (k == 0u) ? APU_REVERB_MAX_PRE : (k <= APU_REVERB_COMBS ? k_comb_len[k - 1u]
                                                                          : k_ap_len[k - 1u - APU_REVERB_COMBS]);
    return (4u * (n + 64u) + 4095u) & ~4095u;
}

void apu_reverb_line_buffer(uint32_t k, uint32_t *offset, uint32_t *size) {
    uint32_t i, off = APU_REVERB_SCRATCH_BASE;
    for (i = 0; i < k; i++) off += buffer_bytes(i);
    if (offset) *offset = off;
    if (size) *size = buffer_bytes(k);
}

uint32_t apu_reverb_scratch_bytes(void) {
    uint32_t off, size;
    apu_reverb_line_buffer(APU_REVERB_LINES - 1u, &off, &size);
    return off + size - APU_REVERB_SCRATCH_BASE;
}

static void put(apu_reverb_word_t *out, uint32_t *n, uint32_t addr, uint32_t v) {
    if (*n < APU_REVERB_MAX_WORDS) {
        out[*n].addr = (uint16_t)addr;
        out[*n].value = v & 0xFFFFFFu;
    }
    (*n)++;
}

static float comb_gain(const apu_reverb_params_t *p, uint32_t len) {
    float t60 = clampf(p->decay_time, 0.1f, 20.0f);
    return clampf(powf(10.0f, -3.0f * (float)len / (t60 * (float)APU_REVERB_RATE)), 0.0f, 0.98f);
}

uint32_t apu_reverb_coefficients(const apu_reverb_params_t *p, apu_reverb_word_t *out) {
    uint32_t n = 0, c;
    float damp = clampf(0.8f * (1.0f - clampf(p->hf_ratio, 0.1f, 1.0f)), 0.0f, 0.8f);
    put(out, &n, X_INGAIN, apu_reverb_q23(clampf(p->input_gain, 0.0f, 1.0f)));
    put(out, &n, X_INLP, apu_reverb_q23(clampf(p->input_lowpass, 0.0f, 0.95f)));
    put(out, &n, X_WET, apu_reverb_q23(clampf(p->gain, 0.0f, 1.0f)));
    put(out, &n, X_APG, apu_reverb_q23(0.3f + 0.4f * clampf(p->diffusion, 0.0f, 1.0f)));
    for (c = 0; c < APU_REVERB_COMBS; c++) {
        put(out, &n, X_CTAB + 6u * c + 3u, apu_reverb_q23(comb_gain(p, k_comb_len[c])));
        put(out, &n, X_CTAB + 6u * c + 4u, apu_reverb_q23(damp));
    }
    return n;
}

uint32_t apu_reverb_layout(const apu_reverb_params_t *p, apu_reverb_word_t *out) {
    uint32_t len[APU_REVERB_LINES], n, k, c, a;
    apu_reverb_line_lengths(p, len);
    n = apu_reverb_coefficients(p, out);
    put(out, &n, X_INST, 0);
    /* Line k: the read descriptor trails the write one by the delay */
    for (k = 0; k < APU_REVERB_LINES; k++) {
        uint32_t off, size, r = X_RCHAIN + 16u * k, w = r + 8u;
        apu_reverb_line_buffer(k, &off, &size);
        put(out, &n, r + 0u, (k + 1u < APU_REVERB_LINES) ? r + 16u : EOL);
        put(out, &n, r + 1u, CTL_R);
        put(out, &n, r + 2u, 32u);
        put(out, &n, r + 3u, X_IN(k));
        put(out, &n, r + 4u, 0u);
        put(out, &n, r + 5u, off);
        put(out, &n, r + 6u, size - 1u);
        put(out, &n, w + 0u, (k + 1u < APU_REVERB_LINES) ? w + 16u : X_FIFO2);
        put(out, &n, w + 1u, CTL_W);
        put(out, &n, w + 2u, 32u);
        put(out, &n, w + 3u, X_OUT(k));
        put(out, &n, w + 4u, 4u * len[k]);
        put(out, &n, w + 5u, off);
        put(out, &n, w + 6u, size - 1u);
    }
    /* The output descriptor for OUT L/R */
    put(out, &n, X_FIFO2 + 0u, EOL);
    put(out, &n, X_FIFO2 + 1u, CTL_FIFO);
    put(out, &n, X_FIFO2 + 2u, (32u << 4) | 1u);
    put(out, &n, X_FIFO2 + 3u, X_OUTL);
    put(out, &n, X_FIFO2 + 4u, 0u);
    put(out, &n, X_FIFO2 + 5u, 0u);
    put(out, &n, X_FIFO2 + 6u, 0u);
    for (c = 0; c < APU_REVERB_COMBS; c++) {
        uint32_t e = X_CTAB + 6u * c;
        put(out, &n, e + 0u, X_IN(1u + c));
        put(out, &n, e + 1u, X_OUT(1u + c));
        put(out, &n, e + 2u, c < APU_REVERB_COMBS / 2u ? X_ACCL : X_ACCR);
        put(out, &n, e + 5u, 0u);
    }
    for (a = 0; a < APU_REVERB_ALLPASSES; a++) {
        uint32_t e = X_ATAB + 3u * a, k2 = 1u + APU_REVERB_COMBS + a;
        put(out, &n, e + 0u, X_IN(k2));
        put(out, &n, e + 1u, X_OUT(k2));
        put(out, &n, e + 2u, a < APU_REVERB_ALLPASSES / 2u ? X_ACCL : X_ACCR);
    }
    return n;
}

/* ---------------------------------------------------------------- reference */

typedef int64_t acc_t;   /* 56-bit accumulator: value << 24 */

static int32_t sx24(uint32_t v) {
    return (int32_t)(v << 8) >> 8;
}

static acc_t ld(int32_t v) {
    return (acc_t)v * 16777216;
}

/* Fractional product of two 24-bit words, aligned like the accumulator */
static acc_t prod(int32_t x, int32_t y) {
    return (acc_t)x * (acc_t)y * 2;
}

/* Accumulator to a 24-bit word: the top word, saturated if it does not fit */
static int32_t lim(acc_t a) {
    if (a >= ((acc_t)1 << 47)) return 0x7FFFFF;
    if (a < -((acc_t)1 << 47)) return -0x800000;
    return (int32_t)(a >> 24);
}

int apu_reverb_ref_init(apu_reverb_ref_t *r, const apu_reverb_params_t *p) {
    apu_reverb_word_t w[APU_REVERB_MAX_WORDS];
    uint32_t k, n, i;
    memset(r, 0, sizeof(*r));
    apu_reverb_line_lengths(p, r->len);
    for (k = 0; k < APU_REVERB_LINES; k++) {
        uint32_t size;
        apu_reverb_line_buffer(k, NULL, &size);
        r->size[k] = size / 4u;
        r->line[k] = (int32_t *)calloc(r->size[k], sizeof(int32_t));
        if (!r->line[k]) {
            apu_reverb_ref_free(r);
            return -1;
        }
        r->wpos[k] = r->len[k];
    }
    n = apu_reverb_coefficients(p, w);
    for (i = 0; i < n; i++) {
        int32_t v = sx24(w[i].value);
        if (w[i].addr == X_INGAIN) r->ingain = v;
        else if (w[i].addr == X_INLP) r->inlp = v;
        else if (w[i].addr == X_WET) r->wet = v;
        else if (w[i].addr == X_APG) r->apg = v;
        else if (w[i].addr >= X_CTAB && w[i].addr < X_CTAB + 6u * APU_REVERB_COMBS) {
            uint32_t c = (w[i].addr - X_CTAB) / 6u, f = (w[i].addr - X_CTAB) % 6u;
            if (f == 3u) r->g[c] = v;
            if (f == 4u) r->damp[c] = v;
        }
    }
    return 0;
}

void apu_reverb_ref_free(apu_reverb_ref_t *r) {
    uint32_t k;
    for (k = 0; k < APU_REVERB_LINES; k++) {
        free(r->line[k]);
        r->line[k] = NULL;
    }
}

static void read_block(const apu_reverb_ref_t *r, uint32_t k, int32_t out[32]) {
    uint32_t i, pos = (r->wpos[k] + r->size[k] - r->len[k]) % r->size[k];
    for (i = 0; i < 32u; i++) out[i] = r->line[k][(pos + i) % r->size[k]];
}

static void write_block(apu_reverb_ref_t *r, uint32_t k, const int32_t in[32]) {
    uint32_t i;
    for (i = 0; i < 32u; i++) r->line[k][(r->wpos[k] + i) % r->size[k]] = in[i];
    r->wpos[k] = (r->wpos[k] + 32u) % r->size[k];
}

void apu_reverb_ref_frame(apu_reverb_ref_t *r, int32_t bins[3][32], int32_t out[2][32]) {
    int32_t in[APU_REVERB_LINES][32], wr[APU_REVERB_LINES][32], acc[2][32];
    uint32_t k, i, c, a;
    acc_t b;

    for (k = 0; k < APU_REVERB_LINES; k++) read_block(r, k, in[k]);

    /* input: scale, lowpass */
    b = ld(r->inst);
    for (i = 0; i < 32u; i++) {
        int32_t s = lim(prod(bins[2][i], r->ingain));
        int32_t d = lim(b - ld(s));
        b = ld(s) + prod(d, r->inlp);
        wr[0][i] = lim(b);
    }
    r->inst = lim(b);

    memset(acc, 0, sizeof(acc));
    for (c = 0; c < APU_REVERB_COMBS; c++) {
        int32_t *sum = acc[c < APU_REVERB_COMBS / 2u ? 0 : 1];
        b = ld(r->lp[c]);
        for (i = 0; i < 32u; i++) {
            int32_t y = in[1u + c][i], d, l;
            sum[i] = lim(ld(sum[i]) + ld(y));
            d = lim(b - ld(y));
            b = ld(y) + prod(d, r->damp[c]);
            l = lim(b);
            wr[1u + c][i] = lim(ld(in[0][i]) + prod(l, r->g[c]));
        }
        r->lp[c] = lim(b);
    }

    for (a = 0; a < APU_REVERB_ALLPASSES; a++) {
        int32_t *s = acc[a < APU_REVERB_ALLPASSES / 2u ? 0 : 1];
        uint32_t kk = 1u + APU_REVERB_COMBS + a;
        for (i = 0; i < 32u; i++) {
            int32_t v = in[kk][i];
            acc_t t = ld(s[i]) + prod(v, r->apg);
            int32_t w = lim(t);
            wr[kk][i] = w;
            s[i] = lim(ld(v) - prod(w, r->apg));
        }
    }

    for (i = 0; i < 32u; i++) {
        out[0][i] = lim(ld(bins[0][i]) + prod(acc[0][i], r->wet));
        out[1][i] = lim(ld(bins[1][i]) + prod(acc[1][i], r->wet));
    }

    for (k = 0; k < APU_REVERB_LINES; k++) write_block(r, k, wr[k]);
}
