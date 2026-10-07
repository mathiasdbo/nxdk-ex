#include "apu_voice_mgr.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "al_source.h"
#include <string.h>

/*
 * ============================================================================
 * Hardware Voice Tracking State
 * ============================================================================
 */
static int s_hw_voice_owner[NV_PAPU_NUM_3D_VOICES];

/*
 * ============================================================================
 * Voice Manager Subsystem Lifecycle
 * ============================================================================
 */

void apu_voice_mgr_init(void) {
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
    }
}

void apu_voice_mgr_deinit(void) {
    uintptr_t apu_base = al_source_get_apu_base();
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        if (s_hw_voice_owner[v] != AL_HW_VOICE_INVALID) {
            NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context(v);
            if (vctx) {
                vctx->master_vol_left = 0;
                vctx->master_vol_right = 0;
                memset(vctx->mixbin_gain, 0, sizeof(vctx->mixbin_gain));
            }
            apu_voice_stop(apu_base, v);
            s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
        }
    }
}

int apu_voice_mgr_get_owner(uint32_t hw_voice_idx) {
    if (hw_voice_idx >= NV_PAPU_NUM_3D_VOICES) {
        return AL_HW_VOICE_INVALID;
    }
    return s_hw_voice_owner[hw_voice_idx];
}

/*
 * ============================================================================
 * Dynamic Priority Calculation
 * ============================================================================
 * Priority Formula:
 *   P = priority_base * (effective_gain / max(distance, 0.1f))
 * Looping sources receive +2.0 boost (base 3.0 vs 1.0).
 * Sources with gain or effective gain < 0.001 are strictly priority 0.
 * ============================================================================
 */

float apu_voice_mgr_calc_priority(const ALsource *src) {
    if (!src || src->gain < 0.001f) {
        return 0.0f;
    }

    AL_SPATIAL_CALC calc;
    al_source_calc_spatial(src, &calc);

    if (calc.effective_gain < 0.001f) {
        return 0.0f;
    }

    float d = calc.distance;
    if (d < 0.1f) {
        d = 0.1f;
    }

    float base_priority = src->looping ? 3.0f : 1.0f;
    return base_priority * (calc.effective_gain / d);
}

/*
 * ============================================================================
 * Hardware Voice Allocation & Priority Stealing
 * ============================================================================
 */

int apu_voice_mgr_allocate(ALsource *src) {
    if (!src) {
        return AL_HW_VOICE_INVALID;
    }

    uintptr_t apu_base = al_source_get_apu_base();

    /* If source already holds a valid hardware slot and matches owner, keep it */
    if (src->hw_voice_idx >= 0 && src->hw_voice_idx < (int)NV_PAPU_NUM_3D_VOICES) {
        if (s_hw_voice_owner[src->hw_voice_idx] == (int)src->id) {
            return src->hw_voice_idx;
        }
    }

    /* Pass 1: find free slot or slot whose owner is not playing or completed one-shot */
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        int owner_id = s_hw_voice_owner[v];
        if (owner_id <= 0) {
            s_hw_voice_owner[v] = (int)src->id;
            src->hw_voice_idx = (int)v;
            return (int)v;
        }

        ALsource *owner = al_source_get((ALuint)owner_id);
        if (!owner || (owner->state != AL_PLAYING && owner->state != AL_PAUSED)) {
            if (owner) {
                owner->hw_voice_idx = AL_HW_VOICE_INVALID;
            }
            s_hw_voice_owner[v] = (int)src->id;
            src->hw_voice_idx = (int)v;
            return (int)v;
        }

        /* Check if one-shot voice playback completed in hardware */
        if (!owner->looping && apu_voice_is_active(apu_base, v) == 0) {
            owner->state = AL_STOPPED;
            owner->hw_voice_idx = AL_HW_VOICE_INVALID;
            s_hw_voice_owner[v] = (int)src->id;
            src->hw_voice_idx = (int)v;
            return (int)v;
        }
    }

    /* Pass 2: all 64 slots busy, calculate priority of src and find victim */
    float new_priority = apu_voice_mgr_calc_priority(src);
    int victim_voice = -1;
    float min_priority = 1e30f;

    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        int owner_id = s_hw_voice_owner[v];
        ALsource *owner = al_source_get((ALuint)owner_id);
        float p = owner ? apu_voice_mgr_calc_priority(owner) : 0.0f;
        if (p < min_priority) {
            min_priority = p;
            victim_voice = (int)v;
        }
    }

    if (victim_voice >= 0 && new_priority > min_priority) {
        int victim_owner_id = s_hw_voice_owner[victim_voice];
        ALsource *victim = al_source_get((ALuint)victim_owner_id);
        NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context((uint32_t)victim_voice);

        /*
         * Click-prevention preemption protocol:
         * 1. Preserve victim progress (PRD index and fractional sample phase).
         */
        if (vctx && victim) {
            victim->saved_prd_index = vctx->current_prd_index;
            victim->saved_sample_pos_frac = vctx->sample_pos_frac;
        }

        /*
         * 2. Zero gains to eliminate clicks/pops and DC offsets before halting.
         */
        if (vctx) {
            vctx->master_vol_left = 0;
            vctx->master_vol_right = 0;
            memset(vctx->mixbin_gain, 0, sizeof(vctx->mixbin_gain));
        }

        /*
         * 3. Halt channel and confirm halt in hardware registers.
         */
        apu_voice_stop(apu_base, (uint32_t)victim_voice);
        if (apu_voice_is_active(apu_base, (uint32_t)victim_voice) != 0) {
            apu_voice_stop(apu_base, (uint32_t)victim_voice);
        }

        /*
         * 4. Detach victim to virtual standby (retaining AL_PLAYING state).
         */
        if (victim) {
            victim->hw_voice_idx = AL_HW_VOICE_INVALID;
        }

        /*
         * 5. Assign hardware voice slot to the higher-priority preemptor.
         */
        s_hw_voice_owner[victim_voice] = (int)src->id;
        src->hw_voice_idx = victim_voice;
        return victim_voice;
    }

    /* Fallback: voice remains logically playing but virtualized in standby */
    src->hw_voice_idx = AL_HW_VOICE_INVALID;
    return AL_HW_VOICE_INVALID;
}

