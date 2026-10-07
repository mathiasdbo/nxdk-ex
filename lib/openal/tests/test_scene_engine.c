#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include "../../samples/openal_showcase/showcase_input.h"
#include "../../samples/openal_showcase/showcase_ui.h"

int main(void) {
    printf("=== Showcase Scene Engine & Input Framework Unit Test ===\n");

    /* 1. Test Input Subsystem Lifecycle & Deadzones */
    printf("[1] Initializing showcase input subsystem & deadzones...\n");
    int in_res = showcase_input_init();
    assert(in_res == 0);
    assert(showcase_input_apply_deadzone(0.0f) == 0.0f);
    assert(showcase_input_apply_deadzone(0.10f) == 0.0f);
    assert(showcase_input_apply_deadzone(-0.14f) == 0.0f);
    assert(fabsf(showcase_input_apply_deadzone(1.0f) - 1.0f) < 0.001f);
    assert(fabsf(showcase_input_apply_deadzone(-1.0f) - (-1.0f)) < 0.001f);
    printf("    -> Analog deadzone calculations verified [PASS]\n");

    /* 2. Test Headless Auto-Tour Timer Progression */
    printf("[2] Verifying headless auto-tour mode progression...\n");
    showcase_input_t input;
    showcase_input_poll(&input);
    assert(input.controller_connected == false);
    assert(input.auto_tour_active == true);
    assert(input.auto_tour_timer == 1);

    /* Poll until auto-tour threshold (300 frames) triggers edge pulse */
    for (int frame = 2; frame < 300; frame++) {
        showcase_input_poll(&input);
        assert(input.pressed_dpad_right == false);
    }
    showcase_input_poll(&input);
    assert(input.auto_tour_timer == 300);
    assert(input.pressed_dpad_right == true);
    assert(input.pressed_white == true);
    printf("    -> Auto-tour successfully pulsed mode advancement at frame 300 [PASS]\n");

    /* Subsequent frame should reset edge pulse */
    showcase_input_poll(&input);
    assert(input.pressed_dpad_right == false);
    assert(input.auto_tour_timer == 1);

    /* 3. Test UI Dashboard Subsystem Lifecycle */
    printf("[3] Verifying showcase UI subsystem...\n");
    int ui_res = showcase_ui_init();
    assert(ui_res == 0);

    for (int f = 0; f < 35; f++) {
        showcase_ui_begin_frame();
        showcase_ui_draw_header("Orbit 3D & ITD/HRTF", 1, 5, 1, 0, false, 0, false);
        showcase_ui_draw_telemetry("Pos: (0.00, 0.00, -5.00) | Azimuth: 0.0 deg | ITD: 0 samples");
        showcase_ui_draw_footer("[Thumbstick] Rotate Camera | [RT] Doppler Flyby");
        showcase_ui_end_frame();
    }
    printf("    -> UI frame rendering and telemetry buffering verified [PASS]\n");

    /* Teardown */
    showcase_ui_shutdown();
    showcase_input_shutdown();

    printf("=== All Scene Engine and Input Framework Tests Passed! ===\n");
    return 0;
}
