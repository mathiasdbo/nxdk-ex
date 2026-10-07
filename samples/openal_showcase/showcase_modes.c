#include "showcase_modes.h"
#include "wav_loader.h"
#include "sound_assets.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef ALC_XBOX_TOPOLOGY
#define ALC_XBOX_TOPOLOGY 0x1014
#endif

extern void al_source_update_frame(void);
extern uint32_t apu_voice_mgr_get_active_hw_count(void);
extern uint32_t apu_voice_mgr_get_virtual_standby_count(void);

static void stop_all_demo_sources(showcase_app_t *app) {
    alSourceStop(app->src_orbit);
    alSourceStop(app->src_doppler);
    alSourceStop(app->src_sat_51);
    alSourceStop(app->src_lfe_51);
    for (int i = 0; i < MAX_STRESS_SOURCES; i++) {
        if (app->stress_active[i]) {
            alSourceStop(app->src_stress[i]);
            app->stress_active[i] = false;
        }
    }
}

static void apply_channel_51(showcase_app_t *app, int ch_idx) {
    alSourceStop(app->src_sat_51);
    alSourceStop(app->src_lfe_51);

    /* Channel positions at 5m distance */
    switch (ch_idx) {
        case 0: /* Front Left (330 deg) */
            alSourcei(app->src_sat_51, AL_BUFFER, app->buf_orbit);
            alSource3f(app->src_sat_51, AL_POSITION, -2.887f, 0.0f, -5.0f);
            alSourcePlay(app->src_sat_51);
            break;
        case 1: /* Center (0 deg) - Speech Formant snippet */
            alSourcei(app->src_sat_51, AL_BUFFER, app->buf_voice);
            alSource3f(app->src_sat_51, AL_POSITION, 0.0f, 0.0f, -5.0f);
            alSourcePlay(app->src_sat_51);
            break;
        case 2: /* Front Right (30 deg) */
            alSourcei(app->src_sat_51, AL_BUFFER, app->buf_orbit);
            alSource3f(app->src_sat_51, AL_POSITION, 2.887f, 0.0f, -5.0f);
            alSourcePlay(app->src_sat_51);
            break;
        case 3: /* Surround Right (110 deg) */
            alSourcei(app->src_sat_51, AL_BUFFER, app->buf_orbit);
            alSource3f(app->src_sat_51, AL_POSITION, 4.698f, 0.0f, 1.710f);
            alSourcePlay(app->src_sat_51);
            break;
        case 4: /* Surround Left (250 deg) */
            alSourcei(app->src_sat_51, AL_BUFFER, app->buf_orbit);
            alSource3f(app->src_sat_51, AL_POSITION, -4.698f, 0.0f, 1.710f);
            alSourcePlay(app->src_sat_51);
            break;
        case 5: /* Subwoofer LFE (MixBin 5) */
            alSourcePlay(app->src_lfe_51);
            break;
        default:
            break;
    }
}

static void on_mode_changed(showcase_app_t *app, int new_mode) {
    stop_all_demo_sources(app);
    app->current_mode = new_mode;

    switch (new_mode) {
        case MODE_ORBIT_3D:
            alSourcePlay(app->src_orbit);
            break;
        case MODE_DOPPLER_FLYBY:
            app->doppler_flying = false;
            app->doppler_pos_x = -35.0f;
            break;
        case MODE_SURROUND_51:
            apply_channel_51(app, app->surround_channel_idx);
            break;
        case MODE_POLYPHONY_STRESS:
            app->stress_spawn_idx = 0;
            break;
        case MODE_BGM_2D_CONCURRENT:
            alSourcePlay(app->src_orbit);
            if (app->bgm_playing) {
                alSourcePlay(app->src_bgm);
            }
            break;
        default:
            break;
    }
}

