/*
 * Host helper for tests/test_gp_programs.py (not a test on its own): prints
 * the X-memory layout apu_reverb.c writes for the GP reverb, a deterministic
 * input, and the host reference's output for it, so the simulated GP program
 * can be compared with the reference word for word.
 *
 *   gpsim_reverb_ref <preset> <frames> <xbase> <scratch>
 * (the slot's X block and scratch byte offset). Output lines: "W addr value"
 * (layout), then per frame "I bin s0..s31" for
 * bins 0-3 (dry pair, input pair) and "O ch s0..s31" for both output channels (24-bit words, hex).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apu_reverb.h"

static uint32_t s_seed = 1u;

static int32_t noise(int32_t amp) {
    s_seed = s_seed * 1664525u + 1013904223u;
    return (int32_t)((s_seed >> 8) % (uint32_t)(2 * amp + 1)) - amp;
}

int main(int argc, char **argv) {
    static apu_reverb_word_t w[APU_REVERB_MAX_WORDS];
    apu_reverb_params_t p;
    apu_reverb_ref_t r;
    int preset = argc > 1 ? atoi(argv[1]) : 3;
    int frames = argc > 2 ? atoi(argv[2]) : 200;
    uint32_t xbase = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0xE0u;
    uint32_t scratch = argc > 4 ? (uint32_t)strtoul(argv[4], NULL, 0) : APU_REVERB_SCRATCH_BASE;
    uint32_t n, i;
    int f, b, k;

    if (apu_reverb_preset(preset, &p) != 0) return 1;
    p.input_gain = 0.25f;
    n = apu_reverb_layout(&p, xbase, scratch, w);
    for (i = 0; i < n; i++) printf("W %x %x\n", (unsigned)w[i].addr, (unsigned)w[i].value);
    if (apu_reverb_ref_init(&r, &p) != 0) return 1;
    for (f = 0; f < frames; f++) {
        int32_t bins[4][32], out[2][32];
        for (b = 0; b < 4; b++) {
            for (k = 0; k < 32; k++) {
                /* a click on the send, then noise; full-scale words on the dry bins now and then */
                int32_t v = (f == 0 && k == 0 && b == 2) ? 0x600000 : (b == 3 && f < 40) ? 0 : noise(f % 17 == 3 ? 0x7FFFFF : 0x200000);
                bins[b][k] = v;
            }
        }
        for (b = 0; b < 4; b++) {
            printf("I %d", b);
            for (k = 0; k < 32; k++) printf(" %x", (unsigned)bins[b][k] & 0xFFFFFFu);
            printf("\n");
        }
        apu_reverb_ref_frame(&r, bins, out);
        for (b = 0; b < 2; b++) {
            printf("O %d", b);
            for (k = 0; k < 32; k++) printf(" %x", (unsigned)out[b][k] & 0xFFFFFFu);
            printf("\n");
        }
    }
    apu_reverb_ref_free(&r);
    return 0;
}
