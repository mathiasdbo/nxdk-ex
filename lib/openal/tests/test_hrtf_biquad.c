#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <AL/al.h>
#include <AL/alc.h>
#include "apu_spatial.h"
#include "al_source.h"
#include "al_listener.h"
#include "al_buffer.h"
#include "alc_context.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define MOCK_MMIO_SIZE 0x40000

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== OpenAL 1.1 HRTF Biquad Filter & Voice Context Host Test ===\n");

    /*
     * ------------------------------------------------------------------------
     * Test 1: Elevation & Pinna Shadow Cutoff Frequency Calculation
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing Elevation & Pinna Cutoff Frequency Mapping...\n");

    /* 1.1: Neutral plane (y = 0, z = 0) -> f_base = 14000 Hz, fc = 14000 Hz */
    float fc_neutral = apu_calc_elevation_cutoff(0.0f, 0.0f);
    assert(fabsf(fc_neutral - 14000.0f) < 1e-3f);

    /* 1.2: Minimum elevation (y = -1.0, z = 0) -> f_base = 10000 Hz, fc = 10000 Hz */
    float fc_down = apu_calc_elevation_cutoff(-1.0f, 0.0f);
    assert(fabsf(fc_down - 10000.0f) < 1e-3f);

    /* 1.3: Maximum elevation (y = +1.0, z = 0) -> f_base = 18000 Hz, fc = 18000 Hz */
    float fc_up = apu_calc_elevation_cutoff(1.0f, 0.0f);
    assert(fabsf(fc_up - 18000.0f) < 1e-3f);

    /* 1.4: Intermediate elevation (y = +0.5, z = 0) -> f_base = 16000 Hz */
    float fc_mid_up = apu_calc_elevation_cutoff(0.5f, 0.0f);
    assert(fabsf(fc_mid_up - 16000.0f) < 1e-3f);

    /* 1.5: Intermediate elevation (y = -0.5, z = 0) -> f_base = 12000 Hz */
    float fc_mid_down = apu_calc_elevation_cutoff(-0.5f, 0.0f);
    assert(fabsf(fc_mid_down - 12000.0f) < 1e-3f);

    /* 1.6: Rear pinna shadow (z > 0):
     * y = 0.0, z = +1.0 -> fc = 14000 - 4000 * 1.0 = 10000 Hz
     */
    float fc_rear = apu_calc_elevation_cutoff(0.0f, 1.0f);
    assert(fabsf(fc_rear - 10000.0f) < 1e-3f);

    /* y = 0.0, z = +0.5 -> fc = 14000 - 4000 * 0.5 = 12000 Hz */
    float fc_rear_half = apu_calc_elevation_cutoff(0.0f, 0.5f);
    assert(fabsf(fc_rear_half - 12000.0f) < 1e-3f);

    /* y = +1.0, z = +1.0 -> fc = 18000 - 4000 * 1.0 = 14000 Hz */
    float fc_up_rear = apu_calc_elevation_cutoff(1.0f, 1.0f);
    assert(fabsf(fc_up_rear - 14000.0f) < 1e-3f);

    /* y = -1.0, z = +1.0 -> fc = 10000 - 4000 * 1.0 = 6000 Hz */
    float fc_down_rear = apu_calc_elevation_cutoff(-1.0f, 1.0f);
    assert(fabsf(fc_down_rear - 6000.0f) < 1e-3f);

    /* 1.7: Front offset (z <= 0) produces no rear shadow attenuation */
    float fc_front = apu_calc_elevation_cutoff(0.0f, -1.0f);
    assert(fabsf(fc_front - 14000.0f) < 1e-3f);

    float fc_front_half = apu_calc_elevation_cutoff(1.0f, -0.5f);
    assert(fabsf(fc_front_half - 18000.0f) < 1e-3f);

    /* 1.8: Coordinate clamping: y and z clamped to [-1.0, 1.0] */
    float fc_clamp_pos = apu_calc_elevation_cutoff(5.0f, 0.0f);
    assert(fabsf(fc_clamp_pos - 18000.0f) < 1e-3f);

    float fc_clamp_neg = apu_calc_elevation_cutoff(-5.0f, 0.0f);
    assert(fabsf(fc_clamp_neg - 10000.0f) < 1e-3f);

    float fc_clamp_z = apu_calc_elevation_cutoff(0.0f, 10.0f);
    assert(fabsf(fc_clamp_z - 10000.0f) < 1e-3f);

    /* 1.9: Cutoff frequency clamping [1000 Hz, 20000 Hz] */
    assert(apu_calc_elevation_cutoff(-1.0f, 10.0f) >= 1000.0f);
    assert(apu_calc_elevation_cutoff(1.0f, -10.0f) <= 20000.0f);

    printf("    -> Elevation cutoff mapping: y=-1: %.0fHz, y=0: %.0fHz, y=+1: %.0fHz, rear(z=+1): %.0fHz [PASS]\n",
           fc_down, fc_neutral, fc_up, fc_rear);

    /*
     * ------------------------------------------------------------------------
     * Test 2: Passthrough Identity Biquad Filter
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing Passthrough Identity Biquad Filter (Q14)...\n");

    APU_BIQUAD_COEFFS_Q14 pass;
    memset(&pass, 0xFF, sizeof(pass));
    apu_get_biquad_passthrough_q14(&pass);

    assert(pass.b0 == 16384); /* 1.0f in Q14 */
    assert(pass.b1 == 0);
    assert(pass.b2 == 0);
    assert(pass.a1 == 0);
    assert(pass.a2 == 0);

    /* Null pointer check */
    apu_get_biquad_passthrough_q14(NULL);

    printf("    -> Passthrough: b0=%d (0x%04X), b1=%d, b2=%d, a1=%d, a2=%d [PASS]\n",
           pass.b0, (uint16_t)pass.b0, pass.b1, pass.b2, pass.a1, pass.a2);

    /*
     * ------------------------------------------------------------------------
     * Test 3: Butterworth Lowpass Biquad Math, Q14 Scaling & Stability
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing Butterworth Lowpass Math, Q14 Scaling & Stability (1k, 8k, 14k, 20k)...\n");

    APU_BIQUAD_COEFFS_Q14 q_coeffs;

    /* 3.1: Cutoff fc = 1000 Hz at fs = 48000 Hz */
    apu_calc_butterworth_lowpass_q14(1000.0f, 48000.0f, &q_coeffs);
    assert(q_coeffs.b0 == 64);
    assert(q_coeffs.b1 == 128);
    assert(q_coeffs.b2 == 64);
    assert(q_coeffs.a1 == -29743);
    assert(q_coeffs.a2 == 13615);

    /* Stability check at 1000 Hz: |a2| < 1.0 and |a1| < 1.0 + a2 */
    float a1_f = (float)q_coeffs.a1 / 16384.0f;
    float a2_f = (float)q_coeffs.a2 / 16384.0f;
    assert(fabsf(a2_f) < 1.0f);
    assert(fabsf(a1_f) < 1.0f + a2_f);

    /* 3.2: Cutoff fc = 8000 Hz at fs = 48000 Hz */
    apu_calc_butterworth_lowpass_q14(8000.0f, 48000.0f, &q_coeffs);
    assert(q_coeffs.b0 == 2540);
    assert(q_coeffs.b1 == 5081);
    assert(q_coeffs.b2 == 2540);
    assert(q_coeffs.a1 == -10161);
    assert(q_coeffs.a2 == 3939);

    a1_f = (float)q_coeffs.a1 / 16384.0f;
    a2_f = (float)q_coeffs.a2 / 16384.0f;
    assert(fabsf(a2_f) < 1.0f);
    assert(fabsf(a1_f) < 1.0f + a2_f);

    /* 3.3: Cutoff fc = 14000 Hz at fs = 48000 Hz */
    apu_calc_butterworth_lowpass_q14(14000.0f, 48000.0f, &q_coeffs);
    assert(q_coeffs.b0 == 6127);
    assert(q_coeffs.b1 == 12255);
    assert(q_coeffs.b2 == 6127);
    assert(q_coeffs.a1 == 5039);
    assert(q_coeffs.a2 == 3086);

    a1_f = (float)q_coeffs.a1 / 16384.0f;
    a2_f = (float)q_coeffs.a2 / 16384.0f;
    assert(fabsf(a2_f) < 1.0f);
    assert(fabsf(a1_f) < 1.0f + a2_f);

    /* 3.4: Cutoff fc = 20000 Hz at fs = 48000 Hz */
    apu_calc_butterworth_lowpass_q14(20000.0f, 48000.0f, &q_coeffs);
    assert(q_coeffs.b0 == 11294);
    assert(q_coeffs.b1 == 22587);
    assert(q_coeffs.b2 == 11294);
    assert(q_coeffs.a1 == 20965);
    assert(q_coeffs.a2 == 7825);

    a1_f = (float)q_coeffs.a1 / 16384.0f;
    a2_f = (float)q_coeffs.a2 / 16384.0f;
    assert(fabsf(a2_f) < 1.0f);
    assert(fabsf(a1_f) < 1.0f + a2_f);

    /* 3.5: Frequency sweep stability verification from 100 Hz to 23000 Hz */
    for (int freq = 100; freq <= 23000; freq += 100) {
        APU_BIQUAD_COEFFS_Q14 c;
        apu_calc_butterworth_lowpass_q14((float)freq, 48000.0f, &c);

        /* Symmetry of Butterworth lowpass numerator: b0 == b2 */
        assert(c.b0 == c.b2);

        /* Relationship: b1 ~= 2 * b0 (within rounding of +/- 1) */
        int32_t b1_diff = (int32_t)c.b1 - 2 * (int32_t)c.b0;
        assert(abs(b1_diff) <= 1);

        /* Strict stability conditions */
        float a1_check = (float)c.a1 / 16384.0f;
        float a2_check = (float)c.a2 / 16384.0f;
        assert(fabsf(a2_check) < 1.0f);
        assert(fabsf(a1_check) < 1.0f + a2_check);
    }

    /* 3.6: Edge cases: invalid/negative cutoff or NULL pointer */
    apu_calc_butterworth_lowpass_q14(1000.0f, 48000.0f, NULL);
    APU_BIQUAD_COEFFS_Q14 fallback;
    apu_calc_butterworth_lowpass_q14(-500.0f, 48000.0f, &fallback);
    assert(fallback.b0 == 16384);

    printf("    -> Exact Q14 coeffs verified at 1k, 8k, 14k, 20k Hz [PASS]\n");
    printf("    -> Stability (|a2| < 1.0, |a1| < 1.0 + a2) verified across entire frequency spectrum [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 4: Hardware Voice Context DWORD 20-23 Memory Layout
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing Hardware Voice Context DWORD 20-23 Memory Layout...\n");

    /* Total context size must strictly be 128 bytes */
    assert(sizeof(NVAPU_VOICE_CONTEXT_3D) == 128);

    /* DWORD 20 starts at byte offset 80 (20 * 4) */
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, hrtf_b0) == 80);
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, hrtf_b1) == 82);

    /* DWORD 21 starts at byte offset 84 (21 * 4) */
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, hrtf_b2) == 84);
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, hrtf_a1) == 86);

    /* DWORD 22 starts at byte offset 88 (22 * 4) */
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, hrtf_a2) == 88);
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, reserved1) == 90);

    /* DWORD 24 starts at byte offset 96 (24 * 4) */
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, mixbin_routing_mask) == 96);

    /* Verify DWORD access mapping */
    NVAPU_VOICE_CONTEXT_3D test_ctx;
    apu_voice_context_reset(&test_ctx);
    assert(test_ctx.hrtf_b0 == 16384); /* Passthrough default */
    assert(test_ctx.hrtf_b1 == 0);
    assert(test_ctx.hrtf_b2 == 0);
    assert(test_ctx.hrtf_a1 == 0);
    assert(test_ctx.hrtf_a2 == 0);

    /* Assign distinct coefficients and verify raw DWORD memory */
    test_ctx.hrtf_b0 = 0x1234;
    test_ctx.hrtf_b1 = (int16_t)0x5678;
    test_ctx.hrtf_b2 = 0x2345;
    test_ctx.hrtf_a1 = (int16_t)0x6789;
    test_ctx.hrtf_a2 = 0x3456;

    uint32_t *dwords = (uint32_t *)&test_ctx;
    uint32_t exp_dw20 = (uint16_t)0x1234 | ((uint32_t)(uint16_t)0x5678 << 16);
    uint32_t exp_dw21 = (uint16_t)0x2345 | ((uint32_t)(uint16_t)0x6789 << 16);
    uint32_t exp_dw22 = (uint16_t)0x3456 | ((uint32_t)0 << 16);

    assert(dwords[20] == exp_dw20);
    assert(dwords[21] == exp_dw21);
    assert((dwords[22] & 0xFFFF) == (exp_dw22 & 0xFFFF));

    printf("    -> Voice Context DWORD 20-23 layout verified (offset 80-95, 128-byte total) [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: End-to-End Voice Context Real-Time Synchronization
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing Voice Context HRTF Biquad Updates (Play & Real-Time Sync)...\n");

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

    /* 5.1: Create mono 48kHz buffer */
    ALuint mono_buf = 0;
    alGenBuffers(1, &mono_buf);
    assert(alGetError() == AL_NO_ERROR);
    int16_t pcm_samples[256] = {0};
    alBufferData(mono_buf, AL_FORMAT_MONO16, pcm_samples, sizeof(pcm_samples), 48000);
    assert(alGetError() == AL_NO_ERROR);

    /* Create source */
    ALuint src = 0;
    alGenSources(1, &src);
    assert(alGetError() == AL_NO_ERROR);
    alSourcei(src, AL_BUFFER, (ALint)mono_buf);
    assert(alGetError() == AL_NO_ERROR);

    /* Place source directly overhead: (0, 10, 0)
     * Listener at (0, 0, 0) looking at -Z, up at +Y.
     * local_pos = (0, 10, 0), distance = 10.0.
     * y_rel = 10 / 10 = +1.0, z_rel = 0 / 10 = 0.0.
     * fc = 14000 + 4000 * 1.0 = 18000 Hz.
     */
    alSource3f(src, AL_POSITION, 0.0f, 10.0f, 0.0f);
    alSourcePlay(src);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *s = al_source_get(src);
    assert(s != NULL && s->hw_voice_idx >= 0);
    uint32_t v_idx = (uint32_t)s->hw_voice_idx;
    NVAPU_VOICE_CONTEXT_3D *vctx = &voice_array[v_idx];

    /* Expected coefficients at 18000 Hz */
    APU_BIQUAD_COEFFS_Q14 exp_18k;
    apu_calc_butterworth_lowpass_q14(18000.0f, 48000.0f, &exp_18k);

    assert(vctx->mode_3d == 1);
    assert(vctx->hrtf_b0 == exp_18k.b0);
    assert(vctx->hrtf_b1 == exp_18k.b1);
    assert(vctx->hrtf_b2 == exp_18k.b2);
    assert(vctx->hrtf_a1 == exp_18k.a1);
    assert(vctx->hrtf_a2 == exp_18k.a2);

    printf("    -> Source overhead (+Y): fc=18000Hz, b0=%d, b1=%d, a1=%d, a2=%d [PASS]\n",
           vctx->hrtf_b0, vctx->hrtf_b1, vctx->hrtf_a1, vctx->hrtf_a2);

    /* 5.2: Real-time source repositioning to horizontal front: (0, 0, -10)
     * local_pos = (0, 0, 10) in local front.
     * y_rel = 0.0, z_rel = -1.0.
     * fc = 14000 Hz.
     */
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -10.0f);
    assert(alGetError() == AL_NO_ERROR);

    APU_BIQUAD_COEFFS_Q14 exp_14k;
    apu_calc_butterworth_lowpass_q14(14000.0f, 48000.0f, &exp_14k);

    assert(vctx->hrtf_b0 == exp_14k.b0);
    assert(vctx->hrtf_b1 == exp_14k.b1);
    assert(vctx->hrtf_b2 == exp_14k.b2);
    assert(vctx->hrtf_a1 == exp_14k.a1);
    assert(vctx->hrtf_a2 == exp_14k.a2);

    printf("    -> Real-time Move to Front (-Z): fc=14000Hz, b0=%d, b1=%d, a1=%d, a2=%d [PASS]\n",
           vctx->hrtf_b0, vctx->hrtf_b1, vctx->hrtf_a1, vctx->hrtf_a2);

    /* 5.3: Real-time source repositioning behind listener: (0, 0, 10)
     * local_pos = (0, 0, -10) -> z_local = -(-10) = +10.
     * y_rel = 0.0, z_rel = +1.0.
     * fc = 14000 - 4000 * 1.0 = 10000 Hz.
     */
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 10.0f);
    assert(alGetError() == AL_NO_ERROR);

    APU_BIQUAD_COEFFS_Q14 exp_10k;
    apu_calc_butterworth_lowpass_q14(10000.0f, 48000.0f, &exp_10k);

    assert(vctx->hrtf_b0 == exp_10k.b0);
    assert(vctx->hrtf_b1 == exp_10k.b1);
    assert(vctx->hrtf_b2 == exp_10k.b2);
    assert(vctx->hrtf_a1 == exp_10k.a1);
    assert(vctx->hrtf_a2 == exp_10k.a2);

    printf("    -> Real-time Move to Rear (+Z): fc=10000Hz, b0=%d, b1=%d, a1=%d, a2=%d [PASS]\n",
           vctx->hrtf_b0, vctx->hrtf_b1, vctx->hrtf_a1, vctx->hrtf_a2);

    /* 5.4: Real-time listener orientation update:
     * Keep source at (0, 0, -10) (was front).
     * Now rotate listener 180 degrees so listener looks towards +Z (forward=(0,0,1), up=(0,1,0)).
     * Source is now physically BEHIND the listener!
     * z_rel becomes +1.0 -> fc = 10000 Hz.
     */
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -10.0f); /* Initially front -> 14000 Hz */
    assert(vctx->hrtf_b0 == exp_14k.b0);

    float ori_turn[6] = {0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f}; /* Look at +Z */
    alListenerfv(AL_ORIENTATION, ori_turn);

    assert(vctx->hrtf_b0 == exp_10k.b0);
    assert(vctx->hrtf_b1 == exp_10k.b1);
    assert(vctx->hrtf_b2 == exp_10k.b2);
    assert(vctx->hrtf_a1 == exp_10k.a1);
    assert(vctx->hrtf_a2 == exp_10k.a2);

    printf("    -> Real-time Listener Turn (180 deg): Source becomes rear, fc=10000Hz [PASS]\n");

    /* Reset listener orientation */
    float ori_default[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, ori_default);

    /* 5.5: Stereo source playback bypasses 3D processing (passthrough coefficients) */
    alSourceStop(src);

    ALuint stereo_buf = 0;
    alGenBuffers(1, &stereo_buf);
    int16_t stereo_samples[512] = {0};
    alBufferData(stereo_buf, AL_FORMAT_STEREO16, stereo_samples, sizeof(stereo_samples), 48000);

    alSourcei(src, AL_BUFFER, (ALint)stereo_buf);
    alSource3f(src, AL_POSITION, 0.0f, 10.0f, 0.0f);
    alSourcePlay(src);

    assert(vctx->mode_3d == 0);
    assert(vctx->hrtf_b0 == 16384);
    assert(vctx->hrtf_b1 == 0);
    assert(vctx->hrtf_b2 == 0);
    assert(vctx->hrtf_a1 == 0);
    assert(vctx->hrtf_a2 == 0);

    printf("    -> Stereo Source Bypass: mode_3d=0, Passthrough b0=16384, b1..a2=0 [PASS]\n");

    alSourceStop(src);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &mono_buf);
    alDeleteBuffers(1, &stereo_buf);

    /* Cleanup */
    apu_voice_subsystem_deinit((uintptr_t)mock_mmio);
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_itd_subsystem_deinit();
    apu_mem_shutdown();
    free(voice_array);
    free(mock_mmio);

    printf("=== All HRTF Biquad Filter & Voice Context Tests Passed Successfully! ===\n");
    return 0;
}