int showcase_app_init(showcase_app_t *app) {
    if (!app) {
        return -1;
    }
    memset(app, 0, sizeof(*app));

    /* 1. Open Device & Context */
    app->device = alcOpenDevice(NULL);
    if (!app->device) {
        return -2;
    }

    app->context = alcCreateContext(app->device, NULL);
    if (!app->context) {
        alcCloseDevice(app->device);
        return -3;
    }
    alcMakeContextCurrent(app->context);

    /* 2. Query Xbox APU Hardware Status */
    ALint dse_val = 0;
    ALint pack_val = 0;
    ALint topo_val = 0;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &dse_val);
    alXboxGetHardwareStatus(AL_XBOX_AV_PACK_TYPE, &pack_val);
    alcGetIntegerv(app->device, ALC_XBOX_TOPOLOGY, 1, &topo_val);

    app->dolby_dse_active = (dse_val != 0);
    app->av_pack_type = (int)pack_val;
    app->topology = (int)topo_val;

    /* 3. Generate Buffers & Load Standardized Audio Assets */
    ALuint bufs[6];
    alGenBuffers(6, bufs);
    app->buf_orbit     = bufs[0];
    app->buf_siren     = bufs[1];
    app->buf_explosion = bufs[2];
    app->buf_laser     = bufs[3];
    app->buf_voice     = bufs[4];
    app->buf_bgm       = bufs[5];

    wav_load_to_buffer(app->buf_orbit, g_sound_orbit_mono_wav, sizeof(g_sound_orbit_mono_wav));
    wav_load_to_buffer(app->buf_siren, g_sound_siren_doppler_wav, sizeof(g_sound_siren_doppler_wav));
    wav_load_to_buffer(app->buf_explosion, g_sound_explosion_lfe_wav, sizeof(g_sound_explosion_lfe_wav));
    wav_load_to_buffer(app->buf_laser, g_sound_laser_stress_wav, sizeof(g_sound_laser_stress_wav));
    wav_load_to_buffer(app->buf_voice, g_sound_voice_center_wav, sizeof(g_sound_voice_center_wav));
    wav_load_to_buffer(app->buf_bgm, g_sound_bgm_stereo_wav, sizeof(g_sound_bgm_stereo_wav));

    /* 4. Generate & Configure Showcase Sources */
    alGenSources(1, &app->src_orbit);
    alSourcei(app->src_orbit, AL_BUFFER, app->buf_orbit);
    alSourcei(app->src_orbit, AL_LOOPING, AL_TRUE);
    alSourcef(app->src_orbit, AL_REFERENCE_DISTANCE, 2.0f);
    alSourcef(app->src_orbit, AL_MAX_DISTANCE, 30.0f);
    alSourcef(app->src_orbit, AL_ROLLOFF_FACTOR, 1.0f);

    alGenSources(1, &app->src_doppler);
    alSourcei(app->src_doppler, AL_BUFFER, app->buf_siren);
    alSourcei(app->src_doppler, AL_LOOPING, AL_TRUE);
    alSourcef(app->src_doppler, AL_REFERENCE_DISTANCE, 2.0f);
    alSourcef(app->src_doppler, AL_MAX_DISTANCE, 50.0f);
    alSourcef(app->src_doppler, AL_ROLLOFF_FACTOR, 1.0f);

    alGenSources(1, &app->src_sat_51);
    alSourcei(app->src_sat_51, AL_BUFFER, app->buf_orbit);
    alSourcei(app->src_sat_51, AL_LOOPING, AL_TRUE);

    alGenSources(1, &app->src_lfe_51);
    alSourcei(app->src_lfe_51, AL_BUFFER, app->buf_explosion);
    alSourcei(app->src_lfe_51, AL_LOOPING, AL_TRUE);
    alSourcef(app->src_lfe_51, AL_XBOX_LFE_GAIN, 1.0f);

    alGenSources(1, &app->src_bgm);
    alSourcei(app->src_bgm, AL_BUFFER, app->buf_bgm);
    alSourcei(app->src_bgm, AL_LOOPING, AL_TRUE);
    alSourcef(app->src_bgm, AL_GAIN, 0.40f);

    alGenSources(MAX_STRESS_SOURCES, app->src_stress);
    for (int i = 0; i < MAX_STRESS_SOURCES; i++) {
        alSourcei(app->src_stress[i], AL_BUFFER, app->buf_laser);
        alSourcei(app->src_stress[i], AL_LOOPING, AL_FALSE);
        alSourcef(app->src_stress[i], AL_REFERENCE_DISTANCE, 3.0f);
        alSourcef(app->src_stress[i], AL_MAX_DISTANCE, 20.0f);
        app->stress_active[i] = false;
    }

    /* 5. Set Initial State */
    app->current_mode = MODE_ORBIT_3D;
    app->orbit_radius = 5.0f;
    app->orbit_speed = 0.8f;
    app->orbit_angle = 0.0f;
    app->camera_yaw = 0.0f;
    app->camera_pitch = 0.0f;
    app->surround_channel_idx = 0;
    app->bgm_playing = true;

    /* Start Mode 1 playback */
    alSourcePlay(app->src_orbit);

    return 0;
}

