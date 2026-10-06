#include "al_source.h"
#include "al_listener.h"
#include <string.h>
#include <math.h>
#include <float.h>

/*
 * ============================================================================
 * Internal State & Global Source Pool
 * ============================================================================
 */
static ALsource g_sources[AL_MAX_SOURCES];
static bool s_subsystem_initialized = false;
static int s_hw_voice_owner[NV_PAPU_NUM_3D_VOICES];
static uintptr_t s_apu_base = NV_PAPU_BASE;
static ALenum s_distance_model = AL_INVERSE_DISTANCE_CLAMPED;
static float s_doppler_factor = 1.0f;
static float s_speed_of_sound = 343.3f;

/*
 * ============================================================================
 * Internal Helpers & Lifecycle Management
 * ============================================================================
 */

void al_source_set_apu_base(uintptr_t base) {
    s_apu_base = (base != 0) ? base : NV_PAPU_BASE;
}

uintptr_t al_source_get_apu_base(void) {
    return s_apu_base;
}

static void source_reset_defaults(ALsource *src, ALuint id) {
    src->id = id;
    src->in_use = false;
    src->state = AL_INITIAL;
    src->buffer = NULL;
    src->hw_voice_idx = AL_HW_VOICE_INVALID;
    src->looping = AL_FALSE;
    src->pitch = 1.0f;
    src->gain = 1.0f;
    src->min_gain = 0.0f;
    src->max_gain = 1.0f;
    src->position[0] = 0.0f;
    src->position[1] = 0.0f;
    src->position[2] = 0.0f;
    src->velocity[0] = 0.0f;
    src->velocity[1] = 0.0f;
    src->velocity[2] = 0.0f;
    src->direction[0] = 0.0f;
    src->direction[1] = 0.0f;
    src->direction[2] = 0.0f;
    src->source_relative = AL_FALSE;
    src->reference_distance = 1.0f;
    src->max_distance = FLT_MAX;
    src->rolloff_factor = 1.0f;
    src->cone_inner_angle = 360.0f;
    src->cone_outer_angle = 360.0f;
    src->cone_outer_gain = 0.0f;
}

void al_source_init_subsystem(void) {
    for (size_t i = 0; i < AL_MAX_SOURCES; i++) {
        source_reset_defaults(&g_sources[i], (ALuint)(i + 1));
    }
    for (size_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
    }
    s_distance_model = AL_INVERSE_DISTANCE_CLAMPED;
    s_doppler_factor = 1.0f;
    s_speed_of_sound = 343.3f;
    s_subsystem_initialized = true;
}

void al_source_cleanup_subsystem(void) {
    for (size_t i = 0; i < AL_MAX_SOURCES; i++) {
        ALsource *src = &g_sources[i];
        if (src->in_use) {
            if (src->state == AL_PLAYING || src->state == AL_PAUSED) {
                if (src->hw_voice_idx >= 0) {
                    apu_voice_stop(s_apu_base, (uint32_t)src->hw_voice_idx);
                }
            }
            if (src->buffer != NULL) {
                al_buffer_release(src->buffer);
                src->buffer = NULL;
            }
            source_reset_defaults(src, (ALuint)(i + 1));
        }
    }
    for (size_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
    }
    s_distance_model = AL_INVERSE_DISTANCE_CLAMPED;
    s_doppler_factor = 1.0f;
    s_speed_of_sound = 343.3f;
    s_subsystem_initialized = false;
}

static void ensure_subsystem_initialized(void) {
    if (!s_subsystem_initialized) {
        al_source_init_subsystem();
    }
}

ALsource *al_source_get(ALuint id) {
    ensure_subsystem_initialized();
    if (id < 1 || id > AL_MAX_SOURCES) {
        return NULL;
    }
    ALsource *src = &g_sources[id - 1];
    if (!src) {
        return NULL;
    }
    return src->in_use ? src : NULL;
}

/*
 * ============================================================================
 * 3D Spatial Audio & Attenuation Helpers (OpenAL 1.1)
 * ============================================================================
 */

void apu_spatial_set_distance_model(ALenum model) {
    s_distance_model = model;
}

ALenum apu_spatial_get_distance_model(void) {
    return s_distance_model;
}

void apu_spatial_set_doppler_factor(float factor) {
    s_doppler_factor = factor;
}

float apu_spatial_get_doppler_factor(void) {
    return s_doppler_factor;
}

void apu_spatial_set_speed_of_sound(float speed) {
    s_speed_of_sound = speed;
}

float apu_spatial_get_speed_of_sound(void) {
    return s_speed_of_sound;
}

