/*
 * Voice steal policy and standby promotion tests:
 *   - victim = lowest priority, ties broken by the oldest start (play_seq)
 *   - steal when new_priority >= victim priority, never when new_priority == 0
 *   - preemption protocol (save position, mute, stop, standby resume) intact
 *   - promotion computes each standby priority once per update, best first,
 *     ties to the lowest source id
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <AL/al.h>
#include "al_source.h"
#include "al_buffer.h"
#include "al_listener.h"
#include "apu_voice.h"
#include "apu_voice_mgr.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x10000
#define NUM_HW NV_PAPU_NUM_3D_VOICES

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

static uint8_t *g_mmio = NULL;
static NVAPU_VOICE_CONTEXT_3D *g_voices = NULL;
static ALuint g_buf = 0;

/* Fresh sources, voices, MMIO and listener; the buffer is kept */
static void reset_world(void) {
    al_source_cleanup_subsystem();
    memset(g_mmio, 0, MOCK_MMIO_SIZE);
    CHECK(apu_voice_subsystem_init((uintptr_t)g_mmio, g_voices, 0x10000) == 0);
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)g_mmio);
    CHECK(alGetError() == AL_NO_ERROR);
}

static ALuint new_source(float dist, int looping, float gain) {
    ALuint id = 0;
    alGenSources(1, &id);
    CHECK(id != 0);
    alSourcei(id, AL_BUFFER, (ALint)g_buf);
    alSourcef(id, AL_GAIN, gain);
    alSource3f(id, AL_POSITION, 0.0f, 0.0f, -dist);
    alSourcei(id, AL_LOOPING, looping ? AL_TRUE : AL_FALSE);
    return id;
}

static ALsource *get(ALuint id) {
    ALsource *s = al_source_get(id);
    CHECK(s != NULL);
    return s;
}

static int slot_of(ALuint id) {
    return get(id)->hw_voice_idx;
}

static int in_standby(ALuint id) {
    ALsource *s = get(id);
    return s->state == AL_PLAYING && s->hw_voice_idx == AL_HW_VOICE_INVALID;
}

/* 64 identical looping sources, started in order; returns their ids */
static void fill_identical(ALuint ids[NUM_HW], float gain) {
    for (uint32_t i = 0; i < NUM_HW; i++) {
        ids[i] = new_source(2.0f, 1, gain);
        alSourcePlay(ids[i]);
        CHECK(slot_of(ids[i]) >= 0);
    }
    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW);
    CHECK(apu_voice_mgr_get_virtual_standby_count() == 0);
}