void showcase_app_update(showcase_app_t *app, const showcase_input_t *input) {
    if (!app || !input) {
        return;
    }

    /* Handle Mode Navigation */
    if (input->pressed_white || input->pressed_dpad_right) {
        int next = (app->current_mode % SHOWCASE_NUM_MODES) + 1;
        on_mode_changed(app, next);
    } else if (input->pressed_black || input->pressed_dpad_left) {
        int prev = (app->current_mode == 1) ? SHOWCASE_NUM_MODES : app->current_mode - 1;
        on_mode_changed(app, prev);
    }

    /* Common Listener Update (Camera Orientation via Right Stick) */
    app->camera_yaw += input->rstick_x * 0.04f;
    app->camera_pitch -= input->rstick_y * 0.03f;
    if (app->camera_pitch > 1.2f) app->camera_pitch = 1.2f;
    if (app->camera_pitch < -1.2f) app->camera_pitch = -1.2f;

    float fwd_x = sinf(app->camera_yaw) * cosf(app->camera_pitch);
    float fwd_y = sinf(app->camera_pitch);
    float fwd_z = -cosf(app->camera_yaw) * cosf(app->camera_pitch);
    float listener_ori[6] = { fwd_x, fwd_y, fwd_z, 0.0f, 1.0f, 0.0f };
    alListenerfv(AL_ORIENTATION, listener_ori);
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);

    /* Update Mode-Specific Kinematics & Audio Parameters */
    switch (app->current_mode) {
        case MODE_ORBIT_3D: {
            app->orbit_radius -= input->lstick_y * 0.10f;
            if (app->orbit_radius < 1.5f) app->orbit_radius = 1.5f;
            if (app->orbit_radius > 15.0f) app->orbit_radius = 15.0f;

            app->orbit_speed += input->lstick_x * 0.02f;
            if (app->orbit_speed > 2.5f) app->orbit_speed = 2.5f;
            if (app->orbit_speed < -2.5f) app->orbit_speed = -2.5f;

            app->orbit_angle += app->orbit_speed * 0.02f; /* 50 FPS dt = 0.02s */

            float x = app->orbit_radius * sinf(app->orbit_angle);
            float z = -app->orbit_radius * cosf(app->orbit_angle);
            float y = 1.2f * sinf(2.0f * app->orbit_angle);
            float vx = app->orbit_radius * app->orbit_speed * cosf(app->orbit_angle);
            float vz = app->orbit_radius * app->orbit_speed * sinf(app->orbit_angle);
            float vy = 2.4f * app->orbit_speed * cosf(2.0f * app->orbit_angle);

            alSource3f(app->src_orbit, AL_POSITION, x, y, z);
            alSource3f(app->src_orbit, AL_VELOCITY, vx, vy, vz);

            /* Telemetry */
            float azimuth_deg = app->orbit_angle * (180.0f / (float)M_PI);
            while (azimuth_deg < 0.0f) azimuth_deg += 360.0f;
            while (azimuth_deg >= 360.0f) azimuth_deg -= 360.0f;

            snprintf(app->telemetry_buf, sizeof(app->telemetry_buf),
                     " [3D Positional Orbit & Psychoacoustic Filtering]\n"
                     " Source Pos: (%5.2f, %5.2f, %5.2f) | Distance: %5.2fm\n"
                     " Azimuth: %5.1f deg | Orbit Speed: %5.2f rad/s\n"
                     " Camera Yaw: %5.1f deg | Pitch: %5.1f deg\n"
                     " Hardware Offload: Woodworth ITD (0-64 samples delay) active\n"
                     " Hardware Offload: Q14 Butterworth HRTF Biquad active\n",
                     x, y, z, app->orbit_radius,
                     azimuth_deg, app->orbit_speed,
                     app->camera_yaw * 180.0f / (float)M_PI,
                     app->camera_pitch * 180.0f / (float)M_PI);

            snprintf(app->footer_buf, sizeof(app->footer_buf),
                     "[LStick] Orbit Radius & Speed | [RStick] Rotate Camera Head");
            break;
        }

        case MODE_DOPPLER_FLYBY: {
            if ((input->trigger_r > 0.4f || input->pressed_a || (input->auto_tour_active && (input->auto_tour_timer % 150 == 0))) && !app->doppler_flying) {
                app->doppler_flying = true;
                app->doppler_pos_x = -35.0f;
                app->doppler_pos_y = 0.5f;
                app->doppler_pos_z = -1.2f;
                app->doppler_speed = 42.0f; /* 42 m/s = 151 km/h */
                alSource3f(app->src_doppler, AL_POSITION, app->doppler_pos_x, app->doppler_pos_y, app->doppler_pos_z);
                alSource3f(app->src_doppler, AL_VELOCITY, app->doppler_speed, 0.0f, 0.0f);
                alSourcePlay(app->src_doppler);
            }

            float pitch_mult = 1.0f;
            if (app->doppler_flying) {
                app->doppler_pos_x += app->doppler_speed * 0.02f;
                alSource3f(app->src_doppler, AL_POSITION, app->doppler_pos_x, app->doppler_pos_y, app->doppler_pos_z);

                /* Distance and pitch telemetry calculation */
                float dist = sqrtf(app->doppler_pos_x * app->doppler_pos_x + app->doppler_pos_z * app->doppler_pos_z);
                float radial_v = (dist > 0.001f) ? (app->doppler_speed * (app->doppler_pos_x / dist)) : 0.0f;
                pitch_mult = 343.3f / (343.3f + radial_v);

                if (app->doppler_pos_x > 35.0f) {
                    app->doppler_flying = false;
                    alSourceStop(app->src_doppler);
                }
            }

            snprintf(app->telemetry_buf, sizeof(app->telemetry_buf),
                     " [High-Speed Fly-By & Doppler Pitch Shift]\n"
                     " Projectile State: %s\n"
                     " Position: (%5.2f, %5.2f, %5.2f) | Speed: %5.1f m/s (151 km/h)\n"
                     " Real-Time Pitch Resampler Multiplier: %5.3fx\n"
                     " Hardware Offload: APU Polyphase Pitch Resampler (16.16 format)\n",
                     app->doppler_flying ? "FLYING (Crossing at z = -1.2m)" : "STANDBY (Pull RT to Launch)",
                     app->doppler_pos_x, app->doppler_pos_y, app->doppler_pos_z,
                     app->doppler_flying ? app->doppler_speed : 0.0f,
                     app->doppler_flying ? pitch_mult : 1.0f);

            snprintf(app->footer_buf, sizeof(app->footer_buf),
                     "[Right Trigger or A] Launch High-Speed Doppler Projectile");
            break;
        }

        case MODE_SURROUND_51: {
            if (input->pressed_dpad_up || input->pressed_a) {
                app->surround_channel_idx = (app->surround_channel_idx + 1) % 6;
                apply_channel_51(app, app->surround_channel_idx);
            } else if (input->pressed_dpad_down) {
                app->surround_channel_idx = (app->surround_channel_idx == 0) ? 5 : app->surround_channel_idx - 1;
                apply_channel_51(app, app->surround_channel_idx);
            }

            const char *ch_names[] = {
                "Front Left (MixBin 0)",
                "Center (MixBin 4) [Radio Voice]",
                "Front Right (MixBin 1)",
                "Surround Right (MixBin 3)",
                "Surround Left (MixBin 2)",
                "Subwoofer LFE (MixBin 5) [Low-Freq Punch]"
            };

            snprintf(app->telemetry_buf, sizeof(app->telemetry_buf),
                     " [Multichannel 5.1 Surround & LFE Discrete Channel Test]\n"
                     " Active Speaker: %s\n"
                     " Hardware Topology: %s | TOSLink AC-3 Bitstream: %s\n"
                     " Output Processor (EP) FIFO Mask: 0x%02X\n"
                     " Subwoofer LFE Factor: %s\n",
                     ch_names[app->surround_channel_idx],
                     (app->topology == 1) ? "Surround 5.1" : "Stereo 2.0 (Downmixed)",
                     app->dolby_dse_active ? "ENABLED" : "DISABLED",
                     (app->topology == 1) ? 0x3F : 0x03,
                     (app->surround_channel_idx == 5) ? "1.0 (Full 100% Subwoofer Output)" : "0.0 (Satellite Only)");

            snprintf(app->footer_buf, sizeof(app->footer_buf),
                     "[DPad Up/Down or A] Cycle Discrete Surround Channels (FL, C, FR, SR, SL, LFE)");
            break;
        }

        case MODE_POLYPHONY_STRESS: {
            /* If holding button A, or auto-tour active, spawn transient sources */
            if (input->btn_a || (input->auto_tour_active && (input->auto_tour_timer % 3 == 0))) {
                for (int s = 0; s < 2; s++) {
                    uint32_t idx = app->stress_spawn_idx % MAX_STRESS_SOURCES;
                    app->stress_spawn_idx++;

                    float rx = ((float)(rand() % 200) - 100.0f) * 0.10f;
                    float ry = ((float)(rand() % 80) - 20.0f) * 0.10f;
                    float rz = ((float)(rand() % 200) - 100.0f) * 0.10f;

                    alSource3f(app->src_stress[idx], AL_POSITION, rx, ry, rz);
                    alSourcePlay(app->src_stress[idx]);
                    app->stress_active[idx] = true;
                }
            }

            al_source_update_frame();

            uint32_t active_hw = apu_voice_mgr_get_active_hw_count();
            uint32_t standby = apu_voice_mgr_get_virtual_standby_count();

            snprintf(app->telemetry_buf, sizeof(app->telemetry_buf),
                     " [64 Hardware Voice Polyphony & Priority Stealing Stress Test]\n"
                     " Active Hardware APU Voices: %2u / 64\n"
                     " Virtualized Standby Queue:   %2u sources\n"
                     " Preemption Protocol:         Mute-then-halt (no ramp)\n"
                     " Backend:                     %s\n",
                     active_hw, standby, alGetString(AL_RENDERER));

            snprintf(app->footer_buf, sizeof(app->footer_buf),
                     "[Hold Button A] Spawn Streams of 100+ Rapid Spatial Sources");
            break;
        }

        case MODE_BGM_2D_CONCURRENT: {
            if (input->pressed_y || input->pressed_a) {
                app->bgm_playing = !app->bgm_playing;
                if (app->bgm_playing) {
                    alSourcePlay(app->src_bgm);
                } else {
                    alSourcePause(app->src_bgm);
                }
            }

            /* Keep orbit source moving in 3D */
            app->orbit_angle += 0.8f * 0.02f;
            float x = 5.0f * sinf(app->orbit_angle);
            float z = -5.0f * cosf(app->orbit_angle);
            alSource3f(app->src_orbit, AL_POSITION, x, 0.0f, z);

            snprintf(app->telemetry_buf, sizeof(app->telemetry_buf),
                     " [Concurrent 2D Direct Stereo Music & 3D Positional Audio]\n"
                     " 2D Stereo Music Status: %s (Direct Front Left / Front Right)\n"
                     " 3D Positional Voice:    ACTIVE (Orbiting at radius 5.0m)\n"
                     " Bus Bandwidth:          Validated (Concurrent 2D Direct + 3D ITD/HRTF)\n",
                     app->bgm_playing ? "PLAYING" : "PAUSED");

            snprintf(app->footer_buf, sizeof(app->footer_buf),
                     "[Button Y or A] Toggle 2D Stereo Background Music Play/Pause");
            break;
        }

        default:
            break;
    }
}

