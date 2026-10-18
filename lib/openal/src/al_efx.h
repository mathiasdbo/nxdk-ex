#ifndef AL_EFX_INTERNAL_H
#define AL_EFX_INTERNAL_H

/*
 * ALC_EXT_EFX objects (lib/openal/docs/EFX_PLAN.md 3.1): effects, filters,
 * auxiliary effect slots and the per-source / listener EFX state. This file
 * stores and validates and maps the reverb onto the GP's; al_source.c routes
 * the sends and drives the GP (EFX_PLAN.md 3.2) through al_efx_slot() and
 * al_efx_active_slot(), and hears of slot changes through
 * al_source_effects_changed().
 */

#include <AL/al.h>
#include <AL/efx.h>
#include <stdbool.h>
#include <stdint.h>

#include "apu_gp.h"
#include "apu_reverb.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AL_EFX_MAX_EFFECTS  256u
#define AL_EFX_MAX_FILTERS  256u
#define AL_EFX_MAX_SLOTS    4u
#define AL_EFX_MAX_SENDS    2
#define AL_EFX_GP_PARAMS    32u     /* AL_XBOX_GP_PARAMS */

/* An AL_XBOX_gp_program image (al_efx.c): shared, never changed, freed with its last user */
typedef struct al_efx_program_s al_efx_program_t;

/* Reverb parameters: AL_EFFECT_REVERB uses a subset of AL_EFFECT_EAXREVERB's */
typedef struct {
    ALfloat density, diffusion, gain, gain_hf, gain_lf;
    ALfloat decay_time, decay_hf_ratio, decay_lf_ratio;
    ALfloat reflections_gain, reflections_delay, reflections_pan[3];
    ALfloat late_reverb_gain, late_reverb_delay, late_reverb_pan[3];
    ALfloat echo_time, echo_depth, modulation_time, modulation_depth;
    ALfloat air_absorption_gain_hf, hf_reference, lf_reference, room_rolloff_factor;
    ALint decay_hf_limit;
} al_efx_reverb_t;

/* An effect's state: what an effect object holds and what a slot copies */
typedef struct {
    ALenum type;                /* AL_EFFECT_NULL, AL_EFFECT_REVERB, AL_EFFECT_EAXREVERB, AL_EFFECT_XBOX_GP_PROGRAM */
    al_efx_reverb_t reverb;
    al_efx_program_t *program;  /* AL_EFFECT_XBOX_GP_PROGRAM: the image, NULL before alXboxEffectProgram */
    uint32_t params[AL_EFX_GP_PARAMS];   /* AL_XBOX_GP_PARAM0 + n, 24-bit */
} al_efx_effect_props_t;

/* A filter's state (copied into a source when set) */
typedef struct {
    ALenum type;                /* AL_FILTER_NULL or AL_FILTER_LOWPASS */
    ALfloat gain;               /* AL_LOWPASS_GAIN, 0..1 (1 for AL_FILTER_NULL) */
    ALfloat gain_hf;            /* AL_LOWPASS_GAINHF, 0..1: the gain at 5 kHz */
} al_efx_filter_props_t;

/** A filter's overall gain and its gain at 5 kHz (1 / 1 for AL_FILTER_NULL). */
ALfloat al_efx_filter_gain(const al_efx_filter_props_t *f);
ALfloat al_efx_filter_gain_hf(const al_efx_filter_props_t *f);

typedef struct {
    bool in_use;
    ALuint effect;              /* the effect id last attached (AL_EFFECTSLOT_EFFECT reads it back) */
    al_efx_effect_props_t props;
    ALfloat gain;               /* AL_EFFECTSLOT_GAIN */
    ALboolean send_auto;        /* AL_EFFECTSLOT_AUXILIARY_SEND_AUTO */
    uint32_t refs;              /* source sends pointing at the slot */
} al_efx_slot_t;

/* A source's EFX state (ALsource.efx) */
typedef struct {
    al_efx_filter_props_t direct_filter;
    struct {
        ALuint slot;            /* 0 = no send */
        al_efx_filter_props_t filter;
    } send[AL_EFX_MAX_SENDS];
    ALfloat air_absorption_factor;
    ALfloat room_rolloff_factor;
    ALfloat cone_outer_gain_hf;
    ALboolean direct_gain_hf_auto;
    ALboolean send_gain_auto;
    ALboolean send_gain_hf_auto;
} al_efx_source_t;

/** Defaults for a new or reset source. */
void al_efx_source_defaults(al_efx_source_t *s);

/** Drop the source's slot references (source deleted or reset). */
void al_efx_source_release(al_efx_source_t *s);

/*
 * Source parameters: each returns true if `param` is an EFX source parameter
 * (and has then set any error itself), false to let the caller report
 * AL_INVALID_ENUM.
 */
bool al_efx_source_setf(al_efx_source_t *s, ALenum param, ALfloat value);
bool al_efx_source_seti(al_efx_source_t *s, ALenum param, ALint value);
bool al_efx_source_set3i(al_efx_source_t *s, ALenum param, ALint v1, ALint v2, ALint v3);
bool al_efx_source_getf(const al_efx_source_t *s, ALenum param, ALfloat *value);
bool al_efx_source_geti(const al_efx_source_t *s, ALenum param, ALint *value);

/** AL_METERS_PER_UNIT (listener). */
ALfloat al_efx_meters_per_unit(void);
bool al_efx_set_meters_per_unit(ALfloat value);

/** Sends per source of the current context (ALC_MAX_AUXILIARY_SENDS), 0..AL_EFX_MAX_SENDS. */
void al_efx_set_max_sends(ALint sends);
ALint al_efx_max_sends(void);

/** The slot `id` (1..AL_EFX_MAX_SLOTS) if it exists, else NULL. */
const al_efx_slot_t *al_efx_slot(ALuint id);

/** The slot holding an effect other than AL_EFFECT_NULL (one at a time), 0 if none. */
ALuint al_efx_active_slot(void);

/** Free every effect, filter and slot (last device closed). */
void al_efx_reset(void);

/**
 * The GP reverb's parameters for a slot holding AL_EFFECT_REVERB or
 * AL_EFFECT_EAXREVERB (EFX_PLAN.md 3.5, an approximation): decay time, HF
 * ratio (at most 1), wet gain = GAIN x LATE_REVERB_GAIN x the slot's gain,
 * pre-delay = REFLECTIONS_DELAY + LATE_REVERB_DELAY (at most 0.1 s),
 * diffusion, and GAINHF as a one-pole lowpass on the input at HFREFERENCE.
 */
void al_efx_reverb_params(const al_efx_slot_t *slot, apu_reverb_params_t *out);

/** The one-pole coefficient whose gain at `hz` (48 kHz) is `gain_hf` (0 for 1 and above), at most 0.95. */
float al_efx_onepole_for_gain(float gain_hf, float hz);

/** Room rolloff of a source's send to `slot`: the source's plus, with the slot's send-auto flag, the effect's. */
ALfloat al_efx_room_rolloff(const al_efx_source_t *s, const al_efx_slot_t *slot);

/**
 * The kernel of a slot holding AL_EFFECT_XBOX_GP_PROGRAM with an image: fills
 * `k` (pointing into the image) and `params`. False for any other slot.
 */
bool al_efx_slot_kernel(const al_efx_slot_t *slot, apu_gp_kernel_t *k, const uint32_t **params);

#ifdef __cplusplus
}
#endif

#endif /* AL_EFX_INTERNAL_H */