float apu_calc_distance_gain(float distance, float ref_dist, float max_dist, float rolloff, ALenum model) {
    if (distance < 0.0f) {
        distance = 0.0f;
    }
    if (ref_dist <= 0.0f || rolloff == 0.0f) {
        return 1.0f;
    }
    if (max_dist < ref_dist) {
        max_dist = ref_dist;
    }

    switch (model) {
        case AL_NONE:
            return 1.0f;

        case AL_INVERSE_DISTANCE: {
            float denom = ref_dist + rolloff * (distance - ref_dist);
            if (denom <= 0.0f) {
                return 1.0f;
            }
            return ref_dist / denom;
        }

        case AL_INVERSE_DISTANCE_CLAMPED: {
            float d_clamp = distance;
            if (d_clamp < ref_dist) d_clamp = ref_dist;
            if (d_clamp > max_dist) d_clamp = max_dist;
            float denom = ref_dist + rolloff * (d_clamp - ref_dist);
            if (denom <= 0.0f) {
                return 1.0f;
            }
            return ref_dist / denom;
        }

        case AL_LINEAR_DISTANCE: {
            if (max_dist <= ref_dist) {
                return 1.0f;
            }
            float d = distance;
            if (d > max_dist) d = max_dist;
            float gain = 1.0f - rolloff * ((d - ref_dist) / (max_dist - ref_dist));
            if (gain < 0.0f) gain = 0.0f;
            return gain;
        }

        case AL_LINEAR_DISTANCE_CLAMPED: {
            if (max_dist <= ref_dist) {
                return 1.0f;
            }
            float d_clamp = distance;
            if (d_clamp < ref_dist) d_clamp = ref_dist;
            if (d_clamp > max_dist) d_clamp = max_dist;
            float gain = 1.0f - rolloff * ((d_clamp - ref_dist) / (max_dist - ref_dist));
            if (gain < 0.0f) gain = 0.0f;
            return gain;
        }

        case AL_EXPONENT_DISTANCE: {
            if (distance <= 0.0f) {
                return 1.0f;
            }
            return powf(distance / ref_dist, -rolloff);
        }

        case AL_EXPONENT_DISTANCE_CLAMPED: {
            float d_clamp = distance;
            if (d_clamp < ref_dist) d_clamp = ref_dist;
            if (d_clamp > max_dist) d_clamp = max_dist;
            if (d_clamp <= 0.0f) {
                return 1.0f;
            }
            return powf(d_clamp / ref_dist, -rolloff);
        }

        default:
            return 1.0f;
    }
}

float apu_calc_cone_gain(const float source_pos[3], const float source_dir[3], const float listener_pos[3],
                         float inner_deg, float outer_deg, float outer_gain) {
    if (!source_pos || !source_dir || !listener_pos) {
        return 1.0f;
    }

    /* Omnidirectional if direction is zero or angles are full sphere */
    if (source_dir[0] == 0.0f && source_dir[1] == 0.0f && source_dir[2] == 0.0f) {
        return 1.0f;
    }
    if (inner_deg >= 360.0f && outer_deg >= 360.0f) {
        return 1.0f;
    }

    float lx = listener_pos[0] - source_pos[0];
    float ly = listener_pos[1] - source_pos[1];
    float lz = listener_pos[2] - source_pos[2];
    float dist = sqrtf(lx * lx + ly * ly + lz * lz);
    if (dist == 0.0f) {
        return 1.0f;
    }

    float dir_len = sqrtf(source_dir[0] * source_dir[0] +
                          source_dir[1] * source_dir[1] +
                          source_dir[2] * source_dir[2]);
    if (dir_len == 0.0f) {
        return 1.0f;
    }

    float dot = source_dir[0] * lx + source_dir[1] * ly + source_dir[2] * lz;
    float cos_theta = dot / (dir_len * dist);
    if (cos_theta > 1.0f) cos_theta = 1.0f;
    if (cos_theta < -1.0f) cos_theta = -1.0f;

    float theta_deg = acosf(cos_theta) * (180.0f / 3.14159265358979323846f);

    if (theta_deg <= inner_deg) {
        return 1.0f;
    }
    if (theta_deg >= outer_deg) {
        return outer_gain;
    }
    if (outer_deg <= inner_deg) {
        return outer_gain;
    }

    float factor = (theta_deg - inner_deg) / (outer_deg - inner_deg);
    return 1.0f - (1.0f - outer_gain) * factor;
}

float apu_calc_doppler_pitch(const float source_pos[3], const float source_vel[3],
                            const float listener_pos[3], const float listener_vel[3],
                            float doppler_factor, float speed_of_sound) {
    if (doppler_factor == 0.0f || speed_of_sound <= 0.0f) {
        return 1.0f;
    }
    if (!source_pos || !source_vel || !listener_pos || !listener_vel) {
        return 1.0f;
    }

    /* Vector from source to listener (line of sight) */
    float slx = listener_pos[0] - source_pos[0];
    float sly = listener_pos[1] - source_pos[1];
    float slz = listener_pos[2] - source_pos[2];
    float dist = sqrtf(slx * slx + sly * sly + slz * slz);
    if (dist == 0.0f) {
        return 1.0f;
    }

    /* Normalized direction from source to listener */
    float ux = slx / dist;
    float uy = sly / dist;
    float uz = slz / dist;

    /* Scalar velocity projections along line of sight */
    float vls = listener_vel[0] * ux + listener_vel[1] * uy + listener_vel[2] * uz;
    float vss = source_vel[0] * ux + source_vel[1] * uy + source_vel[2] * uz;

    /* Clamping per OpenAL 1.1 Specification Section 3.5.2 */
    float max_v = speed_of_sound / doppler_factor;
    if (vls > max_v) vls = max_v;
    if (vss >= max_v * 0.999f) vss = max_v * 0.999f;

    float num = speed_of_sound - doppler_factor * vls;
    float denom = speed_of_sound - doppler_factor * vss;
    if (denom <= 0.0f) {
        return 1.0f;
    }

    float factor = num / denom;
    if (factor < 0.0f) factor = 0.0f;
    return factor;
}

