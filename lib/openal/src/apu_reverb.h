#ifndef APU_REVERB_H
#define APU_REVERB_H

/*
 * Reverb on the GP DSP (lib/openal/docs/GP_REVERB_PLAN.md): the parameters,
 * the delay-line layout and the X-memory words that drive dsp/gp_reverb.asm,
 * plus a host reference of the program's arithmetic.
 *
 * Topology (our own, comb/allpass family): mono send (mixbin 2) -> gain ->
 * one-pole lowpass -> pre-delay -> 4 damped feedback combs per ear, summed ->
 * 2 series allpasses per ear -> wet gain, added to bins 0/1. Thirteen delay
 * lines in system RAM, each moved one 32-sample block per frame by DMA.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_REVERB_LINES        13u     /* pre-delay, 8 combs, 4 allpasses */
#define APU_REVERB_COMBS        8u
#define APU_REVERB_ALLPASSES    4u
#define APU_REVERB_RATE         48000u
#define APU_REVERB_MIN_PRE      32u     /* samples: a line is at least one frame long */
#define APU_REVERB_MAX_PRE      4800u   /* 100 ms */
#define APU_REVERB_SCRATCH_BASE 0x2000u /* scratch bytes below hold the program image */

typedef struct {
    float decay_time;       /* seconds to -60 dB, 0.1 .. 20 */
    float hf_ratio;         /* high-frequency decay relative to low, 0.1 .. 1 (1 = no damping) */
    float gain;             /* wet level, 0 .. 1 */
    float pre_delay;        /* seconds, 0 .. 0.1 */
    float diffusion;        /* 0 .. 1 */
    float input_gain;       /* send scaling before the combs, 0 .. 1 (headroom) */
    float input_lowpass;    /* one-pole coefficient on the send, 0 (off) .. 0.95 */
} apu_reverb_params_t;

/* One X-memory word the program needs (address in DSP words, 24-bit value) */
typedef struct {
    uint16_t addr;
    uint32_t value;
} apu_reverb_word_t;

#define APU_REVERB_MAX_WORDS 320u

void apu_reverb_default_params(apu_reverb_params_t *p);

/** Delay of each line in samples (0: pre-delay, 1..8: combs L L L L R R R R, 9..12: allpasses L L R R). */
void apu_reverb_line_lengths(const apu_reverb_params_t *p, uint32_t len[APU_REVERB_LINES]);

/** Scratch bytes the lines need after APU_REVERB_SCRATCH_BASE (all lines, largest pre-delay). */
uint32_t apu_reverb_scratch_bytes(void);

/** Byte offset of line k's circular buffer in the scratch space, and its size. */
void apu_reverb_line_buffer(uint32_t k, uint32_t *offset, uint32_t *size);

/** Coefficient words only (gains, comb feedback and damping, allpass gain): safe to change while running. */
uint32_t apu_reverb_coefficients(const apu_reverb_params_t *p, apu_reverb_word_t *out);

/**
 * Every word the program reads: descriptors (offsets set for the line lengths), tables,
 * coefficients and zeroed filter states. Write them, zero the line buffers, then set bit 0
 * of X:$11. Returns the number of words.
 */
uint32_t apu_reverb_layout(const apu_reverb_params_t *p, apu_reverb_word_t *out);

/** Float to a DSP Q23 word (saturated). */
uint32_t apu_reverb_q23(float v);

/*
 * Host reference: the program's arithmetic on 24-bit words (56-bit accumulator,
 * convergent rounding to 24 bits on moves, saturation), frame by frame. State
 * for the delay lines is kept here; feed it bins 0/1/2 of each frame, get OUT L/R.
 */
typedef struct {
    int32_t *line[APU_REVERB_LINES];
    uint32_t size[APU_REVERB_LINES];   /* samples */
    uint32_t wpos[APU_REVERB_LINES];
    uint32_t len[APU_REVERB_LINES];
    int32_t ingain, inlp, inst, wet, apg;
    int32_t g[APU_REVERB_COMBS], damp[APU_REVERB_COMBS], lp[APU_REVERB_COMBS];
} apu_reverb_ref_t;

int apu_reverb_ref_init(apu_reverb_ref_t *r, const apu_reverb_params_t *p);
void apu_reverb_ref_free(apu_reverb_ref_t *r);
/** bins: 3 x 32 signed 24-bit samples (bin 0, bin 1, send). out: 2 x 32 (L then R). */
void apu_reverb_ref_frame(apu_reverb_ref_t *r, int32_t bins[3][32], int32_t out[2][32]);

#ifdef __cplusplus
}
#endif

#endif /* APU_REVERB_H */