static void test_tie_evicts_oldest(void) {
    printf("[1] Equal priorities: the newcomer evicts the oldest source...\n");
    reset_world();
    ALuint ids[NUM_HW];
    fill_identical(ids, 1.0f);

    /* Identical sources have identical priority; play_seq follows start order */
    float p0 = apu_voice_mgr_calc_priority(get(ids[0]));
    CHECK(p0 > 0.0f);
    for (uint32_t i = 1; i < NUM_HW; i++) {
        CHECK(apu_voice_mgr_calc_priority(get(ids[i])) == p0);
        CHECK(get(ids[i])->play_seq > get(ids[i - 1])->play_seq);
    }

    /* 65th identical source, allocated directly to inspect the protocol steps */
    ALsource *oldest = get(ids[0]);
    int slot = oldest->hw_voice_idx;
    NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)slot);
    ctx->current_prd_index = 7;
    ctx->sample_pos_frac = 0x00001234u;

    ALuint n65 = new_source(2.0f, 1, 1.0f);
    ALsource *s65 = get(n65);
    CHECK(apu_voice_mgr_calc_priority(s65) == p0);
    int got = apu_voice_mgr_allocate(s65);
    CHECK(got == slot);
    CHECK(apu_voice_mgr_get_owner((uint32_t)slot) == (int)n65);

    /* Saved position, mute, halt, standby (still AL_PLAYING) */
    CHECK(oldest->saved_prd_index == 7);
    CHECK(oldest->saved_sample_pos_frac == 0x00001234u);
    CHECK(ctx->master_vol_left == 0 && ctx->master_vol_right == 0);
    for (int m = 0; m < 16; m++) {
        CHECK(ctx->mixbin_gain[m] == 0);
    }
    CHECK(apu_voice_is_active((uintptr_t)g_mmio, (uint32_t)slot) == 0);
    CHECK(oldest->hw_voice_idx == AL_HW_VOICE_INVALID);
    CHECK(oldest->state == AL_PLAYING);
    CHECK(in_standby(ids[0]));

    al_source_program_hw_voice(s65, (uint32_t)got);
    s65->state = AL_PLAYING;
    CHECK(apu_voice_is_active((uintptr_t)g_mmio, (uint32_t)slot) == 1);
    CHECK(ctx->master_vol_left > 0);
    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW);
    CHECK(apu_voice_mgr_get_virtual_standby_count() == 1);

    /* Standby resume: free another slot, the evicted source returns where it was */
    alSourceStop(ids[5]);
    al_source_update_frame();
    CHECK(oldest->hw_voice_idx >= 0);
    const NVAPU_VOICE_CONTEXT_3D *rc = apu_voice_get_context((uint32_t)oldest->hw_voice_idx);
    CHECK(rc->current_prd_index == 7);
    CHECK(rc->sample_pos_frac == 0x00001234u);
    CHECK(oldest->saved_prd_index == 0 && oldest->saved_sample_pos_frac == 0);
    CHECK(rc->master_vol_left > 0);
    CHECK(apu_voice_is_active((uintptr_t)g_mmio, (uint32_t)oldest->hw_voice_idx) == 1);
    printf("    -> oldest of 64 identical sources evicted, protocol and standby resume intact.\n");
}

static void test_eviction_chain_and_restart(void) {
    printf("[2] Successive newcomers evict in start order; a restart makes a source young...\n");
    reset_world();
    ALuint ids[NUM_HW];
    fill_identical(ids, 1.0f);

    ALuint news[8];
    for (int k = 0; k < 6; k++) {
        news[k] = new_source(2.0f, 1, 1.0f);
        alSourcePlay(news[k]);
        CHECK(slot_of(news[k]) >= 0);
        CHECK(in_standby(ids[k]));
        for (int j = k + 1; j < 8; j++) {
            CHECK(slot_of(ids[j]) >= 0); /* not yet evicted */
        }
        CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW);
        CHECK(apu_voice_mgr_get_virtual_standby_count() == (uint32_t)(k + 1));
    }

    /* Restarting ids[6] gives it a new start: it is no longer the oldest */
    uint32_t seq_before = get(ids[6])->play_seq;
    int slot6 = slot_of(ids[6]);
    alSourcePlay(ids[6]);
    CHECK(get(ids[6])->play_seq > seq_before);
    CHECK(slot_of(ids[6]) == slot6);

    news[6] = new_source(2.0f, 1, 1.0f);
    alSourcePlay(news[6]);
    CHECK(slot_of(news[6]) >= 0);
    CHECK(slot_of(ids[6]) == slot6);   /* kept */
    CHECK(in_standby(ids[7]));         /* next oldest evicted instead */
    printf("    -> evictions follow play_seq; restart refreshes it.\n");
}