void al_source_calc_spatial(const ALsource *src, AL_SPATIAL_CALC *calc) {
    if (!src || !calc) {
        return;
    }

    float lis_pos[3] = {0.0f, 0.0f, 0.0f};
    float lis_vel[3] = {0.0f, 0.0f, 0.0f};
    al_listener_get_position(lis_pos);
    al_listener_get_velocity(lis_vel);

    if (src->source_relative) {
        calc->local_pos[0] = src->position[0];
        calc->local_pos[1] = src->position[1];
        calc->local_pos[2] = src->position[2];

        float dist = sqrtf(src->position[0] * src->position[0] +
                           src->position[1] * src->position[1] +
                           src->position[2] * src->position[2]);
        calc->distance = dist;

        calc->distance_gain = apu_calc_distance_gain(
            dist,
            src->reference_distance,
            src->max_distance,
            src->rolloff_factor,
            s_distance_model
        );

        float origin[3] = {0.0f, 0.0f, 0.0f};
        calc->cone_gain = apu_calc_cone_gain(
            src->position,
            src->direction,
            origin,
            src->cone_inner_angle,
            src->cone_outer_angle,
            src->cone_outer_gain
        );

        calc->doppler_pitch = 1.0f;
    } else {
        al_listener_world_to_local(src->position, calc->local_pos);

        float dx = src->position[0] - lis_pos[0];
        float dy = src->position[1] - lis_pos[1];
        float dz = src->position[2] - lis_pos[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);
        calc->distance = dist;

        calc->distance_gain = apu_calc_distance_gain(
            dist,
            src->reference_distance,
            src->max_distance,
            src->rolloff_factor,
            s_distance_model
        );

        calc->cone_gain = apu_calc_cone_gain(
            src->position,
            src->direction,
            lis_pos,
            src->cone_inner_angle,
            src->cone_outer_angle,
            src->cone_outer_gain
        );

        calc->doppler_pitch = apu_calc_doppler_pitch(
            src->position,
            src->velocity,
            lis_pos,
            lis_vel,
            s_doppler_factor,
            s_speed_of_sound
        );
    }

    /* Combine into effective gain: clamp(source_gain * distance_gain * cone_gain, min_gain, max_gain) * listener_gain */
    float nominal = src->gain * calc->distance_gain * calc->cone_gain;
    if (nominal < src->min_gain) nominal = src->min_gain;
    if (nominal > src->max_gain) nominal = src->max_gain;
    float eff = nominal * al_listener_get_gain();
    if (eff < 0.0f) eff = 0.0f;
    if (eff > 1.0f) eff = 1.0f;
    calc->effective_gain = eff;
}

/*
 * ============================================================================
 * Hardware Pitch & Master Volume Mapping Helpers
 * ============================================================================
 */
#define APU_PITCH_STEP_MIN 0x00001000u
#define APU_PITCH_STEP_MAX 0x00040000u

static uint32_t source_calc_pitch_step(ALsizei freq, float pitch) {
    uint32_t step = APU_CALC_PITCH_STEP(freq, pitch);
    if (step < APU_PITCH_STEP_MIN) {
        step = APU_PITCH_STEP_MIN;
    } else if (step > APU_PITCH_STEP_MAX) {
        step = APU_PITCH_STEP_MAX;
    }
    return step;
}

static uint16_t source_calc_master_volume(const ALsource *src) {
    AL_SPATIAL_CALC calc;
    al_source_calc_spatial(src, &calc);
    return (uint16_t)(calc.effective_gain * 65535.0f + 0.5f);
}

static void source_update_hw_pitch(const ALsource *src) {
    if (src->state == AL_PLAYING && src->hw_voice_idx >= 0 && src->buffer != NULL) {
        NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)src->hw_voice_idx);
        if (ctx) {
            AL_SPATIAL_CALC calc;
            al_source_calc_spatial(src, &calc);
            float eff_pitch = src->pitch * calc.doppler_pitch;
            ctx->pitch_step = source_calc_pitch_step(src->buffer->frequency, eff_pitch);
        }
    }
}

static void source_update_hw_gain(const ALsource *src) {
    if (src->state == AL_PLAYING && src->hw_voice_idx >= 0) {
        NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)src->hw_voice_idx);
        if (ctx) {
            uint16_t master_vol = source_calc_master_volume(src);
            ctx->master_vol_left = master_vol;
            ctx->master_vol_right = master_vol;
        }
    }
}

void al_source_update_all_spatial(void) {
    ensure_subsystem_initialized();
    for (size_t i = 0; i < AL_MAX_SOURCES; i++) {
        ALsource *src = &g_sources[i];
        if (src->in_use && src->state == AL_PLAYING && src->hw_voice_idx >= 0) {
            source_update_hw_gain(src);
            source_update_hw_pitch(src);
        }
    }
}

void al_source_update_all_gains(void) {
    al_source_update_all_spatial();
}

/*
 * ============================================================================
 * Dynamic Priority & Voice Virtualization Management
 * ============================================================================
 * Dynamic priority formula:
 *   P = base_priority * (gain / max(distance, 0.1))
 * Looping sources receive +2.0 priority boost (base = 3.0 vs 1.0).
 * Sources with gain < 0.001 are assigned priority 0.
 * ============================================================================
 */

static float calculate_source_priority(const ALsource *src) {
    if (!src || src->gain < 0.001f) {
        return 0.0f;
    }

    float lis_pos[3] = {0.0f, 0.0f, 0.0f};
    al_listener_get_position(lis_pos);

    float dx = src->position[0];
    float dy = src->position[1];
    float dz = src->position[2];
    if (!src->source_relative) {
        dx -= lis_pos[0];
        dy -= lis_pos[1];
        dz -= lis_pos[2];
    }
    float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 0.1f) {
        dist = 0.1f;
    }

    float base_priority = src->looping ? 3.0f : 1.0f;
    return base_priority * (src->gain / dist);
}

