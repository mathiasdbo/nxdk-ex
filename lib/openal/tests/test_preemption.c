#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <AL/al.h>
#include "al_source.h"
#include "al_buffer.h"
#include "al_listener.h"
#include "apu_voice.h"
#include "apu_voice_mgr.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x10000

static ALuint create_test_buffer(ALenum format, ALsizei freq) {
    ALuint buf = 0;
    alGenBuffers(1, &buf);
    assert(buf != 0);
    int16_t dummy_data[256] = {0};
    alBufferData(buf, format, dummy_data, sizeof(dummy_data), freq);
    assert(alGetError() == AL_NO_ERROR);
    return buf;
}

/*
 * Resume a standby source through the voice manager (steal or free slot, then
 * restore the saved position). alSourcePlay() on a playing source restarts it
 * from the beginning, so tests that check position preservation drive this
 * resume path directly.
 */
static void resume_from_standby(ALsource *src) {
    int idx = apu_voice_mgr_allocate(src);
    if (idx >= 0) {
        al_source_program_hw_voice(src, (uint32_t)idx);
    }
}

int main(void) {
    printf("=== OpenAL Voice Preemption & Standby Resumption Host Test ===\n");

    /* Initialize physical memory allocator */
    int mem_init_res = apu_mem_init(0);
    assert(mem_init_res == 0);

    /* Allocate mock MMIO register space */
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);

    /* Allocate mock 3D Voice Context Array (64 voices * 128 bytes = 8192 bytes) */
    NVAPU_VOICE_CONTEXT_3D *voice_array = (NVAPU_VOICE_CONTEXT_3D *)calloc(
        NV_PAPU_NUM_3D_VOICES, sizeof(NVAPU_VOICE_CONTEXT_3D)
    );
    assert(voice_array != NULL);

    /* Initialize APU Voice subsystem with mock MMIO */
    int vp_init_res = apu_voice_subsystem_init((uintptr_t)mock_mmio, voice_array, 0x10000);
    assert(vp_init_res == 0);

    /* Initialize OpenAL subsystems */
    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)mock_mmio);

    assert(alGetError() == AL_NO_ERROR);

    /* ====================================================================== */
    /* Test 1: Preemption Protocol (mute-then-halt, no ramp)                  */
    /* ====================================================================== */
    printf("[1] Testing preemption protocol (mute-then-halt & halt confirmation)...\n");
    ALuint buf = create_test_buffer(AL_FORMAT_MONO16, 44100);
    ALuint sources[64] = {0};
    alGenSources(64, sources);
    assert(alGetError() == AL_NO_ERROR);

    /* Fill all 64 hardware voices: dist = 1.0 + 0.1 * i */
    for (int i = 0; i < 64; i++) {
        alSourcei(sources[i], AL_BUFFER, (ALint)buf);
        alSourcef(sources[i], AL_GAIN, 1.0f);
        alSource3f(sources[i], AL_POSITION, 0.0f, 0.0f, 1.0f + 0.1f * (float)i);
        alSourcei(sources[i], AL_LOOPING, AL_FALSE);
        alSourcePlay(sources[i]);
    }
    assert(apu_voice_mgr_get_active_hw_count() == 64);

    /*
     * The source with largest distance (sources[63], dist = 7.3) has the lowest priority.
     * It holds hardware voice slot 63.
     */
    ALsource *victim = al_source_get(sources[63]);
    assert(victim != NULL);
    int victim_slot = victim->hw_voice_idx;
    assert(victim_slot == 63);
    assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)victim_slot) == 1);

    NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context((uint32_t)victim_slot);
    assert(vctx != NULL);
    assert(vctx->master_vol_left > 0);
    assert(vctx->master_vol_right > 0);

    /* Simulate active playback progress on victim prior to preemption */
    vctx->current_prd_index = 5;
    vctx->sample_pos_frac = 0x12345678;

    /* Create higher-priority source (source 65, closer distance = 0.5) */
    ALuint src65_id = 0;
    alGenSources(1, &src65_id);
    assert(src65_id == 65);
    alSourcei(src65_id, AL_BUFFER, (ALint)buf);
    alSourcef(src65_id, AL_GAIN, 1.0f);
    alSource3f(src65_id, AL_POSITION, 0.0f, 0.0f, 0.5f);
    alSourcei(src65_id, AL_LOOPING, AL_FALSE);

    ALsource *src65 = al_source_get(src65_id);
    assert(src65 != NULL);

    /* Directly allocate to inspect state immediately upon preemption */
    int stolen_slot = apu_voice_mgr_allocate(src65);
    assert(stolen_slot == victim_slot);

    /* 1a. Validate victim progress preservation */
    assert(victim->saved_prd_index == 5);
    assert(victim->saved_sample_pos_frac == 0x12345678);

    /* 1b. Validate victim detached to virtual standby */
    assert(victim->hw_voice_idx == AL_HW_VOICE_INVALID);
    assert(victim->state == AL_PLAYING);

    /* 1c. Validate mute: master volumes and mixbins zeroed (abrupt cut, no ramp) */
    assert(vctx->master_vol_left == 0);
    assert(vctx->master_vol_right == 0);
    for (int m = 0; m < 16; m++) {
        assert(vctx->mixbin_gain[m] == 0);
    }

    /* 1d. Validate channel halt confirmation in register */
    assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)victim_slot) == 0);
    uint32_t active1 = apu_read32((uintptr_t)mock_mmio, NV_PAPU_VP_ACTIVE_1);
    assert((active1 & (1u << (victim_slot - 32))) == 0);

    /* Program source 65 into the stolen slot */
    al_source_program_hw_voice(src65, (uint32_t)stolen_slot);
    src65->state = AL_PLAYING;
    assert(src65->hw_voice_idx == victim_slot);
    assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)victim_slot) == 1);
    assert(vctx->master_vol_left > 0);
    assert(vctx->master_vol_right > 0);
    printf("    -> Preemption protocol verified (gains zeroed, halt confirmed, progress preserved).\n");

    /* ====================================================================== */
    /* Test 2: Preempted Standby Resumption with Preserved Sample Position    */
    /* ====================================================================== */
    printf("[2] Testing standby resumption (restoring saved PRD index and fractional phase)...\n");

    /* Victim source 64 is currently in virtual standby */
    assert(victim->hw_voice_idx == AL_HW_VOICE_INVALID);
    assert(victim->saved_prd_index == 5);
    assert(victim->saved_sample_pos_frac == 0x12345678);

    /* Stop source 1 (occupying hardware voice slot 0) to free a slot */
    ALsource *src1 = al_source_get(sources[0]);
    assert(src1 != NULL && src1->hw_voice_idx == 0);
    alSourceStop(sources[0]);
    assert(src1->state == AL_STOPPED);
    assert(src1->hw_voice_idx == AL_HW_VOICE_INVALID);
    assert(apu_voice_is_active((uintptr_t)mock_mmio, 0) == 0);

    /* Frame update tick: triggers reap and standby promotion */
    al_source_update_frame();

    /* Victim source 64 should now be promoted into slot 0 */
    assert(victim->hw_voice_idx == 0);
    assert(victim->state == AL_PLAYING);
    assert(apu_voice_mgr_get_owner(0) == (int)victim->id);

    /* Verify saved progress in victim struct was consumed and reset to 0 */
    assert(victim->saved_prd_index == 0);
    assert(victim->saved_sample_pos_frac == 0);

    /* Verify hardware voice context 0 restored the exact saved progress */
    NVAPU_VOICE_CONTEXT_3D *vctx0 = apu_voice_get_context(0);
    assert(vctx0 != NULL);
    assert(vctx0->current_prd_index == 5);
    assert(vctx0->sample_pos_frac == 0x12345678);

    /* Verify full pitch, master gains, and mixbins are restored */
    assert(vctx0->master_vol_left > 0);
    assert(vctx0->master_vol_right > 0);
    assert(vctx0->pitch_step > 0);
    assert(apu_voice_is_active((uintptr_t)mock_mmio, 0) == 1);
    printf("    -> Standby resumption verified (resumed at PRD 5, frac 0x12345678, full gains restored).\n");

    /* ====================================================================== */
    /* Test 3: Direct alSourcePlay on a Standby Source Restarts It            */
    /* ====================================================================== */
    printf("[3] Testing direct alSourcePlay() on a standby source (restart from beginning)...\n");
    {
        /* Simulate source 64 preempted again */
        vctx0->current_prd_index = 9;
        vctx0->sample_pos_frac = 0xABCDEF00;

        ALuint src66_id = 0;
        alGenSources(1, &src66_id);
        assert(src66_id == 66);
        alSourcei(src66_id, AL_BUFFER, (ALint)buf);
        alSourcef(src66_id, AL_GAIN, 1.0f);
        alSource3f(src66_id, AL_POSITION, 0.0f, 0.0f, 0.2f); /* Super close -> high priority */
        alSourcei(src66_id, AL_LOOPING, AL_FALSE);

        alSourcePlay(src66_id);
        /* Source 66 should steal slot 0 from victim 64 */
        assert(victim->hw_voice_idx == AL_HW_VOICE_INVALID);
        assert(victim->saved_prd_index == 9);
        assert(victim->saved_sample_pos_frac == 0xABCDEF00);

        /* Stop source 2 (occupying slot 1) to create an available slot */
        alSourceStop(sources[1]);
        assert(apu_voice_is_active((uintptr_t)mock_mmio, 1) == 0);

        /* OpenAL 1.1: Play on a playing (standby) source restarts it, so the
         * saved position is dropped (resumption is done by promotion, Test 2). */
        alSourcePlay(victim->id);

        /* Victim should acquire the free slot 1 and start from the beginning */
        assert(victim->hw_voice_idx == 1);
        assert(victim->saved_prd_index == 0);
        assert(victim->saved_sample_pos_frac == 0);

        NVAPU_VOICE_CONTEXT_3D *vctx1 = apu_voice_get_context(1);
        assert(vctx1 != NULL);
        assert(vctx1->current_prd_index == 0);
        assert(vctx1->sample_pos_frac == 0);
        assert(apu_voice_is_active((uintptr_t)mock_mmio, 1) == 1);
    }
    printf("    -> Direct alSourcePlay() on a standby source restarts from the beginning.\n");

    /* ====================================================================== */
    /* Test 4: Manual Stop and Rewind Resets Saved Preemption Progress        */
    /* ====================================================================== */
    printf("[4] Testing manual alSourceStop() and alSourceRewind() reset of saved offsets...\n");
    {
        /* 4a. Stop test */
        victim->saved_prd_index = 15;
        victim->saved_sample_pos_frac = 0x99990000;
        alSourceStop(victim->id);
        assert(victim->state == AL_STOPPED);
        assert(victim->saved_prd_index == 0);
        assert(victim->saved_sample_pos_frac == 0);

        /* Starting after stop must begin at offset 0 */
        alSourcePlay(victim->id);
        assert(victim->hw_voice_idx >= 0);
        NVAPU_VOICE_CONTEXT_3D *ctx_stopped = apu_voice_get_context((uint32_t)victim->hw_voice_idx);
        assert(ctx_stopped->current_prd_index == 0);
        assert(ctx_stopped->sample_pos_frac == 0);

        /* 4b. Rewind test */
        victim->saved_prd_index = 22;
        victim->saved_sample_pos_frac = 0x77770000;
        alSourceRewind(victim->id);
        assert(victim->state == AL_INITIAL);
        assert(victim->saved_prd_index == 0);
        assert(victim->saved_sample_pos_frac == 0);

        /* Starting after rewind must begin at offset 0 */
        alSourcePlay(victim->id);
        assert(victim->hw_voice_idx >= 0);
        NVAPU_VOICE_CONTEXT_3D *ctx_rewound = apu_voice_get_context((uint32_t)victim->hw_voice_idx);
        assert(ctx_rewound->current_prd_index == 0);
        assert(ctx_rewound->sample_pos_frac == 0);
    }
    printf("    -> Manual stop & rewind reset verified (offsets cleared to 0).\n");

    /* ====================================================================== */
    /* Test 5: Rapid Preemption Cycles Stress Test (190+ Preemptions)         */
    /* ====================================================================== */
    printf("[5] Testing rapid preemption stress test (190+ multi-voice preemption cycles)...\n");
    {
        /* Reset and clean up all sources to start fresh stress test */
        al_source_cleanup_subsystem();
        al_source_init_subsystem();
        al_source_set_apu_base((uintptr_t)mock_mmio);

        /* Allocate Batch A (64 sources, IDs 1..64) */
        ALuint batch_a[64] = {0};
        alGenSources(64, batch_a);
        for (int i = 0; i < 64; i++) {
            alSourcei(batch_a[i], AL_BUFFER, (ALint)buf);
            alSourcef(batch_a[i], AL_GAIN, 1.0f);
            alSource3f(batch_a[i], AL_POSITION, 0.0f, 0.0f, 5.0f + 0.01f * (float)i);
            alSourcei(batch_a[i], AL_LOOPING, AL_TRUE);
            alSourcePlay(batch_a[i]);
        }
        assert(apu_voice_mgr_get_active_hw_count() == 64);
        assert(apu_voice_mgr_get_virtual_standby_count() == 0);

        /* Allocate Batch B (64 sources, IDs 65..128) */
        ALuint batch_b[64] = {0};
        alGenSources(64, batch_b);
        for (int i = 0; i < 64; i++) {
            alSourcei(batch_b[i], AL_BUFFER, (ALint)buf);
            alSourcef(batch_b[i], AL_GAIN, 1.0f);
            alSource3f(batch_b[i], AL_POSITION, 0.0f, 0.0f, 1.0f + 0.001f * (float)i);
            alSourcei(batch_b[i], AL_LOOPING, AL_TRUE);
        }

        /* ------------------------------------------------------------------ */
        /* Round 1: Batch B preempts Batch A across all 64 hardware voices     */
        /* ------------------------------------------------------------------ */
        /* Record specific progress markers for Batch A voices */
        for (int v = 0; v < 64; v++) {
            NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)v);
            assert(ctx != NULL);
            ctx->current_prd_index = (uint32_t)(100 + v);
            ctx->sample_pos_frac = (uint32_t)(0x10000000 + v * 0x1000);
        }

        /* Play all 64 sources in Batch B -> triggers 64 preemptions */
        for (int i = 0; i < 64; i++) {
            alSourcePlay(batch_b[i]);
            ALsource *sb = al_source_get(batch_b[i]);
            assert(sb != NULL);
            assert(sb->hw_voice_idx >= 0 && sb->hw_voice_idx < 64);
        }
        assert(apu_voice_mgr_get_active_hw_count() == 64);
        assert(apu_voice_mgr_get_virtual_standby_count() == 64);

        /* Verify all 64 Batch A sources are in standby with exact preserved offsets */
        for (int i = 0; i < 64; i++) {
            ALsource *sa = al_source_get(batch_a[i]);
            assert(sa != NULL);
            assert(sa->hw_voice_idx == AL_HW_VOICE_INVALID);
            assert(sa->state == AL_PLAYING);
            assert(sa->saved_prd_index >= 100 && sa->saved_prd_index < 164);
            assert((sa->saved_sample_pos_frac & 0xF0000000) == 0x10000000);
        }

        /* ------------------------------------------------------------------ */
        /* Round 2: Batch A moves closer and preempts Batch B back (64 preemptions) */
        /* ------------------------------------------------------------------ */
        /* Record specific progress markers for Batch B voices */
        for (int v = 0; v < 64; v++) {
            NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)v);
            assert(ctx != NULL);
            ctx->current_prd_index = (uint32_t)(200 + v);
            ctx->sample_pos_frac = (uint32_t)(0x20000000 + v * 0x1000);
        }

        /* Move Batch A to distance 0.2 (P=15.0 > Batch B P=3.0) */
        for (int i = 0; i < 64; i++) {
            alSource3f(batch_a[i], AL_POSITION, 0.0f, 0.0f, 0.2f + 0.0005f * (float)i);
        }

        /* Resume all Batch A sources -> preempts all Batch B voices back */
        for (int i = 0; i < 64; i++) {
            ALsource *sa = al_source_get(batch_a[i]);
            assert(sa != NULL);
            resume_from_standby(sa);
            assert(sa->hw_voice_idx >= 0 && sa->hw_voice_idx < 64);
            /* Saved offsets consumed and restored into HW */
            assert(sa->saved_prd_index == 0);
            assert(sa->saved_sample_pos_frac == 0);
            NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)sa->hw_voice_idx);
            assert(ctx->current_prd_index >= 100 && ctx->current_prd_index < 164);
            assert((ctx->sample_pos_frac & 0xF0000000) == 0x10000000);
        }
        assert(apu_voice_mgr_get_active_hw_count() == 64);
        assert(apu_voice_mgr_get_virtual_standby_count() == 64);

        /* Verify all 64 Batch B sources are now in standby with preserved offsets */
        for (int i = 0; i < 64; i++) {
            ALsource *sb = al_source_get(batch_b[i]);
            assert(sb != NULL);
            assert(sb->hw_voice_idx == AL_HW_VOICE_INVALID);
            assert(sb->state == AL_PLAYING);
            assert(sb->saved_prd_index >= 200 && sb->saved_prd_index < 264);
            assert((sb->saved_sample_pos_frac & 0xF0000000) == 0x20000000);
        }

        /* ------------------------------------------------------------------ */
        /* Round 3: Batch B moves to distance 0.1 and preempts Batch A (64 preemptions) */
        /* ------------------------------------------------------------------ */
        for (int v = 0; v < 64; v++) {
            NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)v);
            assert(ctx != NULL);
            ctx->current_prd_index = (uint32_t)(300 + v);
            ctx->sample_pos_frac = (uint32_t)(0x30000000 + v * 0x1000);
        }

        /* Move Batch B to distance 0.1 (P=30.0 > Batch A P=15.0) */
        for (int i = 0; i < 64; i++) {
            alSource3f(batch_b[i], AL_POSITION, 0.0f, 0.0f, 0.1f + 0.0001f * (float)i);
        }

        for (int i = 0; i < 64; i++) {
            ALsource *sb = al_source_get(batch_b[i]);
            assert(sb != NULL);
            resume_from_standby(sb);
            assert(sb->hw_voice_idx >= 0 && sb->hw_voice_idx < 64);
            assert(sb->saved_prd_index == 0);
            assert(sb->saved_sample_pos_frac == 0);
            NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)sb->hw_voice_idx);
            assert(ctx->current_prd_index >= 200 && ctx->current_prd_index < 264);
            assert((ctx->sample_pos_frac & 0xF0000000) == 0x20000000);
        }
        assert(apu_voice_mgr_get_active_hw_count() == 64);
        assert(apu_voice_mgr_get_virtual_standby_count() == 64);

        /* ------------------------------------------------------------------ */
        /* Round 4: Automatic Standby Promotion (64 standby resumptions)      */
        /* ------------------------------------------------------------------ */
        /* Stop all Batch B sources one by one and verify automatic promotion of Batch A */
        for (int i = 0; i < 64; i++) {
            alSourceStop(batch_b[i]);
            al_source_update_frame();
        }
        assert(apu_voice_mgr_get_active_hw_count() == 64);
        assert(apu_voice_mgr_get_virtual_standby_count() == 0);

        /* All Batch A sources must be active and resumed with preserved progress */
        for (int i = 0; i < 64; i++) {
            ALsource *sa = al_source_get(batch_a[i]);
            assert(sa != NULL);
            assert(sa->hw_voice_idx >= 0 && sa->hw_voice_idx < 64);
            assert(sa->saved_prd_index == 0);
            assert(sa->saved_sample_pos_frac == 0);
            NVAPU_VOICE_CONTEXT_3D *ctx = apu_voice_get_context((uint32_t)sa->hw_voice_idx);
            assert(ctx->current_prd_index >= 300 && ctx->current_prd_index < 364);
            assert((ctx->sample_pos_frac & 0xF0000000) == 0x30000000);
            assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)sa->hw_voice_idx) == 1);
        }
    }
    printf("    -> 192 rapid hardware preemptions + 64 standby promotions verified with 0 errors.\n");

    /* ====================================================================== */
    /* Cleanup                                                                */
    /* ====================================================================== */
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    free(voice_array);
    free(mock_mmio);
    apu_mem_shutdown();

    printf("=== All Preemption & Standby Resumption Tests Passed! ===\n");
    return 0;
}
