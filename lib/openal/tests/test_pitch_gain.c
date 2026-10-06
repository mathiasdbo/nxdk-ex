#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <AL/al.h>
#include "al_source.h"
#include "al_listener.h"
#include "al_buffer.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x10000

int main(void) {
    printf("=== OpenAL Dynamic Pitch & Gain Mapping Host Test ===\n");

    /* 1. Subsystem initialization */
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
    al_listener_init();
    al_source_set_apu_base((uintptr_t)mock_mmio);

    assert(alGetError() == AL_NO_ERROR);

    /* Prepare sample buffers: 48 kHz mono and 44.1 kHz mono */
    ALuint bufs[2] = {0};
    alGenBuffers(2, bufs);
    assert(alGetError() == AL_NO_ERROR);

    int16_t pcm_data[256] = {0};
    alBufferData(bufs[0], AL_FORMAT_MONO16, pcm_data, sizeof(pcm_data), 48000);
    assert(alGetError() == AL_NO_ERROR);

    alBufferData(bufs[1], AL_FORMAT_MONO16, pcm_data, sizeof(pcm_data), 44100);
    assert(alGetError() == AL_NO_ERROR);

    ALuint sources[2] = {0};
    alGenSources(2, sources);
    assert(alGetError() == AL_NO_ERROR);

    /* Attach 48kHz buffer to source 0 and 44.1kHz buffer to source 1 */
    alSourcei(sources[0], AL_BUFFER, (ALint)bufs[0]);
    alSourcei(sources[1], AL_BUFFER, (ALint)bufs[1]);
    assert(alGetError() == AL_NO_ERROR);

    /*
     * ------------------------------------------------------------------------
     * Test 1: 48kHz buffer with pitch 1.0f produces exactly 0x00010000 (unity)
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing 48kHz buffer with pitch 1.0f produces exactly 0x00010000...\n");
    alSourcePlay(sources[0]);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *src0 = al_source_get(sources[0]);
    assert(src0 != NULL && src0->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *ctx0 = apu_voice_get_context((uint32_t)src0->hw_voice_idx);
    assert(ctx0 != NULL);

    assert(ctx0->pitch_step == APU_PITCH_STEP_UNITY);
    assert(ctx0->pitch_step == 0x00010000u);
    printf("    -> Source 0 (48kHz) pitch_step: 0x%08X (unity 0x00010000) [PASS]\n", ctx0->pitch_step);

    /*
     * ------------------------------------------------------------------------
     * Test 2: 44.1kHz buffer with pitch 1.0f produces 0x0000EB85 (nominal 0x0000EB33)
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing 44.1kHz buffer with pitch 1.0f produces 0x0000EB85 / 0x0000EB33...\n");
    alSourcePlay(sources[1]);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *src1 = al_source_get(sources[1]);
    assert(src1 != NULL && src1->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *ctx1 = apu_voice_get_context((uint32_t)src1->hw_voice_idx);
    assert(ctx1 != NULL);

    uint32_t expected_44k = APU_CALC_PITCH_STEP(44100, 1.0f);
    assert(expected_44k == 0x0000EB33u);
    assert(ctx1->pitch_step == expected_44k);
    /* Spec criteria verification: 44100 / 48000 * 65536 = 0xEB33 (or 0xEB85 spec reference) */
    assert(ctx1->pitch_step == 0x0000EB33u || ctx1->pitch_step == 0x0000EB85u);
    printf("    -> Source 1 (44.1kHz) pitch_step: 0x%08X (nominal 0x0000EB33, spec ref 0x0000EB85) [PASS]\n",
           ctx1->pitch_step);

    /*
     * ------------------------------------------------------------------------
     * Test 3: Pitch 2.0f doubles pitch_step
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing pitch 2.0f doubles pitch_step...\n");
    uint32_t step_48k_1x = ctx0->pitch_step;
    uint32_t step_44k_1x = ctx1->pitch_step;

    alSourcef(sources[0], AL_PITCH, 2.0f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx0->pitch_step == 2u * step_48k_1x);
    assert(ctx0->pitch_step == 0x00020000u);

    alSourcef(sources[1], AL_PITCH, 2.0f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx1->pitch_step == 2u * step_44k_1x);
    assert(ctx1->pitch_step == APU_CALC_PITCH_STEP(44100, 2.0f));
    printf("    -> Pitch 2.0f doubled 48kHz to 0x%08X and 44.1kHz to 0x%08X [PASS]\n",
           ctx0->pitch_step, ctx1->pitch_step);

    /*
     * ------------------------------------------------------------------------
     * Test 4: Real-time update of AL_PITCH while playing updates ctx->pitch_step
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing real-time update of AL_PITCH and clamping...\n");
    alSourcef(sources[0], AL_PITCH, 1.5f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx0->pitch_step == APU_CALC_PITCH_STEP(48000, 1.5f));
    assert(ctx0->pitch_step == 0x00018000u);

    alSourcef(sources[0], AL_PITCH, 0.5f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx0->pitch_step == APU_CALC_PITCH_STEP(48000, 0.5f));
    assert(ctx0->pitch_step == 0x00008000u);

    /* Clamping to maximum rate [0x00040000] (4.0x at 48kHz) */
    alSourcef(sources[0], AL_PITCH, 10.0f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx0->pitch_step == 0x00040000u);

    /* Clamping to minimum rate [0x00001000] (0.0625x) */
    alSourcef(sources[0], AL_PITCH, 0.001f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx0->pitch_step == 0x00001000u);

    /* Invalid pitch <= 0.0f must set AL_INVALID_VALUE */
    alSourcef(sources[0], AL_PITCH, 0.0f);
    assert(alGetError() == AL_INVALID_VALUE);

    alSourcef(sources[0], AL_PITCH, -2.5f);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Restore pitch 1.0f */
    alSourcef(sources[0], AL_PITCH, 1.0f);
    alSourcef(sources[1], AL_PITCH, 1.0f);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> Real-time pitch step update and [0x1000, 0x40000] clamping verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: Real-time update of AL_GAIN while playing updates master volume
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing real-time update of AL_GAIN and master volume words...\n");
    assert(al_listener_get_gain() == 1.0f);

    alSourcef(sources[0], AL_GAIN, 0.75f);
    assert(alGetError() == AL_NO_ERROR);
    uint16_t vol_75 = (uint16_t)(0.75f * 65535.0f + 0.5f);
    assert(ctx0->master_vol_left == vol_75);
    assert(ctx0->master_vol_right == vol_75);

    /* Clamping with AL_MIN_GAIN and AL_MAX_GAIN */
    alSourcef(sources[0], AL_MIN_GAIN, 0.2f);
    alSourcef(sources[0], AL_MAX_GAIN, 0.8f);
    assert(alGetError() == AL_NO_ERROR);

    /* Below min_gain -> clamped to 0.2f */
    alSourcef(sources[0], AL_GAIN, 0.05f);
    assert(alGetError() == AL_NO_ERROR);
    uint16_t vol_min = (uint16_t)(0.2f * 65535.0f + 0.5f);
    assert(ctx0->master_vol_left == vol_min);
    assert(ctx0->master_vol_right == vol_min);

    /* Above max_gain -> clamped to 0.8f */
    alSourcef(sources[0], AL_GAIN, 0.95f);
    assert(alGetError() == AL_NO_ERROR);
    uint16_t vol_max = (uint16_t)(0.8f * 65535.0f + 0.5f);
    assert(ctx0->master_vol_left == vol_max);
    assert(ctx0->master_vol_right == vol_max);

    /* Updating AL_MAX_GAIN while playing dynamically reclamps master volume */
    alSourcef(sources[0], AL_MAX_GAIN, 0.6f);
    assert(alGetError() == AL_NO_ERROR);
    uint16_t vol_new_max = (uint16_t)(0.6f * 65535.0f + 0.5f);
    assert(ctx0->master_vol_left == vol_new_max);
    assert(ctx0->master_vol_right == vol_new_max);

    /* Invalid gain < 0.0f */
    alSourcef(sources[0], AL_GAIN, -0.5f);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Invalid min/max gain out of [0.0, 1.0] */
    alSourcef(sources[0], AL_MIN_GAIN, -0.1f);
    assert(alGetError() == AL_INVALID_VALUE);
    alSourcef(sources[0], AL_MAX_GAIN, 1.5f);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Reset gain bounds to default [0.0, 1.0] */
    alSourcef(sources[0], AL_MIN_GAIN, 0.0f);
    alSourcef(sources[0], AL_MAX_GAIN, 1.0f);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> Real-time AL_GAIN and min/max clamping verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 6: Global alListenerf(AL_GAIN, 0.5f) scales active voice master volume
     * ------------------------------------------------------------------------
     */
    printf("[6] Testing global alListenerf(AL_GAIN, 0.5f) scales active voices...\n");
    alSourcef(sources[0], AL_GAIN, 0.8f);
    alSourcef(sources[1], AL_GAIN, 0.6f);
    assert(alGetError() == AL_NO_ERROR);

    uint16_t vol0_pre = (uint16_t)(0.8f * 65535.0f + 0.5f);
    uint16_t vol1_pre = (uint16_t)(0.6f * 65535.0f + 0.5f);
    assert(ctx0->master_vol_left == vol0_pre);
    assert(ctx1->master_vol_left == vol1_pre);

    /* Set global listener gain to 0.5f */
    alListenerf(AL_GAIN, 0.5f);
    assert(alGetError() == AL_NO_ERROR);

    ALfloat listener_gain = 0.0f;
    alGetListenerf(AL_GAIN, &listener_gain);
    assert(listener_gain == 0.5f);
    assert(al_listener_get_gain() == 0.5f);

    /* Both active sources must immediately reflect scaled master volume */
    /* Source 0: eff_gain = 0.8 * 0.5 = 0.4 */
    /* Source 1: eff_gain = 0.6 * 0.5 = 0.3 */
    uint16_t vol0_post = (uint16_t)(0.4f * 65535.0f + 0.5f);
    uint16_t vol1_post = (uint16_t)(0.3f * 65535.0f + 0.5f);
    assert(ctx0->master_vol_left == vol0_post);
    assert(ctx0->master_vol_right == vol0_post);
    assert(ctx1->master_vol_left == vol1_post);
    assert(ctx1->master_vol_right == vol1_post);

    /* Global listener gain to 0.0f (mute) */
    alListenerf(AL_GAIN, 0.0f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx0->master_vol_left == 0);
    assert(ctx0->master_vol_right == 0);
    assert(ctx1->master_vol_left == 0);
    assert(ctx1->master_vol_right == 0);

    /* Invalid listener gain < 0.0f */
    alListenerf(AL_GAIN, -0.2f);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Restore listener gain to 1.0f */
    alListenerf(AL_GAIN, 1.0f);
    assert(alGetError() == AL_NO_ERROR);
    assert(ctx0->master_vol_left == vol0_pre);
    assert(ctx1->master_vol_left == vol1_pre);
    printf("    -> Global listener gain immediately scaled all active voices [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 7: Listener Vectors and Query APIs (Position, Velocity, Orientation)
     * ------------------------------------------------------------------------
     */
    printf("[7] Testing listener vectors and query APIs...\n");
    alListener3f(AL_POSITION, 10.0f, -5.0f, 2.5f);
    assert(alGetError() == AL_NO_ERROR);
    ALfloat pos[3] = {0};
    alGetListenerfv(AL_POSITION, pos);
    assert(pos[0] == 10.0f && pos[1] == -5.0f && pos[2] == 2.5f);

    alListener3f(AL_VELOCITY, 1.0f, 0.0f, -1.0f);
    assert(alGetError() == AL_NO_ERROR);
    ALfloat vel[3] = {0};
    alGetListenerfv(AL_VELOCITY, vel);
    assert(vel[0] == 1.0f && vel[1] == 0.0f && vel[2] == -1.0f);

    ALfloat ori[6] = {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    alListenerfv(AL_ORIENTATION, ori);
    assert(alGetError() == AL_NO_ERROR);
    ALfloat ori_get[6] = {0};
    alGetListenerfv(AL_ORIENTATION, ori_get);
    assert(memcmp(ori, ori_get, sizeof(ori)) == 0);

    /* Invalid pointer query */
    alGetListenerfv(AL_POSITION, NULL);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Invalid enum query */
    alListenerf(0x9999, 1.0f);
    assert(alGetError() == AL_INVALID_ENUM);
    printf("    -> Listener vectors and queries conform to OpenAL 1.1 [PASS]\n");

    /* 8. Subsystem Cleanup */
    alSourceStopv(2, sources);
    alDeleteSources(2, sources);
    alDeleteBuffers(2, bufs);
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    free(voice_array);
    free(mock_mmio);

    printf("=== All Pitch & Gain Mapping Tests Passed Successfully! ===\n");
    return 0;
}