static void test_silent_never_evicts(void) {
    printf("[3] A silent newcomer never evicts an audible source...\n");
    reset_world();
    ALuint ids[NUM_HW];
    fill_identical(ids, 1.0f);

    ALuint holders[NUM_HW];
    for (uint32_t i = 0; i < NUM_HW; i++) {
        holders[i] = (ALuint)apu_voice_mgr_get_owner(i);
    }

    ALuint silent = new_source(2.0f, 1, 0.0f);
    alSourcePlay(silent);
    CHECK(in_standby(silent));
    ALuint quiet = new_source(2.0f, 1, 0.0005f); /* below the 0.001 threshold */
    alSourcePlay(quiet);
    CHECK(in_standby(quiet));
    for (uint32_t i = 0; i < NUM_HW; i++) {
        CHECK((ALuint)apu_voice_mgr_get_owner(i) == holders[i]);
    }
    CHECK(apu_voice_mgr_get_virtual_standby_count() == 2);

    /* Everything silent (listener gain 0): still no eviction */
    alListenerf(AL_GAIN, 0.0f);
    ALuint late = new_source(2.0f, 1, 1.0f);
    alSourcePlay(late);
    CHECK(in_standby(late));
    for (uint32_t i = 0; i < NUM_HW; i++) {
        CHECK((ALuint)apu_voice_mgr_get_owner(i) == holders[i]);
    }
    alListenerf(AL_GAIN, 1.0f);

    /* Silent holders do not block an audible newcomer: it takes the oldest */
    reset_world();
    ALuint quietids[NUM_HW];
    fill_identical(quietids, 0.0f);
    ALuint loud = new_source(2.0f, 1, 1.0f);
    alSourcePlay(loud);
    CHECK(slot_of(loud) >= 0);
    CHECK(in_standby(quietids[0]));
    CHECK(slot_of(quietids[1]) >= 0);
    printf("    -> priority 0 never steals; an audible newcomer replaces a silent holder.\n");
}

static void test_lower_priority_does_not_evict(void) {
    printf("[4] Lower-priority newcomers do not evict, higher ones do...\n");
    reset_world();
    ALuint ids[NUM_HW];
    fill_identical(ids, 1.0f);

    /* Clearly lower: one-shot and far away */
    ALuint far_one = new_source(40.0f, 0, 1.0f);
    alSourcePlay(far_one);
    CHECK(in_standby(far_one));
    /* Same position but one-shot (priority base 1 vs 3 for looping) */
    ALuint oneshot = new_source(2.0f, 0, 1.0f);
    alSourcePlay(oneshot);
    CHECK(in_standby(oneshot));
    /* Slightly farther looping source: lower priority, no steal */
    ALuint slightly = new_source(2.01f, 1, 1.0f);
    CHECK(apu_voice_mgr_calc_priority(get(slightly)) < apu_voice_mgr_calc_priority(get(ids[0])));
    alSourcePlay(slightly);
    CHECK(in_standby(slightly));
    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW);
    for (uint32_t i = 0; i < NUM_HW; i++) {
        CHECK(slot_of(ids[i]) >= 0);
    }

    /* Slightly closer looping source: higher priority, steals the oldest */
    ALuint closer = new_source(1.99f, 1, 1.0f);
    CHECK(apu_voice_mgr_calc_priority(get(closer)) > apu_voice_mgr_calc_priority(get(ids[0])));
    alSourcePlay(closer);
    CHECK(slot_of(closer) >= 0);
    CHECK(in_standby(ids[0]));
    printf("    -> only >= priority steals; lower and silent newcomers wait in standby.\n");
}

static void test_priority_beats_age(void) {
    printf("[5] Priority decides first, age only breaks ties...\n");
    reset_world();

    /* Oldest source is the closest (highest priority), newest the farthest */
    ALuint ids[NUM_HW];
    for (uint32_t i = 0; i < NUM_HW; i++) {
        ids[i] = new_source(1.0f + 0.1f * (float)i, 1, 1.0f);
        alSourcePlay(ids[i]);
        CHECK(slot_of(ids[i]) >= 0);
    }
    ALuint n = new_source(3.0f, 1, 1.0f);
    alSourcePlay(n);
    CHECK(slot_of(n) >= 0);
    CHECK(in_standby(ids[NUM_HW - 1]));  /* lowest priority is the victim ... */
    CHECK(slot_of(ids[0]) >= 0);          /* ... not the oldest source */

    /* Ties only count within the lowest-priority group:
     * 16 near (older), 32 identical far, 16 near (newer). */
    reset_world();
    ALuint mix[NUM_HW];
    for (uint32_t i = 0; i < NUM_HW; i++) {
        float d = (i >= 16 && i < 48) ? 4.0f : 2.0f;
        mix[i] = new_source(d, 1, 1.0f);
        alSourcePlay(mix[i]);
        CHECK(slot_of(mix[i]) >= 0);
    }
    ALuint far_new = new_source(4.0f, 1, 1.0f); /* ties with the far group */
    alSourcePlay(far_new);
    CHECK(slot_of(far_new) >= 0);
    CHECK(in_standby(mix[16]));         /* oldest of the lowest-priority group */
    CHECK(slot_of(mix[0]) >= 0);        /* oldest overall is higher priority, kept */
    for (uint32_t i = 17; i < 48; i++) {
        CHECK(slot_of(mix[i]) >= 0);
    }
    printf("    -> lowest priority first; oldest start only among equals.\n");
}

