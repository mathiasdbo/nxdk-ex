#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#include <AL/al.h>
#include <AL/alc.h>
#include "alc_context.h"
#include "../../samples/openal_showcase/showcase_audio.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MOCK_MMIO_SIZE 0x30000
#define FRAMES 4096u

static int16_t s_out[2 * FRAMES];
static uint32_t s_checksum = 2166136261u;

static void mix(uint32_t frames) {
    uint32_t i;
    assert(frames <= FRAMES);
    showcase_audio_mix(s_out, frames);
    /* FNV-1a over the output: the MMX and scalar builds must agree bit for bit */
    for (i = 0; i < 2u * frames; i++) {
        s_checksum = (s_checksum ^ (uint16_t)s_out[i]) * 16777619u;
    }
}

static void energy(uint32_t from, uint32_t frames, double *l, double *r) {
    uint32_t i;
    *l = *r = 0.0;
    for (i = from; i < from + frames; i++) {
        *l += (double)s_out[2 * i] * s_out[2 * i];
        *r += (double)s_out[2 * i + 1] * s_out[2 * i + 1];
    }
}

/* Lag (in samples) of the left channel behind the right one, by cross-correlation */
static int ear_lag(uint32_t from, uint32_t frames) {
    int best = 0, lag;
    double best_c = -1e300;
    for (lag = -40; lag <= 40; lag++) {
        double c = 0.0;
        uint32_t i;
        for (i = from + 64; i < from + frames - 64; i++) {
            c += (double)s_out[2 * (i + lag)] * s_out[2 * i + 1];
        }
        if (c > best_c) {
            best_c = c;
            best = lag;
        }
    }
    return best;
}

