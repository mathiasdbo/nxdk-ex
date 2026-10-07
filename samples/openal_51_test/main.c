#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "apu_spatial.h"
#include "apu_eeprom.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#ifndef ALC_XBOX_TOPOLOGY
#define ALC_XBOX_TOPOLOGY 0x1014
#endif

/*
 * ============================================================================
 * Audio Stream Constants: 48 kHz Mono 16-bit Seamless Waveforms
 * ============================================================================
 * 4800 mono samples at 48000 Hz = 0.100s (100 ms seamless loop).
 *
 * Satellite speakers:
 *   - 440 Hz fundamental tone (A4) with rich harmonic overtone series.
 *   - 44 integer cycles per 100 ms buffer (440 * 0.1 = 44).
 *   - Perfectly aligned zero-crossings at boundaries ensure zero click/pop.
 *
 * LFE Subwoofer:
 *   - 60 Hz deep bass pure sine wave.
 *   - 6 integer cycles per 100 ms buffer (60 * 0.1 = 6).
 *   - Clean seamless loop for subwoofer crossover testing.
 * ============================================================================
 */
#define SAMPLE_RATE_HZ          48000
#define NUM_MONO_SAMPLES        4800
#define SATELLITE_FREQ_HZ       440
#define LFE_FREQ_HZ             60
#define SATELLITE_AMPLITUDE     24000.0
#define LFE_AMPLITUDE           28000.0

#define FRAMES_PER_MODE         150   /* 150 frames @ 50 FPS = 3.0 seconds */
#define SLEEP_MS                20    /* 20 ms per frame = 50 FPS */
#define TELEMETRY_INTERVAL      25    /* Telemetry output every 500 ms */

static int16_t s_sat_pcm[NUM_MONO_SAMPLES];
static int16_t s_lfe_pcm[NUM_MONO_SAMPLES];

/*
 * Synthesize rich harmonic 440 Hz tone for satellite speaker isolation.
 */
static void synthesize_satellite_waveform(int16_t *buffer, size_t num_samples)
{
    static double s_temp[NUM_MONO_SAMPLES];
    double max_abs = 0.0;

    for (size_t i = 0; i < num_samples; ++i) {
        double t = (double)i / (double)SAMPLE_RATE_HZ;
        double sum = 0.0;
        for (int k = 1; k <= 8; ++k) {
            double freq = (double)(SATELLITE_FREQ_HZ * k);
            double rad = 2.0 * M_PI * freq * t;
            double amp = 1.0 / (double)k;
            sum += amp * sin(rad);
        }
        s_temp[i] = sum;
        if (fabs(sum) > max_abs) {
            max_abs = fabs(sum);
        }
    }

    double scale = (max_abs > 1e-9) ? (SATELLITE_AMPLITUDE / max_abs) : 1.0;
    for (size_t i = 0; i < num_samples; ++i) {
        double val = s_temp[i] * scale;
        buffer[i] = (int16_t)(val + (val >= 0.0 ? 0.5 : -0.5));
    }
}

/*
 * Synthesize pure 60 Hz deep sine wave for LFE / Subwoofer channel isolation.
 */
static void synthesize_lfe_waveform(int16_t *buffer, size_t num_samples)
{
    for (size_t i = 0; i < num_samples; ++i) {
        double t = (double)i / (double)SAMPLE_RATE_HZ;
        double rad = 2.0 * M_PI * (double)LFE_FREQ_HZ * t;
        double val = sin(rad) * LFE_AMPLITUDE;
        buffer[i] = (int16_t)(val + (val >= 0.0 ? 0.5 : -0.5));
    }
}

/*
 * Test Mode Metadata
 */
typedef struct {
    const char *name;
    const char *channel_tag;
    int target_mixbin;
    float x;
    float y;
    float z;
    float azimuth_deg;
} ChannelTestMode;

