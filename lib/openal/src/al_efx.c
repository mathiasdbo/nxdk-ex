/*
 * ALC_EXT_EFX 1.0 objects (lib/openal/docs/EFX_PLAN.md 3.1, task phase8-01):
 * effects, filters, auxiliary effect slots, and the EFX source / listener
 * parameters. Storage and validation only; nothing reaches the APU yet.
 *
 * Supported: effect types AL_EFFECT_NULL, AL_EFFECT_REVERB, AL_EFFECT_EAXREVERB,
 * and on the library's own GP kernels AL_EFFECT_ECHO, AL_EFFECT_CHORUS,
 * AL_EFFECT_FLANGER, AL_EFFECT_RING_MODULATOR, AL_EFFECT_DISTORTION,
 * AL_EFFECT_EQUALIZER, AL_EFFECT_COMPRESSOR and AL_EFFECT_AUTOWAH (the
 * frequency shifter, vocal morpher and pitch shifter are AL_INVALID_VALUE, as
 * the extension allows), filter types AL_FILTER_NULL and AL_FILTER_LOWPASS (the voice filter;
 * highpass and bandpass are AL_INVALID_VALUE), AL_EFX_MAX_SLOTS slots, each
 * with its own GP kernel (phase9-04: an attach fails only when the GP's shared
 * budgets are full), and up to AL_EFX_MAX_SENDS sends per source.
 */
#define AL_ALEXT_PROTOTYPES
#include "al_efx.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <AL/alext.h>

#include "al_buffer.h"   /* alSetError */
#include "al_source.h"   /* al_source_effects_changed */
#include "gp_kefx_autowah.h"
#include "gp_kefx_chorus.h"
#include "gp_kefx_compressor.h"
#include "gp_kefx_distortion.h"
#include "gp_kefx_echo.h"
#include "gp_kefx_equalizer.h"
#include "gp_kefx_ringmod.h"

/* ------------------------------------------------------------------ parameter tables */

enum { P_FLOAT, P_VEC, P_BOOL, P_INT };   /* P_BOOL, P_INT: an ALint in [min, max] */

typedef struct {
    ALenum param;
    uint8_t kind;
    uint16_t offset;    /* into al_efx_reverb_t */
    ALfloat min, max, def;
} param_desc_t;

#define RV(f) ((uint16_t)offsetof(al_efx_reverb_t, f))

static const param_desc_t s_reverb_params[] = {
    { AL_REVERB_DENSITY, P_FLOAT, RV(density), AL_REVERB_MIN_DENSITY, AL_REVERB_MAX_DENSITY, AL_REVERB_DEFAULT_DENSITY },
    { AL_REVERB_DIFFUSION, P_FLOAT, RV(diffusion), AL_REVERB_MIN_DIFFUSION, AL_REVERB_MAX_DIFFUSION,
      AL_REVERB_DEFAULT_DIFFUSION },
    { AL_REVERB_GAIN, P_FLOAT, RV(gain), AL_REVERB_MIN_GAIN, AL_REVERB_MAX_GAIN, AL_REVERB_DEFAULT_GAIN },
    { AL_REVERB_GAINHF, P_FLOAT, RV(gain_hf), AL_REVERB_MIN_GAINHF, AL_REVERB_MAX_GAINHF, AL_REVERB_DEFAULT_GAINHF },
    { AL_REVERB_DECAY_TIME, P_FLOAT, RV(decay_time), AL_REVERB_MIN_DECAY_TIME, AL_REVERB_MAX_DECAY_TIME,
      AL_REVERB_DEFAULT_DECAY_TIME },
    { AL_REVERB_DECAY_HFRATIO, P_FLOAT, RV(decay_hf_ratio), AL_REVERB_MIN_DECAY_HFRATIO, AL_REVERB_MAX_DECAY_HFRATIO,
      AL_REVERB_DEFAULT_DECAY_HFRATIO },
    { AL_REVERB_REFLECTIONS_GAIN, P_FLOAT, RV(reflections_gain), AL_REVERB_MIN_REFLECTIONS_GAIN,
      AL_REVERB_MAX_REFLECTIONS_GAIN, AL_REVERB_DEFAULT_REFLECTIONS_GAIN },
    { AL_REVERB_REFLECTIONS_DELAY, P_FLOAT, RV(reflections_delay), AL_REVERB_MIN_REFLECTIONS_DELAY,
      AL_REVERB_MAX_REFLECTIONS_DELAY, AL_REVERB_DEFAULT_REFLECTIONS_DELAY },
    { AL_REVERB_LATE_REVERB_GAIN, P_FLOAT, RV(late_reverb_gain), AL_REVERB_MIN_LATE_REVERB_GAIN,
      AL_REVERB_MAX_LATE_REVERB_GAIN, AL_REVERB_DEFAULT_LATE_REVERB_GAIN },
    { AL_REVERB_LATE_REVERB_DELAY, P_FLOAT, RV(late_reverb_delay), AL_REVERB_MIN_LATE_REVERB_DELAY,
      AL_REVERB_MAX_LATE_REVERB_DELAY, AL_REVERB_DEFAULT_LATE_REVERB_DELAY },
    { AL_REVERB_AIR_ABSORPTION_GAINHF, P_FLOAT, RV(air_absorption_gain_hf), AL_REVERB_MIN_AIR_ABSORPTION_GAINHF,
      AL_REVERB_MAX_AIR_ABSORPTION_GAINHF, AL_REVERB_DEFAULT_AIR_ABSORPTION_GAINHF },
    { AL_REVERB_ROOM_ROLLOFF_FACTOR, P_FLOAT, RV(room_rolloff_factor), AL_REVERB_MIN_ROOM_ROLLOFF_FACTOR,
      AL_REVERB_MAX_ROOM_ROLLOFF_FACTOR, AL_REVERB_DEFAULT_ROOM_ROLLOFF_FACTOR },
    { AL_REVERB_DECAY_HFLIMIT, P_BOOL, RV(decay_hf_limit), AL_FALSE, AL_TRUE, AL_REVERB_DEFAULT_DECAY_HFLIMIT },
};

