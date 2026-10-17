/*
 * ALC_EXT_EFX 1.0 objects (lib/openal/docs/EFX_PLAN.md 3.1, task phase8-01):
 * effects, filters, auxiliary effect slots, and the EFX source / listener
 * parameters. Storage and validation only; nothing reaches the APU yet.
 *
 * Supported: effect types AL_EFFECT_NULL, AL_EFFECT_REVERB and
 * AL_EFFECT_EAXREVERB (every other type is AL_INVALID_VALUE, as the extension
 * allows), filter type AL_FILTER_NULL, AL_EFX_MAX_SLOTS slots of which one may
 * hold an effect at a time (the GP runs one program), and up to
 * AL_EFX_MAX_SENDS sends per source.
 */
#define AL_ALEXT_PROTOTYPES
#include "al_efx.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#include <AL/alext.h>

#include "al_buffer.h"   /* alSetError */

/* ------------------------------------------------------------------ parameter tables */

enum { P_FLOAT, P_VEC, P_BOOL };

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

static const param_desc_t *find_param(ALenum type, ALenum param) {
    const param_desc_t *t = NULL;
    size_t i, n = 0;
    if (type == AL_EFFECT_REVERB) {
        t = s_reverb_params;
        n = COUNT(s_reverb_params);
    } else if (type == AL_EFFECT_EAXREVERB) {
        t = s_eaxreverb_params;
        n = COUNT(s_eaxreverb_params);
    }
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

/* Every reverb field at its default (the EAX table covers the reverb's fields too) */
static void reverb_defaults(al_efx_reverb_t *r) {
    size_t i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < COUNT(s_eaxreverb_params); i++) {
        const param_desc_t *d = &s_eaxreverb_params[i];
        if (d->kind == P_BOOL) {
            *int_field(r, d) = (ALint)d->def;
        } else if (d->kind == P_VEC) {
            float_field(r, d)[0] = float_field(r, d)[1] = float_field(r, d)[2] = d->def;
        } else {
            *float_field(r, d) = d->def;
        }
    }
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

/* ------------------------------------------------------------------ effects */

static void effect_set_type(al_efx_effect_props_t *p, ALenum type) {
    p->type = type;
    reverb_defaults(&p->reverb);
}

AL_API void AL_APIENTRY alGenEffects(ALsizei n, ALuint *effects) {
    ALsizei k;
    if (!gen_names(n, effects, &s_effects[0].in_use, sizeof(effect_t), AL_EFX_MAX_EFFECTS)) return;
    for (k = 0; k < n; k++) {
        effect_t *e = &s_effects[effects[k] - 1u];
        e->in_use = true;
        effect_set_type(&e->props, AL_EFFECT_NULL);
    }
}

AL_API void AL_APIENTRY alDeleteEffects(ALsizei n, const ALuint *effects) {
    ALsizei k;
    if (!check_names(n, effects, &s_effects[0].in_use, sizeof(effect_t), AL_EFX_MAX_EFFECTS)) return;
    /* A slot keeps its copy of the effect (the extension's copy semantics) */
    for (k = 0; k < n; k++) {
        if (effects[k] != 0u) s_effects[effects[k] - 1u].in_use = false;
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
        if (value != AL_EFFECT_NULL && value != AL_EFFECT_REVERB && value != AL_EFFECT_EAXREVERB) {
            alSetError(AL_INVALID_VALUE);
            return;
        }
        effect_set_type(&e->props, value);
        return;
    }
    d = find_param(e->props.type, param);
    if (!d || d->kind != P_BOOL) {
        alSetError(AL_INVALID_ENUM);
        return;
    }
    if (value != AL_FALSE && value != AL_TRUE) {
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
    d = find_param(e->props.type, param);
    if (!d || d->kind != P_BOOL) {
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

AL_API void AL_APIENTRY alGenFilters(ALsizei n, ALuint *filters) {
    ALsizei k;
    if (!gen_names(n, filters, &s_filters[0].in_use, sizeof(filter_t), AL_EFX_MAX_FILTERS)) return;
    for (k = 0; k < n; k++) {
        filter_t *f = &s_filters[filters[k] - 1u];
        f->in_use = true;
        f->props.type = AL_FILTER_NULL;
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
    /* Lowpass, highpass and bandpass wait for the VP filter probe (phase8-00) */
    if (value != AL_FILTER_NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    f->props.type = value;
}

AL_API void AL_APIENTRY alFilteriv(ALuint filter, ALenum param, const ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    alFilteri(filter, param, values[0]);
}

AL_API void AL_APIENTRY alFilterf(ALuint filter, ALenum param, ALfloat value) {
    (void)param;
    (void)value;
    alSetError(get_filter(filter) ? AL_INVALID_ENUM : AL_INVALID_NAME);
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
    (void)param;
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    alSetError(get_filter(filter) ? AL_INVALID_ENUM : AL_INVALID_NAME);
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
        memset(s, 0, sizeof(*s));
        s->in_use = true;
        effect_set_type(&s->props, AL_EFFECT_NULL);
        s->gain = AL_EFFECTSLOT_DEFAULT_GAIN;
        s->send_auto = AL_EFFECTSLOT_DEFAULT_AUXILIARY_SEND_AUTO;
    }
}

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
        if (slots[k] != 0u) s_slots[slots[k] - 1u].in_use = false;
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
            ALuint active;
            if (value != 0) {
                e = get_effect((ALuint)value);
                if (!e) {
                    alSetError(AL_INVALID_VALUE);
                    return;
                }
            }
            /* One effect per device: the GP runs one program (EFX_PLAN 3.1) */
            active = al_efx_active_slot();
            if (e && e->props.type != AL_EFFECT_NULL &&
                ((active != 0u && active != slot) || alIsEnabled(AL_XBOX_REVERB))) {
                alSetError(AL_INVALID_OPERATION);
                return;
            }
            if (e) {
                s->props = e->props;
            } else {
                effect_set_type(&s->props, AL_EFFECT_NULL);
            }
            s->effect = (ALuint)value;
            break;
        }
        case AL_EFFECTSLOT_AUXILIARY_SEND_AUTO:
            if (value != AL_FALSE && value != AL_TRUE) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            s->send_auto = (ALboolean)value;
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
    memset(s, 0, sizeof(*s));
    s->direct_filter.type = AL_FILTER_NULL;
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
            s->direct_filter.type = f ? f->props.type : AL_FILTER_NULL;
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
    s->send[send].filter.type = f ? f->props.type : AL_FILTER_NULL;
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

void al_efx_reset(void) {
    memset(s_effects, 0, sizeof(s_effects));
    memset(s_filters, 0, sizeof(s_filters));
    memset(s_slots, 0, sizeof(s_slots));
    s_meters_per_unit = AL_DEFAULT_METERS_PER_UNIT;
    s_max_sends = AL_EFX_MAX_SENDS;
}
