#ifndef SHOWCASE_INPUT_H
#define SHOWCASE_INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Analog sticks: normalized to [-1.0f, +1.0f] */
    float lstick_x;
    float lstick_y;
    float rstick_x;
    float rstick_y;

    /* Analog triggers: normalized to [0.0f, +1.0f] */
    float trigger_l;
    float trigger_r;

    /* Continuous button states (held) */
    bool btn_a;
    bool btn_b;
    bool btn_x;
    bool btn_y;
    bool btn_white;
    bool btn_black;
    bool btn_start;
    bool btn_back;
    bool btn_dpad_up;
    bool btn_dpad_down;
    bool btn_dpad_left;
    bool btn_dpad_right;

    /* Single-frame edge triggers (true only on the frame pressed) */
    bool pressed_a;
    bool pressed_b;
    bool pressed_x;
    bool pressed_y;
    bool pressed_white;
    bool pressed_black;
    bool pressed_start;
    bool pressed_back;
    bool pressed_dpad_up;
    bool pressed_dpad_down;
    bool pressed_dpad_left;
    bool pressed_dpad_right;

    /* Status */
    bool controller_connected;
    bool auto_tour_active;
    uint32_t auto_tour_timer;
} showcase_input_t;

/**
 * Apply analog deadzone filtering.
 * @param val Raw axis value [-1.0f, +1.0f].
 * @return Filtered value.
 */
float showcase_input_apply_deadzone(float val);

/**
 * Initialize controller input subsystem.
 * @return 0 on success.
 */
int showcase_input_init(void);

/**
 * Poll controller input, apply analog deadzones, and calculate single-frame edges.
 * @param out_input Pointer to showcase_input_t structure to populate.
 */
void showcase_input_poll(showcase_input_t *out_input);

/**
 * Shut down controller input subsystem.
 */
void showcase_input_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* SHOWCASE_INPUT_H */