static const ChannelTestMode s_test_modes[7] = {
    /* Mode 0: FRONT LEFT (FL) - Azimuth 330 deg (-30 deg) */
    { "FRONT LEFT",      "FL",  0, -2.887f, 0.0f, -5.0f, 330.0f },
    /* Mode 1: CENTER (C) - Azimuth 0 deg */
    { "CENTER",          "C",   4,  0.0f,   0.0f, -5.0f,   0.0f },
    /* Mode 2: FRONT RIGHT (FR) - Azimuth 30 deg */
    { "FRONT RIGHT",     "FR",  1,  2.887f, 0.0f, -5.0f,  30.0f },
    /* Mode 3: SURROUND RIGHT (SR) - Azimuth 110 deg */
    { "SURROUND RIGHT",  "SR",  3,  4.698f, 0.0f,  1.710f, 110.0f },
    /* Mode 4: SURROUND LEFT (SL) - Azimuth 250 deg (-110 deg) */
    { "SURROUND LEFT",   "SL",  2, -4.698f, 0.0f,  1.710f, 250.0f },
    /* Mode 5: LFE SUBWOOFER - MixBin 5 */
    { "LFE SUBWOOFER",   "LFE", 5,  0.0f,   0.0f, -5.0f,   0.0f },
    /* Mode 6: SURROUND ORBIT - Dynamic rotary panning */
    { "SURROUND ORBIT",  "ORB", -1, 0.0f,   0.0f, -5.0f,   0.0f }
};

static const char *get_av_pack_name(ALint pack)
{
    switch (pack) {
        case AL_XBOX_AV_PACK_SCART:     return "Advanced SCART (RGB + Optical)";
        case AL_XBOX_AV_PACK_HDTV:      return "HDTV Component (YPbPr + Optical)";
        case AL_XBOX_AV_PACK_VGA:       return "VGA Pack";
        case AL_XBOX_AV_PACK_RFU:       return "RF Modulator";
        case AL_XBOX_AV_PACK_SVIDEO:    return "Advanced S-Video (+ Optical)";
        case AL_XBOX_AV_PACK_COMPOSITE: return "Standard Composite AV";
        case AL_XBOX_AV_PACK_NONE:      return "Disconnected (No Pack)";
        default:                        return "Unknown AV Pack";
    }
}

static const char *get_topology_name(ALCint topo)
{
    switch (topo) {
        case 1:  return "Surround 5.1 (Dolby Digital DSE / Optical S/PDIF)";
        case 0:  return "Stereo 2.0 (AC'97 Analog Downmix)";
        default: return "Unknown Topology";
    }
}