static int allocate_hw_voice(ALsource *src) {
    /* Check if current source already holds a valid voice */
    if (src->hw_voice_idx >= 0 && src->hw_voice_idx < (int)NV_PAPU_NUM_3D_VOICES) {
        if (s_hw_voice_owner[src->hw_voice_idx] == (int)src->id) {
            return src->hw_voice_idx;
        }
    }

    /* 1. First pass: look for an unassigned voice or an inactive voice */
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        int owner_id = s_hw_voice_owner[v];
        if (owner_id <= 0) {
            s_hw_voice_owner[v] = (int)src->id;
            src->hw_voice_idx = (int)v;
            return (int)v;
        }

        ALsource *owner = al_source_get((ALuint)owner_id);
        if (!owner || owner->state != AL_PLAYING) {
            if (owner) {
                owner->hw_voice_idx = AL_HW_VOICE_INVALID;
            }
            s_hw_voice_owner[v] = (int)src->id;
            src->hw_voice_idx = (int)v;
            return (int)v;
        }

        /* Check if one-shot voice playback completed in hardware */
        if (!owner->looping && apu_voice_is_active(s_apu_base, v) == 0) {
            owner->state = AL_STOPPED;
            owner->hw_voice_idx = AL_HW_VOICE_INVALID;
            s_hw_voice_owner[v] = (int)src->id;
            src->hw_voice_idx = (int)v;
            return (int)v;
        }
    }

    /* 2. Second pass: All 64 voices active, execute priority stealing */
    float new_priority = calculate_source_priority(src);
    int victim_voice = -1;
    float min_priority = 1e30f;

    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        int owner_id = s_hw_voice_owner[v];
        ALsource *owner = al_source_get((ALuint)owner_id);
        float p = owner ? calculate_source_priority(owner) : 0.0f;
        if (p < min_priority) {
            min_priority = p;
            victim_voice = (int)v;
        }
    }

    if (victim_voice >= 0 && new_priority > min_priority) {
        /*
         * Click-prevention preemption protocol:
         * 1. Zero master volume and mixbin gains to eliminate DC offsets/clicks.
         * 2. Clear active bit in hardware register.
         * 3. Disconnect victim source (marks as virtualized/standby).
         * 4. Assign hardware voice slot to the new higher-priority source.
         */
        NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context((uint32_t)victim_voice);
        if (vctx) {
            vctx->master_vol_left = 0;
            vctx->master_vol_right = 0;
            memset(vctx->mixbin_gain, 0, sizeof(vctx->mixbin_gain));
        }
        apu_voice_stop(s_apu_base, (uint32_t)victim_voice);

        int victim_owner_id = s_hw_voice_owner[victim_voice];
        ALsource *victim = al_source_get((ALuint)victim_owner_id);
        if (victim) {
            victim->hw_voice_idx = AL_HW_VOICE_INVALID;
        }

        s_hw_voice_owner[victim_voice] = (int)src->id;
        src->hw_voice_idx = victim_voice;
        return victim_voice;
    }

    /* Fallback: Voice remains logically playing but virtualized */
    src->hw_voice_idx = AL_HW_VOICE_INVALID;
    return AL_HW_VOICE_INVALID;
}

/*
 * ============================================================================
 * OpenAL 1.1 Source Lifecycle APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alGenSources(ALsizei n, ALuint *sources) {
    ensure_subsystem_initialized();

    if (n < 0) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (n == 0) {
        return;
    }
    if (!sources) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    /* Verify sufficient free slots exist in pool */
    ALsizei available = 0;
    for (size_t i = 0; i < AL_MAX_SOURCES; i++) {
        if (!g_sources[i].in_use) {
            available++;
        }
    }

    if (available < n) {
        alSetError(AL_OUT_OF_MEMORY);
        return;
    }

    /* Allocate source slots */
    ALsizei allocated = 0;
    for (size_t i = 0; i < AL_MAX_SOURCES && allocated < n; i++) {
        if (!g_sources[i].in_use) {
            ALsource *src = &g_sources[i];
            source_reset_defaults(src, (ALuint)(i + 1));
            src->in_use = true;
            sources[allocated++] = src->id;
        }
    }
}

AL_API void AL_APIENTRY alDeleteSources(ALsizei n, const ALuint *sources) {
    ensure_subsystem_initialized();

    if (n < 0) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    if (n == 0) {
        return;
    }
    if (!sources) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    /* Validation phase: all IDs must be valid (or 0) */
    for (ALsizei i = 0; i < n; i++) {
        ALuint id = sources[i];
        if (id == 0) {
            continue;
        }
        if (id > AL_MAX_SOURCES || !g_sources[id - 1].in_use) {
            alSetError(AL_INVALID_NAME);
            return;
        }
    }

    /* Deletion phase */
    for (ALsizei i = 0; i < n; i++) {
        ALuint id = sources[i];
        if (id == 0) {
            continue;
        }
        ALsource *src = &g_sources[id - 1];
        if (src->in_use) {
            alSourceStop(id);
            if (src->buffer) {
                al_buffer_release(src->buffer);
                src->buffer = NULL;
            }
            source_reset_defaults(src, (ALuint)(i + 1));
        }
    }
}

