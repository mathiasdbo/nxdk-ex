#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include <AL/al.h>
#include <AL/alc.h>
#include "alc_context.h"
#include "../../samples/openal_showcase/showcase_modes.h"
#include "../../samples/openal_showcase/showcase_scene.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MOCK_MMIO_SIZE 0x30000

/* Every vertex is a finite NDC position the rasteriser can take */
static void check_frame(const char *what) {
    showcase_scene_stats_t st;
    uint32_t i;

    showcase_scene_get_stats(&st);
    assert(st.vertices != NULL);
    assert(st.vertex_count > 0);
    assert(st.vertex_count <= st.vertex_capacity);
    assert(st.batch_count > 0);
    assert(!st.overflow);
    for (i = 0; i < st.vertex_count; i++) {
        const showcase_scene_vertex_t *v = &st.vertices[i];
        assert(isfinite(v->x) && isfinite(v->y) && isfinite(v->z));
        /* Screen-space guard band used by the scene: about +-4x the screen */
        assert(fabsf(v->x) < 8.0f && fabsf(v->y) < 12.0f);
    }
    printf("    -> %-28s %6u vertices, %3u batches [PASS]\n", what,
           (unsigned)st.vertex_count, (unsigned)st.batch_count);
}

static void check_legend(const showcase_app_t *app) {
    showcase_legend_entry_t e[SHOWCASE_LEGEND_MAX_ENTRIES];
    int n = showcase_app_get_legend(app, e, SHOWCASE_LEGEND_MAX_ENTRIES);
    int i;
    int saw_next = 0, saw_exit = 0, saw_walk = 0, saw_reset = 0, left = 0, right = 0;

    /* Mode controls + 3 global entries, each with an input and a label */
    assert(n >= 4 && n <= SHOWCASE_LEGEND_MAX_ENTRIES);
    for (i = 0; i < n; i++) {
        assert(e[i].buttons[0] != SHOWCASE_BTN_NONE);
        assert(e[i].action != NULL && e[i].action[0] != '\0');
        /* Fits the legend column next to the icons */
        assert(strlen(e[i].action) + (e[i].hold ? 5u : 0u) <= 28u);
        if (e[i].buttons[0] == SHOWCASE_BTN_WHITE && e[i].buttons[1] == SHOWCASE_BTN_BLACK && e[i].global) {
            saw_next = 1;
        }
        if (e[i].buttons[0] == SHOWCASE_BTN_X && e[i].global) saw_walk = 1;
        if (e[i].buttons[0] == SHOWCASE_BTN_B && e[i].global) saw_reset = 1;
        if (e[i].global) right++; else left++;
        if (e[i].combo && e[i].buttons[0] == SHOWCASE_BTN_BACK && e[i].buttons[1] == SHOWCASE_BTN_START) {
            saw_exit = 1;
        }
    }
    assert(saw_next && saw_exit && saw_walk && saw_reset);
    assert(left <= 4 && right <= 4);   /* four legend rows per column */

    /* Truncation is safe */
    assert(showcase_app_get_legend(app, e, 1) == 1);
    assert(showcase_app_get_legend(app, e, 0) == 0);
    assert(showcase_app_get_legend(NULL, e, 4) == 0);
}

