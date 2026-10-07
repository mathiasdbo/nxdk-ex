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

    /* Mode 1: Orbit State */
    float orbit_angle;
    float orbit_radius;
    float orbit_speed;
    float camera_yaw;
    float camera_pitch;

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
 * Release all OpenAL resources, stop sources, and close device.
 * @param app Pointer to showcase_app_t structure.
 */
void showcase_app_shutdown(showcase_app_t *app);

#ifdef __cplusplus
}
#endif

#endif /* SHOWCASE_MODES_H */
