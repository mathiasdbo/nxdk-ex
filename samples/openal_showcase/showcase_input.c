#include "showcase_input.h"
#include <string.h>
#include <math.h>

#if defined(__has_include)
#  if __has_include(<SDL.h>)
#    define HAS_SDL 1
#    include <SDL.h>
#  endif
#endif

#define ANALOG_DEADZONE  0.15f
#define AUTO_TOUR_FRAMES 300  /* 300 frames @ 50 FPS = 6.0 seconds per mode */

static showcase_input_t s_prev_input;
static uint32_t s_auto_tour_counter = 0;
static bool s_has_controller = false;

#if defined(HAS_SDL)
static SDL_GameController *s_pad = NULL;
#endif

float showcase_input_apply_deadzone(float val) {
    if (fabsf(val) < ANALOG_DEADZONE) {
        return 0.0f;
    }
    float sign = (val > 0.0f) ? 1.0f : -1.0f;
    return sign * (fabsf(val) - ANALOG_DEADZONE) / (1.0f - ANALOG_DEADZONE);
}

int showcase_input_init(void) {
    memset(&s_prev_input, 0, sizeof(s_prev_input));
    s_auto_tour_counter = 0;
    s_has_controller = false;

#if defined(HAS_SDL)
    if (SDL_Init(SDL_INIT_GAMECONTROLLER) == 0) {
        /* Open first available game controller */
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (SDL_IsGameController(i)) {
                s_pad = SDL_GameControllerOpen(i);
                if (s_pad != NULL) {
                    s_has_controller = true;
                    break;
                }
            }
        }
    }
#endif

    return 0;
}

void showcase_input_poll(showcase_input_t *out_input) {
    if (!out_input) {
        return;
    }

    memset(out_input, 0, sizeof(*out_input));

#if defined(HAS_SDL)
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            if (s_pad == NULL) {
                s_pad = SDL_GameControllerOpen(e.cdevice.which);
                if (s_pad != NULL) {
                    s_has_controller = true;
                }
            }
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            if (s_pad != NULL && s_pad == SDL_GameControllerFromInstanceID(e.cdevice.which)) {
                SDL_GameControllerClose(s_pad);
                s_pad = NULL;
                s_has_controller = false;
            }
        }
    }

    if (s_pad != NULL) {
        SDL_GameControllerUpdate();

        int16_t lx = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTX);
        int16_t ly = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_LEFTY);
        int16_t rx = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_RIGHTX);
        int16_t ry = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_RIGHTY);
        int16_t tl = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int16_t tr = SDL_GameControllerGetAxis(s_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

        out_input->lstick_x = showcase_input_apply_deadzone((float)lx / 32767.0f);
        out_input->lstick_y = showcase_input_apply_deadzone((float)ly / 32767.0f);
        out_input->rstick_x = showcase_input_apply_deadzone((float)rx / 32767.0f);
        out_input->rstick_y = showcase_input_apply_deadzone((float)ry / 32767.0f);

        out_input->trigger_l = (tl > 0) ? ((float)tl / 32767.0f) : 0.0f;
        out_input->trigger_r = (tr > 0) ? ((float)tr / 32767.0f) : 0.0f;

        out_input->btn_a = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_A) != 0;
        out_input->btn_b = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_B) != 0;
        out_input->btn_x = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_X) != 0;
        out_input->btn_y = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_Y) != 0;
        out_input->btn_white = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_LEFTSHOULDER) != 0;
        out_input->btn_black = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) != 0;
        out_input->btn_start = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_START) != 0;
        out_input->btn_back = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_BACK) != 0;
        out_input->btn_dpad_up = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_UP) != 0;
        out_input->btn_dpad_down = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) != 0;
        out_input->btn_dpad_left = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) != 0;
        out_input->btn_dpad_right = SDL_GameControllerGetButton(s_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) != 0;

        out_input->controller_connected = true;
        out_input->auto_tour_active = false;
        s_auto_tour_counter = 0;
    }
#endif

    if (!s_has_controller) {
        /* Headless / Auto-Tour Mode */
        out_input->controller_connected = false;
        out_input->auto_tour_active = true;
        s_auto_tour_counter++;
        out_input->auto_tour_timer = s_auto_tour_counter;

        /* Auto-advance mode every AUTO_TOUR_FRAMES */
        if (s_auto_tour_counter >= AUTO_TOUR_FRAMES) {
            out_input->pressed_dpad_right = true;
            out_input->pressed_white = true;
            s_auto_tour_counter = 0;
        }
    }

    /* Compute single-frame edge triggers */
    out_input->pressed_a = (out_input->btn_a && !s_prev_input.btn_a);
    out_input->pressed_b = (out_input->btn_b && !s_prev_input.btn_b);
    out_input->pressed_x = (out_input->btn_x && !s_prev_input.btn_x);
    out_input->pressed_y = (out_input->btn_y && !s_prev_input.btn_y);
    out_input->pressed_white = out_input->pressed_white || (out_input->btn_white && !s_prev_input.btn_white);
    out_input->pressed_black = (out_input->btn_black && !s_prev_input.btn_black);
    out_input->pressed_start = (out_input->btn_start && !s_prev_input.btn_start);
    out_input->pressed_back = (out_input->btn_back && !s_prev_input.btn_back);
    out_input->pressed_dpad_up = (out_input->btn_dpad_up && !s_prev_input.btn_dpad_up);
    out_input->pressed_dpad_down = (out_input->btn_dpad_down && !s_prev_input.btn_dpad_down);
    out_input->pressed_dpad_left = (out_input->btn_dpad_left && !s_prev_input.btn_dpad_left);
    out_input->pressed_dpad_right = out_input->pressed_dpad_right || (out_input->btn_dpad_right && !s_prev_input.btn_dpad_right);

    s_prev_input = *out_input;
}

void showcase_input_shutdown(void) {
#if defined(HAS_SDL)
    if (s_pad != NULL) {
        SDL_GameControllerClose(s_pad);
        s_pad = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
#endif
    s_has_controller = false;
}