int main(void) {
    showcase_app_t app;
    showcase_input_t input;
    uint8_t *mock_mmio;
    int mode, f;

    printf("=== Showcase NV2A Scene (CPU frame build) Test ===\n");

    mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    assert(showcase_app_init(&app) == 0);
    assert(showcase_scene_init() == 0);
    memset(&input, 0, sizeof(input));

    /* 1. Camera: the chase camera looks the way the listener faces */
    printf("[1] Projection follows the listener orientation...\n");
    showcase_scene_build(&app, &input);
    {
        float ahead[3] = { 0.0f, 0.0f, -6.0f };   /* listener faces -Z at yaw 0 */
        float right[3] = { 4.0f, 0.0f, -6.0f };
        float behind_cam[3] = { 0.0f, 0.0f, 60.0f };
        float ax, ay, rx, ry, bx, by;
        assert(showcase_scene_project(ahead, &ax, &ay));
        assert(fabsf(ax - SHOWCASE_SCENE_VIEW_CX) < 1.0f);
        assert(ay > 0.0f && ay < SHOWCASE_SCENE_HEIGHT);
        assert(showcase_scene_project(right, &rx, &ry));
        assert(rx > ax + 20.0f);                  /* listener's right is screen right */
        assert(!showcase_scene_project(behind_cam, &bx, &by));
    }
    printf("    -> Ahead projects to the view center, right to the right, behind is culled [PASS]\n");

    /* Turn the head 90 degrees right: world +X is now straight ahead */
    app.camera_yaw = (float)(M_PI / 2.0);
    app.camera_pitch = 0.0f;
    for (f = 0; f < 3; f++) {
        showcase_scene_build(&app, &input);
    }
    {
        float east[3] = { 6.0f, 0.0f, 0.0f };
        float ex, ey;
        assert(showcase_scene_project(east, &ex, &ey));
        assert(fabsf(ex - SHOWCASE_SCENE_VIEW_CX) < 1.0f);
    }
    app.camera_yaw = 0.0f;
    printf("    -> Camera follows a 90 degree head turn [PASS]\n");

    /* 2. Every mode builds a bounded, finite frame and has a legend */
    printf("[2] Building every mode...\n");
    for (mode = 1; mode <= SHOWCASE_NUM_MODES; mode++) {
        char what[64];
        memset(&input, 0, sizeof(input));
        input.controller_connected = (mode % 2) == 0;
        for (f = 0; f < 40; f++) {
            /* Exercise the mode: launch, cycle speakers, spawn sources */
            input.btn_a = (mode == MODE_POLYPHONY_STRESS);
            input.pressed_a = (f == 1);
            input.rstick_x = 0.4f;
            showcase_app_update(&app, &input);
            showcase_scene_build(&app, &input);
        }
        assert(app.current_mode == mode);
        snprintf(what, sizeof(what), "mode %d %s", mode,
                 mode == MODE_POLYPHONY_STRESS ? "(64+ sources)" : "");
        check_frame(what);
        check_legend(&app);
        assert(strlen(showcase_app_mode_title(mode)) > 0);

        memset(&input, 0, sizeof(input));
        input.pressed_dpad_right = true;
        showcase_app_update(&app, &input);
    }
    assert(app.current_mode == MODE_ORBIT_3D);
    assert(strcmp(showcase_app_mode_title(0), "") == 0);
    assert(strcmp(showcase_app_mode_title(SHOWCASE_NUM_MODES + 1), "") == 0);
    printf("    -> All %d modes build a valid frame with a legend [PASS]\n", SHOWCASE_NUM_MODES);

    /* 3. Head pitched fully up/down and an orbit at the clamp limits stay valid */
    printf("[3] Extreme camera and orbit values...\n");
    app.camera_pitch = 1.2f;
    app.orbit_radius = 15.0f;
    showcase_scene_build(&app, &input);
    check_frame("pitch +1.2, radius 15 m");
    app.camera_pitch = -1.2f;
    app.orbit_radius = 1.5f;
    showcase_scene_build(&app, &input);
    check_frame("pitch -1.2, radius 1.5 m");

    /* 4. Repeated init/shutdown */
    printf("[4] Walking (X) and reset (B)...\n");
    memset(&input, 0, sizeof(input));
    app.camera_yaw = 0.0f;
    app.camera_pitch = 0.0f;
    app.walk_mode = false;
    input.pressed_x = true;
    showcase_app_update(&app, &input);
    assert(app.walk_mode);
    check_legend(&app);
    input.pressed_x = false;
    input.lstick_y = -1.0f;                      /* stick up = forward = -Z at yaw 0 */
    for (f = 0; f < 50; f++) {                   /* 50 x 20 ms at 3 m/s */
        showcase_app_update(&app, &input);
    }
    assert(app.listener_pos[2] < -2.9f && app.listener_pos[2] > -3.1f);
    assert(fabsf(app.listener_pos[0]) < 1e-3f);
    {
        float lp[3], ahead[3], ax, ay;
        alGetListener3f(AL_POSITION, &lp[0], &lp[1], &lp[2]);
        assert(fabsf(lp[2] - app.listener_pos[2]) < 1e-4f);   /* the OpenAL listener moved */
        for (f = 0; f < 3; f++) {
            showcase_scene_build(&app, &input);
        }
        check_frame("walked 3 m forward");
        ahead[0] = lp[0];
        ahead[1] = 0.0f;
        ahead[2] = lp[2] - 6.0f;
        assert(showcase_scene_project(ahead, &ax, &ay));
        assert(fabsf(ax - SHOWCASE_SCENE_VIEW_CX) < 1.0f);   /* the camera follows the listener */
    }
    memset(&input, 0, sizeof(input));
    input.pressed_b = true;
    app.camera_yaw = 1.0f;
    showcase_app_update(&app, &input);
    assert(app.listener_pos[0] == 0.0f && app.listener_pos[2] == 0.0f && app.camera_yaw == 0.0f);
    input.pressed_b = false;
    input.pressed_x = true;
    showcase_app_update(&app, &input);
    assert(!app.walk_mode);
    printf("    -> X toggles walking, the stick moves the listener at 3 m/s, B resets [PASS]\n");

    printf("[5] Scene lifecycle...\n");
    showcase_scene_shutdown();
    showcase_scene_shutdown();
    assert(showcase_scene_init() == 0);
    showcase_scene_build(&app, &input);
    check_frame("after re-init");
    showcase_scene_shutdown();
    showcase_scene_build(&app, &input); /* no buffer: must not crash */
    {
        showcase_scene_stats_t st;
        showcase_scene_get_stats(&st);
        assert(st.vertex_count == 0 && st.vertices == NULL);
    }
    printf("    -> Init/shutdown cycles and build without a buffer [PASS]\n");

    showcase_app_shutdown(&app);
    alc_set_apu_base(0);
    free(mock_mmio);

    printf("=== All Showcase Scene Tests Passed Successfully! ===\n");
    return 0;
}
