/*
 * Source-layer regression tests:
 *   1. one spatial computation per source update (no redundant math)
 *   3. paused sources keep their voice context current
 *   4. alSourcePlay on a playing source restarts it from the beginning
 *   5. a preempted source paused in standby resumes at its saved position
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

#define CHECK(cond) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            exit(1); \
        } \
    } while (0)

/* Run stmt and require exactly n spatial computations */
#define EXPECT_CALCS(n, stmt) \
    do { \
        uint32_t calc_before_ = al_source_debug_spatial_calc_count(); \
        stmt; \
        uint32_t calc_used_ = al_source_debug_spatial_calc_count() - calc_before_; \
        if (calc_used_ != (uint32_t)(n)) { \
            fprintf(stderr, "%s:%d: '%s' used %u spatial computations, expected %u\n", \
                    __FILE__, __LINE__, #stmt, (unsigned)calc_used_, (unsigned)(n)); \
            exit(1); \
        } \
    } while (0)

static uint8_t *g_mmio = NULL;
static NVAPU_VOICE_CONTEXT_3D *g_voices = NULL;
static ALuint g_mono = 0;
static ALuint g_stereo = 0;

static ALuint create_buffer(ALenum format, ALsizei freq) {
    ALuint buf = 0;
    int16_t data[256] = {0};
    alGenBuffers(1, &buf);
    CHECK(buf != 0);
    alBufferData(buf, format, data, sizeof(data), freq);
    CHECK(alGetError() == AL_NO_ERROR);
    return buf;
}

/* Fresh sources, voices, MMIO and listener; buffers are kept */
static void reset_world(void) {
    al_source_cleanup_subsystem();
    memset(g_mmio, 0, MOCK_MMIO_SIZE);
    CHECK(apu_voice_subsystem_init((uintptr_t)g_mmio, g_voices, 0x10000) == 0);
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)g_mmio);
    CHECK(alGetError() == AL_NO_ERROR);
}

static ALuint new_source(ALuint buf, float x, float y, float z, int looping) {
    ALuint id = 0;
    alGenSources(1, &id);
    CHECK(id != 0);
    alSourcei(id, AL_BUFFER, (ALint)buf);
    alSource3f(id, AL_POSITION, x, y, z);
    alSourcei(id, AL_LOOPING, looping ? AL_TRUE : AL_FALSE);
    return id;
}

/* Pitch step clamp mirrors al_source.c (APU_PITCH_STEP_MIN/MAX) */
static uint32_t expected_pitch_step(ALsizei freq, float pitch) {
    uint32_t step = APU_CALC_PITCH_STEP(freq, pitch);
    if (step < 0x00001000u) step = 0x00001000u;
    if (step > 0x00040000u) step = 0x00040000u;
    return step;
}

/*
 * The voice context must hold exactly what the per-field update helpers used
 * to produce: each field derived from al_source_calc_spatial() of the source.
 */
static void check_ctx_matches_calc(const ALsource *src) {
    CHECK(src->hw_voice_idx >= 0);
    const NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)src->hw_voice_idx);
    CHECK(ctx != NULL);

    AL_SPATIAL_CALC calc;
    al_source_calc_spatial(src, &calc);

    uint16_t vol = (uint16_t)(calc.effective_gain * 65535.0f + 0.5f);
    CHECK(ctx->master_vol_left == vol);
    CHECK(ctx->master_vol_right == vol);
    CHECK(ctx->pitch_step ==
          expected_pitch_step(src->buffer->frequency, src->pitch * calc.doppler_pitch));

    if (src->buffer->channels == 1) {
        CHECK(ctx->mode_3d == 1);
        CHECK(ctx->itd_delay_left == calc.itd_delay_left);
        CHECK(ctx->itd_delay_right == calc.itd_delay_right);
        CHECK(ctx->hrtf_b0 == calc.hrtf_coeffs.b0);
        CHECK(ctx->hrtf_b1 == calc.hrtf_coeffs.b1);
        CHECK(ctx->hrtf_b2 == calc.hrtf_coeffs.b2);
        CHECK(ctx->hrtf_a1 == calc.hrtf_coeffs.a1);
        CHECK(ctx->hrtf_a2 == calc.hrtf_coeffs.a2);
        for (int i = 0; i < 6; i++) {
            CHECK(ctx->mixbin_gain[i] == calc.mixbin_gain[i]);
        }
    }
}