static void test_play_seq_wraps(void) {
    printf("[6] Age comparison survives play_seq wrap-around...\n");
    reset_world();
    ALuint ids[NUM_HW];
    fill_identical(ids, 1.0f);

    /* ids[0] is the oldest start, counting through UINT32_MAX back to small values */
    for (uint32_t i = 0; i < NUM_HW; i++) {
        get(ids[i])->play_seq = 0xFFFFFFF0u + i;
    }
    ALuint n = new_source(2.0f, 1, 1.0f);
    alSourcePlay(n);
    CHECK(slot_of(n) >= 0);
    CHECK(in_standby(ids[0]));
    CHECK(slot_of(ids[16]) >= 0); /* wrapped value 0, but newer than ids[0] */
    printf("    -> oldest start chosen across the 2^32 wrap.\n");
}

/* ----------------------------------------------------------------------------
 * Promotion phase
 * ------------------------------------------------------------------------- */

static void test_promotion_cost_and_order(void) {
    printf("[7] Promotion computes each standby priority once, best first...\n");
    reset_world();

    /* 256 one-shot sources at increasing distance: 64 on hardware, 192 standby */
    ALuint all[AL_MAX_SOURCES];
    for (int i = 0; i < AL_MAX_SOURCES; i++) {
        all[i] = new_source(1.0f + 0.05f * (float)i, 0, 1.0f);
        alSourcePlay(all[i]);
    }
    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW);
    CHECK(apu_voice_mgr_get_virtual_standby_count() == AL_MAX_SOURCES - NUM_HW);

    /* No free slot: the update must not evaluate any standby priority */
    uint32_t before = al_source_debug_spatial_calc_count();
    al_source_update_frame();
    CHECK(al_source_debug_spatial_calc_count() == before);

    /* Reference ranking of the standby sources: priority descending, id ascending */
    float prio[AL_MAX_SOURCES];
    for (int i = 0; i < AL_MAX_SOURCES; i++) {
        prio[i] = in_standby(all[i]) ? apu_voice_mgr_calc_priority(get(all[i])) : -1.0f;
    }
    int expect[10];
    for (int k = 0; k < 10; k++) {
        int best = -1;
        for (int i = 0; i < AL_MAX_SOURCES; i++) {
            if (prio[i] > 0.0f && (best < 0 || prio[i] > prio[best])) {
                best = i;
            }
        }
        CHECK(best >= 0);
        expect[k] = best;
        prio[best] = -1.0f;
    }

    for (int i = 0; i < 10; i++) {
        alSourceStop(all[i * 3]); /* free 10 slots */
    }
    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW - 10);

    before = al_source_debug_spatial_calc_count();
    al_source_update_frame();
    uint32_t used = al_source_debug_spatial_calc_count() - before;
    /* One evaluation per standby candidate (not per free slot) plus one spatial
     * computation to program each promoted voice. */
    CHECK(used == (AL_MAX_SOURCES - NUM_HW) + 10);

    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW);
    CHECK(apu_voice_mgr_get_virtual_standby_count() == AL_MAX_SOURCES - NUM_HW - 10);
    for (int k = 0; k < 10; k++) {
        ALsource *s = get(all[expect[k]]);
        CHECK(s->hw_voice_idx >= 0);
        CHECK(apu_voice_mgr_get_owner((uint32_t)s->hw_voice_idx) == (int)s->id);
        CHECK(apu_voice_is_active((uintptr_t)g_mmio, (uint32_t)s->hw_voice_idx) == 1);
    }
    /* Best candidates were assigned to free slots in ascending slot order */
    int last_slot = -1;
    for (int k = 0; k < 10; k++) {
        int slot = slot_of(all[expect[k]]);
        CHECK(slot > last_slot);
        last_slot = slot;
    }
    printf("    -> %u computations for 192 candidates and 10 promotions.\n", (unsigned)used);
}

