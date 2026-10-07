#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#if defined(__NXDK__) || defined(_XBOX)
#  include <windows.h>
#  include <hal/debug.h>
#else
#  include <windows.h>
#endif

#include "showcase_input.h"
#include "showcase_ui.h"
#include "showcase_modes.h"

int main(void) {
    /* 1. Initialize UI Framebuffer & Video Mode */
    if (showcase_ui_init() != 0) {
        return 1;
    }

    /* 2. Initialize Gamepad / Auto-Tour Input */
    if (showcase_input_init() != 0) {
        showcase_ui_shutdown();
        return 2;
    }

    /* 3. Initialize OpenAL APU Audio & Asset Subsystem */
    static showcase_app_t app;
    if (showcase_app_init(&app) != 0) {
        showcase_input_shutdown();
        showcase_ui_shutdown();
        return 3;
    }

    showcase_input_t input;
    bool running = true;

    /* 4. Main Event & Audio Simulation Loop (~50 FPS / 20ms frames) */
    while (running) {
        showcase_ui_begin_frame();
        showcase_input_poll(&input);

        /* Exit combo: Back + Start */
        if (input.btn_back && input.btn_start) {
            running = false;
        }

        showcase_app_update(&app, &input);
        showcase_app_render_ui(&app);
        showcase_ui_end_frame();

        Sleep(20);
    }

    /* 5. Clean Teardown */
    showcase_app_shutdown(&app);
    showcase_input_shutdown();
    showcase_ui_shutdown();

    return 0;
}
