#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <AL/al.h>
#include <AL/alc.h>
#include "apu_spatial.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * ============================================================================
 * Audio Stream Constants: 48 kHz Mono 16-bit Rich Harmonic Tone
 * ============================================================================
 * 4800 mono samples at 48000 Hz = 0.100s (100 ms seamless loop).
 * Fundamental frequency: 440 Hz (A4).
 * Over 0.100s, 440 Hz completes exactly 44 integer cycles (440 * 0.100 = 44).
 * Each harmonic k completes exactly k * 44 integer cycles.
 * Because every harmonic phase aligns perfectly at sample 0 and sample 4800,
 * the 100 ms buffer loops seamlessly with zero phase discontinuities or clicks.
 *
 * Rich harmonics up to ~16 kHz (36 harmonics) give the library's HRTF
 * pinna/elevation biquad model (10 kHz - 18 kHz cutoff) and its Woodworth ITD
 * model broadband content to act on. The default backend outputs no sound, so
 * none of this is audible unless a real output path is added.
 * ============================================================================
 */
#define SAMPLE_RATE_HZ          48000
#define TONE_FREQUENCY_HZ       440
#define NUM_MONO_SAMPLES        4800
#define HARMONIC_COUNT          36
#define TARGET_PEAK_AMPLITUDE   28000.0

/*
 * ============================================================================
 * 3D Orbit Motion Dynamics
 * ============================================================================
 * Radius: R = 5.0 m
 * Angular Velocity: omega = 0.5 rad/s (~12.57 seconds per full orbit)
 * Horizontal Position: x = R * sin(theta), z = -R * cos(theta)
 *   theta = 0:    Front (0, 0, -R)
 *   theta = pi/2: Right (R, 0, 0)
 *   theta = pi:   Rear  (0, 0, R)
 *   theta = 3pi/2:Left  (-R, 0, 0)
 * Vertical Elevation: y = 1.5m * sin(2 * theta)
 * Tangential Velocities:
 *   vx = R * omega * cos(theta)
 *   vz = R * omega * sin(theta)
 *   vy = 3.0 * omega * cos(2 * theta)
 * Update Rate: 50 FPS (Sleep 20 ms, dt = 0.02s, dtheta = 0.01 rad/frame)
 * Telemetry: Every 25 frames (~500 ms)
 * ============================================================================
 */
#define ORBIT_RADIUS            5.0f
#define ORBIT_OMEGA             0.5f
#define ORBIT_ELEV_AMP          1.5f
#define TIME_STEP_SEC           0.02f
#define SLEEP_MS                20
#define TELEMETRY_INTERVAL      25

static int16_t s_pcm_data[NUM_MONO_SAMPLES];

/*
 * Synthesize band-limited rich 440 Hz harmonic waveform with 1/k rolloff.
 * Two-pass generation with normalization to TARGET_PEAK_AMPLITUDE to prevent
 * integer overflow while maximizing signal-to-noise ratio.
 */
static void synthesize_rich_440hz_waveform(int16_t *buffer, size_t num_samples)
{
    static double s_temp[NUM_MONO_SAMPLES];
    double max_abs = 0.0;

    for (size_t i = 0; i < num_samples; ++i) {
        double t = (double)i / (double)SAMPLE_RATE_HZ;
        double sum = 0.0;

        for (int k = 1; k <= HARMONIC_COUNT; ++k) {
            double freq = (double)(TONE_FREQUENCY_HZ * k);
            double rad = 2.0 * M_PI * freq * t;
            double amp = 1.0 / (double)k;
            sum += amp * sin(rad);
        }

        s_temp[i] = sum;
        if (fabs(sum) > max_abs) {
            max_abs = fabs(sum);
        }
    }

    double scale = (max_abs > 1e-9) ? (TARGET_PEAK_AMPLITUDE / max_abs) : 1.0;
    for (size_t i = 0; i < num_samples; ++i) {
        double val = s_temp[i] * scale;
        buffer[i] = (int16_t)(val + (val >= 0.0 ? 0.5 : -0.5));
    }
}