int main(void) {
    uint8_t *mmio;
    ALuint buf_mono, buf_stereo, buf_short, src;
    static int16_t pcm[48000];
    static int16_t pcm_st[2 * 4800];
    uint32_t i;
    double l, r;
    ALint state;
    ALCdevice *dev;
    ALCcontext *ctx;

    printf("=== Showcase AC97 Software Mixer Test (%s bus) ===\n",
#if (defined(__i386__) || defined(_M_IX86) || defined(__x86_64__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(SHOWCASE_AUDIO_SCALAR)
           "MMX"
#else
           "scalar"
#endif
    );

    mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mmio != NULL);
    alc_set_apu_base((uintptr_t)mmio);
    dev = alcOpenDevice(NULL);
    assert(dev != NULL);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx != NULL);
    alcMakeContextCurrent(ctx);
    assert(showcase_audio_init() == 0);

    /* Band-limited noise-like test tone (sum of incommensurate sines) */
    for (i = 0; i < 48000u; i++) {
        double t = (double)i / 48000.0;
        pcm[i] = (int16_t)(9000.0 * (sin(2 * M_PI * 220.0 * t) + 0.7 * sin(2 * M_PI * 563.0 * t) +
                                     0.5 * sin(2 * M_PI * 1291.0 * t)));
    }
    for (i = 0; i < 4800u; i++) {
        pcm_st[2 * i] = (int16_t)(12000.0 * sin(2 * M_PI * 440.0 * i / 48000.0));
        pcm_st[2 * i + 1] = 0;
    }

    alGenBuffers(1, &buf_mono);
    alBufferData(buf_mono, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenBuffers(1, &buf_stereo);
    alBufferData(buf_stereo, AL_FORMAT_STEREO16, pcm_st, (ALsizei)sizeof(pcm_st), 48000);
    alGenBuffers(1, &buf_short);
    alBufferData(buf_short, AL_FORMAT_MONO16, pcm, 960 * 2, 48000);   /* 20 ms one-shot */
    assert(alGetError() == AL_NO_ERROR);

    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)buf_mono);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcef(src, AL_REFERENCE_DISTANCE, 2.0f);

    /* 1. Silence with nothing playing */
    printf("[1] Silence with no playing source...\n");
    mix(1024);
    for (i = 0; i < 2048u; i++) {
        assert(s_out[i] == 0);
    }
    printf("    -> All-zero output [PASS]\n");

    /* 2. Source on the listener's right (listener faces -Z) */
    printf("[2] Source to the right...\n");
    alSource3f(src, AL_POSITION, 3.0f, 0.0f, 0.0f);
    alSourcePlay(src);
    mix(FRAMES);
    mix(FRAMES);               /* past the fade-in */
    energy(0, FRAMES, &l, &r);
    printf("    energy L %.3g  R %.3g  lag %d\n", l, r, ear_lag(0, FRAMES));
    assert(r > 4.0 * l);
    assert(ear_lag(0, FRAMES) > 3);          /* left ear hears it later */
    printf("    -> Right channel dominates and the left ear is delayed (ITD) [PASS]\n");

    /* 3. Mirror: source on the left */
    printf("[3] Source to the left...\n");
    alSource3f(src, AL_POSITION, -3.0f, 0.0f, 0.0f);
    mix(FRAMES);
    mix(FRAMES);
    energy(0, FRAMES, &l, &r);
    assert(l > 4.0 * r);
    assert(ear_lag(0, FRAMES) < -3);
    printf("    -> Left channel dominates and the right ear is delayed [PASS]\n");

    /* 4. Distance attenuation */
    printf("[4] Distance attenuation...\n");
    {
        double near_e, far_e;
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, -2.0f);
        mix(FRAMES); mix(FRAMES);
        energy(0, FRAMES, &l, &r);
        near_e = l + r;
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, -16.0f);
        mix(FRAMES); mix(FRAMES);
        energy(0, FRAMES, &l, &r);
        far_e = l + r;
        assert(near_e > 4.0 * far_e && far_e > 0.0);
    }
    printf("    -> Farther source is quieter [PASS]\n");

    /* 5. Pause is silent, play resumes */
    printf("[5] Pause and resume...\n");
    alSourcePause(src);
    mix(FRAMES);
    for (i = 0; i < 2u * FRAMES; i++) {
        assert(s_out[i] == 0);
    }
    alSourcePlay(src);
    mix(FRAMES);
    energy(FRAMES / 2, FRAMES / 2, &l, &r);
    assert(l + r > 0.0);
    alSourceStop(src);
    printf("    -> Silent while paused, audible after play [PASS]\n");

    /* 6. 2D stereo buffer plays straight to left/right */
    printf("[6] 2D stereo buffer...\n");
    alSourcei(src, AL_BUFFER, (ALint)buf_stereo);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourcePlay(src);
    mix(FRAMES); mix(FRAMES);
    energy(0, FRAMES, &l, &r);
    assert(l > 0.0 && r == 0.0);
    alSourceStop(src);
    printf("    -> Left-only content stays on the left [PASS]\n");

    /* 7. A finished one-shot frees its voice and the library reports AL_STOPPED */
    printf("[7] One-shot completion...\n");
    alSourcei(src, AL_BUFFER, (ALint)buf_short);
    alSourcei(src, AL_LOOPING, AL_FALSE);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -2.0f);
    alSourcePlay(src);
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);
    mix(FRAMES);                             /* 85 ms > 20 ms of data */
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_STOPPED);
    mix(1024);
    for (i = 0; i < 2048u; i++) {
        assert(s_out[i] == 0);
    }
    printf("    -> Voice released, source AL_STOPPED, output silent [PASS]\n");

    printf("    mixer checksum %08x\n", (unsigned)s_checksum);

    showcase_audio_shutdown();
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf_mono);
    alDeleteBuffers(1, &buf_stereo);
    alDeleteBuffers(1, &buf_short);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    alc_set_apu_base(0);
    free(mmio);

    printf("=== All Showcase Mixer Tests Passed Successfully! ===\n");
    return 0;
}
