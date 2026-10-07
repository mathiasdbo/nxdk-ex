#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "apu_spatial.h"
#include "al_source.h"
#include "al_listener.h"
#include "al_buffer.h"
#include "alc_context.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x40000
#define PI_CONST 3.14159265358979323846f

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== OpenAL 1.1 ITU-R BS.775 5.1 Multichannel Panning Host Test ===\n");

    /*
     * ------------------------------------------------------------------------
     * Test 1: ITU-R BS.775 Discrete Channel Isolation
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing ITU-R BS.775 Discrete Angle Channel Isolation...\n");

    /* Case 1.1: Center (0 deg / 0.0 rad) -> (x = 0, z = -1) */
    {
        APU_PAN_GAINS_51 gains;
        uint8_t mixbins[16];
        apu_calc_panning_51(0.0f, -1.0f, 0.0f, &gains);
        apu_calc_mixbin_gains_51(&gains, mixbins);

        assert(fabsf(gains.c - 1.0f) < 1e-4f);
        assert(fabsf(gains.fl) < 1e-4f);
        assert(fabsf(gains.fr) < 1e-4f);
        assert(fabsf(gains.sl) < 1e-4f);
        assert(fabsf(gains.sr) < 1e-4f);
        assert(fabsf(gains.lfe) < 1e-4f);

        assert(mixbins[4] == 255); /* MixBin 4 = Center */
        assert(mixbins[0] == 0);   /* FL */
        assert(mixbins[1] == 0);   /* FR */
        assert(mixbins[2] == 0);   /* SL */
        assert(mixbins[3] == 0);   /* SR */
        assert(mixbins[5] == 0);   /* LFE */
    }

    /* Case 1.2: Front Right (+30 deg / pi/6 rad) -> (x = sin(30), z = -cos(30)) */
    {
        float x = sinf(30.0f * PI_CONST / 180.0f);
        float z = -cosf(30.0f * PI_CONST / 180.0f);
        APU_PAN_GAINS_51 gains;
        uint8_t mixbins[16];
        apu_calc_panning_51(x, z, 0.0f, &gains);
        apu_calc_mixbin_gains_51(&gains, mixbins);

        assert(fabsf(gains.fr - 1.0f) < 1e-4f);
        assert(fabsf(gains.c) < 1e-4f);
        assert(fabsf(gains.fl) < 1e-4f);
        assert(fabsf(gains.sl) < 1e-4f);
        assert(fabsf(gains.sr) < 1e-4f);

        assert(mixbins[1] == 255); /* MixBin 1 = FR */
        assert(mixbins[0] == 0);
        assert(mixbins[2] == 0);
        assert(mixbins[3] == 0);
        assert(mixbins[4] == 0);
    }

    /* Case 1.3: Surround Right (+110 deg / 11*pi/18 rad) -> (x = sin(110), z = -cos(110)) */
    {
        float x = sinf(110.0f * PI_CONST / 180.0f);
        float z = -cosf(110.0f * PI_CONST / 180.0f);
        APU_PAN_GAINS_51 gains;
        uint8_t mixbins[16];
        apu_calc_panning_51(x, z, 0.0f, &gains);
        apu_calc_mixbin_gains_51(&gains, mixbins);

        assert(fabsf(gains.sr - 1.0f) < 1e-4f);
        assert(fabsf(gains.c) < 1e-4f);
        assert(fabsf(gains.fl) < 1e-4f);
        assert(fabsf(gains.fr) < 1e-4f);
        assert(fabsf(gains.sl) < 1e-4f);

        assert(mixbins[3] == 255); /* MixBin 3 = SR */
        assert(mixbins[0] == 0);
        assert(mixbins[1] == 0);
        assert(mixbins[2] == 0);
        assert(mixbins[4] == 0);
    }

    /* Case 1.4: Rear Center (+180 deg / pi rad) -> (x = 0, z = +1) */
    {
        APU_PAN_GAINS_51 gains;
        uint8_t mixbins[16];
        apu_calc_panning_51(0.0f, 1.0f, 0.0f, &gains);
        apu_calc_mixbin_gains_51(&gains, mixbins);

        /* At 180 deg (midway between 110 deg SR and 250 deg SL), SL and SR must be equal */
        assert(fabsf(gains.sl - gains.sr) < 1e-4f);
        float expected_rear = sqrtf(0.5f); /* 0.70710678 */
        assert(fabsf(gains.sl - expected_rear) < 1e-3f);
        assert(fabsf(gains.sr - expected_rear) < 1e-3f);
        assert(fabsf(gains.c) < 1e-4f);
        assert(fabsf(gains.fl) < 1e-4f);
        assert(fabsf(gains.fr) < 1e-4f);

        /* Quantized 8-bit MixBin gains: round(0.7071 * 255) = 180 */
        assert(mixbins[2] == 180); /* SL */
        assert(mixbins[3] == 180); /* SR */
        assert(mixbins[0] == 0);
        assert(mixbins[1] == 0);
        assert(mixbins[4] == 0);
    }

    /* Case 1.5: Surround Left (+250 deg / 25*pi/18 rad) -> (x = sin(250), z = -cos(250)) */
    {
        float x = sinf(250.0f * PI_CONST / 180.0f);
        float z = -cosf(250.0f * PI_CONST / 180.0f);
        APU_PAN_GAINS_51 gains;
        uint8_t mixbins[16];
        apu_calc_panning_51(x, z, 0.0f, &gains);
        apu_calc_mixbin_gains_51(&gains, mixbins);

        assert(fabsf(gains.sl - 1.0f) < 1e-4f);
        assert(fabsf(gains.c) < 1e-4f);
        assert(fabsf(gains.fl) < 1e-4f);
        assert(fabsf(gains.fr) < 1e-4f);
        assert(fabsf(gains.sr) < 1e-4f);

        assert(mixbins[2] == 255); /* MixBin 2 = SL */
        assert(mixbins[0] == 0);
        assert(mixbins[1] == 0);
        assert(mixbins[3] == 0);
        assert(mixbins[4] == 0);
    }

    /* Case 1.6: Front Left (+330 deg / 11*pi/6 rad) -> (x = sin(330), z = -cos(330)) */
    {
        float x = sinf(330.0f * PI_CONST / 180.0f);
        float z = -cosf(330.0f * PI_CONST / 180.0f);
        APU_PAN_GAINS_51 gains;
        uint8_t mixbins[16];
        apu_calc_panning_51(x, z, 0.0f, &gains);
        apu_calc_mixbin_gains_51(&gains, mixbins);

        assert(fabsf(gains.fl - 1.0f) < 1e-4f);
        assert(fabsf(gains.c) < 1e-4f);
        assert(fabsf(gains.fr) < 1e-4f);
        assert(fabsf(gains.sl) < 1e-4f);
        assert(fabsf(gains.sr) < 1e-4f);

        assert(mixbins[0] == 255); /* MixBin 0 = FL */
        assert(mixbins[1] == 0);
        assert(mixbins[2] == 0);
        assert(mixbins[3] == 0);
        assert(mixbins[4] == 0);
    }

    /* Case 1.7: Origin / Coincident horizontal center (radius < 0.00001f) */
    {
        APU_PAN_GAINS_51 gains;
        uint8_t mixbins[16];
        apu_calc_panning_51(0.0f, 0.0f, 0.0f, &gains);
        apu_calc_mixbin_gains_51(&gains, mixbins);

        /* Evenly distributed across 4 quad corners (0.5f each), C = 0 */
        assert(fabsf(gains.fl - 0.5f) < 1e-4f);
        assert(fabsf(gains.fr - 0.5f) < 1e-4f);
        assert(fabsf(gains.sl - 0.5f) < 1e-4f);
        assert(fabsf(gains.sr - 0.5f) < 1e-4f);
        assert(fabsf(gains.c) < 1e-4f);

        /* 0.5 * 255 = 127.5 -> round to 128 */
        assert(mixbins[0] == 128);
        assert(mixbins[1] == 128);
        assert(mixbins[2] == 128);
        assert(mixbins[3] == 128);
        assert(mixbins[4] == 0);
    }

    printf("    -> Discrete channel isolation verified at 0, 30, 110, 180, 250, 330 deg and center [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 2: Equal-Power Energy Conservation Across 360-degree Circle
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing Equal-Power Energy Conservation Across 360-degree Azimuth Circle...\n");

    /* Step around 360-degree horizontal azimuth in 0.1 degree increments (3600 points) */
    for (int step = 0; step < 3600; step++) {
        float deg = (float)step * 0.1f;
        float rad = deg * PI_CONST / 180.0f;
        float x = sinf(rad);
        float z = -cosf(rad);

        APU_PAN_GAINS_51 gains;
        apu_calc_panning_51(x, z, 0.0f, &gains);

        /* Energy conservation: sum of squared gains == 1.0 within 0.001 */
        float energy = (gains.fl * gains.fl) +
                       (gains.fr * gains.fr) +
                       (gains.sl * gains.sl) +
                       (gains.sr * gains.sr) +
                       (gains.c  * gains.c);

        assert(fabsf(energy - 1.0f) < 0.001f);

        /* Non-negativity check */
        assert(gains.fl >= 0.0f && gains.fl <= 1.0f);
        assert(gains.fr >= 0.0f && gains.fr <= 1.0f);
        assert(gains.sl >= 0.0f && gains.sl <= 1.0f);
        assert(gains.sr >= 0.0f && gains.sr <= 1.0f);
        assert(gains.c  >= 0.0f && gains.c  <= 1.0f);

        /* MixBin multiplier quantization check */
        uint8_t mixbins[16];
        apu_calc_mixbin_gains_51(&gains, mixbins);
        for (int m = 6; m < 16; m++) {
            assert(mixbins[m] == 0); /* Unused mixbins 6..15 must always be zero */
        }
    }

    /* Verify center origin point energy conservation */
    {
        APU_PAN_GAINS_51 origin_gains;
        apu_calc_panning_51(0.0f, 0.0f, 0.0f, &origin_gains);
        float origin_energy = (origin_gains.fl * origin_gains.fl) +
                              (origin_gains.fr * origin_gains.fr) +
                              (origin_gains.sl * origin_gains.sl) +
                              (origin_gains.sr * origin_gains.sr) +
                              (origin_gains.c  * origin_gains.c);
        assert(fabsf(origin_energy - 1.0f) < 0.001f);
    }

    printf("    -> Energy conservation sum(g_i^2) == 1.0 +/- 0.001 verified across 3600 azimuth angles [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 3: Hardware Voice Context Layout & MixBin Routing Masks
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing Voice Context Memory Layout & MixBin Routing Masks...\n");

    /* Structure size must strictly be 128 bytes */
    assert(sizeof(NVAPU_VOICE_CONTEXT_3D) == 128);

    /* DWORD 24: mixbin_routing_mask at byte offset 96 */
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, mixbin_routing_mask) == 96);

    /* DWORD 28: mixbin_gain[16] at byte offset 112 */
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, mixbin_gain) == 112);

    /* Verify reset values */
    NVAPU_VOICE_CONTEXT_3D ctx_test;
    apu_voice_context_reset(&ctx_test);
    assert(ctx_test.mixbin_routing_mask == 0);
    for (int i = 0; i < 16; i++) {
        assert(ctx_test.mixbin_gain[i] == 0);
    }

    printf("    -> Voice Context DWORD 24 (offset 96) and DWORD 28-29 (offset 112) verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: AL_XBOX_LFE_GAIN Parameter Query & Setting
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing AL_XBOX_LFE_GAIN Parameter Getters, Setters & Range Clamping...\n");

    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_listener_init();

    ALuint lfe_source = 0;
    alGenSources(1, &lfe_source);
    assert(alGetError() == AL_NO_ERROR);

    /* Default value must be 0.0f */
    ALfloat lfe_val = -1.0f;
    alGetSourcef(lfe_source, AL_XBOX_LFE_GAIN, &lfe_val);
    assert(alGetError() == AL_NO_ERROR);
    assert(lfe_val == 0.0f);

    /* Set valid values */
    alSourcef(lfe_source, AL_XBOX_LFE_GAIN, 0.25f);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcef(lfe_source, AL_XBOX_LFE_GAIN, &lfe_val);
    assert(fabsf(lfe_val - 0.25f) < 1e-4f);

    alSourcef(lfe_source, AL_XBOX_LFE_GAIN, 1.0f);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcef(lfe_source, AL_XBOX_LFE_GAIN, &lfe_val);
    assert(lfe_val == 1.0f);

    /* Vector setting alSourcefv */
    ALfloat vec_val = 0.5f;
    alSourcefv(lfe_source, AL_XBOX_LFE_GAIN, &vec_val);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcef(lfe_source, AL_XBOX_LFE_GAIN, &lfe_val);
    assert(fabsf(lfe_val - 0.5f) < 1e-4f);

    /* Out of bounds checks: negative and > 1.0f trigger AL_INVALID_VALUE */
    alSourcef(lfe_source, AL_XBOX_LFE_GAIN, -0.1f);
    assert(alGetError() == AL_INVALID_VALUE);

    alSourcef(lfe_source, AL_XBOX_LFE_GAIN, 1.1f);
    assert(alGetError() == AL_INVALID_VALUE);

    alDeleteSources(1, &lfe_source);
    assert(alGetError() == AL_NO_ERROR);

    printf("    -> AL_XBOX_LFE_GAIN defaults to 0.0f and enforces [0.0, 1.0] range [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: End-to-End Playback Routing Masks, Gains & Real-Time Context Sync
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing End-to-End Playback Routing Masks & Real-Time Context Sync...\n");

    int mem_res = apu_mem_init(0);
    assert(mem_res == 0);
    assert(apu_itd_subsystem_init() == 0);

    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);

    NVAPU_VOICE_CONTEXT_3D *voice_array = (NVAPU_VOICE_CONTEXT_3D *)calloc(
        NV_PAPU_NUM_3D_VOICES, sizeof(NVAPU_VOICE_CONTEXT_3D)
    );
    assert(voice_array != NULL);

    int vp_res = apu_voice_subsystem_init((uintptr_t)mock_mmio, voice_array, 0x10000);
    assert(vp_res == 0);

    al_buffer_init_subsystem();
    al_source_init_subsystem();
    al_listener_init();
    al_source_set_apu_base((uintptr_t)mock_mmio);

    /* 5.1: Mono 3D Source Playback (5.1 Routing Mask 0x3F) */
    ALuint mono_buf = 0;
    alGenBuffers(1, &mono_buf);
    int16_t mono_pcm[256] = {0};
    alBufferData(mono_buf, AL_FORMAT_MONO16, mono_pcm, sizeof(mono_pcm), 48000);

    ALuint mono_src = 0;
    alGenSources(1, &mono_src);
    alSourcei(mono_src, AL_BUFFER, (ALint)mono_buf);

    /* Position at Center (0, 0, -5) with LFE gain 0.5f */
    alSource3f(mono_src, AL_POSITION, 0.0f, 0.0f, -5.0f);
    alSourcef(mono_src, AL_XBOX_LFE_GAIN, 0.5f);
    alSourcePlay(mono_src);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *s_mono = al_source_get(mono_src);
    assert(s_mono != NULL && s_mono->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *vctx_mono = &voice_array[(uint32_t)s_mono->hw_voice_idx];

    /* Verification: Mask must be 0x0000003F (enables MixBins 0..5) */
    assert(vctx_mono->mixbin_routing_mask == 0x0000003F);
    assert(vctx_mono->mixbin_gain[4] == 255); /* Center */
    assert(vctx_mono->mixbin_gain[0] == 0);   /* FL */
    assert(vctx_mono->mixbin_gain[1] == 0);   /* FR */
    assert(vctx_mono->mixbin_gain[2] == 0);   /* SL */
    assert(vctx_mono->mixbin_gain[3] == 0);   /* SR */
    assert(vctx_mono->mixbin_gain[5] == 128); /* LFE = round(0.5 * 255) = 128 */

    /* 5.2: Real-time update - move mono source to Front Right (+30 deg) */
    float fr_x = 5.0f * sinf(30.0f * PI_CONST / 180.0f);
    float fr_z = -5.0f * cosf(30.0f * PI_CONST / 180.0f);
    alSource3f(mono_src, AL_POSITION, fr_x, 0.0f, fr_z);
    assert(vctx_mono->mixbin_gain[1] == 255); /* FR */
    assert(vctx_mono->mixbin_gain[4] == 0);   /* C drops to 0 */

    /* 5.3: Real-time update - move mono source to Surround Right (+110 deg) */
    float sr_x = 5.0f * sinf(110.0f * PI_CONST / 180.0f);
    float sr_z = -5.0f * cosf(110.0f * PI_CONST / 180.0f);
    alSource3f(mono_src, AL_POSITION, sr_x, 0.0f, sr_z);
    assert(vctx_mono->mixbin_gain[3] == 255); /* SR */
    assert(vctx_mono->mixbin_gain[1] == 0);   /* FR drops to 0 */

    /* 5.4: Real-time update - move mono source to Rear Center (+180 deg) */
    alSource3f(mono_src, AL_POSITION, 0.0f, 0.0f, 5.0f);
    assert(vctx_mono->mixbin_gain[2] == 180); /* SL */
    assert(vctx_mono->mixbin_gain[3] == 180); /* SR */
    assert(vctx_mono->mixbin_gain[0] == 0);
    assert(vctx_mono->mixbin_gain[1] == 0);
    assert(vctx_mono->mixbin_gain[4] == 0);

    /* 5.5: Real-time update - move mono source to Surround Left (+250 deg) */
    float sl_x = 5.0f * sinf(250.0f * PI_CONST / 180.0f);
    float sl_z = -5.0f * cosf(250.0f * PI_CONST / 180.0f);
    alSource3f(mono_src, AL_POSITION, sl_x, 0.0f, sl_z);
    assert(vctx_mono->mixbin_gain[2] == 255); /* SL */
    assert(vctx_mono->mixbin_gain[3] == 0);

    /* 5.6: Real-time update - move mono source to Front Left (+330 deg) */
    float fl_x = 5.0f * sinf(330.0f * PI_CONST / 180.0f);
    float fl_z = -5.0f * cosf(330.0f * PI_CONST / 180.0f);
    alSource3f(mono_src, AL_POSITION, fl_x, 0.0f, fl_z);
    assert(vctx_mono->mixbin_gain[0] == 255); /* FL */
    assert(vctx_mono->mixbin_gain[2] == 0);

    /* 5.7: Real-time LFE gain update during active playback */
    alSourcef(mono_src, AL_XBOX_LFE_GAIN, 1.0f);
    assert(vctx_mono->mixbin_gain[5] == 255);

    alSourcef(mono_src, AL_XBOX_LFE_GAIN, 0.0f);
    assert(vctx_mono->mixbin_gain[5] == 0);

    /* 5.8: Real-time Listener Orientation Update:
     * Place source directly at world north (0, 0, -5).
     * Rotate listener to face East (+X): forward = (1, 0, 0), up = (0, 1, 0).
     * World North relative to listener facing East is to the left (-Z in world -> -X in listener frame).
     * Listener relative angle becomes 270 deg (between SL 250 deg and FL 330 deg).
     */
    alSource3f(mono_src, AL_POSITION, 0.0f, 0.0f, -5.0f);
    ALfloat ori_east[6] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_east);

    /* Relative angle is 270 deg (Sector 3: SL [250] to FL [330]) */
    /* phi = (270 - 250) / 80 = 0.25 -> g_SL = cos(0.25 * pi/2) = cos(pi/8) ~= 0.92388 -> 236 */
    /* g_FL = sin(pi/8) ~= 0.38268 -> 98 */
    assert(vctx_mono->mixbin_gain[2] > 200); /* SL dominates */
    assert(vctx_mono->mixbin_gain[0] > 50);  /* FL has residual */
    assert(vctx_mono->mixbin_gain[1] == 0);  /* FR silent */
    assert(vctx_mono->mixbin_gain[3] == 0);  /* SR silent */
    assert(vctx_mono->mixbin_gain[4] == 0);  /* Center silent */

    /* Restore listener orientation */
    ALfloat ori_default[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_default);

    /* Stop mono source */
    alSourceStop(mono_src);
    assert(s_mono->state == AL_STOPPED);

    /* 5.9: Stereo 2D Direct Playback (2D Direct Routing Mask 0x03) */
    ALuint stereo_buf = 0;
    alGenBuffers(1, &stereo_buf);
    int16_t stereo_pcm[512] = {0};
    alBufferData(stereo_buf, AL_FORMAT_STEREO16, stereo_pcm, sizeof(stereo_pcm), 48000);

    ALuint stereo_src = 0;
    alGenSources(1, &stereo_src);
    alSourcei(stereo_src, AL_BUFFER, (ALint)stereo_buf);
    alSourcePlay(stereo_src);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *s_stereo = al_source_get(stereo_src);
    assert(s_stereo != NULL && s_stereo->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *vctx_stereo = &voice_array[(uint32_t)s_stereo->hw_voice_idx];

    /* Stereo source must use 2D routing mask 0x00000003 and unity FL/FR multipliers */
    assert(vctx_stereo->mixbin_routing_mask == 0x00000003);
    assert(vctx_stereo->mixbin_gain[0] == 0xFF);
    assert(vctx_stereo->mixbin_gain[1] == 0xFF);
    for (int m = 2; m < 16; m++) {
        assert(vctx_stereo->mixbin_gain[m] == 0);
    }

    /* Moving stereo source position must NOT alter direct stereo mixbins */
    alSource3f(stereo_src, AL_POSITION, 10.0f, 0.0f, 0.0f);
    assert(vctx_stereo->mixbin_routing_mask == 0x00000003);
    assert(vctx_stereo->mixbin_gain[0] == 0xFF);
    assert(vctx_stereo->mixbin_gain[1] == 0xFF);
    assert(vctx_stereo->mixbin_gain[2] == 0);

    alSourceStop(stereo_src);

    /* Cleanup */
    alDeleteSources(1, &mono_src);
    alDeleteSources(1, &stereo_src);
    alDeleteBuffers(1, &mono_buf);
    alDeleteBuffers(1, &stereo_buf);
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    apu_itd_subsystem_deinit();
    apu_mem_shutdown();
    free(voice_array);

    free(mock_mmio);

    printf("    -> 3D Mono (0x3F mask) and 2D Stereo (0x03 mask) routing verified [PASS]\n");
    printf("    -> Real-time voice context mixbin updates on position, listener & LFE verified [PASS]\n");

    printf("=== All ITU-R BS.775 5.1 Multichannel Panning Tests Passed Successfully! ===\n");
    return 0;
}
