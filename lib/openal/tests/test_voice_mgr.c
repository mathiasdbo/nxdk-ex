#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
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

int main(void) {
    printf("=== OpenAL Voice Virtualization & Priority Manager Host Test ===\n");

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
    /* Test 1: Dynamic Priority Calculation Formula                           */
    /* ====================================================================== */
    printf("[1] Testing dynamic priority calculation formula...\n");
    {
        ALuint src_id = 0;
        alGenSources(1, &src_id);
        assert(src_id == 1);
        ALsource *src = al_source_get(src_id);
        assert(src != NULL);

        /* Default listener is at origin (0,0,0) with gain 1.0 */
        alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
        alListenerf(AL_GAIN, 1.0f);

        /* 1a. One-shot source at distance 1.0, gain 1.0 */
        alSource3f(src_id, AL_POSITION, 0.0f, 0.0f, 1.0f);
        alSourcef(src_id, AL_GAIN, 1.0f);
        alSourcei(src_id, AL_LOOPING, AL_FALSE);

        float p_oneshot = apu_voice_mgr_calc_priority(src);
        /* Expected: base_priority = 1.0, effective_gain = 1.0, dist = 1.0 -> P = 1.0 */
        assert(fabsf(p_oneshot - 1.0f) < 0.02f);

        /* 1b. Looping boost (+2.0 boost, base 3.0 vs 1.0) */
        alSourcei(src_id, AL_LOOPING, AL_TRUE);
        float p_looping = apu_voice_mgr_calc_priority(src);
        /* Expected: base_priority = 3.0, effective_gain = 1.0, dist = 1.0 -> P = 3.0 */
        assert(fabsf(p_looping - 3.0f) < 0.02f);
        assert(fabsf(p_looping - (p_oneshot + 2.0f)) < 0.05f);

        /* 1c. Distance rolloff: priority decreases with distance */
        alSourcei(src_id, AL_LOOPING, AL_FALSE);
        alSource3f(src_id, AL_POSITION, 0.0f, 0.0f, 2.0f);
        float p_dist2 = apu_voice_mgr_calc_priority(src);
        assert(p_dist2 < p_oneshot);

        alSource3f(src_id, AL_POSITION, 0.0f, 0.0f, 5.0f);
        float p_dist5 = apu_voice_mgr_calc_priority(src);
        assert(p_dist5 < p_dist2);

        /* 1d. Min distance clamping (distance < 0.1 clamped to 0.1) */
        alSource3f(src_id, AL_POSITION, 0.0f, 0.0f, 0.05f);
        float p_clamped = apu_voice_mgr_calc_priority(src);
        /* Dist 0.05 < 0.1 -> clamped to 0.1. Effective gain 1.0 -> P = 1.0 * (1.0 / 0.1) = 10.0 */
        assert(fabsf(p_clamped - 10.0f) < 0.1f);

        /* 1e. Zero-gain threshold: gain < 0.001f strictly results in priority 0.0f */
        alSourcef(src_id, AL_GAIN, 0.0005f);
        assert(apu_voice_mgr_calc_priority(src) == 0.0f);

        alSourcef(src_id, AL_GAIN, 0.0f);
        assert(apu_voice_mgr_calc_priority(src) == 0.0f);

        /* Effective gain < 0.001f via listener gain */
        alSourcef(src_id, AL_GAIN, 1.0f);
        alListenerf(AL_GAIN, 0.0f);
        assert(apu_voice_mgr_calc_priority(src) == 0.0f);

        /* Restore listener gain and delete test source */
        alListenerf(AL_GAIN, 1.0f);
        alDeleteSources(1, &src_id);
        assert(alGetError() == AL_NO_ERROR);
    }
    printf("    -> Priority calculation verified (looping boost, distance rolloff, zero threshold).\n");

    /* ====================================================================== */
    /* Test 2: Filling All 64 Hardware Voice Slots with 64 Sources            */
    /* ====================================================================== */
    printf("[2] Testing filling all 64 hardware slots with 64 sources...\n");
    ALuint buf = create_test_buffer(AL_FORMAT_MONO16, 44100);
    ALuint sources[64] = {0};
    alGenSources(64, sources);
    assert(alGetError() == AL_NO_ERROR);

    for (int i = 0; i < 64; i++) {
        alSourcei(sources[i], AL_BUFFER, (ALint)buf);
        alSourcef(sources[i], AL_GAIN, 1.0f);
        /* Position sources at distinct distances: dist = 1.0 + 0.1 * i */
        alSource3f(sources[i], AL_POSITION, 0.0f, 0.0f, 1.0f + 0.1f * (float)i);
        alSourcei(sources[i], AL_LOOPING, AL_FALSE);
        alSourcePlay(sources[i]);
    }

    /* Verify all 64 sources obtained distinct hardware voice slots */
    bool slot_used[NV_PAPU_NUM_3D_VOICES] = {false};
    for (int i = 0; i < 64; i++) {
        ALsource *s = al_source_get(sources[i]);
        assert(s != NULL);
        assert(s->state == AL_PLAYING);
        assert(s->hw_voice_idx >= 0 && s->hw_voice_idx < 64);
        assert(!slot_used[s->hw_voice_idx]);
        slot_used[s->hw_voice_idx] = true;
        assert(apu_voice_mgr_get_owner((uint32_t)s->hw_voice_idx) == (int)s->id);
        assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)s->hw_voice_idx) == 1);
    }

    assert(apu_voice_mgr_get_active_hw_count() == 64);
    assert(apu_voice_mgr_get_virtual_standby_count() == 0);

    /* Verify all bits set in hardware active bitmasks */
    uint32_t act0 = apu_read32((uintptr_t)mock_mmio, NV_PAPU_VP_ACTIVE_0);
    uint32_t act1 = apu_read32((uintptr_t)mock_mmio, NV_PAPU_VP_ACTIVE_1);
    assert(act0 == 0xFFFFFFFFu);
    assert(act1 == 0xFFFFFFFFu);
    printf("    -> All 64 HW voice channels successfully assigned and active.\n");

    /* ====================================================================== */
    /* Test 3: Adding 65th Lower-Priority Source Enters Virtual Standby       */
    /* ====================================================================== */
    printf("[3] Testing adding lower-priority source enters virtual standby...\n");
    ALuint src65 = 0;
    alGenSources(1, &src65);
    assert(src65 == 65);
    alSourcei(src65, AL_BUFFER, (ALint)buf);
    /* Set very low gain and large distance -> low priority */
    alSourcef(src65, AL_GAIN, 0.05f);
    alSource3f(src65, AL_POSITION, 0.0f, 0.0f, 50.0f);
    alSourcei(src65, AL_LOOPING, AL_FALSE);

    alSourcePlay(src65);

    ALsource *s65 = al_source_get(src65);
    assert(s65 != NULL);
    assert(s65->state == AL_PLAYING);
    assert(s65->hw_voice_idx == AL_HW_VOICE_INVALID);

    /* Active HW count is still 64, virtual standby count is 1 */
    assert(apu_voice_mgr_get_active_hw_count() == 64);
    assert(apu_voice_mgr_get_virtual_standby_count() == 1);
    printf("    -> Source 65 correctly entered virtual standby (AL_HW_VOICE_INVALID).\n");

    /* ====================================================================== */
    /* Test 4: Adding 66th Higher-Priority Source Steals Lowest-Priority Slot  */
    /* ====================================================================== */
    printf("[4] Testing higher-priority source steals lowest-priority voice...\n");
    /* Identify the victim among the 64 currently active sources */
    int victim_voice = -1;
    ALuint victim_id = 0;
    float min_p = 1e30f;
    for (int i = 0; i < 64; i++) {
        ALsource *s = al_source_get(sources[i]);
        if (s && s->hw_voice_idx >= 0) {
            float p = apu_voice_mgr_calc_priority(s);
            if (p < min_p) {
                min_p = p;
                victim_voice = s->hw_voice_idx;
                victim_id = s->id;
            }
        }
    }
    assert(victim_voice >= 0);
    assert(victim_id != 0);

    ALuint src66 = 0;
    alGenSources(1, &src66);
    assert(src66 == 66);
    alSourcei(src66, AL_BUFFER, (ALint)buf);
    /* Very high priority: close distance, full gain, looping boost */
    alSourcef(src66, AL_GAIN, 1.0f);
    alSource3f(src66, AL_POSITION, 0.0f, 0.0f, 0.1f);
    alSourcei(src66, AL_LOOPING, AL_TRUE);

    alSourcePlay(src66);

    ALsource *s66 = al_source_get(src66);
    assert(s66 != NULL);
    assert(s66->state == AL_PLAYING);
    /* Must have stolen victim_voice */
    assert(s66->hw_voice_idx == victim_voice);
    assert(apu_voice_mgr_get_owner((uint32_t)victim_voice) == (int)s66->id);

    /* Victim must be detached to standby but remain AL_PLAYING */
    ALsource *victim_src = al_source_get(victim_id);
    assert(victim_src != NULL);
    assert(victim_src->state == AL_PLAYING);
    assert(victim_src->hw_voice_idx == AL_HW_VOICE_INVALID);

    /* Active HW count is 64, standby count is 2 (src65 + victim) */
    assert(apu_voice_mgr_get_active_hw_count() == 64);
    assert(apu_voice_mgr_get_virtual_standby_count() == 2);
    assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)victim_voice) == 1);
    printf("    -> Preemption success: voice slot %d stolen by source 66 from victim source %u.\n",
           victim_voice, victim_id);

    /* ====================================================================== */
    /* Test 5: Completed One-Shot Hardware Voice Auto-Reap and Promotion      */
    /* ====================================================================== */
    printf("[5] Testing completed one-shot auto-reap and automatic standby promotion...\n");
    /* Find an active one-shot source that is not s66 */
    int reap_slot = -1;
    ALuint reap_id = 0;
    for (int i = 0; i < 64; i++) {
        ALsource *s = al_source_get(sources[i]);
        if (s && s->hw_voice_idx >= 0 && !s->looping && s->hw_voice_idx != victim_voice) {
            reap_slot = s->hw_voice_idx;
            reap_id = s->id;
            break;
        }
    }
    assert(reap_slot >= 0);

    /* Simulate hardware completion of one-shot playback (clear active bit in MMIO) */
    apu_voice_stop((uintptr_t)mock_mmio, (uint32_t)reap_slot);
    assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)reap_slot) == 0);

    /* Standby sources waiting: victim_src and s65.
     * Between the two, victim_src has higher priority than s65. */
    float p_victim = apu_voice_mgr_calc_priority(victim_src);
    float p_s65 = apu_voice_mgr_calc_priority(s65);
    assert(p_victim > p_s65);

    /* Execute frame update */
    al_source_update_frame();

    /* Verify original source in reap_slot transitioned to AL_STOPPED */
    ALsource *reaped_src = al_source_get(reap_id);
    assert(reaped_src != NULL);
    assert(reaped_src->state == AL_STOPPED);
    assert(reaped_src->hw_voice_idx == AL_HW_VOICE_INVALID);

    /* Verify highest-priority standby source (victim_src) was automatically promoted into reap_slot */
    assert(victim_src->state == AL_PLAYING);
    assert(victim_src->hw_voice_idx == reap_slot);
    assert(apu_voice_mgr_get_owner((uint32_t)reap_slot) == (int)victim_src->id);
    assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)reap_slot) == 1);

    /* Active HW count is 64, standby count drops from 2 to 1 (only s65 left) */
    assert(apu_voice_mgr_get_active_hw_count() == 64);
    assert(apu_voice_mgr_get_virtual_standby_count() == 1);
    printf("    -> Auto-reap and automatic promotion verified: victim source %u promoted to HW slot %d.\n",
           victim_id, reap_slot);

    /* Clean up 66 sources */
    alDeleteSources(64, sources);
    alDeleteSources(1, &src65);
    alDeleteSources(1, &src66);
    assert(apu_voice_mgr_get_active_hw_count() == 0);
    assert(apu_voice_mgr_get_virtual_standby_count() == 0);

    /* ====================================================================== */
    /* Test 6: 256 Virtual Sources Allocation Limits and Tracking             */
    /* ====================================================================== */
    printf("[6] Testing 256 virtual sources allocation limits and tracking...\n");
    ALuint all_sources[AL_MAX_SOURCES] = {0};
    alGenSources(AL_MAX_SOURCES, all_sources);
    assert(alGetError() == AL_NO_ERROR);

    for (int i = 0; i < AL_MAX_SOURCES; i++) {
        assert(alIsSource(all_sources[i]) == AL_TRUE);
    }

    /* Exhaustion: 257th source generation must fail with AL_OUT_OF_MEMORY */
    ALuint overflow_src = 0;
    alGenSources(1, &overflow_src);
    assert(alGetError() == AL_OUT_OF_MEMORY);

    /* Play all 256 sources */
    for (int i = 0; i < AL_MAX_SOURCES; i++) {
        alSourcei(all_sources[i], AL_BUFFER, (ALint)buf);
        alSourcef(all_sources[i], AL_GAIN, 1.0f);
        alSource3f(all_sources[i], AL_POSITION, 0.0f, 0.0f, 1.0f + 0.05f * (float)i);
        alSourcei(all_sources[i], AL_LOOPING, AL_FALSE);
        alSourcePlay(all_sources[i]);
    }

    /* 64 sources must be playing in hardware, 192 sources playing in virtual standby */
    assert(apu_voice_mgr_get_active_hw_count() == 64);
    assert(apu_voice_mgr_get_virtual_standby_count() == (AL_MAX_SOURCES - 64));

    for (int i = 0; i < 64; i++) {
        ALsource *s = al_source_get(all_sources[i]);
        assert(s->state == AL_PLAYING);
        assert(s->hw_voice_idx >= 0 && s->hw_voice_idx < 64);
    }
    for (int i = 64; i < AL_MAX_SOURCES; i++) {
        ALsource *s = al_source_get(all_sources[i]);
        assert(s->state == AL_PLAYING);
        assert(s->hw_voice_idx == AL_HW_VOICE_INVALID);
    }

    /* Stop 10 active hardware sources */
    for (int i = 0; i < 10; i++) {
        alSourceStop(all_sources[i]);
    }
    assert(apu_voice_mgr_get_active_hw_count() == 54);

    /* Run frame update tick: 10 highest-priority standby sources should be promoted */
    al_source_update_frame();
    assert(apu_voice_mgr_get_active_hw_count() == 64);
    assert(apu_voice_mgr_get_virtual_standby_count() == (AL_MAX_SOURCES - 64 - 10));

    /* Verify top promoted sources (64..73) now hold valid hardware voices */
    for (int i = 64; i < 74; i++) {
        ALsource *s = al_source_get(all_sources[i]);
        assert(s->state == AL_PLAYING);
        assert(s->hw_voice_idx >= 0 && s->hw_voice_idx < 64);
    }

    /* Clean up all 256 sources and buffer */
    alDeleteSources(AL_MAX_SOURCES, all_sources);
    assert(alGetError() == AL_NO_ERROR);
    assert(apu_voice_mgr_get_active_hw_count() == 0);
    assert(apu_voice_mgr_get_virtual_standby_count() == 0);

    alDeleteBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);

    printf("    -> 256 virtual sources tracking, pool limits, and bulk promotion verified.\n");

    /* Subsystem deinitialization */
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    apu_mem_shutdown();
    free(voice_array);
    free(mock_mmio);

    printf("=== All Voice Virtualization & Priority Manager Tests Passed! ===\n");
    return 0;
}
