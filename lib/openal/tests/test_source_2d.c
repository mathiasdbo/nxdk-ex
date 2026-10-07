#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include "al_source.h"
#include "al_buffer.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x10000

int main(void) {
    printf("=== OpenAL Source 2D & APU HW Synchronization Host Test ===\n");

    /* 1. Initialize subsystems with mock MMIO and voice array */
    int mem_init_res = apu_mem_init(0);
    assert(mem_init_res == 0);

    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);

    NVAPU_VOICE_CONTEXT_3D *voice_array = (NVAPU_VOICE_CONTEXT_3D *)calloc(
        NV_PAPU_NUM_3D_VOICES, sizeof(NVAPU_VOICE_CONTEXT_3D)
    );
    assert(voice_array != NULL);

    int vp_init_res = apu_voice_subsystem_init((uintptr_t)mock_mmio, voice_array, 0x10000);
    assert(vp_init_res == 0);

    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_source_set_apu_base((uintptr_t)mock_mmio);

    assert(alGetError() == AL_NO_ERROR);

    /* Test 1: Source Generation, Deletion, and alIsSource */
    printf("[1] Testing alGenSources, alDeleteSources, and alIsSource...\n");
    ALuint sources[4] = {0};
    alGenSources(4, sources);
    assert(alGetError() == AL_NO_ERROR);
    assert(sources[0] == 1);
    assert(sources[1] == 2);
    assert(sources[2] == 3);
    assert(sources[3] == 4);

    for (int i = 0; i < 4; i++) {
        assert(alIsSource(sources[i]) == AL_TRUE);
    }
    assert(alIsSource(0) == AL_FALSE);
    assert(alIsSource(999) == AL_FALSE);

    /* Invalid parameters */
    alGenSources(-1, sources);
    assert(alGetError() == AL_INVALID_VALUE);

    alGenSources(1, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    alDeleteSources(-1, sources);
    assert(alGetError() == AL_INVALID_VALUE);

    alDeleteSources(1, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    ALuint invalid_src = 200;
    alDeleteSources(1, &invalid_src);
    assert(alGetError() == AL_INVALID_NAME);

    ALuint zero_src = 0;
    alDeleteSources(1, &zero_src);
    assert(alGetError() == AL_NO_ERROR);

    /* Test 2: Buffer Binding with Ref-Counting */
    printf("[2] Testing buffer attachment and reference counting...\n");
    ALuint bufs[2] = {0};
    alGenBuffers(2, bufs);
    assert(alGetError() == AL_NO_ERROR);

    int16_t mono_data[512] = {0};
    alBufferData(bufs[0], AL_FORMAT_MONO16, mono_data, sizeof(mono_data), 44100);
    assert(alGetError() == AL_NO_ERROR);

    int16_t stereo_data[1024] = {0};
    alBufferData(bufs[1], AL_FORMAT_STEREO16, stereo_data, sizeof(stereo_data), 48000);
    assert(alGetError() == AL_NO_ERROR);

    ALbuffer *b0 = al_buffer_get(bufs[0]);
    ALbuffer *b1 = al_buffer_get(bufs[1]);
    assert(b0 != NULL && b1 != NULL);
    assert(b0->ref_count == 0);
    assert(b1->ref_count == 0);

    /* Attach buffer 0 to source 0 */
    alSourcei(sources[0], AL_BUFFER, (ALint)bufs[0]);
    assert(alGetError() == AL_NO_ERROR);
    assert(b0->ref_count == 1);

    ALint attached_buf = 0;
    alGetSourcei(sources[0], AL_BUFFER, &attached_buf);
    assert(attached_buf == (ALint)bufs[0]);

    /* Cannot delete buffer while attached */
    alDeleteBuffers(1, &bufs[0]);
    assert(alGetError() == AL_INVALID_OPERATION);
    assert(alIsBuffer(bufs[0]) == AL_TRUE);

    /* Attach buffer 1 to source 1 */
    alSourcei(sources[1], AL_BUFFER, (ALint)bufs[1]);
    assert(alGetError() == AL_NO_ERROR);
    assert(b1->ref_count == 1);

    /* Replace source 0's buffer with buffer 1 */
    alSourcei(sources[0], AL_BUFFER, (ALint)bufs[1]);
    assert(alGetError() == AL_NO_ERROR);
    assert(b0->ref_count == 0);
    assert(b1->ref_count == 2);

    /* Detach buffer from source 0 (setting AL_BUFFER to 0) */
    alSourcei(sources[0], AL_BUFFER, 0);
    assert(alGetError() == AL_NO_ERROR);
    assert(b1->ref_count == 1);

    /* Re-attach buffer 0 to source 0 */
    alSourcei(sources[0], AL_BUFFER, (ALint)bufs[0]);
    assert(alGetError() == AL_NO_ERROR);
    assert(b0->ref_count == 1);

    /* Test 3: Source Property Get / Set */
    printf("[3] Testing source property getters and setters...\n");
    alSourcef(sources[0], AL_PITCH, 1.25f);
    assert(alGetError() == AL_NO_ERROR);

    ALfloat pitch = 0.0f;
    alGetSourcef(sources[0], AL_PITCH, &pitch);
    assert(pitch == 1.25f);

    /* Invalid pitch */
    alSourcef(sources[0], AL_PITCH, 0.0f);
    assert(alGetError() == AL_INVALID_VALUE);
    alSourcef(sources[0], AL_PITCH, -1.0f);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Gain & Min/Max Gain */
    alSourcef(sources[0], AL_MIN_GAIN, 0.2f);
    alSourcef(sources[0], AL_MAX_GAIN, 0.8f);
    alSourcef(sources[0], AL_GAIN, 0.5f);
    assert(alGetError() == AL_NO_ERROR);

    ALfloat gain = 0.0f, min_g = 0.0f, max_g = 0.0f;
    alGetSourcef(sources[0], AL_GAIN, &gain);
    alGetSourcef(sources[0], AL_MIN_GAIN, &min_g);
    alGetSourcef(sources[0], AL_MAX_GAIN, &max_g);
    assert(gain == 0.5f);
    assert(min_g == 0.2f);
    assert(max_g == 0.8f);

    /* Looping */
    alSourcei(sources[0], AL_LOOPING, AL_TRUE);
    assert(alGetError() == AL_NO_ERROR);
    ALint looping = 0;
    alGetSourcei(sources[0], AL_LOOPING, &looping);
    assert(looping == AL_TRUE);

    /* Position & 3D Vectors */
    alSource3f(sources[0], AL_POSITION, 1.0f, 2.0f, 3.0f);
    assert(alGetError() == AL_NO_ERROR);
    ALfloat pos[3] = {0};
    alGetSource3f(sources[0], AL_POSITION, &pos[0], &pos[1], &pos[2]);
    assert(pos[0] == 1.0f && pos[1] == 2.0f && pos[2] == 3.0f);

    /* Reset pitch, gain, position, and looping to standard values */
    alSourcef(sources[0], AL_PITCH, 1.0f);
    alSourcef(sources[0], AL_MIN_GAIN, 0.0f);
    alSourcef(sources[0], AL_MAX_GAIN, 1.0f);
    alSourcef(sources[0], AL_GAIN, 1.0f);
    alSource3f(sources[0], AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcei(sources[0], AL_LOOPING, AL_FALSE);

    /* Test 4: Playback State Transitions & APU Voice Synchronization */
    printf("[4] Testing alSourcePlay, Voice Context setup, and MMIO activation...\n");
    ALint state = 0;
    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_INITIAL);

    alSourcePlay(sources[0]);
    assert(alGetError() == AL_NO_ERROR);

    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);

    ALsource *src0 = al_source_get(sources[0]);
    assert(src0 != NULL);
    assert(src0->hw_voice_idx >= 0 && src0->hw_voice_idx < (int)NV_PAPU_NUM_3D_VOICES);

    uint32_t voice_idx = (uint32_t)src0->hw_voice_idx;

    /* Verify APU MMIO register write: NV_PAPU_VP_ACTIVE_0 bit must be set */
    uint32_t active_reg = *(volatile uint32_t *)(mock_mmio + NV_PAPU_VP_ACTIVE_0);
    assert((active_reg & (1u << voice_idx)) != 0);

    /* Verify Voice Processor context configuration in voice array */
    NVAPU_VOICE_CONTEXT_3D *vctx = &voice_array[voice_idx];
    assert(vctx->format == NVAPU_VOICE_FORMAT_PCM16);
    assert(vctx->channels == NVAPU_VOICE_CHANNELS_MONO);
    assert(vctx->mode_3d == 1 || vctx->mode_3d == 0); /* 3D mono or 2D direct playback */
    assert(vctx->loop_mode == NVAPU_VOICE_LOOP_OFF);
    assert(vctx->prd_table_phys == b0->prd_table_phys);
    assert(vctx->master_vol_left == 0xFFFF);
    assert(vctx->master_vol_right == 0xFFFF);
    assert(vctx->mixbin_routing_mask == 0x00000003 || vctx->mixbin_routing_mask == 0x0000003F);
    if (vctx->mixbin_routing_mask == 0x00000003) {
        assert(vctx->mixbin_gain[0] == 0xFF);
        assert(vctx->mixbin_gain[1] == 0xFF);
    }

    /* While playing, attaching a buffer must fail with AL_INVALID_OPERATION */
    alSourcei(sources[0], AL_BUFFER, (ALint)bufs[1]);
    assert(alGetError() == AL_INVALID_OPERATION);

    /* Test 5: Dynamic Pitch / Gain / Looping updates during active playback */
    printf("[5] Testing real-time pitch, gain, and loop updates during playback...\n");
    alSourcef(sources[0], AL_PITCH, 2.0f);
    assert(alGetError() == AL_NO_ERROR);
    uint32_t expected_pitch_step = APU_CALC_PITCH_STEP(44100, 2.0f);
    assert(vctx->pitch_step == expected_pitch_step);

    alSourcef(sources[0], AL_GAIN, 0.5f);
    assert(alGetError() == AL_NO_ERROR);
    assert(vctx->master_vol_left == (uint16_t)(0.5f * 65535.0f + 0.5f));

    alSourcei(sources[0], AL_LOOPING, AL_TRUE);
    assert(alGetError() == AL_NO_ERROR);
    assert(vctx->loop_mode == NVAPU_VOICE_LOOP_ON);

    /* Test 6: Pause and Resume State Transitions */
    printf("[6] Testing alSourcePause and resume via alSourcePlay...\n");
    alSourcePause(sources[0]);
    assert(alGetError() == AL_NO_ERROR);

    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_PAUSED);

    /* Verify pause bit in hardware register */
    uint32_t pause_reg = *(volatile uint32_t *)(mock_mmio + NV_PAPU_VP_PAUSE_0);
    assert((pause_reg & (1u << voice_idx)) != 0);

    /* Resume playback */
    alSourcePlay(sources[0]);
    assert(alGetError() == AL_NO_ERROR);

    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);

    /* Verify pause bit cleared in hardware register */
    pause_reg = *(volatile uint32_t *)(mock_mmio + NV_PAPU_VP_PAUSE_0);
    assert((pause_reg & (1u << voice_idx)) == 0);

    /* Test 7: Stop and Rewind */
    printf("[7] Testing alSourceStop and alSourceRewind...\n");
    alSourceStop(sources[0]);
    assert(alGetError() == AL_NO_ERROR);

    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_STOPPED);

    /* Active bit must be cleared in hardware */
    active_reg = *(volatile uint32_t *)(mock_mmio + NV_PAPU_VP_ACTIVE_0);
    assert((active_reg & (1u << voice_idx)) == 0);
    assert(src0->hw_voice_idx == AL_HW_VOICE_INVALID);

    /* Rewind from AL_STOPPED promotes to AL_INITIAL */
    alSourceRewind(sources[0]);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_INITIAL);

    /* Test 8: Hardware Completion Polling Detection */
    printf("[8] Testing automatic one-shot completion detection...\n");
    alSourcei(sources[0], AL_LOOPING, AL_FALSE);
    alSourcePlay(sources[0]);
    assert(alGetError() == AL_NO_ERROR);
    assert(src0->state == AL_PLAYING);
    assert(src0->hw_voice_idx >= 0);
    voice_idx = (uint32_t)src0->hw_voice_idx;

    /* Simulate APU clearing active bit upon reaching EOT */
    *(volatile uint32_t *)(mock_mmio + NV_PAPU_VP_ACTIVE_0) &= ~(1u << voice_idx);

    /* Query state: must automatically detect completion and transition to AL_STOPPED */
    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_STOPPED);
    assert(src0->hw_voice_idx == AL_HW_VOICE_INVALID);

    /* Test 9: Play with No Buffer */
    printf("[9] Testing alSourcePlay on source with no buffer attached...\n");
    alSourcei(sources[2], AL_BUFFER, 0);
    alSourcePlay(sources[2]);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(sources[2], AL_SOURCE_STATE, &state);
    assert(state == AL_STOPPED);

    /* Test 10: Vector Batch Playback APIs */
    printf("[10] Testing vector playback control functions (alSourcePlayv, etc.)...\n");
    alSourcei(sources[0], AL_BUFFER, (ALint)bufs[0]);
    alSourcei(sources[1], AL_BUFFER, (ALint)bufs[1]);

    ALuint batch[2] = {sources[0], sources[1]};
    alSourcePlayv(2, batch);
    assert(alGetError() == AL_NO_ERROR);

    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);
    alGetSourcei(sources[1], AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);

    alSourcePausev(2, batch);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_PAUSED);
    alGetSourcei(sources[1], AL_SOURCE_STATE, &state);
    assert(state == AL_PAUSED);

    alSourceStopv(2, batch);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_STOPPED);
    alGetSourcei(sources[1], AL_SOURCE_STATE, &state);
    assert(state == AL_STOPPED);

    alSourceRewindv(2, batch);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    assert(state == AL_INITIAL);
    alGetSourcei(sources[1], AL_SOURCE_STATE, &state);
    assert(state == AL_INITIAL);

    /* Test 11: Priority Stealing & Click Prevention Preemption */
    printf("[11] Testing 64-voice priority preemption and click prevention...\n");
    ALuint test_sources[65];
    alGenSources(65, test_sources);
    assert(alGetError() == AL_NO_ERROR);

    /* Attach buffer to all 65 sources */
    for (int i = 0; i < 65; i++) {
        alSourcei(test_sources[i], AL_BUFFER, (ALint)bufs[0]);
    }

    /* Fill all 64 hardware voices with low gain (0.1f) */
    for (int i = 0; i < 64; i++) {
        alSourcef(test_sources[i], AL_GAIN, 0.1f);
        alSourcePlay(test_sources[i]);
        ALsource *s = al_source_get(test_sources[i]);
        assert(s->hw_voice_idx >= 0);
    }

    /* Play 65th source with high gain (1.0f) and looping (priority boost) */
    alSourcef(test_sources[64], AL_GAIN, 1.0f);
    alSourcei(test_sources[64], AL_LOOPING, AL_TRUE);
    alSourcePlay(test_sources[64]);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *s64 = al_source_get(test_sources[64]);
    assert(s64->hw_voice_idx >= 0);
    printf("    -> Preempted voice successfully assigned to high-priority source (HW slot %d)\n",
           s64->hw_voice_idx);

    /* Clean up test sources */
    for (int i = 0; i < 65; i++) {
        alSourceStop(test_sources[i]);
    }
    alDeleteSources(65, test_sources);
    assert(alGetError() == AL_NO_ERROR);

    /* Clean up remaining initial sources before 256 pool limit test */
    alDeleteSources(4, sources);
    assert(alGetError() == AL_NO_ERROR);
    assert(b0->ref_count == 0);
    assert(b1->ref_count == 0);

    /* Test 12: Source pool limit (256 sources) */
    printf("[12] Testing 256 source pool allocation limit...\n");
    ALuint all_sources[256];
    alGenSources(256, all_sources);
    assert(alGetError() == AL_NO_ERROR);
    for (int i = 0; i < 256; i++) {
        assert(alIsSource(all_sources[i]) == AL_TRUE);
    }

    /* 257th source generation must fail with AL_OUT_OF_MEMORY */
    ALuint overflow_source = 0;
    alGenSources(1, &overflow_source);
    assert(alGetError() == AL_OUT_OF_MEMORY);
    assert(overflow_source == 0);

    alDeleteSources(256, all_sources);
    assert(alGetError() == AL_NO_ERROR);

    alDeleteBuffers(2, bufs);
    assert(alGetError() == AL_NO_ERROR);

    /* Subsystem deinitialization */
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    apu_mem_shutdown();

    free(voice_array);
    free(mock_mmio);

    printf("=== All Source 2D & APU HW Synchronization Tests Passed Successfully! ===\n");
    return 0;
}