AL_API ALboolean AL_APIENTRY alIsSource(ALuint source) {
    ensure_subsystem_initialized();

    if (source < 1 || source > AL_MAX_SOURCES) {
        return AL_FALSE;
    }
    return g_sources[source - 1].in_use ? AL_TRUE : AL_FALSE;
}

/*
 * ============================================================================
 * OpenAL 1.1 Source Property Configuration APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alSourcef(ALuint source, ALenum param, ALfloat value) {
    ensure_subsystem_initialized();

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_PITCH:
            if (value <= 0.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->pitch = value;
            source_update_hw_pitch(src);
            break;

        case AL_GAIN:
            if (value < 0.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->gain = value;
            source_update_hw_gain(src);
            break;

        case AL_MIN_GAIN:
            if (value < 0.0f || value > 1.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->min_gain = value;
            source_update_hw_gain(src);
            break;

        case AL_MAX_GAIN:
            if (value < 0.0f || value > 1.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->max_gain = value;
            source_update_hw_gain(src);
            break;

        case AL_REFERENCE_DISTANCE:
            if (value < 0.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->reference_distance = value;
            source_update_hw_gain(src);
            break;

        case AL_MAX_DISTANCE:
            if (value < 0.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->max_distance = value;
            source_update_hw_gain(src);
            break;

        case AL_ROLLOFF_FACTOR:
            if (value < 0.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->rolloff_factor = value;
            source_update_hw_gain(src);
            break;

        case AL_CONE_INNER_ANGLE:
            if (value < 0.0f || value > 360.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->cone_inner_angle = value;
            source_update_hw_gain(src);
            break;

        case AL_CONE_OUTER_ANGLE:
            if (value < 0.0f || value > 360.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->cone_outer_angle = value;
            source_update_hw_gain(src);
            break;

        case AL_CONE_OUTER_GAIN:
            if (value < 0.0f || value > 1.0f) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->cone_outer_gain = value;
            source_update_hw_gain(src);
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alSource3f(ALuint source, ALenum param, ALfloat v1, ALfloat v2, ALfloat v3) {
    ensure_subsystem_initialized();

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_POSITION:
            src->position[0] = v1;
            src->position[1] = v2;
            src->position[2] = v3;
            source_update_hw_gain(src);
            source_update_hw_pitch(src);
            break;

        case AL_VELOCITY:
            src->velocity[0] = v1;
            src->velocity[1] = v2;
            src->velocity[2] = v3;
            source_update_hw_pitch(src);
            break;

        case AL_DIRECTION:
            src->direction[0] = v1;
            src->direction[1] = v2;
            src->direction[2] = v3;
            source_update_hw_gain(src);
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alSourcefv(ALuint source, ALenum param, const ALfloat *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            alSource3f(source, param, values[0], values[1], values[2]);
            break;

        case AL_PITCH:
        case AL_GAIN:
        case AL_MIN_GAIN:
        case AL_MAX_GAIN:
        case AL_REFERENCE_DISTANCE:
        case AL_MAX_DISTANCE:
        case AL_ROLLOFF_FACTOR:
        case AL_CONE_INNER_ANGLE:
        case AL_CONE_OUTER_ANGLE:
        case AL_CONE_OUTER_GAIN:
            alSourcef(source, param, values[0]);
            break;

        default:
            if (!alIsSource(source)) {
                alSetError(AL_INVALID_NAME);
            } else {
                alSetError(AL_INVALID_ENUM);
            }
            break;
    }
}

AL_API void AL_APIENTRY alSourcei(ALuint source, ALenum param, ALint value) {
    ensure_subsystem_initialized();

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_BUFFER:
            if (src->state == AL_PLAYING || src->state == AL_PAUSED) {
                alSetError(AL_INVALID_OPERATION);
                return;
            }
            if (value == 0) {
                if (src->buffer != NULL) {
                    al_buffer_release(src->buffer);
                    src->buffer = NULL;
                }
            } else {
                ALbuffer *buf = al_buffer_get((ALuint)value);
                if (!buf) {
                    alSetError(AL_INVALID_NAME);
                    return;
                }
                if (src->buffer != NULL) {
                    al_buffer_release(src->buffer);
                }
                src->buffer = buf;
                al_buffer_retain(buf);
            }
            break;

        case AL_LOOPING:
            if (value != AL_TRUE && value != AL_FALSE) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->looping = (ALboolean)value;
            if (src->state == AL_PLAYING && src->hw_voice_idx >= 0) {
                NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)src->hw_voice_idx);
                if (ctx) {
                    ctx->loop_mode = src->looping ? NVAPU_VOICE_LOOP_ON : NVAPU_VOICE_LOOP_OFF;
                }
            }
            break;

        case AL_SOURCE_RELATIVE:
            if (value != AL_TRUE && value != AL_FALSE) {
                alSetError(AL_INVALID_VALUE);
                return;
            }
            src->source_relative = (ALboolean)value;
            source_update_hw_gain(src);
            source_update_hw_pitch(src);
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alSource3i(ALuint source, ALenum param, ALint v1, ALint v2, ALint v3) {
    alSource3f(source, param, (ALfloat)v1, (ALfloat)v2, (ALfloat)v3);
}

AL_API void AL_APIENTRY alSourceiv(ALuint source, ALenum param, const ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            alSource3f(source, param, (ALfloat)values[0], (ALfloat)values[1], (ALfloat)values[2]);
            break;

        case AL_BUFFER:
        case AL_LOOPING:
        case AL_SOURCE_RELATIVE:
            alSourcei(source, param, values[0]);
            break;

        default:
            if (!alIsSource(source)) {
                alSetError(AL_INVALID_NAME);
            } else {
                alSetError(AL_INVALID_ENUM);
            }
            break;
    }
}

/*
 * ============================================================================
 * OpenAL 1.1 Source Property Query APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alGetSourcef(ALuint source, ALenum param, ALfloat *value) {
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_PITCH:
            *value = src->pitch;
            break;

        case AL_GAIN:
            *value = src->gain;
            break;

        case AL_MIN_GAIN:
            *value = src->min_gain;
            break;

        case AL_MAX_GAIN:
            *value = src->max_gain;
            break;

        case AL_REFERENCE_DISTANCE:
            *value = src->reference_distance;
            break;

        case AL_MAX_DISTANCE:
            *value = src->max_distance;
            break;

        case AL_ROLLOFF_FACTOR:
            *value = src->rolloff_factor;
            break;

        case AL_CONE_INNER_ANGLE:
            *value = src->cone_inner_angle;
            break;

        case AL_CONE_OUTER_ANGLE:
            *value = src->cone_outer_angle;
            break;

        case AL_CONE_OUTER_GAIN:
            *value = src->cone_outer_gain;
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetSource3f(ALuint source, ALenum param, ALfloat *v1, ALfloat *v2, ALfloat *v3) {
    if (!v1 || !v2 || !v3) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_POSITION:
            *v1 = src->position[0];
            *v2 = src->position[1];
            *v3 = src->position[2];
            break;

        case AL_VELOCITY:
            *v1 = src->velocity[0];
            *v2 = src->velocity[1];
            *v3 = src->velocity[2];
            break;

        case AL_DIRECTION:
            *v1 = src->direction[0];
            *v2 = src->direction[1];
            *v3 = src->direction[2];
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetSourcefv(ALuint source, ALenum param, ALfloat *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            alGetSource3f(source, param, &values[0], &values[1], &values[2]);
            break;

        default:
            alGetSourcef(source, param, &values[0]);
            break;
    }
}

AL_API void AL_APIENTRY alGetSourcei(ALuint source, ALenum param, ALint *value) {
    if (!value) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_SOURCE_STATE:
            if (src->state == AL_PLAYING) {
                if (src->hw_voice_idx >= 0) {
                    int active = apu_voice_is_active(s_apu_base, (uint32_t)src->hw_voice_idx);
                    if (!active && !src->looping) {
                        src->state = AL_STOPPED;
                        s_hw_voice_owner[src->hw_voice_idx] = AL_HW_VOICE_INVALID;
                        src->hw_voice_idx = AL_HW_VOICE_INVALID;
                    }
                }
            }
            *value = src->state;
            break;

        case AL_BUFFER:
            *value = (src->buffer != NULL) ? (ALint)src->buffer->id : 0;
            break;

        case AL_LOOPING:
            *value = (ALint)src->looping;
            break;

        case AL_SOURCE_RELATIVE:
            *value = (ALint)src->source_relative;
            break;

        case AL_BUFFERS_QUEUED:
            *value = (src->buffer != NULL) ? 1 : 0;
            break;

        case AL_BUFFERS_PROCESSED:
            if (src->buffer != NULL && !src->looping && src->state == AL_STOPPED) {
                *value = 1;
            } else {
                *value = 0;
            }
            break;

        case AL_SOURCE_TYPE:
            if (src->buffer == NULL) {
                *value = AL_UNDETERMINED;
            } else {
                *value = AL_STATIC;
            }
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetSource3i(ALuint source, ALenum param, ALint *v1, ALint *v2, ALint *v3) {
    if (!v1 || !v2 || !v3) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    switch (param) {
        case AL_POSITION:
            *v1 = (ALint)src->position[0];
            *v2 = (ALint)src->position[1];
            *v3 = (ALint)src->position[2];
            break;

        case AL_VELOCITY:
            *v1 = (ALint)src->velocity[0];
            *v2 = (ALint)src->velocity[1];
            *v3 = (ALint)src->velocity[2];
            break;

        case AL_DIRECTION:
            *v1 = (ALint)src->direction[0];
            *v2 = (ALint)src->direction[1];
            *v3 = (ALint)src->direction[2];
            break;

        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API void AL_APIENTRY alGetSourceiv(ALuint source, ALenum param, ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    switch (param) {
        case AL_POSITION:
        case AL_VELOCITY:
        case AL_DIRECTION:
            alGetSource3i(source, param, &values[0], &values[1], &values[2]);
            break;

        default:
            alGetSourcei(source, param, &values[0]);
            break;
    }
}

/*
 * ============================================================================
 * OpenAL 1.1 Source Playback Control APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alSourcePlay(ALuint source) {
    ensure_subsystem_initialized();

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    /* If no buffer is attached, transition to AL_STOPPED without error */
    if (src->buffer == NULL) {
        src->state = AL_STOPPED;
        return;
    }

    /* If paused and HW voice is intact, unpause directly */
    if (src->state == AL_PAUSED && src->hw_voice_idx >= 0) {
        apu_voice_pause(s_apu_base, (uint32_t)src->hw_voice_idx, 0);
        src->state = AL_PLAYING;
        return;
    }

    /* Allocate or bind hardware voice index (0..63) */
    int idx = allocate_hw_voice(src);
    if (idx < 0) {
        /* Virtualized playback (logical AL_PLAYING without hardware voice) */
        src->state = AL_PLAYING;
        return;
    }

    /* Populate NVAPU_VOICE_CONTEXT_3D */
    NVAPU_VOICE_CONTEXT_3D ctx;
    apu_voice_context_reset(&ctx);

    /* Format: 0 for 16-bit PCM, 1 for 8-bit PCM */
    ctx.format = (src->buffer->bits == 8) ? NVAPU_VOICE_FORMAT_PCM8 : NVAPU_VOICE_FORMAT_PCM16;
    /* Channels: 1 for stereo, 0 for mono */
    ctx.channels = (src->buffer->channels == 2) ? NVAPU_VOICE_CHANNELS_STEREO : NVAPU_VOICE_CHANNELS_MONO;
    /* Loop mode & Direct 2D playback */
    ctx.loop_mode = src->looping ? NVAPU_VOICE_LOOP_ON : NVAPU_VOICE_LOOP_OFF;
    ctx.mode_3d = 0; /* Direct 2D bypasses 3D HRTF/ITD processing */

    ctx.prd_table_phys = src->buffer->prd_table_phys;
    ctx.current_prd_index = 0;
    ctx.sample_pos_frac = 0;
    AL_SPATIAL_CALC calc;
    al_source_calc_spatial(src, &calc);
    ctx.pitch_step = source_calc_pitch_step(src->buffer->frequency, src->pitch * calc.doppler_pitch);

    uint16_t master_vol = (uint16_t)(calc.effective_gain * 65535.0f + 0.5f);
    ctx.master_vol_left = master_vol;
    ctx.master_vol_right = master_vol;

    /* Route to MixBins 0 and 1 (Front Left, Front Right) at unity multiplier */
    ctx.mixbin_routing_mask = 0x00000003;
    ctx.mixbin_gain[0] = 0xFF;
    ctx.mixbin_gain[1] = 0xFF;

    /* Program hardware voice context, ensure unpaused, and trigger */
    apu_voice_setup((uint32_t)idx, &ctx);
    apu_voice_pause(s_apu_base, (uint32_t)idx, 0);
    apu_voice_trigger(s_apu_base, (uint32_t)idx);

    src->state = AL_PLAYING;
}

