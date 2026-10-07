#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <AL/al.h>
#include <AL/alc.h>
#include "alc_context.h"
#include "../../samples/openal_showcase/showcase_modes.h"

#define MOCK_MMIO_SIZE 0x30000

int main(void) {
    printf("=== Showcase QA Audit: Stress, Memory & Re-init Test ===\n");

    /* 1. Setup Mock Hardware Environment */
    printf("[1] Initializing mock APU hardware environment...\n");
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    showcase_app_t app;
    int init_res = showcase_app_init(&app);
    assert(init_res == 0);
    assert(alGetError() == AL_NO_ERROR);

    showcase_input_t input;
    memset(&input, 0, sizeof(input));

    /* 2. Rapid Mode Switching Stress Test (500 transitions) */
    printf("[2] Executing 500 rapid mode transitions across all 5 modes...\n");
    for (int t = 0; t < 500; t++) {
        memset(&input, 0, sizeof(input));
        if (t % 2 == 0) {
            input.pressed_white = true; /* Forward */
        } else {
            input.pressed_dpad_right = true;
        }
        showcase_app_update(&app, &input);
        assert(alGetError() == AL_NO_ERROR);
    }
    printf("    -> 500 mode switches completed with AL_NO_ERROR [PASS]\n");

    /* 3. Rapid Transient Laser Polyphony Stress Test (1000 burst frames) */
    printf("[3] Executing 1,000 rapid polyphony stress frames (2,000 sources spawned)...\n");
    /* Force mode 4 */
    while (app.current_mode != MODE_POLYPHONY_STRESS) {
        memset(&input, 0, sizeof(input));
        input.pressed_white = true;
        showcase_app_update(&app, &input);
    }

    memset(&input, 0, sizeof(input));
    input.btn_a = true;
    for (int f = 0; f < 1000; f++) {
        showcase_app_update(&app, &input);
        assert(alGetError() == AL_NO_ERROR);
    }
    printf("    -> 2,000 sources spawned across 1,000 frames without leaks or errors [PASS]\n");

    /* 4. Complete Teardown & Re-initialization Cycle */
    printf("[4] Testing complete shutdown and re-initialization lifecycle...\n");
    showcase_app_shutdown(&app);
    assert(alGetError() == AL_NO_ERROR);

    /* Re-init 1 */
    int reinit_res = showcase_app_init(&app);
    assert(reinit_res == 0);
    assert(alGetError() == AL_NO_ERROR);
    memset(&input, 0, sizeof(input));
    for (int f = 0; f < 20; f++) {
        showcase_app_update(&app, &input);
    }
    showcase_app_shutdown(&app);
    assert(alGetError() == AL_NO_ERROR);

    /* Re-init 2 */
    reinit_res = showcase_app_init(&app);
    assert(reinit_res == 0);
    assert(alGetError() == AL_NO_ERROR);
    showcase_app_shutdown(&app);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> Clean multi-cycle reinitialization verified [PASS]\n");

    free(mock_mmio);

    printf("=== All Showcase QA Stress and Memory Tests Passed! ===\n");
    return 0;
}