/*
 * ============================================================================
 * Hardware Voice Release
 * ============================================================================
 */

void apu_voice_mgr_release(ALsource *src) {
    if (!src) {
        return;
    }

    if (src->hw_voice_idx >= 0 && src->hw_voice_idx < (int)NV_PAPU_NUM_3D_VOICES) {
        uint32_t v = (uint32_t)src->hw_voice_idx;
        NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context(v);
        if (vctx) {
            vctx->master_vol_left = 0;
            vctx->master_vol_right = 0;
            memset(vctx->mixbin_gain, 0, sizeof(vctx->mixbin_gain));
        }
        apu_voice_stop(al_source_get_apu_base(), v);

        if (s_hw_voice_owner[v] == (int)src->id) {
            s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
        }
        src->hw_voice_idx = AL_HW_VOICE_INVALID;
    } else {
        src->hw_voice_idx = AL_HW_VOICE_INVALID;
    }
}

/*
 * ============================================================================
 * Periodic Frame Update & Automatic Standby Promotion
 * ============================================================================
 */

void apu_voice_mgr_update(uintptr_t apu_base) {
    uintptr_t base = (apu_base != 0) ? apu_base : al_source_get_apu_base();

    uint32_t active0 = apu_read32(base, NV_PAPU_VP_ACTIVE_0);
    uint32_t active1 = apu_read32(base, NV_PAPU_VP_ACTIVE_1);

    /* 1. Reap phase: detect completed one-shot hardware voices */
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        int owner_id = s_hw_voice_owner[v];
        if (owner_id <= 0) {
            continue;
        }

        ALsource *owner = al_source_get((ALuint)owner_id);
        if (!owner) {
            s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
            continue;
        }

        uint32_t is_active = (v < 32u) ? (active0 & (1u << v)) : (active1 & (1u << (v - 32u)));
        if (is_active == 0 && !owner->looping) {
            owner->state = AL_STOPPED;
            owner->hw_voice_idx = AL_HW_VOICE_INVALID;
            s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
        } else if (owner->state != AL_PLAYING && owner->state != AL_PAUSED) {
            owner->hw_voice_idx = AL_HW_VOICE_INVALID;
            s_hw_voice_owner[v] = AL_HW_VOICE_INVALID;
        }
    }

    /* 2. Promotion phase: promote highest-priority virtual standby sources to free slots */
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        if (s_hw_voice_owner[v] == AL_HW_VOICE_INVALID) {
            ALsource *best_candidate = NULL;
            float best_priority = 0.0f;

            for (ALuint id = 1; id <= AL_MAX_SOURCES; id++) {
                ALsource *src = al_source_get(id);
                if (src && src->in_use && src->state == AL_PLAYING &&
                    src->hw_voice_idx == AL_HW_VOICE_INVALID && src->buffer != NULL) {
                    float p = apu_voice_mgr_calc_priority(src);
                    if (p > best_priority) {
                        best_priority = p;
                        best_candidate = src;
                    }
                }
            }

            if (best_candidate != NULL && best_priority > 0.0f) {
                s_hw_voice_owner[v] = (int)best_candidate->id;
                best_candidate->hw_voice_idx = (int)v;
                /* Promoted candidate restores saved_prd_index and saved_sample_pos_frac,
                 * clears them, configures 3D voice context, unpauses, and triggers playback. */
                al_source_program_hw_voice(best_candidate, v);
            } else {
                break;
            }
        }
    }
}

/*
 * ============================================================================
 * Subsystem Status & Query APIs
 * ============================================================================
 */

uint32_t apu_voice_mgr_get_active_hw_count(void) {
    uint32_t count = 0;
    for (uint32_t v = 0; v < NV_PAPU_NUM_3D_VOICES; v++) {
        if (s_hw_voice_owner[v] != AL_HW_VOICE_INVALID) {
            count++;
        }
    }
    return count;
}

uint32_t apu_voice_mgr_get_virtual_standby_count(void) {
    uint32_t count = 0;
    for (ALuint id = 1; id <= AL_MAX_SOURCES; id++) {
        ALsource *src = al_source_get(id);
        if (src && src->in_use && src->state == AL_PLAYING && src->hw_voice_idx == AL_HW_VOICE_INVALID) {
            count++;
        }
    }
    return count;
}