static void test_single_computation_per_update(void) {
    printf("[1] One spatial computation per source update...\n");
    reset_world();

    ALuint a = new_source(g_mono, 3.0f, 1.0f, -4.0f, 1);
    ALuint idle = new_source(g_mono, 1.0f, 0.0f, -1.0f, 0);

    /* Sources without a hardware voice cost nothing */
    EXPECT_CALCS(0, alSource3f(a, AL_POSITION, 3.0f, 1.0f, -4.0f));
    EXPECT_CALCS(0, alSourcef(a, AL_GAIN, 1.0f));

    alSourcePlay(a);
    ALsource *sa = al_source_get(a);
    CHECK(sa->state == AL_PLAYING && sa->hw_voice_idx >= 0);

    /* The measured offender: five computations per position change before the fix */
    EXPECT_CALCS(1, alSource3f(a, AL_POSITION, 5.0f, 2.0f, -3.0f));
    check_ctx_matches_calc(sa);

    EXPECT_CALCS(1, alSourcei(a, AL_SOURCE_RELATIVE, AL_TRUE));
    check_ctx_matches_calc(sa);
    EXPECT_CALCS(1, alSourcei(a, AL_SOURCE_RELATIVE, AL_FALSE));
    check_ctx_matches_calc(sa);

    /* Single-purpose setters also compute once */
    EXPECT_CALCS(1, alSourcef(a, AL_GAIN, 0.75f));
    EXPECT_CALCS(1, alSourcef(a, AL_PITCH, 1.5f));
    EXPECT_CALCS(1, alSourcef(a, AL_MIN_GAIN, 0.1f));
    EXPECT_CALCS(1, alSourcef(a, AL_MAX_GAIN, 0.9f));
    EXPECT_CALCS(1, alSourcef(a, AL_REFERENCE_DISTANCE, 2.0f));
    EXPECT_CALCS(1, alSourcef(a, AL_MAX_DISTANCE, 100.0f));
    EXPECT_CALCS(1, alSourcef(a, AL_ROLLOFF_FACTOR, 1.5f));
    EXPECT_CALCS(1, alSourcef(a, AL_CONE_INNER_ANGLE, 60.0f));
    EXPECT_CALCS(1, alSourcef(a, AL_CONE_OUTER_ANGLE, 200.0f));
    EXPECT_CALCS(1, alSourcef(a, AL_CONE_OUTER_GAIN, 0.25f));
    EXPECT_CALCS(1, alSourcef(a, AL_XBOX_LFE_GAIN, 0.5f));
    EXPECT_CALCS(1, alSource3f(a, AL_VELOCITY, 1.0f, 0.0f, 2.0f));
    EXPECT_CALCS(1, alSource3f(a, AL_DIRECTION, 0.0f, 0.0f, -1.0f));
    check_ctx_matches_calc(sa);

    /* Rejected values do no work */
    EXPECT_CALCS(0, alSourcef(a, AL_GAIN, -1.0f));
    CHECK(alGetError() == AL_INVALID_VALUE);

    /* Listener and global updates: exactly one computation per voiced source */
    const int n = 12;
    ALuint extra[12];
    for (int i = 0; i < n - 1; i++) {
        extra[i] = new_source(g_mono, (float)i - 5.0f, 0.5f, -2.0f - (float)i, i & 1);
        alSourcePlay(extra[i]);
        CHECK(al_source_get(extra[i])->hw_voice_idx >= 0);
    }
    /* "a" plus 11 extras = 12 voiced sources; "idle" has no voice */
    const float orient[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    EXPECT_CALCS(n, alListener3f(AL_POSITION, 1.0f, 0.5f, 0.25f));
    EXPECT_CALCS(n, alListener3f(AL_VELOCITY, 0.5f, 0.0f, -1.0f));
    EXPECT_CALCS(n, alListenerfv(AL_ORIENTATION, orient));
    EXPECT_CALCS(n, alListenerf(AL_GAIN, 0.8f));
    EXPECT_CALCS(n, alDistanceModel(AL_LINEAR_DISTANCE));
    EXPECT_CALCS(n, alDopplerFactor(0.5f));
    EXPECT_CALCS(n, alSpeedOfSound(300.0f));
    EXPECT_CALCS(n, al_source_update_all_spatial());
    EXPECT_CALCS(n, al_source_update_all_gains());

    /* A full per-frame listener update costs 3 computations per source, not 15 */
    {
        uint32_t before = al_source_debug_spatial_calc_count();
        alListener3f(AL_POSITION, 2.0f, 0.0f, 1.0f);
        alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.5f);
        alListenerfv(AL_ORIENTATION, orient);
        CHECK(al_source_debug_spatial_calc_count() - before == (uint32_t)(3 * n));
    }

    ALsource *si = al_source_get(idle);
    CHECK(si->hw_voice_idx == AL_HW_VOICE_INVALID);
    CHECK(alGetError() == AL_NO_ERROR);
    printf("    -> single computation per source for setters, listener and global updates.\n");
}

