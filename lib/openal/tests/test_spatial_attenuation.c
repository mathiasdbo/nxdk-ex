#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <AL/al.h>
#include "apu_spatial.h"
#include "al_source.h"
#include "al_listener.h"
#include "al_buffer.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_mem.h"

#define EPSILON 1e-4f
#define MOCK_MMIO_SIZE 0x10000

static inline bool float_approx(float a, float b) {
    return fabsf(a - b) < EPSILON;
}

int main(void) {
    printf("=== OpenAL 1.1 3D Spatial Attenuation & Doppler Engine Host Test ===\n");

    /* Initialize subsystems */
    int mem_res = apu_mem_init(0);
    assert(mem_res == 0);

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
    assert(alGetError() == AL_NO_ERROR);

    /*
     * ------------------------------------------------------------------------
     * Test 1: Distance Attenuation Models Math (Inverse, Linear, Exponent)
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing Distance Attenuation Models Math (d < d_ref, d = d_ref, d > d_ref)...\n");

    float d_ref = 5.0f;
    float d_max = 50.0f;
    float rolloff = 1.0f;

    /* 1.1: Inverse Distance (Unclamped) */
    {
        /* At d = d_ref, gain must be exactly 1.0 */
        float g_ref = apu_calc_distance_gain(d_ref, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE);
        assert(float_approx(g_ref, 1.0f));

        /* At d < d_ref, unclamped gain > 1.0: 5 / (5 + 1*(2.5 - 5)) = 5 / 2.5 = 2.0 */
        float g_near = apu_calc_distance_gain(2.5f, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE);
        assert(float_approx(g_near, 2.0f));

        /* At d > d_ref, gain < 1.0: 5 / (5 + 1*(10 - 5)) = 5 / 10 = 0.5 */
        float g_far = apu_calc_distance_gain(10.0f, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE);
        assert(float_approx(g_far, 0.5f));

        /* At d = 20: 5 / (5 + 15) = 0.25 */
        float g_far2 = apu_calc_distance_gain(20.0f, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE);
        assert(float_approx(g_far2, 0.25f));
        printf("    -> Inverse Distance (unclamped): d<d_ref=%.2f, d=d_ref=%.2f, d>d_ref=%.2f [PASS]\n",
               g_near, g_ref, g_far);
    }

    /* 1.2: Inverse Distance Clamped (Default) */
    {
        /* At d = d_ref, gain must be exactly 1.0 */
        float g_ref = apu_calc_distance_gain(d_ref, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE_CLAMPED);
        assert(float_approx(g_ref, 1.0f));

        /* At d < d_ref, clamped to d_ref, so gain == 1.0 */
        float g_near = apu_calc_distance_gain(2.5f, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE_CLAMPED);
        assert(float_approx(g_near, 1.0f));

        /* At d > d_ref, attenuates: 5 / (5 + 1*(10 - 5)) = 0.5 */
        float g_far = apu_calc_distance_gain(10.0f, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE_CLAMPED);
        assert(float_approx(g_far, 0.5f));

        /* Beyond max_dist (d = 100 > 50), clamped to max_dist: 5 / (5 + 45) = 0.1 */
        float g_beyond = apu_calc_distance_gain(100.0f, d_ref, d_max, rolloff, AL_INVERSE_DISTANCE_CLAMPED);
        assert(float_approx(g_beyond, 0.1f));
        printf("    -> Inverse Distance Clamped: d<d_ref=%.2f, d=d_ref=%.2f, d>d_max=%.2f [PASS]\n",
               g_near, g_ref, g_beyond);
    }

    /* 1.3: Linear Distance (Unclamped min, Clamped max) */
    {
        float d_ref_lin = 10.0f;
        float d_max_lin = 110.0f;

        /* At d = d_ref, gain must be exactly 1.0 */
        float g_ref = apu_calc_distance_gain(d_ref_lin, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE);
        assert(float_approx(g_ref, 1.0f));

        /* At d < d_ref, unclamped: 1.0 - 1.0*(5 - 10)/100 = 1.05 */
        float g_near = apu_calc_distance_gain(5.0f, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE);
        assert(float_approx(g_near, 1.05f));

        /* At d = 60, midway: 1.0 - 1.0*(60 - 10)/100 = 0.5 */
        float g_mid = apu_calc_distance_gain(60.0f, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE);
        assert(float_approx(g_mid, 0.5f));

        /* At d = d_max, gain = 0.0 */
        float g_max = apu_calc_distance_gain(d_max_lin, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE);
        assert(float_approx(g_max, 0.0f));
        printf("    -> Linear Distance (unclamped): d<d_ref=%.2f, d=d_ref=%.2f, d=mid=%.2f, d=max=%.2f [PASS]\n",
               g_near, g_ref, g_mid, g_max);
    }

    /* 1.4: Linear Distance Clamped */
    {
        float d_ref_lin = 10.0f;
        float d_max_lin = 110.0f;

        /* At d = d_ref, gain must be exactly 1.0 */
        float g_ref = apu_calc_distance_gain(d_ref_lin, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE_CLAMPED);
        assert(float_approx(g_ref, 1.0f));

        /* At d < d_ref, clamped to d_ref, gain = 1.0 */
        float g_near = apu_calc_distance_gain(5.0f, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE_CLAMPED);
        assert(float_approx(g_near, 1.0f));

        /* At d = 60, midway: 1.0 - 0.5 = 0.5 */
        float g_mid = apu_calc_distance_gain(60.0f, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE_CLAMPED);
        assert(float_approx(g_mid, 0.5f));

        /* Beyond d_max, clamped to 0.0 */
        float g_beyond = apu_calc_distance_gain(150.0f, d_ref_lin, d_max_lin, rolloff, AL_LINEAR_DISTANCE_CLAMPED);
        assert(float_approx(g_beyond, 0.0f));
        printf("    -> Linear Distance Clamped: d<d_ref=%.2f, d=d_ref=%.2f, d>d_max=%.2f [PASS]\n",
               g_near, g_ref, g_beyond);
    }

    /* 1.5: Exponent Distance (Unclamped) */
    {
        float d_ref_exp = 10.0f;
        float d_max_exp = 100.0f;
        float rolloff_exp = 2.0f;

        /* At d = d_ref, gain must be exactly 1.0 */
        float g_ref = apu_calc_distance_gain(d_ref_exp, d_ref_exp, d_max_exp, rolloff_exp, AL_EXPONENT_DISTANCE);
        assert(float_approx(g_ref, 1.0f));

        /* At d < d_ref (d=5), unclamped: (5/10)^(-2) = (0.5)^(-2) = 4.0 */
        float g_near = apu_calc_distance_gain(5.0f, d_ref_exp, d_max_exp, rolloff_exp, AL_EXPONENT_DISTANCE);
        assert(float_approx(g_near, 4.0f));

        /* At d > d_ref (d=20): (20/10)^(-2) = (2)^(-2) = 0.25 */
        float g_far = apu_calc_distance_gain(20.0f, d_ref_exp, d_max_exp, rolloff_exp, AL_EXPONENT_DISTANCE);
        assert(float_approx(g_far, 0.25f));
        printf("    -> Exponent Distance (unclamped): d<d_ref=%.2f, d=d_ref=%.2f, d>d_ref=%.2f [PASS]\n",
               g_near, g_ref, g_far);
    }

    /* 1.6: Exponent Distance Clamped */
    {
        float d_ref_exp = 10.0f;
        float d_max_exp = 100.0f;
        float rolloff_exp = 2.0f;

        /* At d = d_ref, gain must be exactly 1.0 */
        float g_ref = apu_calc_distance_gain(d_ref_exp, d_ref_exp, d_max_exp, rolloff_exp, AL_EXPONENT_DISTANCE_CLAMPED);
        assert(float_approx(g_ref, 1.0f));

        /* At d < d_ref, clamped to d_ref, gain = 1.0 */
        float g_near = apu_calc_distance_gain(5.0f, d_ref_exp, d_max_exp, rolloff_exp, AL_EXPONENT_DISTANCE_CLAMPED);
        assert(float_approx(g_near, 1.0f));

        /* At d = 20, gain = 0.25 */
        float g_far = apu_calc_distance_gain(20.0f, d_ref_exp, d_max_exp, rolloff_exp, AL_EXPONENT_DISTANCE_CLAMPED);
        assert(float_approx(g_far, 0.25f));

        /* Beyond max (d=200 > 100), clamped to max: (100/10)^(-2) = 10^(-2) = 0.01 */
        float g_beyond = apu_calc_distance_gain(200.0f, d_ref_exp, d_max_exp, rolloff_exp, AL_EXPONENT_DISTANCE_CLAMPED);
        assert(float_approx(g_beyond, 0.01f));
        printf("    -> Exponent Distance Clamped: d<d_ref=%.2f, d=d_ref=%.2f, d>d_max=%.4f [PASS]\n",
               g_near, g_ref, g_beyond);
    }

    /* 1.7: AL_NONE Model (No distance attenuation) */
    {
        float g_near = apu_calc_distance_gain(1.0f, d_ref, d_max, rolloff, AL_NONE);
        float g_ref = apu_calc_distance_gain(5.0f, d_ref, d_max, rolloff, AL_NONE);
        float g_far = apu_calc_distance_gain(500.0f, d_ref, d_max, rolloff, AL_NONE);
        assert(float_approx(g_near, 1.0f));
        assert(float_approx(g_ref, 1.0f));
        assert(float_approx(g_far, 1.0f));
        printf("    -> AL_NONE: Constant unity gain across all distances [PASS]\n");
    }

    /*
     * ------------------------------------------------------------------------
     * Test 2: Directional Sound Cone Attenuation Math
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing Directional Sound Cone Attenuation (on-axis, halfway, off-axis)...\n");

    /* Source located at (0, 0, 0) pointing along +Z: (0, 0, 1) */
    float src_pos[3] = {0.0f, 0.0f, 0.0f};
    float src_dir[3] = {0.0f, 0.0f, 1.0f};
    float inner_deg = 60.0f;
    float outer_deg = 120.0f;
    float outer_gain = 0.25f;

    /* On-axis: Listener directly in front at (0, 0, 10). Angle = 0 deg <= 60 deg -> gain = 1.0 */
    float lis_on_axis[3] = {0.0f, 0.0f, 10.0f};
    float cone_on_axis = apu_calc_cone_gain(src_pos, src_dir, lis_on_axis, inner_deg, outer_deg, outer_gain);
    assert(float_approx(cone_on_axis, 1.0f));

    /* Inside inner cone: Listener at (2, 0, 10). Angle = atan(0.2) ~ 11.3 deg <= 60 deg -> gain = 1.0 */
    float lis_inside[3] = {2.0f, 0.0f, 10.0f};
    float cone_inside = apu_calc_cone_gain(src_pos, src_dir, lis_inside, inner_deg, outer_deg, outer_gain);
    assert(float_approx(cone_inside, 1.0f));

    /* Halfway: Listener at (10, 0, 0). Angle = 90 deg (midway between 60 and 120 deg) */
    /* Expected gain = 1.0 - (1.0 - 0.25) * (90 - 60) / (120 - 60) = 1.0 - 0.75 * 0.5 = 0.625 */
    float lis_halfway[3] = {10.0f, 0.0f, 0.0f};
    float cone_halfway = apu_calc_cone_gain(src_pos, src_dir, lis_halfway, inner_deg, outer_deg, outer_gain);
    assert(float_approx(cone_halfway, 0.625f));

    /* Off-axis: Listener directly behind at (0, 0, -10). Angle = 180 deg >= 120 deg -> gain = outer_gain (0.25) */
    float lis_off_axis[3] = {0.0f, 0.0f, -10.0f};
    float cone_off_axis = apu_calc_cone_gain(src_pos, src_dir, lis_off_axis, inner_deg, outer_deg, outer_gain);
    assert(float_approx(cone_off_axis, outer_gain));

    /* Omnidirectional source: Direction (0, 0, 0) must return unity gain everywhere */
    float zero_dir[3] = {0.0f, 0.0f, 0.0f};
    float cone_omni = apu_calc_cone_gain(src_pos, zero_dir, lis_off_axis, inner_deg, outer_deg, outer_gain);
    assert(float_approx(cone_omni, 1.0f));

    /* Full 360-degree sphere cone angles must return unity gain */
    float cone_sphere = apu_calc_cone_gain(src_pos, src_dir, lis_off_axis, 360.0f, 360.0f, outer_gain);
    assert(float_approx(cone_sphere, 1.0f));

    printf("    -> Cone on-axis: %.3f (1.0), halfway: %.3f (0.625), off-axis: %.3f (0.25) [PASS]\n",
           cone_on_axis, cone_halfway, cone_off_axis);

    /*
     * ------------------------------------------------------------------------
     * Test 3: Doppler Pitch Shift Multiplier (Approaching vs Receding)
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing Doppler Pitch Multiplier (Approaching v > 0 vs Receding v < 0)...\n");

    /* Listener at origin (0, 0, 0), stationary (0, 0, 0) */
    float lis_p[3] = {0.0f, 0.0f, 0.0f};
    float lis_v[3] = {0.0f, 0.0f, 0.0f};
    /* Source at (0, 0, 100). Line of sight from source to listener is along -Z */
    float s_p[3] = {0.0f, 0.0f, 100.0f};
    float speed_of_sound = 343.3f;
    float doppler_factor = 1.0f;

    /* 3.1: Stationary source (v = 0) -> pitch multiplier == 1.0 */
    float s_v_stat[3] = {0.0f, 0.0f, 0.0f};
    float pitch_stat = apu_calc_doppler_pitch(s_p, s_v_stat, lis_p, lis_v, doppler_factor, speed_of_sound);
    assert(float_approx(pitch_stat, 1.0f));

    /* 3.2: Approaching source (moving towards origin with velocity (0, 0, -50)) */
    /* v_ss = 50.0 > 0. Expected pitch = 343.3 / (343.3 - 50.0) = 343.3 / 293.3 ~ 1.17047 */
    float s_v_app[3] = {0.0f, 0.0f, -50.0f};
    float pitch_app = apu_calc_doppler_pitch(s_p, s_v_app, lis_p, lis_v, doppler_factor, speed_of_sound);
    float exp_app = speed_of_sound / (speed_of_sound - 50.0f);
    assert(float_approx(pitch_app, exp_app));
    assert(pitch_app > 1.0f);

    /* 3.3: Receding source (moving away from origin with velocity (0, 0, +50)) */
    /* v_ss = -50.0 < 0. Expected pitch = 343.3 / (343.3 + 50.0) = 343.3 / 393.3 ~ 0.87287 */
    float s_v_rec[3] = {0.0f, 0.0f, 50.0f};
    float pitch_rec = apu_calc_doppler_pitch(s_p, s_v_rec, lis_p, lis_v, doppler_factor, speed_of_sound);
    float exp_rec = speed_of_sound / (speed_of_sound + 50.0f);
    assert(float_approx(pitch_rec, exp_rec));
    assert(pitch_rec < 1.0f);

    /* Pitch ordering check: Approaching pitch > Stationary > Receding pitch */
    assert(pitch_app > pitch_stat);
    assert(pitch_stat > pitch_rec);

    /* 3.4: Doppler Factor scaling */
    /* DF = 2.0 exaggerates pitch shift: 343.3 / (343.3 - 2*50) = 343.3 / 243.3 ~ 1.4110 */
    float pitch_app_df2 = apu_calc_doppler_pitch(s_p, s_v_app, lis_p, lis_v, 2.0f, speed_of_sound);
    assert(pitch_app_df2 > pitch_app);

    /* DF = 0.0 disables Doppler shift -> pitch == 1.0 */
    float pitch_app_df0 = apu_calc_doppler_pitch(s_p, s_v_app, lis_p, lis_v, 0.0f, speed_of_sound);
    assert(float_approx(pitch_app_df0, 1.0f));

    printf("    -> Doppler: Stationary=%.4f (1.0), Approaching=%.4f (>1.0), Receding=%.4f (<1.0) [PASS]\n",
           pitch_stat, pitch_app, pitch_rec);

    /*
     * ------------------------------------------------------------------------
     * Test 4: OpenAL 1.1 Global Distance Model & Doppler APIs
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing OpenAL 1.1 Global APIs (alDistanceModel, alDopplerFactor, alSpeedOfSound)...\n");

    /* Default state check */
    assert(apu_spatial_get_distance_model() == AL_INVERSE_DISTANCE_CLAMPED);
    assert(alGetInteger(AL_DISTANCE_MODEL) == AL_INVERSE_DISTANCE_CLAMPED);
    assert(float_approx(apu_spatial_get_doppler_factor(), 1.0f));
    assert(float_approx(alGetFloat(AL_DOPPLER_FACTOR), 1.0f));
    assert(float_approx(apu_spatial_get_speed_of_sound(), 343.3f));
    assert(float_approx(alGetFloat(AL_SPEED_OF_SOUND), 343.3f));

    /* alDistanceModel configuration */
    alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
    assert(alGetError() == AL_NO_ERROR);
    assert(alGetInteger(AL_DISTANCE_MODEL) == AL_LINEAR_DISTANCE_CLAMPED);
    assert(apu_spatial_get_distance_model() == AL_LINEAR_DISTANCE_CLAMPED);

    ALint query_model = 0;
    alGetIntegerv(AL_DISTANCE_MODEL, &query_model);
    assert(query_model == AL_LINEAR_DISTANCE_CLAMPED);

    alDistanceModel(AL_EXPONENT_DISTANCE);
    assert(alGetError() == AL_NO_ERROR);
    assert(alGetInteger(AL_DISTANCE_MODEL) == AL_EXPONENT_DISTANCE);

    alDistanceModel(AL_NONE);
    assert(alGetError() == AL_NO_ERROR);
    assert(alGetInteger(AL_DISTANCE_MODEL) == AL_NONE);

    /* Invalid distance model generates AL_INVALID_VALUE */
    alDistanceModel(0x9999);
    assert(alGetError() == AL_INVALID_VALUE);

    /* Restore default distance model */
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);
    assert(alGetError() == AL_NO_ERROR);

    /* alDopplerFactor configuration */
    alDopplerFactor(2.5f);
    assert(alGetError() == AL_NO_ERROR);
    assert(float_approx(alGetFloat(AL_DOPPLER_FACTOR), 2.5f));
    assert(float_approx(apu_spatial_get_doppler_factor(), 2.5f));

    ALfloat query_df = 0.0f;
    alGetFloatv(AL_DOPPLER_FACTOR, &query_df);
    assert(float_approx(query_df, 2.5f));

    /* Negative Doppler factor generates AL_INVALID_VALUE */
    alDopplerFactor(-1.0f);
    assert(alGetError() == AL_INVALID_VALUE);
    assert(float_approx(alGetFloat(AL_DOPPLER_FACTOR), 2.5f));

    /* Restore default doppler factor */
    alDopplerFactor(1.0f);
    assert(alGetError() == AL_NO_ERROR);

    /* alSpeedOfSound configuration */
    alSpeedOfSound(1125.0f); /* feet per second */
    assert(alGetError() == AL_NO_ERROR);
    assert(float_approx(alGetFloat(AL_SPEED_OF_SOUND), 1125.0f));
    assert(float_approx(apu_spatial_get_speed_of_sound(), 1125.0f));

    /* Non-positive speed of sound generates AL_INVALID_VALUE */
    alSpeedOfSound(0.0f);
    assert(alGetError() == AL_INVALID_VALUE);
    alSpeedOfSound(-100.0f);
    assert(alGetError() == AL_INVALID_VALUE);
    assert(float_approx(alGetFloat(AL_SPEED_OF_SOUND), 1125.0f));

    /* Restore default speed of sound */
    alSpeedOfSound(343.3f);
    assert(alGetError() == AL_NO_ERROR);

    /* alDopplerVelocity (deprecated) */
    alDopplerVelocity(343.3f);
    assert(alGetError() == AL_NO_ERROR);
    alDopplerVelocity(-1.0f);
    assert(alGetError() == AL_INVALID_VALUE);

    printf("    -> Global APIs and state queries verified [PASS]\n");

    /*
     * ------------------------------------------------------------------------
     * Test 5: End-to-End Real-Time APU Voice Synchronization
     * ------------------------------------------------------------------------
     */
    printf("[5] Testing End-to-End APU Voice Hardware Context Real-Time Synchronization...\n");

    ALuint buf = 0;
    alGenBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);

    int16_t pcm[256] = {0};
    alBufferData(buf, AL_FORMAT_MONO16, pcm, sizeof(pcm), 48000);
    assert(alGetError() == AL_NO_ERROR);

    ALuint src = 0;
    alGenSources(1, &src);
    assert(alGetError() == AL_NO_ERROR);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    assert(alGetError() == AL_NO_ERROR);

    /* Configure source spatial properties: reference distance 10.0, max distance 100.0 */
    alSourcef(src, AL_REFERENCE_DISTANCE, 10.0f);
    alSourcef(src, AL_MAX_DISTANCE, 100.0f);
    alSourcef(src, AL_ROLLOFF_FACTOR, 1.0f);
    alDistanceModel(AL_INVERSE_DISTANCE_CLAMPED);

    /* Place source at (0, 0, 10) -> distance is exactly 10.0 (d = d_ref) */
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 10.0f);
    alSourcePlay(src);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *s = al_source_get(src);
    assert(s != NULL && s->hw_voice_idx >= 0);
    uint32_t v_idx = (uint32_t)s->hw_voice_idx;
    NVAPU_VOICE_CONTEXT_3D *vctx = &voice_array[v_idx];

    /* At d = d_ref, master volume must be full scale 0xFFFF */
    assert(vctx->master_vol_left == 0xFFFF);
    assert(vctx->master_vol_right == 0xFFFF);
    assert(vctx->pitch_step == 0x00010000u); /* Unity pitch step for 48kHz mono */

    /* Move source to (0, 0, 20) in real time -> distance = 20 */
    /* Distance gain = 10 / (10 + 1 * (20 - 10)) = 0.5 -> volume ~ 32768 */
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 20.0f);
    assert(alGetError() == AL_NO_ERROR);
    uint16_t exp_vol_half = (uint16_t)(0.5f * 65535.0f + 0.5f);
    assert(abs((int)vctx->master_vol_left - (int)exp_vol_half) <= 1);
    assert(vctx->master_vol_left == vctx->master_vol_right);

    /* Apply approaching velocity (0, 0, -50) in real time */
    alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, -50.0f);
    assert(alGetError() == AL_NO_ERROR);
    /* Doppler pitch multiplier = 343.3 / (343.3 - 50) ~ 1.17047 */
    float exp_doppler_pitch = 343.3f / (343.3f - 50.0f);
    uint32_t exp_pitch_step = APU_CALC_PITCH_STEP(48000, exp_doppler_pitch);
    assert(vctx->pitch_step == exp_pitch_step);
    assert(vctx->pitch_step > 0x00010000u); /* Pitch increased! */

    /* Move listener to (0, 0, 10) in real time -> distance between listener and source becomes 10 */
    alListener3f(AL_POSITION, 0.0f, 0.0f, 10.0f);
    assert(alGetError() == AL_NO_ERROR);
    /* Now distance = |(0,0,20) - (0,0,10)| = 10 == d_ref -> volume restored to 0xFFFF! */
    assert(vctx->master_vol_left == 0xFFFF);

    /* Clean up playback */
    alSourceStop(src);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    al_listener_reset();

    printf("    -> Real-time voice hardware volume and pitch sync verified [PASS]\n");

    /* Teardown */
    free(voice_array);
    free(mock_mmio);
    al_source_cleanup_subsystem();
    al_buffer_cleanup_subsystem();
    apu_mem_shutdown();

    printf("=== All 3D Spatial Attenuation & Doppler Engine Tests Passed Successfully! ===\n");
    return 0;
}
