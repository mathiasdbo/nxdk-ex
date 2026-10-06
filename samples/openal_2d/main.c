#include <hal/debug.h>
#include <hal/video.h>
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <AL/al.h>
#include <AL/alc.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * ============================================================================
 * Audio Stream Constants: 48 kHz Stereo 16-bit PCM Sine Wave
 * ============================================================================
 * 4800 stereo frames at 48000 Hz = 0.100s (100 ms).
 * A 480 Hz tone over 0.1s completes exactly 48 integer cycles:
 *   480 Hz * 0.1 s = 48 cycles.
 * When looped back from sample frame 4799 to frame 0, phase continuity is
 * perfectly preserved (sin(0) = 0), preventing audible clicks or pops.
 * ============================================================================
 */
#define SAMPLE_RATE_HZ          48000
#define SINE_FREQUENCY_HZ       480
#define NUM_STEREO_SAMPLES      4800
#define NUM_CHANNELS            2
#define SINE_AMPLITUDE          32767.0

static int16_t s_pcm_data[NUM_STEREO_SAMPLES * NUM_CHANNELS];

int main(void)
{
    /* Initialize video mode for debug console output */
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    debugPrint("====================================================\n");
    debugPrint("  nxdk OpenAL 1.1 2D Audio Playback Demo\n");
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

    debugPrint("[2/5] Creating OpenAL context...\n");
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
     * Step 2: Generate 48kHz Stereo 16-bit Sine Wave (100 ms loop)
     * ------------------------------------------------------------------------
     */
    debugPrint("[4/5] Synthesizing 480 Hz stereo sine wave (48 full cycles)...\n");
    for (uint32_t i = 0; i < NUM_STEREO_SAMPLES; ++i) {
        double t = (double)i / (double)SAMPLE_RATE_HZ;
        double rad = 2.0 * M_PI * (double)SINE_FREQUENCY_HZ * t;
        int16_t val = (int16_t)(sin(rad) * SINE_AMPLITUDE);

        s_pcm_data[i * 2 + 0] = val; /* Front Left */
        s_pcm_data[i * 2 + 1] = val; /* Front Right */
    }
    ALsizei pcm_bytes = (ALsizei)sizeof(s_pcm_data);

    /*
     * ------------------------------------------------------------------------
     * Step 3: Create Buffer, Upload PCM Data, and Configure Looping Source
     * ------------------------------------------------------------------------
     */
    debugPrint("[5/5] Creating OpenAL buffer & source with looping playback...\n");
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, AL_FORMAT_STEREO16, s_pcm_data, pcm_bytes, SAMPLE_RATE_HZ);

    ALuint source = 0;
    alGenSources(1, &source);
    alSourcei(source, AL_BUFFER, (ALint)buffer);
    alSourcei(source, AL_LOOPING, AL_TRUE);
    alSourcePlay(source);

    debugPrint("=== OpenAL 1.1 Playback Active ===\n");
    debugPrint("Tone: 480 Hz Sine | Rate: 48 kHz | Channels: Stereo | Loop: ON\n\n");

    /*
     * ------------------------------------------------------------------------
     * Step 4: Main Loop - Real-Time Dynamic Pitch and Gain Modulation
     * ------------------------------------------------------------------------
     */
    const float pitch_presets[] = { 1.0f, 1.25f, 1.5f, 2.0f, 1.0f, 0.75f, 0.5f, 1.0f };
    const size_t num_pitch_presets = sizeof(pitch_presets) / sizeof(pitch_presets[0]);

    const float gain_presets[] = { 1.0f, 0.8f, 0.6f, 0.4f, 0.6f, 0.8f, 1.0f, 0.5f };
    const size_t num_gain_presets = sizeof(gain_presets) / sizeof(gain_presets[0]);

    uint32_t tick = 0;
    while (1) {
        float new_pitch = pitch_presets[tick % num_pitch_presets];
        float new_gain = gain_presets[tick % num_gain_presets];

        /* Modulate pitch and gain in real-time */
        alSourcef(source, AL_PITCH, new_pitch);
        alSourcef(source, AL_GAIN, new_gain);

        /* Query state and current properties */
        ALint state = 0;
        alGetSourcei(source, AL_SOURCE_STATE, &state);

        float cur_pitch = 0.0f;
        float cur_gain = 0.0f;
        alGetSourcef(source, AL_PITCH, &cur_pitch);
        alGetSourcef(source, AL_GAIN, &cur_gain);

        const char *state_str = (state == AL_PLAYING) ? "PLAYING" :
                                (state == AL_PAUSED)  ? "PAUSED"  :
                                (state == AL_STOPPED) ? "STOPPED" : "INITIAL";

        debugPrint("[Tick %3lu] State: %s | Pitch: %.2fx | Gain: %.2f | Buf: %u\n",
                   (unsigned long)tick++, state_str, cur_pitch, cur_gain, buffer);

        Sleep(1000);
    }

    /* Cleanup (unreachable in infinite loop) */
    alSourceStop(source);
    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(device);

    return 0;
}