static void test_bit_identical_results(void) {
    printf("[1b] Applied context fields are bit-identical to al_source_calc_spatial()...\n");
    reset_world();

    ALuint mono = new_source(g_mono, 0.0f, 0.0f, -2.0f, 1);
    ALuint stereo = new_source(g_stereo, 0.0f, 0.0f, -2.0f, 1);
    alSourcePlay(mono);
    alSourcePlay(stereo);
    ALsource *sm = al_source_get(mono);
    ALsource *ss = al_source_get(stereo);
    CHECK(sm->hw_voice_idx >= 0 && ss->hw_voice_idx >= 0);

    const float orient[6] = {0.3f, 0.1f, -0.9f, 0.0f, 1.0f, 0.1f};
    alListenerfv(AL_ORIENTATION, orient);
    alListener3f(AL_VELOCITY, 1.5f, 0.0f, -0.5f);
    alDopplerFactor(1.0f);
    alSourcef(mono, AL_XBOX_LFE_GAIN, 0.4f);
    alSource3f(mono, AL_VELOCITY, -2.0f, 1.0f, 3.0f);
    alSource3f(mono, AL_DIRECTION, 0.2f, 0.0f, -1.0f);
    alSourcef(mono, AL_CONE_INNER_ANGLE, 90.0f);
    alSourcef(mono, AL_CONE_OUTER_ANGLE, 270.0f);
    alSourcef(mono, AL_CONE_OUTER_GAIN, 0.3f);
    alSourcef(mono, AL_PITCH, 1.25f);

    /* Deterministic position sweep, listener moving as well */
    uint32_t rng = 12345u;
    for (int i = 0; i < 300; i++) {
        float v[6];
        for (int k = 0; k < 6; k++) {
            rng = rng * 1664525u + 1013904223u;
            v[k] = (float)((int)((rng >> 8) % 4001u) - 2000) / 100.0f; /* -20.00 .. 20.00 */
        }
        alListener3f(AL_POSITION, v[3], v[4], v[5]);
        alSource3f(mono, AL_POSITION, v[0], v[1], v[2]);
        alSource3f(stereo, AL_POSITION, v[2], v[0], v[1]);
        if ((i % 50) == 0) {
            alSourcei(mono, AL_SOURCE_RELATIVE, (i / 50) & 1);
        }
        check_ctx_matches_calc(sm);
        check_ctx_matches_calc(ss);
    }

    /* 2D (stereo) voices keep their direct routing: no ITD/HRTF/mixbin rewrite */
    const NVAPU_VOICE_CONTEXT_3D *c2d = apu_voice_get_context((uint32_t)ss->hw_voice_idx);
    CHECK(c2d->mode_3d == 0);
    CHECK(c2d->mixbin_gain[0] == 0xFF && c2d->mixbin_gain[1] == 0xFF);
    CHECK(c2d->mixbin_gain[2] == 0 && c2d->mixbin_gain[5] == 0);
    CHECK(c2d->itd_delay_left == 0 && c2d->itd_delay_right == 0);
    printf("    -> 300 position updates produced identical context fields.\n");
}

