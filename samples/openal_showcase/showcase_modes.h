#ifndef SHOWCASE_MODES_H
#define SHOWCASE_MODES_H

#include <stdint.h>
#include <stdbool.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "showcase_input.h"
#include "showcase_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SHOWCASE_NUM_MODES 5

#define MODE_ORBIT_3D        1
#define MODE_DOPPLER_FLYBY   2
#define MODE_SURROUND_51     3
#define MODE_POLYPHONY_STRESS 4
#define MODE_BGM_2D_CONCURRENT 5

#define MAX_STRESS_SOURCES   128

/* Controller inputs named by the on-screen legend */
typedef enum {
    SHOWCASE_BTN_NONE = 0,
    SHOWCASE_BTN_A,
    SHOWCASE_BTN_B,
    SHOWCASE_BTN_X,
    SHOWCASE_BTN_Y,
    SHOWCASE_BTN_WHITE,
    SHOWCASE_BTN_BLACK,
    SHOWCASE_BTN_START,
    SHOWCASE_BTN_BACK,
    SHOWCASE_BTN_DPAD_LEFT_RIGHT,
    SHOWCASE_BTN_DPAD_UP_DOWN,
    SHOWCASE_BTN_DPAD_LEFT,
    SHOWCASE_BTN_DPAD_RIGHT,
    SHOWCASE_BTN_LSTICK,
    SHOWCASE_BTN_RSTICK,
    SHOWCASE_BTN_LTRIGGER,
    SHOWCASE_BTN_RTRIGGER
} showcase_button_t;

#define SHOWCASE_LEGEND_MAX_BUTTONS 2
#define SHOWCASE_LEGEND_MAX_ENTRIES 8

/* One legend line: up to two alternative inputs and the action they trigger */
typedef struct {
    showcase_button_t buttons[SHOWCASE_LEGEND_MAX_BUTTONS];
    const char *action;
    bool hold;          /* input must be held, not just pressed */
    bool combo;         /* buttons pressed together, not alternatives */
    bool global;        /* applies in every mode (drawn in the right column) */
} showcase_legend_entry_t;

typedef struct {
    /* OpenAL Context & Device */
    ALCdevice  *device;
    ALCcontext *context;

    /* Hardware & System Status */
    int  topology;          /* 0 = Stereo 2.0, 1 = Surround 5.1 */
    bool dolby_dse_active;
    int  av_pack_type;

    /* Buffers */
    ALuint buf_orbit;
    ALuint buf_siren;
    ALuint buf_explosion;
    ALuint buf_laser;
    ALuint buf_voice;
    ALuint buf_bgm;
    ALuint buf_rifle;
    ALuint buf_glass;

    /* Primary Showcase Sources */
    ALuint src_orbit;
    ALuint src_doppler;
    ALuint src_sat_51;
    ALuint src_lfe_51;
    ALuint src_bgm;

    /* Stress Test Pool (up to 128 virtual sources) */
    ALuint src_stress[MAX_STRESS_SOURCES];
    bool   stress_active[MAX_STRESS_SOURCES];
    uint32_t stress_spawn_idx;

    /* Mode State */
    int current_mode;
    float dt;               /* seconds per update (set by the main loop; 0.02 = 50 Hz by default) */

    /* Mode 1: Orbit State */
    float orbit_angle;
    float orbit_radius;
    float orbit_speed;
    float camera_yaw;
    float camera_pitch;

    /* Listener walk (X toggles walk mode, B resets position and head) */
    bool  walk_mode;
    float listener_pos[3];
    float listener_vel[3];

    /* Mode 2: Doppler State */
    bool  doppler_flying;
    float doppler_pos_x;
    float doppler_pos_y;
    float doppler_pos_z;
    float doppler_vel_x;
    float doppler_speed;

    /* Mode 3: 5.1 Surround State */
    int   surround_channel_idx; /* 0: FL, 1: C, 2: FR, 3: SR, 4: SL, 5: LFE, 6: Orbit */

    /* Mode 5: 2D BGM State */
    bool  bgm_playing;

    /* Telemetry buffer */
    char telemetry_buf[1024];
    char footer_buf[256];
} showcase_app_t;

/**
 * Initialize OpenAL device, buffers, and sources.
 * @param app Pointer to showcase_app_t structure.
 * @return 0 on success, negative error code on failure.
 */
int showcase_app_init(showcase_app_t *app);

/**
 * Update active mode logic, kinematics, and source parameters.
 * @param app Pointer to showcase_app_t structure.
 * @param input Pointer to current polled input state.
 */
void showcase_app_update(showcase_app_t *app, const showcase_input_t *input);

/**
 * Render on-screen telemetry dashboard for the current active mode.
 * @param app Pointer to showcase_app_t structure.
 */
void showcase_app_render_ui(showcase_app_t *app);

/**
 * Short title of a mode (1..SHOWCASE_NUM_MODES), "" for anything else.
 */
const char *showcase_app_mode_title(int mode);

/**
 * Controller legend for the current mode: the mode's own controls first,
 * then the global ones (mode navigation, exit).
 * @param app Pointer to showcase_app_t structure.
 * @param out Destination array.
 * @param max Capacity of out.
 * @return Number of entries written.
 */
int showcase_app_get_legend(const showcase_app_t *app, showcase_legend_entry_t *out, int max);

/**
 * Release all OpenAL resources, stop sources, and close device.
 * @param app Pointer to showcase_app_t structure.
 */
void showcase_app_shutdown(showcase_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* SHOWCASE_MODES_H */