static const param_desc_t s_eaxreverb_params[] = {
    { AL_EAXREVERB_DENSITY, P_FLOAT, RV(density), AL_EAXREVERB_MIN_DENSITY, AL_EAXREVERB_MAX_DENSITY,
      AL_EAXREVERB_DEFAULT_DENSITY },
    { AL_EAXREVERB_DIFFUSION, P_FLOAT, RV(diffusion), AL_EAXREVERB_MIN_DIFFUSION, AL_EAXREVERB_MAX_DIFFUSION,
      AL_EAXREVERB_DEFAULT_DIFFUSION },
    { AL_EAXREVERB_GAIN, P_FLOAT, RV(gain), AL_EAXREVERB_MIN_GAIN, AL_EAXREVERB_MAX_GAIN, AL_EAXREVERB_DEFAULT_GAIN },
    { AL_EAXREVERB_GAINHF, P_FLOAT, RV(gain_hf), AL_EAXREVERB_MIN_GAINHF, AL_EAXREVERB_MAX_GAINHF,
      AL_EAXREVERB_DEFAULT_GAINHF },
    { AL_EAXREVERB_GAINLF, P_FLOAT, RV(gain_lf), AL_EAXREVERB_MIN_GAINLF, AL_EAXREVERB_MAX_GAINLF,
      AL_EAXREVERB_DEFAULT_GAINLF },
    { AL_EAXREVERB_DECAY_TIME, P_FLOAT, RV(decay_time), AL_EAXREVERB_MIN_DECAY_TIME, AL_EAXREVERB_MAX_DECAY_TIME,
      AL_EAXREVERB_DEFAULT_DECAY_TIME },
    { AL_EAXREVERB_DECAY_HFRATIO, P_FLOAT, RV(decay_hf_ratio), AL_EAXREVERB_MIN_DECAY_HFRATIO,
      AL_EAXREVERB_MAX_DECAY_HFRATIO, AL_EAXREVERB_DEFAULT_DECAY_HFRATIO },
    { AL_EAXREVERB_DECAY_LFRATIO, P_FLOAT, RV(decay_lf_ratio), AL_EAXREVERB_MIN_DECAY_LFRATIO,
      AL_EAXREVERB_MAX_DECAY_LFRATIO, AL_EAXREVERB_DEFAULT_DECAY_LFRATIO },
    { AL_EAXREVERB_REFLECTIONS_GAIN, P_FLOAT, RV(reflections_gain), AL_EAXREVERB_MIN_REFLECTIONS_GAIN,
      AL_EAXREVERB_MAX_REFLECTIONS_GAIN, AL_EAXREVERB_DEFAULT_REFLECTIONS_GAIN },
    { AL_EAXREVERB_REFLECTIONS_DELAY, P_FLOAT, RV(reflections_delay), AL_EAXREVERB_MIN_REFLECTIONS_DELAY,
      AL_EAXREVERB_MAX_REFLECTIONS_DELAY, AL_EAXREVERB_DEFAULT_REFLECTIONS_DELAY },
    { AL_EAXREVERB_REFLECTIONS_PAN, P_VEC, RV(reflections_pan), -FLT_MAX, FLT_MAX,
      AL_EAXREVERB_DEFAULT_REFLECTIONS_PAN_XYZ },
    { AL_EAXREVERB_LATE_REVERB_GAIN, P_FLOAT, RV(late_reverb_gain), AL_EAXREVERB_MIN_LATE_REVERB_GAIN,
      AL_EAXREVERB_MAX_LATE_REVERB_GAIN, AL_EAXREVERB_DEFAULT_LATE_REVERB_GAIN },
    { AL_EAXREVERB_LATE_REVERB_DELAY, P_FLOAT, RV(late_reverb_delay), AL_EAXREVERB_MIN_LATE_REVERB_DELAY,
      AL_EAXREVERB_MAX_LATE_REVERB_DELAY, AL_EAXREVERB_DEFAULT_LATE_REVERB_DELAY },
    { AL_EAXREVERB_LATE_REVERB_PAN, P_VEC, RV(late_reverb_pan), -FLT_MAX, FLT_MAX,
      AL_EAXREVERB_DEFAULT_LATE_REVERB_PAN_XYZ },
    { AL_EAXREVERB_ECHO_TIME, P_FLOAT, RV(echo_time), AL_EAXREVERB_MIN_ECHO_TIME, AL_EAXREVERB_MAX_ECHO_TIME,
      AL_EAXREVERB_DEFAULT_ECHO_TIME },
    { AL_EAXREVERB_ECHO_DEPTH, P_FLOAT, RV(echo_depth), AL_EAXREVERB_MIN_ECHO_DEPTH, AL_EAXREVERB_MAX_ECHO_DEPTH,
      AL_EAXREVERB_DEFAULT_ECHO_DEPTH },
    { AL_EAXREVERB_MODULATION_TIME, P_FLOAT, RV(modulation_time), AL_EAXREVERB_MIN_MODULATION_TIME,
      AL_EAXREVERB_MAX_MODULATION_TIME, AL_EAXREVERB_DEFAULT_MODULATION_TIME },
    { AL_EAXREVERB_MODULATION_DEPTH, P_FLOAT, RV(modulation_depth), AL_EAXREVERB_MIN_MODULATION_DEPTH,
      AL_EAXREVERB_MAX_MODULATION_DEPTH, AL_EAXREVERB_DEFAULT_MODULATION_DEPTH },
    { AL_EAXREVERB_AIR_ABSORPTION_GAINHF, P_FLOAT, RV(air_absorption_gain_hf), AL_EAXREVERB_MIN_AIR_ABSORPTION_GAINHF,
      AL_EAXREVERB_MAX_AIR_ABSORPTION_GAINHF, AL_EAXREVERB_DEFAULT_AIR_ABSORPTION_GAINHF },
    { AL_EAXREVERB_HFREFERENCE, P_FLOAT, RV(hf_reference), AL_EAXREVERB_MIN_HFREFERENCE, AL_EAXREVERB_MAX_HFREFERENCE,
      AL_EAXREVERB_DEFAULT_HFREFERENCE },
    { AL_EAXREVERB_LFREFERENCE, P_FLOAT, RV(lf_reference), AL_EAXREVERB_MIN_LFREFERENCE, AL_EAXREVERB_MAX_LFREFERENCE,
      AL_EAXREVERB_DEFAULT_LFREFERENCE },
    { AL_EAXREVERB_ROOM_ROLLOFF_FACTOR, P_FLOAT, RV(room_rolloff_factor), AL_EAXREVERB_MIN_ROOM_ROLLOFF_FACTOR,
      AL_EAXREVERB_MAX_ROOM_ROLLOFF_FACTOR, AL_EAXREVERB_DEFAULT_ROOM_ROLLOFF_FACTOR },
    { AL_EAXREVERB_DECAY_HFLIMIT, P_BOOL, RV(decay_hf_limit), AL_FALSE, AL_TRUE, AL_EAXREVERB_DEFAULT_DECAY_HFLIMIT },
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const param_desc_t s_echo_params[] = {
    { AL_ECHO_DELAY, P_FLOAT, RV(echo_delay), AL_ECHO_MIN_DELAY, AL_ECHO_MAX_DELAY, AL_ECHO_DEFAULT_DELAY },
    { AL_ECHO_LRDELAY, P_FLOAT, RV(echo_lr_delay), AL_ECHO_MIN_LRDELAY, AL_ECHO_MAX_LRDELAY, AL_ECHO_DEFAULT_LRDELAY },
    { AL_ECHO_DAMPING, P_FLOAT, RV(echo_damping), AL_ECHO_MIN_DAMPING, AL_ECHO_MAX_DAMPING, AL_ECHO_DEFAULT_DAMPING },
    { AL_ECHO_FEEDBACK, P_FLOAT, RV(echo_feedback), AL_ECHO_MIN_FEEDBACK, AL_ECHO_MAX_FEEDBACK,
      AL_ECHO_DEFAULT_FEEDBACK },
    { AL_ECHO_SPREAD, P_FLOAT, RV(echo_spread), AL_ECHO_MIN_SPREAD, AL_ECHO_MAX_SPREAD, AL_ECHO_DEFAULT_SPREAD },
};

static const param_desc_t s_chorus_params[] = {
    { AL_CHORUS_WAVEFORM, P_INT, RV(chorus_waveform), AL_CHORUS_MIN_WAVEFORM, AL_CHORUS_MAX_WAVEFORM,
      AL_CHORUS_DEFAULT_WAVEFORM },
    { AL_CHORUS_PHASE, P_INT, RV(chorus_phase), AL_CHORUS_MIN_PHASE, AL_CHORUS_MAX_PHASE,
      AL_CHORUS_DEFAULT_PHASE },
    { AL_CHORUS_RATE, P_FLOAT, RV(chorus_rate), AL_CHORUS_MIN_RATE, AL_CHORUS_MAX_RATE,
      AL_CHORUS_DEFAULT_RATE },
    { AL_CHORUS_DEPTH, P_FLOAT, RV(chorus_depth), AL_CHORUS_MIN_DEPTH, AL_CHORUS_MAX_DEPTH,
      AL_CHORUS_DEFAULT_DEPTH },
    { AL_CHORUS_FEEDBACK, P_FLOAT, RV(chorus_feedback), AL_CHORUS_MIN_FEEDBACK, AL_CHORUS_MAX_FEEDBACK,
      AL_CHORUS_DEFAULT_FEEDBACK },
    { AL_CHORUS_DELAY, P_FLOAT, RV(chorus_delay), AL_CHORUS_MIN_DELAY, AL_CHORUS_MAX_DELAY,
      AL_CHORUS_DEFAULT_DELAY },
};

static const param_desc_t s_flanger_params[] = {
    { AL_FLANGER_WAVEFORM, P_INT, RV(flanger_waveform), AL_FLANGER_MIN_WAVEFORM, AL_FLANGER_MAX_WAVEFORM,
      AL_FLANGER_DEFAULT_WAVEFORM },
    { AL_FLANGER_PHASE, P_INT, RV(flanger_phase), AL_FLANGER_MIN_PHASE, AL_FLANGER_MAX_PHASE,
      AL_FLANGER_DEFAULT_PHASE },
    { AL_FLANGER_RATE, P_FLOAT, RV(flanger_rate), AL_FLANGER_MIN_RATE, AL_FLANGER_MAX_RATE,
      AL_FLANGER_DEFAULT_RATE },
    { AL_FLANGER_DEPTH, P_FLOAT, RV(flanger_depth), AL_FLANGER_MIN_DEPTH, AL_FLANGER_MAX_DEPTH,
      AL_FLANGER_DEFAULT_DEPTH },
    { AL_FLANGER_FEEDBACK, P_FLOAT, RV(flanger_feedback), AL_FLANGER_MIN_FEEDBACK, AL_FLANGER_MAX_FEEDBACK,
      AL_FLANGER_DEFAULT_FEEDBACK },
    { AL_FLANGER_DELAY, P_FLOAT, RV(flanger_delay), AL_FLANGER_MIN_DELAY, AL_FLANGER_MAX_DELAY,
      AL_FLANGER_DEFAULT_DELAY },
};

static const param_desc_t s_ringmod_params[] = {
    { AL_RING_MODULATOR_FREQUENCY, P_FLOAT, RV(ringmod_frequency), AL_RING_MODULATOR_MIN_FREQUENCY,
      AL_RING_MODULATOR_MAX_FREQUENCY, AL_RING_MODULATOR_DEFAULT_FREQUENCY },
    { AL_RING_MODULATOR_HIGHPASS_CUTOFF, P_FLOAT, RV(ringmod_highpass_cutoff), AL_RING_MODULATOR_MIN_HIGHPASS_CUTOFF,
      AL_RING_MODULATOR_MAX_HIGHPASS_CUTOFF, AL_RING_MODULATOR_DEFAULT_HIGHPASS_CUTOFF },
    { AL_RING_MODULATOR_WAVEFORM, P_INT, RV(ringmod_waveform), AL_RING_MODULATOR_MIN_WAVEFORM,
      AL_RING_MODULATOR_MAX_WAVEFORM, AL_RING_MODULATOR_DEFAULT_WAVEFORM },
};

static const param_desc_t s_distortion_params[] = {
    { AL_DISTORTION_EDGE, P_FLOAT, RV(distortion_edge), AL_DISTORTION_MIN_EDGE, AL_DISTORTION_MAX_EDGE,
      AL_DISTORTION_DEFAULT_EDGE },
    { AL_DISTORTION_GAIN, P_FLOAT, RV(distortion_gain), AL_DISTORTION_MIN_GAIN, AL_DISTORTION_MAX_GAIN,
      AL_DISTORTION_DEFAULT_GAIN },
    { AL_DISTORTION_LOWPASS_CUTOFF, P_FLOAT, RV(distortion_lowpass_cutoff), AL_DISTORTION_MIN_LOWPASS_CUTOFF,
      AL_DISTORTION_MAX_LOWPASS_CUTOFF, AL_DISTORTION_DEFAULT_LOWPASS_CUTOFF },
    { AL_DISTORTION_EQCENTER, P_FLOAT, RV(distortion_eq_center), AL_DISTORTION_MIN_EQCENTER,
      AL_DISTORTION_MAX_EQCENTER, AL_DISTORTION_DEFAULT_EQCENTER },
    { AL_DISTORTION_EQBANDWIDTH, P_FLOAT, RV(distortion_eq_bandwidth), AL_DISTORTION_MIN_EQBANDWIDTH,
      AL_DISTORTION_MAX_EQBANDWIDTH, AL_DISTORTION_DEFAULT_EQBANDWIDTH },
};

static const param_desc_t s_equalizer_params[] = {
    { AL_EQUALIZER_LOW_GAIN, P_FLOAT, RV(eq_low_gain), AL_EQUALIZER_MIN_LOW_GAIN, AL_EQUALIZER_MAX_LOW_GAIN,
      AL_EQUALIZER_DEFAULT_LOW_GAIN },
    { AL_EQUALIZER_LOW_CUTOFF, P_FLOAT, RV(eq_low_cutoff), AL_EQUALIZER_MIN_LOW_CUTOFF, AL_EQUALIZER_MAX_LOW_CUTOFF,
      AL_EQUALIZER_DEFAULT_LOW_CUTOFF },
    { AL_EQUALIZER_MID1_GAIN, P_FLOAT, RV(eq_mid1_gain), AL_EQUALIZER_MIN_MID1_GAIN, AL_EQUALIZER_MAX_MID1_GAIN,
      AL_EQUALIZER_DEFAULT_MID1_GAIN },
    { AL_EQUALIZER_MID1_CENTER, P_FLOAT, RV(eq_mid1_center), AL_EQUALIZER_MIN_MID1_CENTER,
      AL_EQUALIZER_MAX_MID1_CENTER, AL_EQUALIZER_DEFAULT_MID1_CENTER },
    { AL_EQUALIZER_MID1_WIDTH, P_FLOAT, RV(eq_mid1_width), AL_EQUALIZER_MIN_MID1_WIDTH, AL_EQUALIZER_MAX_MID1_WIDTH,
      AL_EQUALIZER_DEFAULT_MID1_WIDTH },
    { AL_EQUALIZER_MID2_GAIN, P_FLOAT, RV(eq_mid2_gain), AL_EQUALIZER_MIN_MID2_GAIN, AL_EQUALIZER_MAX_MID2_GAIN,
      AL_EQUALIZER_DEFAULT_MID2_GAIN },
    { AL_EQUALIZER_MID2_CENTER, P_FLOAT, RV(eq_mid2_center), AL_EQUALIZER_MIN_MID2_CENTER,
      AL_EQUALIZER_MAX_MID2_CENTER, AL_EQUALIZER_DEFAULT_MID2_CENTER },
    { AL_EQUALIZER_MID2_WIDTH, P_FLOAT, RV(eq_mid2_width), AL_EQUALIZER_MIN_MID2_WIDTH, AL_EQUALIZER_MAX_MID2_WIDTH,
      AL_EQUALIZER_DEFAULT_MID2_WIDTH },
    { AL_EQUALIZER_HIGH_GAIN, P_FLOAT, RV(eq_high_gain), AL_EQUALIZER_MIN_HIGH_GAIN, AL_EQUALIZER_MAX_HIGH_GAIN,
      AL_EQUALIZER_DEFAULT_HIGH_GAIN },
    { AL_EQUALIZER_HIGH_CUTOFF, P_FLOAT, RV(eq_high_cutoff), AL_EQUALIZER_MIN_HIGH_CUTOFF,
      AL_EQUALIZER_MAX_HIGH_CUTOFF, AL_EQUALIZER_DEFAULT_HIGH_CUTOFF },
};

static const param_desc_t s_compressor_params[] = {
    { AL_COMPRESSOR_ONOFF, P_INT, RV(compressor_onoff), AL_COMPRESSOR_MIN_ONOFF, AL_COMPRESSOR_MAX_ONOFF,
      AL_COMPRESSOR_DEFAULT_ONOFF },
};

static const param_desc_t s_autowah_params[] = {
    { AL_AUTOWAH_ATTACK_TIME, P_FLOAT, RV(autowah_attack_time), AL_AUTOWAH_MIN_ATTACK_TIME,
      AL_AUTOWAH_MAX_ATTACK_TIME, AL_AUTOWAH_DEFAULT_ATTACK_TIME },
    { AL_AUTOWAH_RELEASE_TIME, P_FLOAT, RV(autowah_release_time), AL_AUTOWAH_MIN_RELEASE_TIME,
      AL_AUTOWAH_MAX_RELEASE_TIME, AL_AUTOWAH_DEFAULT_RELEASE_TIME },
    { AL_AUTOWAH_RESONANCE, P_FLOAT, RV(autowah_resonance), AL_AUTOWAH_MIN_RESONANCE, AL_AUTOWAH_MAX_RESONANCE,
      AL_AUTOWAH_DEFAULT_RESONANCE },
    { AL_AUTOWAH_PEAK_GAIN, P_FLOAT, RV(autowah_peak_gain), AL_AUTOWAH_MIN_PEAK_GAIN, AL_AUTOWAH_MAX_PEAK_GAIN,
      AL_AUTOWAH_DEFAULT_PEAK_GAIN },
};

/* Each effect type's parameters (NULL, 0 for none) */
static const param_desc_t *type_params(ALenum type, size_t *n) {
    switch (type) {
        case AL_EFFECT_REVERB:
            *n = COUNT(s_reverb_params);
            return s_reverb_params;
        case AL_EFFECT_EAXREVERB:
            *n = COUNT(s_eaxreverb_params);
            return s_eaxreverb_params;
        case AL_EFFECT_ECHO:
            *n = COUNT(s_echo_params);
            return s_echo_params;
        case AL_EFFECT_CHORUS:
            *n = COUNT(s_chorus_params);
            return s_chorus_params;
        case AL_EFFECT_FLANGER:
            *n = COUNT(s_flanger_params);
            return s_flanger_params;
        case AL_EFFECT_RING_MODULATOR:
            *n = COUNT(s_ringmod_params);
            return s_ringmod_params;
        case AL_EFFECT_DISTORTION:
            *n = COUNT(s_distortion_params);
            return s_distortion_params;
        case AL_EFFECT_EQUALIZER:
            *n = COUNT(s_equalizer_params);
            return s_equalizer_params;
        case AL_EFFECT_COMPRESSOR:
            *n = COUNT(s_compressor_params);
            return s_compressor_params;
        case AL_EFFECT_AUTOWAH:
            *n = COUNT(s_autowah_params);
            return s_autowah_params;
        default:
            *n = 0;
            return NULL;
    }
}

static const param_desc_t *find_param(ALenum type, ALenum param) {
    size_t i, n;
    const param_desc_t *t = type_params(type, &n);
    for (i = 0; i < n; i++) {
        if (t[i].param == param) return &t[i];
    }
    return NULL;
}

static ALfloat *float_field(al_efx_reverb_t *r, const param_desc_t *d) {
    return (ALfloat *)((char *)r + d->offset);
}

static ALint *int_field(al_efx_reverb_t *r, const param_desc_t *d) {
    return (ALint *)((char *)r + d->offset);
}

/* Every field at its default (the EAX table covers the reverb's fields too) */
static void table_defaults(al_efx_reverb_t *r, const param_desc_t *t, size_t n) {
    size_t i;
    for (i = 0; i < n; i++) {
        const param_desc_t *d = &t[i];
        if (d->kind == P_BOOL || d->kind == P_INT) {
            *int_field(r, d) = (ALint)d->def;
        } else if (d->kind == P_VEC) {
            float_field(r, d)[0] = float_field(r, d)[1] = float_field(r, d)[2] = d->def;
        } else {
            *float_field(r, d) = d->def;
        }
    }
}

static void reverb_defaults(al_efx_reverb_t *r) {
    memset(r, 0, sizeof(*r));
    table_defaults(r, s_eaxreverb_params, COUNT(s_eaxreverb_params));
    table_defaults(r, s_echo_params, COUNT(s_echo_params));
    table_defaults(r, s_chorus_params, COUNT(s_chorus_params));
    table_defaults(r, s_flanger_params, COUNT(s_flanger_params));
    table_defaults(r, s_ringmod_params, COUNT(s_ringmod_params));
    table_defaults(r, s_distortion_params, COUNT(s_distortion_params));
    table_defaults(r, s_equalizer_params, COUNT(s_equalizer_params));
    table_defaults(r, s_compressor_params, COUNT(s_compressor_params));
    table_defaults(r, s_autowah_params, COUNT(s_autowah_params));
}

static bool in_range(ALfloat v, const param_desc_t *d) {
    return isfinite(v) && v >= d->min && v <= d->max;
}

/* ------------------------------------------------------------------ object tables */

typedef struct {
    bool in_use;
    al_efx_effect_props_t props;
} effect_t;

typedef struct {
    bool in_use;
    al_efx_filter_props_t props;
} filter_t;

static effect_t s_effects[AL_EFX_MAX_EFFECTS];
static filter_t s_filters[AL_EFX_MAX_FILTERS];
static al_efx_slot_t s_slots[AL_EFX_MAX_SLOTS];
static ALfloat s_meters_per_unit = AL_DEFAULT_METERS_PER_UNIT;
static ALint s_max_sends = AL_EFX_MAX_SENDS;

static effect_t *get_effect(ALuint id) {
    return (id >= 1u && id <= AL_EFX_MAX_EFFECTS && s_effects[id - 1u].in_use) ? &s_effects[id - 1u] : NULL;
}

static filter_t *get_filter(ALuint id) {
    return (id >= 1u && id <= AL_EFX_MAX_FILTERS && s_filters[id - 1u].in_use) ? &s_filters[id - 1u] : NULL;
}

static al_efx_slot_t *get_slot(ALuint id) {
    return (id >= 1u && id <= AL_EFX_MAX_SLOTS && s_slots[id - 1u].in_use) ? &s_slots[id - 1u] : NULL;
}

/*
 * Shared Gen / Delete: `used` points at the in_use flag of entry 0 and
 * `stride` is the entry size. Gen hands out the lowest free names, all or
 * none (AL_OUT_OF_MEMORY); Delete checks every name first (AL_INVALID_NAME).
 */
static bool *used_at(bool *used, size_t stride, size_t i) {
    return (bool *)((char *)used + stride * i);
}

static bool gen_names(ALsizei n, ALuint *out, bool *used, size_t stride, size_t cap) {
    size_t i, free_count = 0;
    ALsizei k = 0;
    if (n < 0 || (n > 0 && !out)) {
        alSetError(AL_INVALID_VALUE);
        return false;
    }
    for (i = 0; i < cap; i++) {
        if (!*used_at(used, stride, i)) free_count++;
    }
    if ((size_t)n > free_count) {
        alSetError(AL_OUT_OF_MEMORY);
        return false;
    }
    for (i = 0; i < cap && k < n; i++) {
        if (!*used_at(used, stride, i)) {
            out[k++] = (ALuint)(i + 1u);
        }
    }
    return true;
}

static bool check_names(ALsizei n, const ALuint *ids, bool *used, size_t stride, size_t cap) {
    ALsizei k;
    if (n < 0 || (n > 0 && !ids)) {
        alSetError(AL_INVALID_VALUE);
        return false;
    }
    for (k = 0; k < n; k++) {
        if (ids[k] != 0u && (ids[k] > cap || !*used_at(used, stride, ids[k] - 1u))) {
            alSetError(AL_INVALID_NAME);
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ GP programs (AL_XBOX_gp_program) */

struct al_efx_program_s {
    uint32_t refs;
    uint32_t count;
    apu_gp_kernel_t kernel;     /* points into words */
    uint32_t words[1];          /* count words, allocated with the structure */
};

static al_efx_program_t *program_ref(al_efx_program_t *p) {
    if (p) p->refs++;
    return p;
}

static void program_unref(al_efx_program_t *p) {
    if (p && --p->refs == 0u) free(p);
}

/* dst = src with the image's references counted (slots copy effects) */
static void props_assign(al_efx_effect_props_t *dst, const al_efx_effect_props_t *src) {
    al_efx_program_t *old = dst->program;
    *dst = *src;
    program_ref(dst->program);
    program_unref(old);
}

static void props_release(al_efx_effect_props_t *p) {
    program_unref(p->program);
    p->program = NULL;
}

static bool gp_param_index(const al_efx_effect_props_t *p, ALenum param, uint32_t *n) {
    if (p->type != AL_EFFECT_XBOX_GP_PROGRAM || param < AL_XBOX_GP_PARAM0 ||
        param >= AL_XBOX_GP_PARAM0 + (ALenum)AL_EFX_GP_PARAMS) {
        return false;
    }
    *n = (uint32_t)(param - AL_XBOX_GP_PARAM0);
    return true;
}

static int32_t sx24(uint32_t v) {
    return (v & 0x800000u) ? (int32_t)(v | 0xFF000000u) : (int32_t)v;
}

AL_API void AL_APIENTRY alXboxEffectProgram(ALuint effect, const ALuint *words, ALsizei count, ALuint flags) {
    effect_t *e = get_effect(effect);
    al_efx_program_t *p;
    apu_gp_kernel_t k;
    if (!e) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (e->props.type != AL_EFFECT_XBOX_GP_PROGRAM) {
        alSetError(AL_INVALID_OPERATION);
        return;
    }
    if (flags != 0u || count < 0 || (count > 0 && !words)) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (count == 0) {
        props_release(&e->props);   /* no image: the effect runs nothing */
        return;
    }
    if (apu_gp_kernel_parse((const uint32_t *)words, (uint32_t)count, &k) != 0) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    p = (al_efx_program_t *)malloc(sizeof(*p) + ((size_t)count - 1u) * sizeof(uint32_t));
    if (!p) {
        alSetError(AL_OUT_OF_MEMORY);
        return;
    }
    p->refs = 1u;
    p->count = (uint32_t)count;
    memcpy(p->words, words, (size_t)count * sizeof(uint32_t));
    (void)apu_gp_kernel_parse(p->words, p->count, &p->kernel);
    props_release(&e->props);
    e->props.program = p;
}

/* The library's own kernels for built-in effect types (SOFT_GAP_PLAN.md phase9-05) */
static uint32_t s_builtin_params[AL_EFX_MAX_SLOTS][AL_EFX_GP_PARAMS];

static uint32_t q23(float v) {
    return apu_reverb_q23(v) & 0xFFFFFFu;
}

static uint32_t samples(float seconds) {
    return (uint32_t)(seconds * 48000.0f + 0.5f);
}

/* AL_EFFECT_ECHO -> gp_kefx_echo's parameters: the two taps, feedback, the
 * damping pole, the taps' left / right gains (SPREAD, constant power: -1 puts
 * the first tap left and the second right) */
static void echo_params(const al_efx_reverb_t *r, uint32_t *p) {
    float s = r->echo_spread;
    float a = sqrtf(0.5f * (1.0f - s)), b = sqrtf(0.5f * (1.0f + s));
    memset(p, 0, AL_EFX_GP_PARAMS * sizeof(uint32_t));
    p[0] = samples(r->echo_delay);
    p[1] = samples(r->echo_delay + r->echo_lr_delay);
    p[2] = q23(r->echo_feedback);
    p[3] = q23(r->echo_damping);
    p[4] = q23(a);   /* tap 1 left */
    p[5] = q23(b);   /* tap 1 right */
    p[6] = q23(b);   /* tap 2 left */
    p[7] = q23(a);   /* tap 2 right */
}

/* AL_EFFECT_CHORUS / AL_EFFECT_FLANGER -> gp_kefx_chorus's parameters: the
 * centre delay D and the LFO's swing DD = DEPTH x D (as fractions of 2048
 * samples; D + DD is at most 1536, the chorus's 16 ms twice), feedback, the
 * LFO step a sample (a cycle is 2.0), the right tap's phase offset, the waveform */
static void chorus_params(ALint wave, ALint phase, float rate, float depth, float fb, float delay, uint32_t *p) {
    float d = delay * 48000.0f;
    memset(p, 0, AL_EFX_GP_PARAMS * sizeof(uint32_t));
    p[0] = q23(d / 2048.0f);
    p[1] = q23(depth * d / 2048.0f);
    p[2] = q23(fb);
    p[3] = q23(2.0f * rate / 48000.0f);
    p[4] = q23((float)phase / 180.0f);
    p[5] = (uint32_t)wave;
}

/* ---- the on-chip effects (SOFT_GAP_PLAN.md phase9-07): OpenAL Soft's algorithms, docs/GP_KERNELS.md 10 */

#define AL_EFX_RATE 48000.0
#define AL_EFX_PI 3.14159265358979323846

enum { BQ_LOWPASS, BQ_HIGHPASS, BQ_BANDPASS, BQ_PEAKING, BQ_LOWSHELF, BQ_HIGHSHELF };

static double dmin(double a, double b) {
    return (a < b) ? a : b;
}

/* A Q23 word from a double (rounded, saturated) */
static uint32_t q23d(double v) {
    double s = floor(v * 8388608.0 + 0.5);
    if (s > 8388607.0) s = 8388607.0;
    if (s < -8388608.0) s = -8388608.0;
    return (uint32_t)(int32_t)s & 0xFFFFFFu;
}

/* OpenAL Soft's biquad design (its BiquadFilter: rcpQFromSlope, rcpQFromBandwidth, setParams) */
static double rcpq_from_slope(double gain, double slope) {
    return sqrt((gain + 1.0 / gain) * (1.0 / slope - 1.0) + 2.0);
}

static double rcpq_from_bandwidth(double f0norm, double bw) {
    double w0 = 2.0 * AL_EFX_PI * f0norm;
    return 2.0 * sinh(log(2.0) / 2.0 * bw * w0 / sin(w0));
}

/* c = b0 b1 b2 a1 a2, normalised by a0 */
static void biquad(int kind, double f0norm, double g, double rcpq, double c[5]) {
    double w0 = 2.0 * AL_EFX_PI * f0norm, cw = cos(w0), al = sin(w0) / 2.0 * rcpq, s = 2.0 * sqrt(g) * al;
    double a[3], b[3];
    int i;
    switch (kind) {
        case BQ_HIGHSHELF:
            b[0] = g * ((g + 1.0) + (g - 1.0) * cw + s);
            b[1] = -2.0 * g * ((g - 1.0) + (g + 1.0) * cw);
            b[2] = g * ((g + 1.0) + (g - 1.0) * cw - s);
            a[0] = (g + 1.0) - (g - 1.0) * cw + s;
            a[1] = 2.0 * ((g - 1.0) - (g + 1.0) * cw);
            a[2] = (g + 1.0) - (g - 1.0) * cw - s;
            break;
        case BQ_LOWSHELF:
            b[0] = g * ((g + 1.0) - (g - 1.0) * cw + s);
            b[1] = 2.0 * g * ((g - 1.0) - (g + 1.0) * cw);
            b[2] = g * ((g + 1.0) - (g - 1.0) * cw - s);
            a[0] = (g + 1.0) + (g - 1.0) * cw + s;
            a[1] = -2.0 * ((g - 1.0) + (g + 1.0) * cw);
            a[2] = (g + 1.0) + (g - 1.0) * cw - s;
            break;
        case BQ_PEAKING:
            b[0] = 1.0 + al * g;
            b[1] = -2.0 * cw;
            b[2] = 1.0 - al * g;
            a[0] = 1.0 + al / g;
            a[1] = -2.0 * cw;
            a[2] = 1.0 - al / g;
            break;
        case BQ_LOWPASS:
            b[0] = (1.0 - cw) / 2.0;
            b[1] = 1.0 - cw;
            b[2] = (1.0 - cw) / 2.0;
            a[0] = 1.0 + al;
            a[1] = -2.0 * cw;
            a[2] = 1.0 - al;
            break;
        case BQ_HIGHPASS:
            b[0] = (1.0 + cw) / 2.0;
            b[1] = -(1.0 + cw);
            b[2] = (1.0 + cw) / 2.0;
            a[0] = 1.0 + al;
            a[1] = -2.0 * cw;
            a[2] = 1.0 - al;
            break;
        default:   /* BQ_BANDPASS */
            b[0] = al;
            b[1] = 0.0;
            b[2] = -al;
            a[0] = 1.0 + al;
            a[1] = -2.0 * cw;
            a[2] = 1.0 - al;
            break;
    }
    for (i = 0; i < 3; i++) {
        c[i] = b[i] / a[0];
    }
    c[3] = a[1] / a[0];
    c[4] = a[2] / a[0];
}

/* A kernel's 5 words for a biquad: b0 b1 b2 -a1 -a2 times `scale` (1/2 or 1/4), the b's times `gain` too */
static void biquad_words(const double c[5], double gain, double scale, uint32_t *w) {
    w[0] = q23d(c[0] * gain * scale);
    w[1] = q23d(c[1] * gain * scale);
    w[2] = q23d(c[2] * gain * scale);
    w[3] = q23d(-c[3] * scale);
    w[4] = q23d(-c[4] * scale);
}

/* AL_EFFECT_RING_MODULATOR -> gp_kefx_ringmod's words: the phase step (2^24 a cycle; 0: no modulation),
 * the waveform (3: none), the highpass (slope 0.75, halved; the identity below 1 Hz, as OpenAL Soft's
 * 0 Hz is; at most 0.49 fs) */
static void ringmod_params(const al_efx_reverb_t *r, uint32_t *p) {
    double step = floor((double)r->ringmod_frequency / AL_EFX_RATE * 16777216.0 + 0.5), c[5];
    p[0] = (uint32_t)dmin(step, 16777215.0);
    p[1] = (p[0] == 0u) ? 3u : (uint32_t)r->ringmod_waveform;
    if (r->ringmod_highpass_cutoff < 1.0f) {
        p[2] = q23d(0.5);
        return;
    }
    biquad(BQ_HIGHPASS, dmin(r->ringmod_highpass_cutoff / AL_EFX_RATE, 0.49), 1.0, rcpq_from_slope(1.0, 0.75), c);
    biquad_words(c, 1.0, 0.5, &p[2]);
}

/* AL_EFFECT_DISTORTION -> gp_kefx_distortion's words: the lowpass and the bandpass at 48 kHz with the Q
 * OpenAL Soft's 4x-oversampled design gives them (halved; the bandpass's b's times the EFX gain and the
 * slot's; at most 0.45 fs, 1 / Q at most 20), and d of the folded waveshaper,
 * d = ((1 - e) / (1 + e))^3, e = min(sin(pi/2 EDGE), 0.99) */
static void distortion_params(const al_efx_reverb_t *r, float slot_gain, uint32_t *p) {
    double lpc = r->distortion_lowpass_cutoff, eqc = r->distortion_eq_center, c[5];
    double e = dmin(sin(AL_EFX_PI / 2.0 * r->distortion_edge), 0.99), d = pow((1.0 - e) / (1.0 + e), 3.0);
    double bw = dmin(r->distortion_eq_bandwidth / (eqc * 0.67), 20.0);
    biquad(BQ_LOWPASS, dmin(lpc / AL_EFX_RATE, 0.45), 1.0,
           rcpq_from_bandwidth(lpc / AL_EFX_RATE / 4.0, (lpc / 2.0) / (lpc * 0.67)), c);
    biquad_words(c, 1.0, 0.5, &p[0]);
    biquad(BQ_BANDPASS, dmin(eqc / AL_EFX_RATE, 0.45), 1.0,
           dmin(rcpq_from_bandwidth(eqc / AL_EFX_RATE / 4.0, bw), 20.0), c);
    biquad_words(c, (double)r->distortion_gain * slot_gain, 0.5, &p[5]);
    p[10] = q23d(d);
    p[11] = q23d(1.0 - d);
    if (p[11] > 0x7FFFFFu - p[10]) {
        p[11] = 0x7FFFFFu - p[10];   /* D = d + (1 - d) u stays below 1 */
    }
}

/* AL_EFFECT_EQUALIZER -> gp_kefx_equalizer's words: the bands not at gain 1 (the identity there in
 * OpenAL Soft too), low shelf, mid 1, mid 2, high shelf in that order, each designed with the square root
 * of its gain (slope 0.75 for the shelves, the width in octaves for the peaks), quartered */
static void equalizer_band(int kind, double f0norm, float gain, double width, uint32_t *p) {
    double g = sqrt(gain), c[5];
    biquad(kind, f0norm, g, (kind == BQ_PEAKING) ? rcpq_from_bandwidth(f0norm, width) : rcpq_from_slope(g, 0.75), c);
    biquad_words(c, 1.0, 0.25, &p[1u + 5u * p[0]]);
    p[0]++;
}

static void equalizer_params(const al_efx_reverb_t *r, uint32_t *p) {
    p[0] = 0u;
    if (r->eq_low_gain != 1.0f) {
        equalizer_band(BQ_LOWSHELF, r->eq_low_cutoff / AL_EFX_RATE, r->eq_low_gain, 0.0, p);
    }
    if (r->eq_mid1_gain != 1.0f) {
        equalizer_band(BQ_PEAKING, r->eq_mid1_center / AL_EFX_RATE, r->eq_mid1_gain, r->eq_mid1_width, p);
    }
    if (r->eq_mid2_gain != 1.0f) {
        equalizer_band(BQ_PEAKING, r->eq_mid2_center / AL_EFX_RATE, r->eq_mid2_gain, r->eq_mid2_width, p);
    }
    if (r->eq_high_gain != 1.0f) {
        equalizer_band(BQ_HIGHSHELF, r->eq_high_cutoff / AL_EFX_RATE, r->eq_high_gain, 0.0, p);
    }
}

/* AL_EFFECT_COMPRESSOR -> gp_kefx_compressor's words: on, OpenAL Soft's fixed rates (the envelope from
 * 1/2 to 2 in 0.1 s, back in 0.2 s) as A - 1 and R, the slot's gain */
static void compressor_params(const al_efx_reverb_t *r, float slot_gain, uint32_t *p) {
    p[0] = (uint32_t)r->compressor_onoff;
    p[1] = q23d(pow(2.0 / 0.5, 1.0 / (AL_EFX_RATE * 0.1)) - 1.0);
    p[2] = q23d(pow(0.5 / 2.0, 1.0 / (AL_EFX_RATE * 0.2)));
    p[3] = q23d(slot_gain);
}

/* AL_EFFECT_AUTOWAH -> gp_kefx_autowah's words: OpenAL Soft's derived values (1 - the attack and release
 * rates, the release at least 1 ms; the peak gain 1 - log10(PEAK_GAIN / 31621), / 16; 20 .. 2500 Hz; the
 * resonance's gain sqrt(10 log10(RESONANCE) / 3), / 4 and its reciprocal), the slot's gain */
static void autowah_params(const al_efx_reverb_t *r, float slot_gain, uint32_t *p) {
    double rel = r->autowah_release_time, res = sqrt(log10(r->autowah_resonance) * 10.0 / 3.0);
    if (rel < 0.001) rel = 0.001;
    p[0] = q23d(1.0 - exp(-1.0 / (r->autowah_attack_time * AL_EFX_RATE)));
    p[1] = q23d(1.0 - exp(-1.0 / (rel * AL_EFX_RATE)));
    p[2] = q23d((1.0 - log10(r->autowah_peak_gain / 31621.0)) / 16.0);
    p[3] = q23d(16.0 * (2500.0 - 20.0) / AL_EFX_RATE);
    p[4] = q23d(20.0 / AL_EFX_RATE);
    p[5] = q23d(res / 4.0);
    p[6] = q23d(1.0 / res);
    p[7] = q23d(slot_gain);
}

bool al_efx_slot_gain_in_kernel(const al_efx_slot_t *slot) {
    return slot && (slot->props.type == AL_EFFECT_DISTORTION || slot->props.type == AL_EFFECT_COMPRESSOR ||
                    slot->props.type == AL_EFFECT_AUTOWAH);
}

static void builtin_params(const al_efx_slot_t *slot, uint32_t *p) {
    const al_efx_reverb_t *r = &slot->props.reverb;
    memset(p, 0, AL_EFX_GP_PARAMS * sizeof(uint32_t));
    switch (slot->props.type) {
        case AL_EFFECT_ECHO:
            echo_params(r, p);
            break;
        case AL_EFFECT_CHORUS:
            chorus_params(r->chorus_waveform, r->chorus_phase, r->chorus_rate, r->chorus_depth, r->chorus_feedback,
                          r->chorus_delay, p);
            break;
        case AL_EFFECT_FLANGER:
            chorus_params(r->flanger_waveform, r->flanger_phase, r->flanger_rate, r->flanger_depth,
                          r->flanger_feedback, r->flanger_delay, p);
            break;
        case AL_EFFECT_RING_MODULATOR:
            ringmod_params(r, p);
            break;
        case AL_EFFECT_DISTORTION:
            distortion_params(r, slot->gain, p);
            break;
        case AL_EFFECT_EQUALIZER:
            equalizer_params(r, p);
            break;
        case AL_EFFECT_COMPRESSOR:
            compressor_params(r, slot->gain, p);
            break;
        default:   /* AL_EFFECT_AUTOWAH */
            autowah_params(r, slot->gain, p);
            break;
    }
}

/* The library's kernels, one image per effect type (the chorus's runs the flanger too) */
static const struct {
    ALenum type;
    const uint32_t *image;
    uint32_t words;
} s_builtins[] = {
    { AL_EFFECT_ECHO, gp_kefx_echo_image, GP_KEFX_ECHO_IMAGE_WORDS },
    { AL_EFFECT_CHORUS, gp_kefx_chorus_image, GP_KEFX_CHORUS_IMAGE_WORDS },
    { AL_EFFECT_FLANGER, gp_kefx_chorus_image, GP_KEFX_CHORUS_IMAGE_WORDS },
    { AL_EFFECT_RING_MODULATOR, gp_kefx_ringmod_image, GP_KEFX_RINGMOD_IMAGE_WORDS },
    { AL_EFFECT_DISTORTION, gp_kefx_distortion_image, GP_KEFX_DISTORTION_IMAGE_WORDS },
    { AL_EFFECT_EQUALIZER, gp_kefx_equalizer_image, GP_KEFX_EQUALIZER_IMAGE_WORDS },
    { AL_EFFECT_COMPRESSOR, gp_kefx_compressor_image, GP_KEFX_COMPRESSOR_IMAGE_WORDS },
    { AL_EFFECT_AUTOWAH, gp_kefx_autowah_image, GP_KEFX_AUTOWAH_IMAGE_WORDS },
};

static bool builtin_kernel(ALenum type, apu_gp_kernel_t *k) {
    static apu_gp_kernel_t parsed[COUNT(s_builtins)];
    static int8_t state[COUNT(s_builtins)];   /* 0: not parsed yet, 1: good, -1: bad */
    size_t i;
    for (i = 0; i < COUNT(s_builtins); i++) {
        if (s_builtins[i].type != type) {
            continue;
        }
        if (state[i] == 0) {
            state[i] = (apu_gp_kernel_parse(s_builtins[i].image, s_builtins[i].words, &parsed[i]) == 0) ? 1 : -1;
        }
        if (state[i] > 0 && k) *k = parsed[i];
        return state[i] > 0;
    }
    return false;
}

static bool is_builtin_kernel(ALenum type) {
    size_t i;
    for (i = 0; i < COUNT(s_builtins); i++) {
        if (s_builtins[i].type == type) return true;
    }
    return false;
}

bool al_efx_slot_is_kernel(const al_efx_slot_t *slot) {
    return slot && (slot->props.type == AL_EFFECT_XBOX_GP_PROGRAM || is_builtin_kernel(slot->props.type));
}

/* The GP kernel an effect needs in a slot (the reverb's for the reverbs), false for none */
static bool props_kernel(const al_efx_effect_props_t *e, apu_gp_kernel_t *k) {
    if (e->type == AL_EFFECT_REVERB || e->type == AL_EFFECT_EAXREVERB) {
        const apu_gp_kernel_t *r = apu_gp_reverb_kernel();
        if (r) *k = *r;
        return r != NULL;
    }
    if (is_builtin_kernel(e->type)) {
        return builtin_kernel(e->type, k);
    }
    if (e->type == AL_EFFECT_XBOX_GP_PROGRAM && e->program) {
        *k = e->program->kernel;
        return true;
    }
    return false;
}

/* Do the slots' kernels fit the GP together with `e` in slot `id` (EFX_PLAN.md 3.1: the shared budgets)? */
static bool effects_fit(ALuint id, const al_efx_effect_props_t *e) {
    apu_gp_kernel_t k[AL_EFX_MAX_SLOTS];
    const apu_gp_kernel_t *set[APU_GP_SLOTS] = { NULL, NULL, NULL, NULL };
    uint32_t i;
    for (i = 0; i < AL_EFX_MAX_SLOTS && i < APU_GP_SLOTS; i++) {
        const al_efx_effect_props_t *p = (i + 1u == id) ? e : (s_slots[i].in_use ? &s_slots[i].props : NULL);
        if (p && props_kernel(p, &k[i])) {
            set[i] = &k[i];
        }
    }
    return apu_gp_kernels_fit(set);
}

bool al_efx_slot_kernel(const al_efx_slot_t *slot, apu_gp_kernel_t *k, const uint32_t **params) {
    if (!slot) {
        return false;
    }
    if (is_builtin_kernel(slot->props.type)) {
        uint32_t *p = s_builtin_params[slot - s_slots];
        if (!builtin_kernel(slot->props.type, k)) {
            return false;
        }
        builtin_params(slot, p);
        if (params) *params = p;
        return true;
    }
    if (slot->props.type != AL_EFFECT_XBOX_GP_PROGRAM || !slot->props.program) {
        return false;
    }
    if (k) *k = slot->props.program->kernel;
    if (params) *params = slot->props.params;
    return true;
}


/* ------------------------------------------------------------------ effects */

static void effect_set_type(al_efx_effect_props_t *p, ALenum type) {
    props_release(p);
    p->type = type;
    reverb_defaults(&p->reverb);
    memset(p->params, 0, sizeof(p->params));
}

AL_API void AL_APIENTRY alGenEffects(ALsizei n, ALuint *effects) {
    ALsizei k;
    if (!gen_names(n, effects, &s_effects[0].in_use, sizeof(effect_t), AL_EFX_MAX_EFFECTS)) return;
    for (k = 0; k < n; k++) {
        effect_t *e = &s_effects[effects[k] - 1u];
        e->in_use = true;
        e->props.program = NULL;
        effect_set_type(&e->props, AL_EFFECT_NULL);
    }
}

AL_API void AL_APIENTRY alDeleteEffects(ALsizei n, const ALuint *effects) {
    ALsizei k;
    if (!check_names(n, effects, &s_effects[0].in_use, sizeof(effect_t), AL_EFX_MAX_EFFECTS)) return;
    /* A slot keeps its copy of the effect (the extension's copy semantics) */
    for (k = 0; k < n; k++) {
        if (effects[k] != 0u) {
            s_effects[effects[k] - 1u].in_use = false;
            props_release(&s_effects[effects[k] - 1u].props);
        }
    }
}

AL_API ALboolean AL_APIENTRY alIsEffect(ALuint effect) {
    return (effect == 0u || get_effect(effect)) ? AL_TRUE : AL_FALSE;
}

AL_API void AL_APIENTRY alEffecti(ALuint effect, ALenum param, ALint value) {
    effect_t *e = get_effect(effect);
    const param_desc_t *d;
    if (!e) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (param == AL_EFFECT_TYPE) {
        if (value != AL_EFFECT_NULL && value != AL_EFFECT_REVERB && value != AL_EFFECT_EAXREVERB &&
            !is_builtin_kernel(value) && value != AL_EFFECT_XBOX_GP_PROGRAM) {
            alSetError(AL_INVALID_VALUE);
            return;
        }
        effect_set_type(&e->props, value);
        return;
    }
    {
        uint32_t n;
        if (gp_param_index(&e->props, param, &n)) {
            if (value < -0x800000 || value > 0xFFFFFF) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            e->props.params[n] = (uint32_t)value & 0xFFFFFFu;
            return;
        }
    }
    d = find_param(e->props.type, param);
    if (!d || (d->kind != P_BOOL && d->kind != P_INT)) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    if ((ALfloat)value < d->min || (ALfloat)value > d->max) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    *int_field(&e->props.reverb, d) = value;
}

AL_API void AL_APIENTRY alEffectiv(ALuint effect, ALenum param, const ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    alEffecti(effect, param, values[0]);
}

AL_API void AL_APIENTRY alEffectf(ALuint effect, ALenum param, ALfloat value) {
    effect_t *e = get_effect(effect);
    const param_desc_t *d;
    if (!e) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    {
        uint32_t n;
        if (gp_param_index(&e->props, param, &n)) {
            if (!isfinite(value) || value < -1.0f || value > 1.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            e->props.params[n] = apu_reverb_q23(value) & 0xFFFFFFu;
            return;
        }
    }
    d = find_param(e->props.type, param);
    if (!d || d->kind != P_FLOAT) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    if (!in_range(value, d)) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    *float_field(&e->props.reverb, d) = value;
}

AL_API void AL_APIENTRY alEffectfv(ALuint effect, ALenum param, const ALfloat *values) {
    effect_t *e = get_effect(effect);
    const param_desc_t *d;
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!e) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    d = find_param(e->props.type, param);
    if (d && d->kind == P_VEC) {
        if (!isfinite(values[0]) || !isfinite(values[1]) || !isfinite(values[2])) {
            alSetError(AL_INVALID_VALUE);
            return;
        }
        memcpy(float_field(&e->props.reverb, d), values, 3u * sizeof(ALfloat));
        return;
    }
    alEffectf(effect, param, values[0]);
}

AL_API void AL_APIENTRY alGetEffecti(ALuint effect, ALenum param, ALint *value) {
    effect_t *e = get_effect(effect);
    const param_desc_t *d;
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!e) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (param == AL_EFFECT_TYPE) {
        *value = e->props.type;
        return;
    }
    {
        uint32_t n;
        if (gp_param_index(&e->props, param, &n)) {
            *value = sx24(e->props.params[n]);
            return;
        }
    }
    d = find_param(e->props.type, param);
    if (!d || (d->kind != P_BOOL && d->kind != P_INT)) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    *value = *int_field(&e->props.reverb, d);
}

AL_API void AL_APIENTRY alGetEffectiv(ALuint effect, ALenum param, ALint *values) {
    alGetEffecti(effect, param, values);
}

AL_API void AL_APIENTRY alGetEffectf(ALuint effect, ALenum param, ALfloat *value) {
    effect_t *e = get_effect(effect);
    const param_desc_t *d;
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!e) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    {
        uint32_t n;
        if (gp_param_index(&e->props, param, &n)) {
            *value = (ALfloat)sx24(e->props.params[n]) / 8388608.0f;
            return;
        }
    }
    d = find_param(e->props.type, param);
    if (!d || d->kind != P_FLOAT) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    *value = *float_field(&e->props.reverb, d);
}

AL_API void AL_APIENTRY alGetEffectfv(ALuint effect, ALenum param, ALfloat *values) {
    effect_t *e = get_effect(effect);
    const param_desc_t *d;
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!e) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    d = find_param(e->props.type, param);
    if (d && d->kind == P_VEC) {
        memcpy(values, float_field(&e->props.reverb, d), 3u * sizeof(ALfloat));
        return;
    }
    alGetEffectf(effect, param, values);
}


/* ------------------------------------------------------------------ filters */

/* AL_LOWPASS_GAIN / AL_LOWPASS_GAINHF: 0..1, default 1 (EFX 1.0) */
static void filter_defaults(al_efx_filter_props_t *p, ALenum type) {
    p->type = type;
    p->gain = 1.0f;
    p->gain_hf = 1.0f;
}

ALfloat al_efx_filter_gain(const al_efx_filter_props_t *f) {
    return (f && f->type == AL_FILTER_LOWPASS) ? f->gain : 1.0f;
}

ALfloat al_efx_filter_gain_hf(const al_efx_filter_props_t *f) {
    return (f && f->type == AL_FILTER_LOWPASS) ? f->gain_hf : 1.0f;
}

static ALfloat *filter_field(al_efx_filter_props_t *p, ALenum param) {
    if (p->type != AL_FILTER_LOWPASS) return NULL;
    if (param == AL_LOWPASS_GAIN) return &p->gain;
    if (param == AL_LOWPASS_GAINHF) return &p->gain_hf;
    return NULL;
}

AL_API void AL_APIENTRY alGenFilters(ALsizei n, ALuint *filters) {
    ALsizei k;
    if (!gen_names(n, filters, &s_filters[0].in_use, sizeof(filter_t), AL_EFX_MAX_FILTERS)) return;
    for (k = 0; k < n; k++) {
        filter_t *f = &s_filters[filters[k] - 1u];
        f->in_use = true;
        filter_defaults(&f->props, AL_FILTER_NULL);
    }
}

AL_API void AL_APIENTRY alDeleteFilters(ALsizei n, const ALuint *filters) {
    ALsizei k;
    if (!check_names(n, filters, &s_filters[0].in_use, sizeof(filter_t), AL_EFX_MAX_FILTERS)) return;
    /* Sources keep their copies */
    for (k = 0; k < n; k++) {
        if (filters[k] != 0u) s_filters[filters[k] - 1u].in_use = false;
    }
}

AL_API ALboolean AL_APIENTRY alIsFilter(ALuint filter) {
    return (filter == 0u || get_filter(filter)) ? AL_TRUE : AL_FALSE;
}

AL_API void AL_APIENTRY alFilteri(ALuint filter, ALenum param, ALint value) {
    filter_t *f = get_filter(filter);
    if (!f) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (param != AL_FILTER_TYPE) {
        alSetError(AL_INVALID_ENUM);   /* AL_FILTER_NULL has no parameters */
        return;
    }
    /* Lowpass on the voice filter; highpass and bandpass are not supported */
    if (value != AL_FILTER_NULL && value != AL_FILTER_LOWPASS) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    filter_defaults(&f->props, value);
}

AL_API void AL_APIENTRY alFilteriv(ALuint filter, ALenum param, const ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    alFilteri(filter, param, values[0]);
}

AL_API void AL_APIENTRY alFilterf(ALuint filter, ALenum param, ALfloat value) {
    filter_t *f = get_filter(filter);
    ALfloat *field;
    if (!f) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    field = filter_field(&f->props, param);
    if (!field) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    if (!isfinite(value) || value < 0.0f || value > 1.0f) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    *field = value;
}

AL_API void AL_APIENTRY alFilterfv(ALuint filter, ALenum param, const ALfloat *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    alFilterf(filter, param, values[0]);
}

AL_API void AL_APIENTRY alGetFilteri(ALuint filter, ALenum param, ALint *value) {
    filter_t *f = get_filter(filter);
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!f) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (param != AL_FILTER_TYPE) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    *value = f->props.type;
}