static void test_paused_sources_stay_current(void) {
    printf("[3] Paused sources keep their voice context current...\n");
    reset_world();

    ALuint id = new_source(g_mono, 0.0f, 0.0f, -2.0f, 1);
    alSourcePlay(id);
    ALsource *src = al_source_get(id);
    CHECK(src->state == AL_PLAYING && src->hw_voice_idx >= 0);
    uint32_t v = (uint32_t)src->hw_voice_idx;
    NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context(v);
    CHECK(ctx != NULL);

    alSourcePause(id);
    CHECK(src->state == AL_PAUSED);
    CHECK(apu_read32((uintptr_t)g_mmio, NV_PAPU_VP_PAUSE_0) & (1u << v));

    NVAPU_VOICE_CONTEXT_3D before = *ctx;

    /* Listener movement while paused */
    alListener3f(AL_POSITION, 6.0f, 0.0f, 0.0f);
    CHECK(ctx->master_vol_left != before.master_vol_left);
    CHECK(ctx->itd_delay_left != before.itd_delay_left || ctx->itd_delay_right != before.itd_delay_right ||
          ctx->mixbin_gain[0] != before.mixbin_gain[0] || ctx->mixbin_gain[1] != before.mixbin_gain[1]);
    check_ctx_matches_calc(src);

    /* Source gain change while paused */
    NVAPU_VOICE_CONTEXT_3D mid = *ctx;
    alSourcef(id, AL_GAIN, 0.5f);
    CHECK(ctx->master_vol_left < mid.master_vol_left);
    CHECK(ctx->master_vol_left == ctx->master_vol_right);
    check_ctx_matches_calc(src);

    /* Listener gain while paused */
    mid = *ctx;
    alListenerf(AL_GAIN, 0.5f);
    CHECK(ctx->master_vol_left < mid.master_vol_left);
    check_ctx_matches_calc(src);
    alListenerf(AL_GAIN, 1.0f);

    /* Pitch, velocity (Doppler) and looping while paused */
    alSourcef(id, AL_PITCH, 2.0f);
    alSource3f(id, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    CHECK(ctx->pitch_step == expected_pitch_step(44100, 2.0f));
    alSource3f(id, AL_POSITION, 0.0f, 0.0f, -10.0f);
    alSource3f(id, AL_VELOCITY, 0.0f, 0.0f, 20.0f); /* approaching the listener */
    CHECK(ctx->pitch_step > expected_pitch_step(44100, 2.0f));
    check_ctx_matches_calc(src);
    alSourcei(id, AL_LOOPING, AL_FALSE);
    CHECK(ctx->loop_mode == NVAPU_VOICE_LOOP_OFF);
    alSourcei(id, AL_LOOPING, AL_TRUE);
    CHECK(ctx->loop_mode == NVAPU_VOICE_LOOP_ON);

    /* Still paused, no hardware restart as a side effect */
    CHECK(src->state == AL_PAUSED);
    CHECK(apu_read32((uintptr_t)g_mmio, NV_PAPU_VP_PAUSE_0) & (1u << v));
    CHECK(apu_voice_is_active((uintptr_t)g_mmio, v) == 1);

    /* Resume: unpaused, same voice, context consistent with current state */
    NVAPU_VOICE_CONTEXT_3D paused_ctx = *ctx;
    alSourcePlay(id);
    CHECK(src->state == AL_PLAYING);
    CHECK(src->hw_voice_idx == (int)v);
    CHECK((apu_read32((uintptr_t)g_mmio, NV_PAPU_VP_PAUSE_0) & (1u << v)) == 0);
    CHECK(memcmp(ctx, &paused_ctx, sizeof(paused_ctx)) == 0);
    check_ctx_matches_calc(src);

    /* Paused sources count as one computation each on listener updates */
    ALuint id2 = new_source(g_mono, 1.0f, 0.0f, -3.0f, 1);
    alSourcePlay(id2);
    alSourcePause(id2);
    EXPECT_CALCS(2, alListener3f(AL_POSITION, 0.0f, 1.0f, 0.0f));
    alSourceStop(id2);
    EXPECT_CALCS(1, alListener3f(AL_POSITION, 0.0f, 2.0f, 0.0f));

    printf("    -> paused voice context updated, resume still consistent.\n");
}

static void test_replay_restarts(void) {
    printf("[4] alSourcePlay on a playing source restarts it from the beginning...\n");
    reset_world();

    ALuint id = new_source(g_mono, 0.0f, 0.0f, -2.0f, 0);
    uint32_t stops0 = apu_voice_debug_stop_count();
    alSourcePlay(id);
    ALsource *src = al_source_get(id);
    CHECK(src->state == AL_PLAYING && src->hw_voice_idx >= 0);
    CHECK(apu_voice_debug_stop_count() == stops0); /* a first play halts nothing */
    int slot = src->hw_voice_idx;
    NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)slot);
    uint32_t seq = src->play_seq;
    CHECK(seq != 0);

    /* Simulate playback progress */
    ctx->current_prd_index = 3;
    ctx->sample_pos_frac = 0x00450123u;
    CHECK(apu_voice_mgr_get_active_hw_count() == 1);

    stops0 = apu_voice_debug_stop_count();
    alSourcePlay(id);
    CHECK(alGetError() == AL_NO_ERROR);
    /* The voice is halted exactly once before its context is rewritten */
    CHECK(apu_voice_debug_stop_count() == stops0 + 1);
    CHECK(src->state == AL_PLAYING);
    CHECK(src->hw_voice_idx == slot);
    CHECK(apu_voice_mgr_get_owner((uint32_t)slot) == (int)id);
    CHECK(apu_voice_mgr_get_active_hw_count() == 1);
    CHECK(ctx->current_prd_index == 0);
    CHECK(ctx->sample_pos_frac == 0);
    CHECK(ctx->active == 1);
    CHECK(apu_voice_is_active((uintptr_t)g_mmio, (uint32_t)slot) == 1);
    CHECK((apu_read32((uintptr_t)g_mmio, NV_PAPU_VP_PAUSE_0) & (1u << slot)) == 0);
    CHECK(src->saved_prd_index == 0 && src->saved_sample_pos_frac == 0);
    CHECK(src->play_seq != seq); /* restart counts as a new start */
    check_ctx_matches_calc(src);

    /* Replay keeps the current gain, pitch and looping configuration */
    alSourcef(id, AL_GAIN, 0.5f);
    alSourcei(id, AL_LOOPING, AL_TRUE);
    ctx->current_prd_index = 9;
    alSourcePlay(id);
    CHECK(ctx->current_prd_index == 0);
    CHECK(ctx->loop_mode == NVAPU_VOICE_LOOP_ON);
    check_ctx_matches_calc(src);
    CHECK(ctx->master_vol_left > 0x3F00 && ctx->master_vol_left < 0x4100); /* 0.5 gain at distance 2 -> 0.25 */

    /* One computation to reprogram the voice */
    EXPECT_CALCS(1, alSourcePlay(id));

    /* A source in virtual standby just loses its saved position */
    reset_world();
    ALuint fill[64];
    for (int i = 0; i < 64; i++) {
        fill[i] = new_source(g_mono, 0.0f, 0.0f, -1.0f, 1);
        alSourcePlay(fill[i]);
    }
    CHECK(apu_voice_mgr_get_active_hw_count() == 64);
    ALuint weak = new_source(g_mono, 0.0f, 0.0f, -50.0f, 0); /* clearly lower priority */
    alSourcePlay(weak);
    ALsource *sw = al_source_get(weak);
    CHECK(sw->state == AL_PLAYING && sw->hw_voice_idx == AL_HW_VOICE_INVALID);
    sw->saved_prd_index = 11;
    sw->saved_sample_pos_frac = 0x00ABCDEFu;
    alSourcePlay(weak);
    CHECK(sw->state == AL_PLAYING && sw->hw_voice_idx == AL_HW_VOICE_INVALID);
    CHECK(sw->saved_prd_index == 0 && sw->saved_sample_pos_frac == 0);
    CHECK(apu_voice_mgr_get_active_hw_count() == 64);

    /* Promoted later, it starts at the beginning */
    alSourceStop(fill[0]);
    al_source_update_frame();
    CHECK(sw->hw_voice_idx >= 0);
    const NVAPU_VOICE_CONTEXT_3D *pc = apu_voice_get_context((uint32_t)sw->hw_voice_idx);
    CHECK(pc->current_prd_index == 0 && pc->sample_pos_frac == 0);
    CHECK(apu_voice_is_active((uintptr_t)g_mmio, (uint32_t)sw->hw_voice_idx) == 1);

    /* Plain first play, stop and rewind paths are unchanged */
    alSourceStop(weak);
    CHECK(sw->state == AL_STOPPED);
    alSourcePlay(weak);
    CHECK(sw->state == AL_PLAYING);
    printf("    -> replay restarts at 0 on the same voice; standby replay clears saved position.\n");
}