int main(void)
{
    /* Initialize video mode for debug console output */
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("====================================================\n");
    debugPrint("  nxdk OpenAL 1.1 3D Spatial Audio Demo\n");
    debugPrint("  3D spatializer (library model: ITD + elevation filter)\n");
    debugPrint("====================================================\n\n");

    /*
     * ------------------------------------------------------------------------
     * Step 1: Open Audio Device & Create Context via OpenAL 1.1 ALC API
     * ------------------------------------------------------------------------
     */
    debugPrint("[1/5] Opening default audio device (MCPX APU)...\n");
    ALCdevice *device = alcOpenDevice(NULL);
    if (!device) {
        debugPrint("FATAL: alcOpenDevice failed!\n");
        return 1;
    }
    debugPrint("      Backend: %s\n", alGetString(AL_RENDERER)); /* default backend is silent, see README */

    debugPrint("[2/5] Creating OpenAL 1.1 context...\n");
    ALCcontext *ctx = alcCreateContext(device, NULL);
    if (!ctx) {
        debugPrint("FATAL: alcCreateContext failed!\n");
        alcCloseDevice(device);
        return 1;
    }

    debugPrint("[3/5] Making context current...\n");
    if (!alcMakeContextCurrent(ctx)) {
        debugPrint("FATAL: alcMakeContextCurrent failed!\n");
        alcDestroyContext(ctx);
        alcCloseDevice(device);
        return 1;
    }

    /*
     * ------------------------------------------------------------------------
     * Step 2: Configure OpenAL Listener Properties
     * ------------------------------------------------------------------------
     * Listener is stationed at origin (0, 0, 0) with zero velocity.
     * Forward vector is (0, 0, -1) and Up vector is (0, 1, 0).
     */
    debugPrint("[4/5] Initializing listener properties...\n");
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const ALfloat listener_ori[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    alListenerfv(AL_ORIENTATION, listener_ori);
    alListenerf(AL_GAIN, 1.0f);

    /*
     * ------------------------------------------------------------------------
     * Step 3: Synthesize Mono PCM Buffer & Configure 3D Spatial Source
     * ------------------------------------------------------------------------
     */
    debugPrint("[5/5] Synthesizing 440 Hz rich harmonic mono tone (100 ms)...\n");
    synthesize_rich_440hz_waveform(s_pcm_data, NUM_MONO_SAMPLES);

    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_MONO16, s_pcm_data, (ALsizei)sizeof(s_pcm_data), SAMPLE_RATE_HZ);

    ALuint source = 0;
    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, (ALint)buffer);
    alSourcei(source, AL_LOOPING, AL_TRUE);

    /* Attenuation parameters: reference 2m, max 25m, rolloff 1.0 */
    alSourcef(source, AL_REFERENCE_DISTANCE, 2.0f);
    alSourcef(source, AL_MAX_DISTANCE, 25.0f);
    alSourcef(source, AL_ROLLOFF_FACTOR, 1.0f);

    /* Initial orbit conditions at theta = 0 */
    float theta = 0.0f;
    float x = ORBIT_RADIUS * sinf(theta);
    float z = -ORBIT_RADIUS * cosf(theta);
    float y = ORBIT_ELEV_AMP * sinf(2.0f * theta);
    float vx = ORBIT_RADIUS * ORBIT_OMEGA * cosf(theta);
    float vz = ORBIT_RADIUS * ORBIT_OMEGA * sinf(theta);
    float vy = 2.0f * ORBIT_ELEV_AMP * ORBIT_OMEGA * cosf(2.0f * theta);

    alSource3f(source, AL_POSITION, x, y, z);
    alSource3f(source, AL_VELOCITY, vx, vy, vz);

    /* Start audio playback */
    alSourcePlay(source);

    debugPrint("=== OpenAL 1.1 3D Spatial Audio Active ===\n");
    debugPrint("Orbit: R=%.1fm, w=%.2frad/s | HRTF+ITD: Active | Tone: 440Hz Rich Mono\n\n",
               ORBIT_RADIUS, ORBIT_OMEGA);

    /*
     * ------------------------------------------------------------------------
     * Step 4: Real-Time 3D Orbit Dynamics & Diagnostics Update Loop
     * ------------------------------------------------------------------------
     */
    uint32_t frame_count = 0;
    while (1) {
        /* Advance orbital angle */
        theta += ORBIT_OMEGA * TIME_STEP_SEC;
        if (theta >= 2.0f * (float)M_PI) {
            theta -= 2.0f * (float)M_PI;
        }

        /* Compute 3D position along orbit */
        x = ORBIT_RADIUS * sinf(theta);
        z = -ORBIT_RADIUS * cosf(theta);
        y = ORBIT_ELEV_AMP * sinf(2.0f * theta);

        /* Compute tangential velocity vectors for Doppler calculations */
        vx = ORBIT_RADIUS * ORBIT_OMEGA * cosf(theta);
        vz = ORBIT_RADIUS * ORBIT_OMEGA * sinf(theta);
        vy = 2.0f * ORBIT_ELEV_AMP * ORBIT_OMEGA * cosf(2.0f * theta);

        /* Commit 3D spatial properties to OpenAL source */
        alSource3f(source, AL_POSITION, x, y, z);
        alSource3f(source, AL_VELOCITY, vx, vy, vz);

        /* Telemetry output every TELEMETRY_INTERVAL frames (~500 ms) */
        if ((frame_count % TELEMETRY_INTERVAL) == 0) {
            float dist = sqrtf(x * x + y * y + z * z);
            float az_deg = theta * (180.0f / (float)M_PI);
            float el_deg = atan2f(y, sqrtf(x * x + z * z)) * (180.0f / (float)M_PI);

            float x_rel = (dist > 1e-5f) ? (x / dist) : 0.0f;
            float y_rel = (dist > 1e-5f) ? (y / dist) : 0.0f;
            float z_rel = (dist > 1e-5f) ? (z / dist) : 0.0f;

            uint16_t itd_left = 0;
            uint16_t itd_right = 0;
            apu_calc_itd_taps(x_rel, &itd_left, &itd_right);

            float fc = apu_calc_elevation_cutoff(y_rel, z_rel);

            const float src_pos[3] = { x, y, z };
            const float src_vel[3] = { vx, vy, vz };
            const float lis_pos[3] = { 0.0f, 0.0f, 0.0f };
            const float lis_vel[3] = { 0.0f, 0.0f, 0.0f };
            float doppler = apu_calc_doppler_pitch(src_pos, src_vel, lis_pos, lis_vel, 1.0f, 343.3f);

            debugPrint("[Tick %4lu] Az:%5.1f deg El:%5.1f deg | Pos:(%5.2f, %5.2f, %5.2f) Dist:%4.2fm | ITD L:%2u R:%2u | HRTF:%5.0fHz | Doppler:%.3fx\n",
                       (unsigned long)frame_count,
                       az_deg, el_deg,
                       x, y, z, dist,
                       (unsigned int)itd_left, (unsigned int)itd_right,
                       fc, doppler);
        }

        frame_count++;
        Sleep(SLEEP_MS);
    }

    /* Clean up resources (unreachable in infinite loop) */
    alSourceStop(source);
    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(device);

    return 0;
}
