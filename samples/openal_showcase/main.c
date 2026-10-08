#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#if defined(NXDK) || defined(__NXDK__) || defined(_XBOX)
#  include <windows.h>
#  include <hal/debug.h>
#else
#  include <windows.h>
#endif

#include <AL/al.h>
#include <AL/alext.h>

#include "showcase_input.h"
#include "showcase_ui.h"
#include "showcase_modes.h"
#include "showcase_scene.h"
#include "showcase_audio.h"

int main(void) {
    /* 1. Initialize UI Framebuffer & Video Mode */
    if (showcase_ui_init() != 0) {
        return 1;
    }

    /* 2. NV2A 3D scene and HUD. Without it the text dashboard is shown instead. */
    bool scene_ok = (showcase_scene_init() == 0);
    showcase_ui_set_text_overlay(!scene_ok);

    /* 3. Initialize Gamepad / Auto-Tour Input */
    if (showcase_input_init() != 0) {
        showcase_scene_shutdown();
        showcase_ui_shutdown();
        return 2;
    }

    /* 4. Initialize OpenAL APU Audio & Asset Subsystem */
    static showcase_app_t app;
    if (showcase_app_init(&app) != 0) {
        showcase_input_shutdown();
        showcase_scene_shutdown();
        showcase_ui_shutdown();
        return 3;
    }

    /* 5. Software mix of the OpenAL sources to the AC97 output (the null
     *    backend drives no audio hardware) */
    bool audio_ok = (showcase_audio_init() == 0);
    showcase_scene_set_audio_output(audio_ok && showcase_audio_active());

    showcase_input_t input;
    bool running = true;

    /* Frame timing for the HUD (QueryPerformanceCounter ticks) */
    LARGE_INTEGER freq, t_frame, t_prev, t_a, t_b;
    showcase_perf_t perf = { 0.0f, 0.0f, 0.0f, 0, 0 };
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t_prev);

    /* 6. Main loop, one iteration per video frame */
    while (running) {
        showcase_ui_begin_frame();
        showcase_input_poll(&input);

        /* Exit combo: Back + Start */
        if (input.btn_back && input.btn_start) {
            running = false;
        }

        showcase_app_update(&app, &input);

        /* Library frame tick: reaps finished voices, promotes waiting sources
         * (and services the APU idle trap on the hardware backend) */
        alXboxUpdateVoices();

        QueryPerformanceCounter(&t_a);
        if (audio_ok) {
            showcase_audio_update();
        }
        QueryPerformanceCounter(&t_b);
        /* Smoothed: one mixed chunk is 10.7 ms of audio, so per-frame cost jitters */
        perf.audio_ms += (1000.0f * (float)(t_b.QuadPart - t_a.QuadPart) / (float)freq.QuadPart - perf.audio_ms) * 0.1f;
        if (audio_ok) {
            showcase_audio_get_stats(&perf.voices, &perf.underruns);
        }

        if (scene_ok) {
            QueryPerformanceCounter(&t_a);
            showcase_scene_set_perf(&perf);
            showcase_scene_build(&app, &input);
            showcase_scene_submit();
            QueryPerformanceCounter(&t_b);
            perf.scene_ms += (1000.0f * (float)(t_b.QuadPart - t_a.QuadPart) / (float)freq.QuadPart - perf.scene_ms) * 0.1f;
        }
        showcase_app_render_ui(&app);
        showcase_ui_end_frame();

        /* The 3D frame is paced by the vertical blank (pb_wait_for_vbl in
         * showcase_ui_begin_frame); only the text fallback needs a sleep */
        if (!scene_ok) {
            Sleep(20);
        }

        QueryPerformanceCounter(&t_frame);
        if (t_frame.QuadPart > t_prev.QuadPart) {
            float frame_s = (float)(t_frame.QuadPart - t_prev.QuadPart) / (float)freq.QuadPart;
            float fps = 1.0f / frame_s;
            /* Motion uses the real frame time, clamped against stalls */
            app.dt = (frame_s < 0.005f) ? 0.005f : ((frame_s > 0.05f) ? 0.05f : frame_s);
            perf.fps = (perf.fps <= 0.0f) ? fps : perf.fps + (fps - perf.fps) * 0.1f;
        }
        t_prev = t_frame;
    }

    /* 7. Clean Teardown */
    showcase_audio_shutdown();
    showcase_app_shutdown(&app);
    showcase_input_shutdown();
    showcase_scene_shutdown();
    showcase_ui_shutdown();

    return 0;
}
