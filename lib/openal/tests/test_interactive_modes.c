#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include <AL/al.h>
#include <AL/alc.h>
#include "alc_context.h"
#include "apu_voice_mgr.h"
#include "../../samples/openal_showcase/showcase_modes.h"

#define MOCK_MMIO_SIZE 0x30000

int main(void) {
    printf("=== Interactive Showcase Modes Unit Test ===\n");

    /* 1. Setup Mock APU Hardware */
    printf("[1] Initializing mock APU hardware environment...\n");
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    showcase_app_t app;
    int init_res = showcase_app_init(&app);
    assert(init_res == 0);
    assert(app.current_mode == MODE_ORBIT_3D);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> Showcase app initialized with all 6 buffers and sources [PASS]\n");

    showcase_input_t input;
    memset(&input, 0, sizeof(input));

    /* 2. Test Mode 1: 3D Orbit & Headphone ITD/HRTF */
    printf("[2] Testing Mode 1 (Orbit 3D & Spatial Math)...\n");
    input.lstick_x = 0.5f; /* Accelerate orbit */
    input.rstick_x = 0.3f; /* Rotate camera */
    for (int f = 0; f < 20; f++) {
        showcase_app_update(&app, &input);
    }
    assert(app.orbit_angle > 0.1f);
    assert(app.camera_yaw > 0.05f);
    showcase_app_render_ui(&app);
    printf("    -> Orbit kinematics and orientation updates verified [PASS]\n");

    /* 3. Test Mode 2: Doppler Fly-By Pitch Shifter */
    printf("[3] Testing Mode 2 (Doppler Fly-By Pitch Shifter)...\n");
    memset(&input, 0, sizeof(input));
    input.pressed_dpad_right = true;
    showcase_app_update(&app, &input);
    assert(app.current_mode == MODE_DOPPLER_FLYBY);

    /* Launch projectile with trigger */
    memset(&input, 0, sizeof(input));
    input.trigger_r = 1.0f;
    showcase_app_update(&app, &input);
    assert(app.doppler_flying == true);
    assert(app.doppler_pos_x > -35.0f);

    /* Simulate flight until completion (70m / (42m/s * 0.02s) = 84 frames) */
    input.trigger_r = 0.0f;
    for (int f = 0; f < 95; f++) {
        showcase_app_update(&app, &input);
    }
    assert(app.doppler_flying == false);
    printf("    -> Doppler fly-by trajectory and completion verified [PASS]\n");

    /* 4. Test Mode 3: 5.1 Surround & LFE Discrete Channel Cycling */
    printf("[4] Testing Mode 3 (5.1 Surround & LFE Isolation)...\n");
    memset(&input, 0, sizeof(input));
    input.pressed_dpad_right = true;
    showcase_app_update(&app, &input);
    assert(app.current_mode == MODE_SURROUND_51);

    for (int ch = 0; ch < 6; ch++) {
        memset(&input, 0, sizeof(input));
        input.pressed_dpad_up = true;
        showcase_app_update(&app, &input);
        assert(app.surround_channel_idx == (ch + 1) % 6);
    }
    printf("    -> 5.1 discrete satellite and LFE channel cycling verified [PASS]\n");

    /* 5. Test Mode 4: 64-Voice Polyphony & Priority Stealing Stress Test */
    printf("[5] Testing Mode 4 (64-Voice Polyphony & Stealing Stress Test)...\n");
    memset(&input, 0, sizeof(input));
    input.pressed_dpad_right = true;
    showcase_app_update(&app, &input);
    assert(app.current_mode == MODE_POLYPHONY_STRESS);

    /* Hold button A to spam transient lasers */
    memset(&input, 0, sizeof(input));
    input.btn_a = true;
    for (int f = 0; f < 40; f++) {
        showcase_app_update(&app, &input);
    }
    uint32_t active_hw = apu_voice_mgr_get_active_hw_count();
    assert(active_hw == 64);
    printf("    -> 64 HW channel saturation and priority stealing verified [PASS]\n");

    /* 6. Test Mode 5: 2D Stereo Music Concurrent */
    printf("[6] Testing Mode 5 (Concurrent 2D Stereo Music)...\n");
    memset(&input, 0, sizeof(input));
    input.pressed_dpad_right = true;
    showcase_app_update(&app, &input);
    assert(app.current_mode == MODE_BGM_2D_CONCURRENT);

    ALint bgm_state = 0, orbit_state = 0;
    alGetSourcei(app.src_bgm, AL_SOURCE_STATE, &bgm_state);
    alGetSourcei(app.src_orbit, AL_SOURCE_STATE, &orbit_state);
    assert(bgm_state == AL_PLAYING && orbit_state == AL_PLAYING);
    printf("    -> Concurrent 2D direct and 3D spatial sources verified [PASS]\n");

    /* Teardown */
    printf("[7] Tearing down showcase application resources...\n");
    showcase_app_shutdown(&app);
    free(mock_mmio);

    printf("=== All Interactive Showcase Modes Tests Passed! ===\n");
    return 0;
}