AL_API void AL_APIENTRY alGetFilteriv(ALuint filter, ALenum param, ALint *values) {
    alGetFilteri(filter, param, values);
}

AL_API void AL_APIENTRY alGetFilterf(ALuint filter, ALenum param, ALfloat *value) {
    filter_t *f = get_filter(filter);
    ALfloat *field;
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!f) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    field = filter_field(&f->props, param);
    if (!field) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    *value = *field;
}

AL_API void AL_APIENTRY alGetFilterfv(ALuint filter, ALenum param, ALfloat *values) {
    alGetFilterf(filter, param, values);
}

/* ------------------------------------------------------------------ auxiliary effect slots */

ALuint al_efx_active_slot(void) {
    ALuint i;
    for (i = 0; i < AL_EFX_MAX_SLOTS; i++) {
        if (s_slots[i].in_use && s_slots[i].props.type != AL_EFFECT_NULL) return i + 1u;
    }
    return 0u;
}

const al_efx_slot_t *al_efx_slot(ALuint id) {
    return get_slot(id);
}

AL_API void AL_APIENTRY alGenAuxiliaryEffectSlots(ALsizei n, ALuint *slots) {
    ALsizei k;
    if (!gen_names(n, slots, &s_slots[0].in_use, sizeof(al_efx_slot_t), AL_EFX_MAX_SLOTS)) return;
    for (k = 0; k < n; k++) {
        al_efx_slot_t *s = &s_slots[slots[k] - 1u];
        memset(s, 0, sizeof(*s));   /* (a deleted slot released its image) */
        s->in_use = true;
        effect_set_type(&s->props, AL_EFFECT_NULL);
        s->gain = AL_EFFECTSLOT_DEFAULT_GAIN;
        s->send_auto = AL_EFFECTSLOT_DEFAULT_AUXILIARY_SEND_AUTO;
    }
}