AL_API void AL_APIENTRY alSourcePause(ALuint source) {
    ensure_subsystem_initialized();

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    if (src->state == AL_PLAYING) {
        if (src->hw_voice_idx >= 0) {
            apu_voice_pause(s_apu_base, (uint32_t)src->hw_voice_idx, 1);
        }
        src->state = AL_PAUSED;
    }
}

AL_API void AL_APIENTRY alSourceStop(ALuint source) {
    ensure_subsystem_initialized();

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    if (src->state == AL_PLAYING || src->state == AL_PAUSED) {
        if (src->hw_voice_idx >= 0) {
            NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context((uint32_t)src->hw_voice_idx);
            if (vctx) {
                vctx->master_vol_left = 0;
                vctx->master_vol_right = 0;
                memset(vctx->mixbin_gain, 0, sizeof(vctx->mixbin_gain));
            }
            apu_voice_stop(s_apu_base, (uint32_t)src->hw_voice_idx);
            s_hw_voice_owner[src->hw_voice_idx] = AL_HW_VOICE_INVALID;
            src->hw_voice_idx = AL_HW_VOICE_INVALID;
        }
        src->state = AL_STOPPED;
    }
}

AL_API void AL_APIENTRY alSourceRewind(ALuint source) {
    ensure_subsystem_initialized();

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    if (src->state == AL_PLAYING || src->state == AL_PAUSED || src->state == AL_STOPPED) {
        if (src->hw_voice_idx >= 0) {
            NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context((uint32_t)src->hw_voice_idx);
            if (vctx) {
                vctx->master_vol_left = 0;
                vctx->master_vol_right = 0;
                vctx->sample_pos_frac = 0;
                vctx->current_prd_index = 0;
            }
            apu_voice_stop(s_apu_base, (uint32_t)src->hw_voice_idx);
            s_hw_voice_owner[src->hw_voice_idx] = AL_HW_VOICE_INVALID;
            src->hw_voice_idx = AL_HW_VOICE_INVALID;
        }
        src->state = AL_INITIAL;
    }
}