static void test_paused_standby_resume(void) {
    printf("[5] A preempted source paused in standby resumes at its saved position...\n");
    reset_world();

    ALuint fill[64];
    for (int i = 0; i < 64; i++) {
        /* All at distance 10, except source 20 which is clearly the weakest */
        fill[i] = new_source(g_mono, 0.0f, 0.0f, (i == 20) ? -40.0f : -10.0f, 1);
        alSourcePlay(fill[i]);
    }
    CHECK(apu_voice_mgr_get_active_hw_count() == 64);
    ALsource *victim = al_source_get(fill[20]);
    CHECK(victim->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context((uint32_t)victim->hw_voice_idx);
    vctx->current_prd_index = 5;
    vctx->sample_pos_frac = 0x00120034u;

    /* A strong newcomer preempts the weakest voice, which keeps its position */
    ALuint strong = new_source(g_mono, 0.0f, 0.0f, -1.0f, 1);
    alSourcePlay(strong);
    CHECK(al_source_get(strong)->hw_voice_idx >= 0);
    CHECK(victim->state == AL_PLAYING && victim->hw_voice_idx == AL_HW_VOICE_INVALID);
    CHECK(victim->saved_prd_index == 5 && victim->saved_sample_pos_frac == 0x00120034u);

    /* Pausing the standby source must not drop the saved position */
    alSourcePause(fill[20]);
    CHECK(victim->state == AL_PAUSED && victim->hw_voice_idx == AL_HW_VOICE_INVALID);
    CHECK(victim->saved_prd_index == 5 && victim->saved_sample_pos_frac == 0x00120034u);

    /* Free a slot, then resume through the public API */
    alSourceStop(strong);
    alSourcePlay(fill[20]);
    CHECK(victim->state == AL_PLAYING && victim->hw_voice_idx >= 0);
    const NVAPU_VOICE_CONTEXT_3D *rc = apu_voice_get_context((uint32_t)victim->hw_voice_idx);
    CHECK(rc->current_prd_index == 5 && rc->sample_pos_frac == 0x00120034u);
    CHECK(victim->saved_prd_index == 0 && victim->saved_sample_pos_frac == 0);
    CHECK(apu_voice_is_active((uintptr_t)g_mmio, (uint32_t)victim->hw_voice_idx) == 1);
    printf("    -> resume from standby restores PRD index and phase.\n");
}

int main(void) {
    printf("=== OpenAL Source Regression Host Test ===\n");

    CHECK(apu_mem_init(0) == 0);
    g_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    g_voices = (NVAPU_VOICE_CONTEXT_3D *)calloc(NV_PAPU_NUM_3D_VOICES, sizeof(NVAPU_VOICE_CONTEXT_3D));
    CHECK(g_mmio != NULL && g_voices != NULL);
    CHECK(apu_voice_subsystem_init((uintptr_t)g_mmio, g_voices, 0x10000) == 0);

    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)g_mmio);
    CHECK(alGetError() == AL_NO_ERROR);

    g_mono = create_buffer(AL_FORMAT_MONO16, 44100);
    g_stereo = create_buffer(AL_FORMAT_STEREO16, 44100);

    test_single_computation_per_update();
    test_bit_identical_results();
    test_paused_sources_stay_current();
    test_replay_restarts();
    test_paused_standby_resume();

    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)g_mmio);
    apu_mem_shutdown();
    free(g_voices);
    free(g_mmio);

    printf("=== All Source Regression Tests Passed! ===\n");
    return 0;
}