static void test_promotion_ties_and_silent(void) {
    printf("[8] Promotion ties go to the lowest id; silent sources stay in standby...\n");
    reset_world();
    ALuint hw[NUM_HW];
    fill_identical(hw, 1.0f);

    /* 20 identical standby candidates (farther than every holder) and one silent */
    ALuint wait[20];
    for (int i = 0; i < 20; i++) {
        wait[i] = new_source(8.0f, 1, 1.0f);
        alSourcePlay(wait[i]);
        CHECK(in_standby(wait[i]));
    }
    ALuint mute = new_source(2.0f, 1, 0.0f);
    alSourcePlay(mute);
    CHECK(in_standby(mute));

    /* Three free slots: the three lowest ids among equals are promoted */
    alSourceStop(hw[10]);
    alSourceStop(hw[20]);
    alSourceStop(hw[30]);
    al_source_update_frame();
    for (int i = 0; i < 20; i++) {
        if (i < 3) {
            CHECK(slot_of(wait[i]) >= 0);
        } else {
            CHECK(in_standby(wait[i]));
        }
    }
    CHECK(in_standby(mute));
    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW);

    /* More free slots than audible candidates: slots stay free, silent one waits */
    for (uint32_t i = 0; i < 25; i++) {
        if (i != 10 && i != 20) {
            alSourceStop(hw[i]);
        }
    }
    uint32_t free_slots = NUM_HW - apu_voice_mgr_get_active_hw_count();
    al_source_update_frame();
    CHECK(in_standby(mute));
    for (int i = 0; i < 20; i++) {
        CHECK(slot_of(wait[i]) >= 0);
    }
    CHECK(apu_voice_mgr_get_active_hw_count() == NUM_HW - free_slots + 17);
    CHECK(apu_voice_mgr_get_virtual_standby_count() == 1);
    printf("    -> lowest ids first on ties; priority 0 never promoted.\n");
}

int main(void) {
    printf("=== OpenAL Voice Steal Policy & Promotion Host Test ===\n");

    CHECK(apu_mem_init(0) == 0);
    g_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    g_voices = (NVAPU_VOICE_CONTEXT_3D *)calloc(NUM_HW, sizeof(NVAPU_VOICE_CONTEXT_3D));
    CHECK(g_mmio != NULL && g_voices != NULL);
    CHECK(apu_voice_subsystem_init((uintptr_t)g_mmio, g_voices, 0x10000) == 0);

    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)g_mmio);
    CHECK(alGetError() == AL_NO_ERROR);

    {
        int16_t data[256] = {0};
        alGenBuffers(1, &g_buf);
        CHECK(g_buf != 0);
        alBufferData(g_buf, AL_FORMAT_MONO16, data, sizeof(data), 44100);
        CHECK(alGetError() == AL_NO_ERROR);
    }

    test_tie_evicts_oldest();
    test_eviction_chain_and_restart();
    test_silent_never_evicts();
    test_lower_priority_does_not_evict();
    test_priority_beats_age();
    test_play_seq_wraps();
    test_promotion_cost_and_order();
    test_promotion_ties_and_silent();

    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)g_mmio);
    apu_mem_shutdown();
    free(g_voices);
    free(g_mmio);

    printf("=== All Voice Steal Policy & Promotion Tests Passed! ===\n");
    return 0;
}