AL_API void AL_APIENTRY alSourcePlayv(ALsizei n, const ALuint *sources) {
    ensure_subsystem_initialized();
    if (n < 0 || sources == NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    for (ALsizei i = 0; i < n; i++) {
        alSourcePlay(sources[i]);
    }
}

AL_API void AL_APIENTRY alSourcePausev(ALsizei n, const ALuint *sources) {
    ensure_subsystem_initialized();
    if (n < 0 || sources == NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    for (ALsizei i = 0; i < n; i++) {
        alSourcePause(sources[i]);
    }
}

AL_API void AL_APIENTRY alSourceStopv(ALsizei n, const ALuint *sources) {
    ensure_subsystem_initialized();
    if (n < 0 || sources == NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    for (ALsizei i = 0; i < n; i++) {
        alSourceStop(sources[i]);
    }
}

AL_API void AL_APIENTRY alSourceRewindv(ALsizei n, const ALuint *sources) {
    ensure_subsystem_initialized();
    if (n < 0 || sources == NULL) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    for (ALsizei i = 0; i < n; i++) {
        alSourceRewind(sources[i]);
    }
}

/*
 * ============================================================================
 * OpenAL 1.1 Source Queueing APIs (Stubs for Static 2D Phase)
 * ============================================================================
 */

AL_API void AL_APIENTRY alSourceQueueBuffers(ALuint source, ALsizei nb, const ALuint *buffers) {
    ensure_subsystem_initialized();

    if (nb < 0 || (nb > 0 && buffers == NULL)) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    if (nb == 0) {
        return;
    }

    alSetError(AL_INVALID_OPERATION);
}

AL_API void AL_APIENTRY alSourceUnqueueBuffers(ALuint source, ALsizei nb, ALuint *buffers) {
    ensure_subsystem_initialized();

    if (nb < 0 || (nb > 0 && buffers == NULL)) {
        alSetError(AL_INVALID_VALUE);
        return;
    }

    ALsource *src = al_source_get(source);
    if (!src) {
        alSetError(AL_INVALID_NAME);
        return;
    }

    if (nb == 0) {
        return;
    }

    alSetError(AL_INVALID_OPERATION);
}

/*
 * ============================================================================
 * OpenAL 1.1 Global Distance Model & Doppler Shift APIs
 * ============================================================================
 */

AL_API void AL_APIENTRY alDistanceModel(ALenum distanceModel) {
    ensure_subsystem_initialized();
    switch (distanceModel) {
        case AL_NONE:
        case AL_INVERSE_DISTANCE:
        case AL_INVERSE_DISTANCE_CLAMPED:
        case AL_LINEAR_DISTANCE:
        case AL_LINEAR_DISTANCE_CLAMPED:
        case AL_EXPONENT_DISTANCE:
        case AL_EXPONENT_DISTANCE_CLAMPED:
            s_distance_model = distanceModel;
            al_source_update_all_spatial();
            break;

        default:
            alSetError(AL_INVALID_VALUE);
            break;
    }
}

AL_API void AL_APIENTRY alDopplerFactor(ALfloat value) {
    ensure_subsystem_initialized();
    if (!isfinite(value) || value < 0.0f) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    s_doppler_factor = value;
    al_source_update_all_spatial();
}

AL_API void AL_APIENTRY alDopplerVelocity(ALfloat value) {
    ensure_subsystem_initialized();
    if (!isfinite(value) || value < 0.0f) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    /* Deprecated in OpenAL 1.1 in favor of alSpeedOfSound */
}

AL_API void AL_APIENTRY alSpeedOfSound(ALfloat value) {
    ensure_subsystem_initialized();
    if (!isfinite(value) || value <= 0.0f) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    s_speed_of_sound = value;
    al_source_update_all_spatial();
}

/*
 * ============================================================================
 * OpenAL 1.1 Global State Query APIs
 * ============================================================================
 */

AL_API ALint AL_APIENTRY alGetInteger(ALenum param) {
    ensure_subsystem_initialized();
    switch (param) {
        case AL_DISTANCE_MODEL:
            return (ALint)s_distance_model;
        case AL_DOPPLER_FACTOR:
            return (ALint)s_doppler_factor;
        case AL_SPEED_OF_SOUND:
            return (ALint)s_speed_of_sound;
        default:
            alSetError(AL_INVALID_ENUM);
            return 0;
    }
}

AL_API void AL_APIENTRY alGetIntegerv(ALenum param, ALint *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_subsystem_initialized();
    switch (param) {
        case AL_DISTANCE_MODEL:
            *values = (ALint)s_distance_model;
            break;
        case AL_DOPPLER_FACTOR:
            *values = (ALint)s_doppler_factor;
            break;
        case AL_SPEED_OF_SOUND:
            *values = (ALint)s_speed_of_sound;
            break;
        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API ALfloat AL_APIENTRY alGetFloat(ALenum param) {
    ensure_subsystem_initialized();
    switch (param) {
        case AL_DISTANCE_MODEL:
            return (ALfloat)s_distance_model;
        case AL_DOPPLER_FACTOR:
            return s_doppler_factor;
        case AL_DOPPLER_VELOCITY:
            return 1.0f;
        case AL_SPEED_OF_SOUND:
            return s_speed_of_sound;
        default:
            alSetError(AL_INVALID_ENUM);
            return 0.0f;
    }
}

AL_API void AL_APIENTRY alGetFloatv(ALenum param, ALfloat *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ensure_subsystem_initialized();
    switch (param) {
        case AL_DISTANCE_MODEL:
            *values = (ALfloat)s_distance_model;
            break;
        case AL_DOPPLER_FACTOR:
            *values = s_doppler_factor;
            break;
        case AL_DOPPLER_VELOCITY:
            *values = 1.0f;
            break;
        case AL_SPEED_OF_SOUND:
            *values = s_speed_of_sound;
            break;
        default:
            alSetError(AL_INVALID_ENUM);
            break;
    }
}

AL_API ALdouble AL_APIENTRY alGetDouble(ALenum param) {
    return (ALdouble)alGetFloat(param);
}

AL_API void AL_APIENTRY alGetDoublev(ALenum param, ALdouble *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ALfloat fval = 0.0f;
    alGetFloatv(param, &fval);
    *values = (ALdouble)fval;
}

AL_API ALboolean AL_APIENTRY alGetBoolean(ALenum param) {
    return (alGetInteger(param) != 0) ? AL_TRUE : AL_FALSE;
}

AL_API void AL_APIENTRY alGetBooleanv(ALenum param, ALboolean *values) {
    if (!values) {
        alSetError(AL_INVALID_VALUE);
        return;
    }
    ALint ival = 0;
    alGetIntegerv(param, &ival);
    *values = (ival != 0) ? AL_TRUE : AL_FALSE;
}