int main(void)
{
    /* Initialize video mode for debug console output */
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("====================================================\n");
    debugPrint("  OpenAL 1.1 Multichannel 5.1 Surround Test Demo    \n");
    debugPrint("  MCPX APU Discrete Channel Isolation & DSE Lock   \n");
    debugPrint("====================================================\n\n");

    /*
     * ------------------------------------------------------------------------
     * Step 1: Initialize OpenAL Device & Context
     * ------------------------------------------------------------------------
     */
    debugPrint("[1/5] Opening default audio device (MCPX APU)...\n");
    ALCdevice *device = alcOpenDevice(NULL);
    if (!device) {
        debugPrint("FATAL: alcOpenDevice failed!\n");
        return 1;
    }

    debugPrint("[2/5] Creating OpenAL 1.1 context...\n");
    ALCcontext *ctx = alcCreateContext(device, NULL);
    if (!ctx) {
        debugPrint("FATAL: alcCreateContext failed!\n");
        alcCloseDevice(device);
        return 1;
    }

    debugPrint("[3/5] Activating context...\n");
    if (!alcMakeContextCurrent(ctx)) {
        debugPrint("FATAL: alcMakeContextCurrent failed!\n");
        alcDestroyContext(ctx);
        alcCloseDevice(device);
        return 1;
    }

    /*
     * ------------------------------------------------------------------------
     * Step 2: Query Hardware Status & Output Topology
     * ------------------------------------------------------------------------
     */
    ALint av_pack = 0;
    ALint dse_active = 0;
    ALCint topology = 0;

    alXboxGetHardwareStatus(AL_XBOX_AV_PACK_TYPE, &av_pack);
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &dse_active);
    alcGetIntegerv(device, ALC_XBOX_TOPOLOGY, 1, &topology);

    debugPrint("\n=== Hardware Configuration Diagnostics ===\n");
    debugPrint("  AV Pack Detected : %s (0x%02X)\n", get_av_pack_name(av_pack), (unsigned int)av_pack);
    debugPrint("  Audio Topology   : %s\n", get_topology_name(topology));
    debugPrint("  Dolby Digital DSE: %s\n", dse_active ? "ACTIVE (Optical TOSLink Lock)" : "INACTIVE (AC'97 Stereo)");
    debugPrint("==========================================\n\n");

    /*
     * ------------------------------------------------------------------------
     * Step 3: Configure Listener Properties
     * ------------------------------------------------------------------------
     */
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const ALfloat listener_ori[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    alListenerfv(AL_ORIENTATION, listener_ori);
    alListenerf(AL_GAIN, 1.0f);

    /*
     * ------------------------------------------------------------------------
     * Step 4: Synthesize Waveforms & Prepare Sources
     * ------------------------------------------------------------------------
     */
    debugPrint("[4/5] Synthesizing 48kHz audio test streams (100 ms loops)...\n");
    synthesize_satellite_waveform(s_sat_pcm, NUM_MONO_SAMPLES);
    synthesize_lfe_waveform(s_lfe_pcm, NUM_MONO_SAMPLES);

    ALuint buffers[2] = { 0, 0 };
    alGenBuffers(2, buffers);
    alBufferData(buffers[0], AL_FORMAT_MONO16, s_sat_pcm, (ALsizei)sizeof(s_sat_pcm), SAMPLE_RATE_HZ);
    alBufferData(buffers[1], AL_FORMAT_MONO16, s_lfe_pcm, (ALsizei)sizeof(s_lfe_pcm), SAMPLE_RATE_HZ);

    debugPrint("[5/5] Allocating hardware voices & configuring sources...\n");
    ALuint src_sat = 0;
    ALuint src_lfe = 0;
    alGenSources(1, &src_sat);
    alGenSources(1, &src_lfe);

    /* Source 1 (Satellite): 440 Hz Rich Tone */
    alSourcei(src_sat, AL_BUFFER, (ALint)buffers[0]);
    alSourcei(src_sat, AL_LOOPING, AL_TRUE);
    alSourcef(src_sat, AL_REFERENCE_DISTANCE, 2.0f);
    alSourcef(src_sat, AL_MAX_DISTANCE, 25.0f);
    alSourcef(src_sat, AL_ROLLOFF_FACTOR, 1.0f);
    alSourcef(src_sat, AL_XBOX_LFE_GAIN, 0.0f);

    /* Source 2 (LFE): 60 Hz Deep Sine Wave */
    alSourcei(src_lfe, AL_BUFFER, (ALint)buffers[1]);
    alSourcei(src_lfe, AL_LOOPING, AL_TRUE);
    alSourcef(src_lfe, AL_REFERENCE_DISTANCE, 2.0f);
    alSourcef(src_lfe, AL_MAX_DISTANCE, 25.0f);
    alSourcef(src_lfe, AL_ROLLOFF_FACTOR, 1.0f);
    alSourcef(src_lfe, AL_XBOX_LFE_GAIN, 1.0f);

    debugPrint("\n=== Starting 5.1 Surround Auto-Tour Validation ===\n\n");

    /*
     * ------------------------------------------------------------------------
     * Step 5: Auto-Tour Interactive Progression Loop
     * ------------------------------------------------------------------------
     */
    int current_mode = -1;
    uint32_t mode_timer = 0;
    uint32_t total_frames = 0;
    float orbit_angle = 0.0f;

    while (1) {
        /* Check for mode transition every FRAMES_PER_MODE (~3 seconds) */
        if (mode_timer == 0) {
            current_mode = (current_mode + 1) % 7;
            const ChannelTestMode *tm = &s_test_modes[current_mode];

            debugPrint("\n>>> [Mode %d/7] %s (Target MixBin: %d) <<<\n",
                       current_mode + 1, tm->name, tm->target_mixbin);

            if (current_mode == 5) {
                /* Mode 5: LFE Subwoofer */
                alSourceStop(src_sat);
                alSource3f(src_lfe, AL_POSITION, 0.0f, 0.0f, -5.0f);
                alSourcef(src_lfe, AL_XBOX_LFE_GAIN, 1.0f);
                alSourcePlay(src_lfe);
            } else if (current_mode == 6) {
                /* Mode 6: Orbit rotary panning */
                alSourceStop(src_lfe);
                orbit_angle = 0.0f;
                alSourcef(src_sat, AL_XBOX_LFE_GAIN, 0.0f);
                alSourcePlay(src_sat);
            } else {
                /* Modes 0..4: Discrete satellite speakers */
                alSourceStop(src_lfe);
                alSourcef(src_sat, AL_XBOX_LFE_GAIN, 0.0f);
                alSource3f(src_sat, AL_POSITION, tm->x, tm->y, tm->z);
                alSourcePlay(src_sat);
            }
        }

        /* Update dynamic motion for Orbit mode (Mode 6) */
        float cur_x = 0.0f;
        float cur_y = 0.0f;
        float cur_z = -5.0f;
        float cur_az = 0.0f;
        float cur_lfe = 0.0f;

        if (current_mode == 6) {
            orbit_angle += 0.5f * 0.02f; /* 0.5 rad/s @ 50 FPS */
            if (orbit_angle >= 2.0f * (float)M_PI) {
                orbit_angle -= 2.0f * (float)M_PI;
            }
            cur_x = 5.0f * sinf(orbit_angle);
            cur_z = -5.0f * cosf(orbit_angle);
            cur_y = 0.0f;
            cur_az = orbit_angle * (180.0f / (float)M_PI);
            alSource3f(src_sat, AL_POSITION, cur_x, cur_y, cur_z);
        } else if (current_mode == 5) {
            cur_x = 0.0f;
            cur_z = -5.0f;
            cur_az = 0.0f;
            cur_lfe = 1.0f;
        } else {
            const ChannelTestMode *tm = &s_test_modes[current_mode];
            cur_x = tm->x;
            cur_y = tm->y;
            cur_z = tm->z;
            cur_az = tm->azimuth_deg;
        }

        /* Periodic diagnostic telemetry */
        if ((mode_timer % TELEMETRY_INTERVAL) == 0) {
            APU_PAN_GAINS_51 gains;
            uint8_t mb[16];
            apu_calc_panning_51(cur_x, cur_z, cur_lfe, &gains);
            apu_calc_mixbin_gains_51(&gains, mb);

            const ChannelTestMode *tm = &s_test_modes[current_mode];
            debugPrint("[%s] Az:%5.1f deg | Pos:(%5.2f, %5.2f) | MixBins: [FL:%3u FR:%3u SL:%3u SR:%3u C:%3u LFE:%3u] | DSE:%d\n",
                       tm->channel_tag,
                       cur_az,
                       cur_x, cur_z,
                       (unsigned int)mb[0], (unsigned int)mb[1],
                       (unsigned int)mb[2], (unsigned int)mb[3],
                       (unsigned int)mb[4], (unsigned int)mb[5],
                       (int)dse_active);
        }

        mode_timer = (mode_timer + 1) % FRAMES_PER_MODE;
        total_frames++;
        Sleep(SLEEP_MS);
    }

    /* Cleanup (unreachable in infinite loop) */
    alSourceStop(src_sat);
    alSourceStop(src_lfe);
    alDeleteSources(1, &src_sat);
    alDeleteSources(1, &src_lfe);
    alDeleteBuffers(2, buffers);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(device);

    return 0;
}
