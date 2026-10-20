#ifndef APU_REVERB_H
#define APU_REVERB_H

/*
 * Reverb on the GP DSP (lib/openal/docs/GP_REVERB_PLAN.md): the parameters,
 * the delay-line layout and the X-memory words that drive kernel 0
 * (dsp/gp_kfreeverb.asm, inside the frame loop dsp/gp_loop.asm), plus a host
 * reference of their arithmetic.
 *
 * Topology (the comb/allpass family, with the public-domain Freeverb tuning):
 * mono send (the slot's bin pair) -> gain -> one-pole lowpass -> pre-delay -> 8 damped
 * feedback combs per ear, summed -> 4 series allpasses per ear -> wet gain,
 * added to bins 0/1. 25 delay lines in system RAM, each moved one 32-sample
 * block per frame by DMA. (4 combs and 2 allpasses per ear sounded metallic
 * on a console with a 2.5 s decay.)
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APU_REVERB_LINES        25u     /* pre-delay, 16 combs, 8 allpasses */
#define APU_REVERB_COMBS        16u
#define APU_REVERB_ALLPASSES    8u
#define APU_REVERB_RATE         48000u
#define APU_REVERB_MIN_PRE      32u     /* samples: a line is at least one frame long */
#define APU_REVERB_MAX_PRE      4800u   /* 100 ms */
#define APU_REVERB_SCRATCH_BASE 0x2000u /* scratch bytes below hold the program image */

typedef struct apu_reverb_params_s {
    float decay_time;       /* seconds to -60 dB, 0.1 .. 20 */
    float hf_ratio;         /* high-frequency decay relative to low, 0.1 .. 1 (1 = no damping) */
    float gain;             /* wet level, 0 .. 1 */
    float pre_delay;        /* seconds, 0 .. 0.1 */
    float diffusion;        /* 0 .. 1 */
    float input_gain;       /* send scaling before the combs, 0 .. 1 (headroom) */
    float input_lowpass;    /* one-pole coefficient on the send, 0 (off) .. 0.95 */
    float output_lowpass;   /* two one-pole stages on the whole output, 0 (off) .. 0.98: underwater */
} apu_reverb_params_t;

/* One X-memory word the program needs (address in DSP words, 24-bit value) */
typedef struct {
    uint16_t addr;
    uint32_t value;
} apu_reverb_word_t;

#define APU_REVERB_MAX_WORDS 640u

void apu_reverb_default_params(apu_reverb_params_t *p);

/* Presets (AL_XBOX_REVERB_PRESET_*): our own settings for common spaces */
#define APU_REVERB_PRESETS 8
/** Fill `p` with preset `n` (0..APU_REVERB_PRESETS-1). Returns -1 for an unknown preset. */
int apu_reverb_preset(int n, apu_reverb_params_t *p);

/** Delay of each line in samples (0: pre-delay, 1..16: combs, 8 left then 8 right, 17..24: allpasses, 4 left then 4 right). */
void apu_reverb_line_lengths(const apu_reverb_params_t *p, uint32_t len[APU_REVERB_LINES]);

/** Scratch bytes the lines need after APU_REVERB_SCRATCH_BASE (all lines, largest pre-delay). */
uint32_t apu_reverb_scratch_bytes(void);

/** Byte offset of line k's circular buffer in the scratch space, and its size. */
void apu_reverb_line_buffer(uint32_t k, uint32_t *offset, uint32_t *size);

/**
 * Coefficient words only (gains, comb feedback and damping, allpass gain, the
 * loop's output lowpass): safe to change while running. `xbase`: the X block
 * of the reverb's slot (KX; dsp/gp_kfreeverb.asm is relocatable).
 */
uint32_t apu_reverb_coefficients(const apu_reverb_params_t *p, uint32_t xbase, apu_reverb_word_t *out);

/**
 * Every word the kernel reads in its slot's X block at `xbase`: descriptors
 * (offsets set for the line lengths, the lines from scratch byte `scratch` on,
 * both chains ending at end of list), chain heads, tables, coefficients and
 * zeroed filter states (KIN is the loader's). Write them, zero the line
 * buffers, then switch the slot on. Returns the number of words.
 */
uint32_t apu_reverb_layout(const apu_reverb_params_t *p, uint32_t xbase, uint32_t scratch, apu_reverb_word_t *out);

/** Float to a DSP Q23 word (saturated). */
uint32_t apu_reverb_q23(float v);

/*
 * Host reference: the program's arithmetic on 24-bit words (56-bit accumulator,
 * convergent rounding to 24 bits on moves, saturation), frame by frame. State
 * for the delay lines is kept here; feed it bins 0-3 of each frame, get OUT L/R.
 */
typedef struct {
    int32_t *line[APU_REVERB_LINES];
    uint32_t size[APU_REVERB_LINES];   /* samples */
    uint32_t wpos[APU_REVERB_LINES];
    uint32_t len[APU_REVERB_LINES];
    int32_t ingain, inlp, inst, wet, apg, outlp, outst[4];
    int32_t g[APU_REVERB_COMBS], damp[APU_REVERB_COMBS], lp[APU_REVERB_COMBS];
} apu_reverb_ref_t;

int apu_reverb_ref_init(apu_reverb_ref_t *r, const apu_reverb_params_t *p);
void apu_reverb_ref_free(apu_reverb_ref_t *r);
/** bins: 4 x 32 signed 24-bit samples (dry bins 0/1, the input pair 2/3). out: 2 x 32 (L then R). */
void apu_reverb_ref_frame(apu_reverb_ref_t *r, int32_t bins[4][32], int32_t out[2][32]);

#ifdef __cplusplus
}
#endif

#endif /* APU_REVERB_H */