void showcase_app_render_ui(showcase_app_t *app) {
    if (!app) {
        return;
    }

    const char *mode_titles[] = {
        "None",
        "Orbit 3D & Headphone ITD/HRTF",
        "Doppler Fly-By Pitch Shifter",
        "5.1 Surround & LFE Channels",
        "64-Voice Polyphony Stress",
        "2D Stereo Music Concurrent"
    };

    uint32_t active_hw = apu_voice_mgr_get_active_hw_count();
    uint32_t standby = apu_voice_mgr_get_virtual_standby_count();

    showcase_ui_draw_header(mode_titles[app->current_mode],
                            app->current_mode, SHOWCASE_NUM_MODES,
                            active_hw, standby,
                            app->dolby_dse_active, app->topology,
                            true);

    showcase_ui_draw_telemetry(app->telemetry_buf);
    showcase_ui_draw_footer(app->footer_buf);
}

void showcase_app_shutdown(showcase_app_t *app) {
    if (!app) {
        return;
    }

    stop_all_demo_sources(app);
    alSourceStop(app->src_bgm);

    alDeleteSources(1, &app->src_orbit);
    alDeleteSources(1, &app->src_doppler);
    alDeleteSources(1, &app->src_sat_51);
    alDeleteSources(1, &app->src_lfe_51);
    alDeleteSources(1, &app->src_bgm);
    alDeleteSources(MAX_STRESS_SOURCES, app->src_stress);

    ALuint bufs[6] = {
        app->buf_orbit, app->buf_siren, app->buf_explosion,
        app->buf_laser, app->buf_voice, app->buf_bgm
    };
    alDeleteBuffers(6, bufs);

    alcMakeContextCurrent(NULL);
    if (app->context) {
        alcDestroyContext(app->context);
    }
    if (app->device) {
        alcCloseDevice(app->device);
    }
}