static void timed_drop_slot(ALuint slot);

AL_API void AL_APIENTRY alDeleteAuxiliaryEffectSlots(ALsizei n, const ALuint *slots) {
    ALsizei k;
    if (!check_names(n, slots, &s_slots[0].in_use, sizeof(al_efx_slot_t), AL_EFX_MAX_SLOTS)) return;
    for (k = 0; k < n; k++) {
        if (slots[k] != 0u && s_slots[slots[k] - 1u].refs > 0u) {
            alSetError(AL_INVALID_OPERATION);   /* a source still sends to it */
            return;
        }
    }
    for (k = 0; k < n; k++) {
        if (slots[k] != 0u) {
            timed_drop_slot(slots[k]);
            s_slots[slots[k] - 1u].in_use = false;
            props_release(&s_slots[slots[k] - 1u].props);
        }
    }
    if (n > 0) {
        al_source_effects_changed();   /* an active slot may have gone */
    }
}

AL_API ALboolean AL_APIENTRY alIsAuxiliaryEffectSlot(ALuint slot) {
    return get_slot(slot) ? AL_TRUE : AL_FALSE;
}

AL_API void AL_APIENTRY alAuxiliaryEffectSloti(ALuint slot, ALenum param, ALint value) {
    al_efx_slot_t *s = get_slot(slot);
    if (!s) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    switch (param) {
        case AL_EFFECTSLOT_EFFECT: {
            const effect_t *e = NULL;
            if (value != 0) {
                e = get_effect((ALuint)value);
                if (!e) {
                    alSetError(AL_INVALID_VALUE);
                    return;
                }
            }
            /* Every slot runs its own kernel; refused only when the GP's budgets are full (P, X, Y,
             * scratch, descriptors), or while AL_XBOX_REVERB owns the GP (EFX_PLAN 3.1) */
            if (e && e->props.type != AL_EFFECT_NULL && (alIsEnabled(AL_XBOX_REVERB) || !effects_fit(slot, &e->props))) {
                alSetError(AL_INVALID_OPERATION);
                return;
            }
            timed_drop_slot(slot);   /* an attach now replaces the changes queued for later */
            if (e) {
                props_assign(&s->props, &e->props);
            } else {
                effect_set_type(&s->props, AL_EFFECT_NULL);
            }
            s->effect = (ALuint)value;
            al_source_effects_changed();
            break;
        }
        case AL_EFFECTSLOT_AUXILIARY_SEND_AUTO:
            if (value != AL_FALSE && value != AL_TRUE) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            s->send_auto = (ALboolean)value;
            al_source_effects_changed();
            break;
        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alAuxiliaryEffectSlotiv(ALuint slot, ALenum param, const ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    alAuxiliaryEffectSloti(slot, param, values[0]);
}

AL_API void AL_APIENTRY alAuxiliaryEffectSlotf(ALuint slot, ALenum param, ALfloat value) {
    al_efx_slot_t *s = get_slot(slot);
    if (!s) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (param != AL_EFFECTSLOT_GAIN) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    if (!isfinite(value) || value < AL_EFFECTSLOT_MIN_GAIN || value > AL_EFFECTSLOT_MAX_GAIN) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    s->gain = value;
    if (s->props.type != AL_EFFECT_NULL) {
        al_source_effects_changed();
    }
}

AL_API void AL_APIENTRY alAuxiliaryEffectSlotfv(ALuint slot, ALenum param, const ALfloat *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    alAuxiliaryEffectSlotf(slot, param, values[0]);
}

AL_API void AL_APIENTRY alGetAuxiliaryEffectSloti(ALuint slot, ALenum param, ALint *value) {
    al_efx_slot_t *s = get_slot(slot);
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!s) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    switch (param) {
        case AL_EFFECTSLOT_EFFECT:
            *value = (ALint)s->effect;
            break;
        case AL_EFFECTSLOT_AUXILIARY_SEND_AUTO:
            *value = s->send_auto;
            break;
        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetAuxiliaryEffectSlotiv(ALuint slot, ALenum param, ALint *values) {
    alGetAuxiliaryEffectSloti(slot, param, values);
}

AL_API void AL_APIENTRY alGetAuxiliaryEffectSlotf(ALuint slot, ALenum param, ALfloat *value) {
    al_efx_slot_t *s = get_slot(slot);
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (!s) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (param != AL_EFFECTSLOT_GAIN) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    *value = s->gain;
}

AL_API void AL_APIENTRY alGetAuxiliaryEffectSlotfv(ALuint slot, ALenum param, ALfloat *values) {
    alGetAuxiliaryEffectSlotf(slot, param, values);
}

/* ------------------------------------------------------------------ sources and listener */

void al_efx_source_defaults(al_efx_source_t *s) {
    int i;
    memset(s, 0, sizeof(*s));
    filter_defaults(&s->direct_filter, AL_FILTER_NULL);
    for (i = 0; i < AL_EFX_MAX_SENDS; i++) {
        filter_defaults(&s->send[i].filter, AL_FILTER_NULL);
    }
    s->air_absorption_factor = AL_DEFAULT_AIR_ABSORPTION_FACTOR;
    s->room_rolloff_factor = AL_DEFAULT_ROOM_ROLLOFF_FACTOR;
    s->cone_outer_gain_hf = AL_DEFAULT_CONE_OUTER_GAINHF;
    s->direct_gain_hf_auto = AL_DEFAULT_DIRECT_FILTER_GAINHF_AUTO;
    s->send_gain_auto = AL_DEFAULT_AUXILIARY_SEND_FILTER_GAIN_AUTO;
    s->send_gain_hf_auto = AL_DEFAULT_AUXILIARY_SEND_FILTER_GAINHF_AUTO;
}

void al_efx_source_release(al_efx_source_t *s) {
    int i;
    for (i = 0; i < AL_EFX_MAX_SENDS; i++) {
        al_efx_slot_t *slot = get_slot(s->send[i].slot);
        if (slot && slot->refs > 0u) slot->refs--;
        s->send[i].slot = 0u;
    }
}

static bool set_bounded(ALfloat *dst, ALfloat value, ALfloat lo, ALfloat hi) {
    if (!isfinite(value) || value < lo || value > hi) {
        alSetError(AL_INVALID_VALUE);
        return true;
    }
    *dst = value;
    return true;
}

static bool set_flag(ALboolean *dst, ALint value) {
    if (value != AL_FALSE && value != AL_TRUE) {
        alSetError(AL_INVALID_VALUE);
        return true;
    }
    *dst = (ALboolean)value;
    return true;
}

bool al_efx_source_setf(al_efx_source_t *s, ALenum param, ALfloat value) {
    switch (param) {
        case AL_AIR_ABSORPTION_FACTOR:
            return set_bounded(&s->air_absorption_factor, value, AL_MIN_AIR_ABSORPTION_FACTOR,
                               AL_MAX_AIR_ABSORPTION_FACTOR);
        case AL_ROOM_ROLLOFF_FACTOR:
            return set_bounded(&s->room_rolloff_factor, value, AL_MIN_ROOM_ROLLOFF_FACTOR, AL_MAX_ROOM_ROLLOFF_FACTOR);
        case AL_CONE_OUTER_GAINHF:
            return set_bounded(&s->cone_outer_gain_hf, value, AL_MIN_CONE_OUTER_GAINHF, AL_MAX_CONE_OUTER_GAINHF);
        default:
            return false;
    }
}

bool al_efx_source_seti(al_efx_source_t *s, ALenum param, ALint value) {
    switch (param) {
        case AL_DIRECT_FILTER: {
            const filter_t *f = NULL;
            if (value != 0) {
                f = get_filter((ALuint)value);
                if (!f) {
                    alSetError(AL_INVALID_VALUE);
                    return true;
                }
            }
            if (f) {
                s->direct_filter = f->props;   /* a copy, as the extension says */
            } else {
                filter_defaults(&s->direct_filter, AL_FILTER_NULL);
            }
            return true;
        }
        case AL_DIRECT_FILTER_GAINHF_AUTO:
            return set_flag(&s->direct_gain_hf_auto, value);
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
            return set_flag(&s->send_gain_auto, value);
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
            return set_flag(&s->send_gain_hf_auto, value);
        default:
            return false;
    }
}

bool al_efx_source_set3i(al_efx_source_t *s, ALenum param, ALint slot_id, ALint send, ALint filter_id) {
    al_efx_slot_t *slot = NULL, *old;
    const filter_t *f = NULL;
    if (param != AL_AUXILIARY_SEND_FILTER) {
        return false;
    }
    if (send < 0 || send >= s_max_sends) {
        alSetError(AL_INVALID_VALUE);
        return true;
    }
    if (slot_id != 0) {
        slot = get_slot((ALuint)slot_id);
        if (!slot) {
            alSetError(AL_INVALID_VALUE);
            return true;
        }
    }
    if (filter_id != 0) {
        f = get_filter((ALuint)filter_id);
        if (!f) {
            alSetError(AL_INVALID_VALUE);
            return true;
        }
    }
    old = get_slot(s->send[send].slot);
    if (old && old->refs > 0u) old->refs--;
    if (slot) slot->refs++;
    s->send[send].slot = (ALuint)slot_id;
    if (f) {
        s->send[send].filter = f->props;
    } else {
        filter_defaults(&s->send[send].filter, AL_FILTER_NULL);
    }
    return true;
}

bool al_efx_source_getf(const al_efx_source_t *s, ALenum param, ALfloat *value) {
    switch (param) {
        case AL_AIR_ABSORPTION_FACTOR:
            *value = s->air_absorption_factor;
            return true;
        case AL_ROOM_ROLLOFF_FACTOR:
            *value = s->room_rolloff_factor;
            return true;
        case AL_CONE_OUTER_GAINHF:
            *value = s->cone_outer_gain_hf;
            return true;
        default:
            return false;
    }
}

bool al_efx_source_geti(const al_efx_source_t *s, ALenum param, ALint *value) {
    switch (param) {
        case AL_DIRECT_FILTER_GAINHF_AUTO:
            *value = s->direct_gain_hf_auto;
            return true;
        case AL_AUXILIARY_SEND_FILTER_GAIN_AUTO:
            *value = s->send_gain_auto;
            return true;
        case AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO:
            *value = s->send_gain_hf_auto;
            return true;
        default:
            return false;
    }
}

ALfloat al_efx_meters_per_unit(void) {
    return s_meters_per_unit;
}

bool al_efx_set_meters_per_unit(ALfloat value) {
    if (!isfinite(value) || value < FLT_MIN) {
        return false;
    }
    s_meters_per_unit = value;
    return true;
}

void al_efx_set_max_sends(ALint sends) {
    s_max_sends = (sends < 0) ? 0 : (sends > AL_EFX_MAX_SENDS ? AL_EFX_MAX_SENDS : sends);
}

ALint al_efx_max_sends(void) {
    return s_max_sends;
}

float al_efx_onepole_for_gain(float gain_hf, float hz) {
    /* lp = s + p (lp - s): |H| = (1 - p) / sqrt(1 - 2 p cos w + p^2); solved for |H| = g */
    double g = gain_hf, c = cos(2.0 * 3.14159265358979 * hz / 48000.0), a, k, p;
    if (!(g < 1.0)) return 0.0f;
    if (!(g > 0.0)) return 0.95f;
    a = 1.0 - g * g;
    k = 1.0 - g * g * c;
    p = (k - sqrt(k * k - a * a)) / a;
    return (float)(p < 0.0 ? 0.0 : (p > 0.95 ? 0.95 : p));
}

ALfloat al_efx_room_rolloff(const al_efx_source_t *s, const al_efx_slot_t *slot) {
    ALfloat r = s->room_rolloff_factor;
    if (slot && slot->send_auto &&
        (slot->props.type == AL_EFFECT_REVERB || slot->props.type == AL_EFFECT_EAXREVERB)) {
        r += slot->props.reverb.room_rolloff_factor;
    }
    return r;
}

void al_efx_reverb_params(const al_efx_slot_t *slot, apu_reverb_params_t *out) {
    const al_efx_reverb_t *r = &slot->props.reverb;
    float wet = r->gain * r->late_reverb_gain * slot->gain;
    float pre = r->reflections_delay + r->late_reverb_delay;
    apu_reverb_default_params(out);   /* input gain (headroom) and no output lowpass */
    out->decay_time = r->decay_time;
    out->hf_ratio = (r->decay_hf_ratio < 1.0f) ? r->decay_hf_ratio : 1.0f;
    out->gain = (wet < 1.0f) ? wet : 1.0f;
    out->pre_delay = (pre < 0.1f) ? pre : 0.1f;
    out->diffusion = r->diffusion;
    out->input_lowpass = al_efx_onepole_for_gain(r->gain_hf, r->hf_reference);
    out->output_lowpass = 0.0f;
}

/* ------------------------------------------------------------------ timed slot changes */

/*
 * alXboxEffectSlotAtSample: the effect's state, copied when it is queued,
 * reaches the slot's kernel at the GP frame that mixes `sample` of the
 * streaming source. The GP holds one post at a time (apu_gp_kernel_params_at);
 * the rest wait here, in order, and go out as the GP takes the one before.
 */
#define TIMED_MAX            16u
#define TIMED_LATENCY_FRAMES 1u     /* the GP mixes a frame the VP played one frame later (console: openal_efx 6c) */

typedef struct {
    ALuint slot, effect, source;
    uint32_t sample;
    al_efx_effect_props_t props;
} timed_t;

static timed_t s_timed[TIMED_MAX];
static uint32_t s_timed_n;

static void timed_remove(uint32_t i) {
    props_release(&s_timed[i].props);
    memmove(&s_timed[i], &s_timed[i + 1u], (s_timed_n - i - 1u) * sizeof(timed_t));
    s_timed_n--;
    memset(&s_timed[s_timed_n], 0, sizeof(timed_t));
}

static void timed_drop_slot(ALuint slot) {
    uint32_t i = 0;
    while (i < s_timed_n) {
        if (s_timed[i].slot == slot) {
            timed_remove(i);
        } else {
            i++;
        }
    }
}

/* The same kernel code (a new image restarts the kernel, which no frame can time) */
static bool same_kernel(const al_efx_effect_props_t *a, const al_efx_effect_props_t *b) {
    if (a->type != AL_EFFECT_XBOX_GP_PROGRAM || b->type != AL_EFFECT_XBOX_GP_PROGRAM || !a->program || !b->program) {
        return false;
    }
    return a->program == b->program ||
           (a->program->kernel.words == b->program->kernel.words &&
            a->program->kernel.scratch_bytes == b->program->kernel.scratch_bytes &&
            memcmp(a->program->kernel.code, b->program->kernel.code, a->program->kernel.words * sizeof(uint32_t)) == 0);
}

void al_efx_timed_service(void) {
    while (s_timed_n > 0u) {
        timed_t *t = &s_timed[0];
        al_efx_slot_t *slot = get_slot(t->slot);
        uint32_t pos = 0, now = 0, frame;
        float per = 0.0f;
        al_source_clock_t c;

        if (!slot) {
            timed_remove(0);
            continue;
        }
        c = al_source_stream_clock(t->source, &pos, &now, &per);
        if (c == AL_SOURCE_CLOCK_WAIT || (c == AL_SOURCE_CLOCK_RUN && al_source_gp_timed_busy())) {
            return;   /* try again at the next update */
        }
        frame = now;
        if (c == AL_SOURCE_CLOCK_RUN && per > 0.0f) {
            int32_t ahead = (int32_t)(t->sample - pos);   /* the stream position wraps at 2^32 */
            if (ahead > 0) {
                frame = now + (uint32_t)ceilf((float)ahead / per) + TIMED_LATENCY_FRAMES;
            }
        }
        props_assign(&slot->props, &t->props);
        slot->effect = t->effect;
        timed_remove(0);
        if (slot->props.type != AL_EFFECT_NULL) {
            al_source_gp_params_at((ALuint)(slot - s_slots) + 1u, frame);
        }
    }
}

AL_API void AL_APIENTRY alXboxEffectSlotAtSample(ALuint slot, ALuint effect, ALuint source, ALuint sample) {
    al_efx_slot_t *s = get_slot(slot);
    const effect_t *e = get_effect(effect);
    uint32_t pos, now;
    float per;
    timed_t *t;

    if (!s || slot == 0u || !e || effect == 0u) {
        alSetError(AL_INVALID_NAME);
        return;
    }
    if (al_source_stream_clock(source, &pos, &now, &per) == AL_SOURCE_CLOCK_NONE) {
        alSetError(alIsSource(source) ? AL_INVALID_OPERATION : AL_INVALID_NAME);
        return;
    }
    /* Parameters for the kernel this slot runs, and room in the queue */
    if (!same_kernel(&s->props, &e->props) || s_timed_n == TIMED_MAX) {
        alSetError(AL_INVALID_OPERATION);
        return;
    }
    t = &s_timed[s_timed_n++];
    t->slot = slot;
    t->effect = effect;
    t->source = source;
    t->sample = sample;
    props_assign(&t->props, &e->props);
    al_efx_timed_service();
}

uint32_t al_efx_debug_timed_queued(void) {
    return s_timed_n;
}

void al_efx_reset(void) {
    uint32_t i;
    while (s_timed_n > 0u) timed_remove(0);
    for (i = 0; i < AL_EFX_MAX_EFFECTS; i++) props_release(&s_effects[i].props);
    for (i = 0; i < AL_EFX_MAX_SLOTS; i++) props_release(&s_slots[i].props);
    memset(s_effects, 0, sizeof(s_effects));
    memset(s_filters, 0, sizeof(s_filters));
    memset(s_slots, 0, sizeof(s_slots));
    s_meters_per_unit = AL_DEFAULT_METERS_PER_UNIT;
    s_max_sends = AL_EFX_MAX_SENDS;
}
