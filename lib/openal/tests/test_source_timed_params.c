/*
 * alXboxSourcefvAtSample (AL_XBOX_timed_source): AL_PITCH and
 * AL_XBOX_DIRECT_GAINS changes timed to a streaming source's position. A test
 * clock stands in for the stream's voice (al_source_debug_clock); each
 * alXboxUpdateVoices is one engine frame. The checks, the half-update window,
 * several changes in order, a full queue, a change and a start on one frame,
 * what drops them, a paused or stopped stream, and a device closed with
 * changes and a start still waiting.
 */
#define AL_ALEXT_PROTOTYPES
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include "al_source.h"

#define FRAMES 4800   /* every test sound: 0.1 s at 48 kHz */

static ALuint s_stream, s_ring, s_buf;

static ALint state(ALuint src) {
    ALint st = 0;
    alGetSourcei(src, AL_SOURCE_STATE, &st);
    return st;
}

static float pitch(ALuint src) {
    ALfloat v = -1.0f;
    alGetSourcef(src, AL_PITCH, &v);
    return v;
}

static float gain_l(ALuint src) {
    ALfloat v[2] = { -1.0f, -1.0f };
    alGetSourcefv(src, AL_XBOX_DIRECT_GAINS, v);
    return v[0];
}

static float gain_r(ALuint src) {
    ALfloat v[2] = { -1.0f, -1.0f };
    alGetSourcefv(src, AL_XBOX_DIRECT_GAINS, v);
    return v[1];
}

static void pitch_at(ALuint src, float p, uint32_t sample) {
    alXboxSourcefvAtSample(src, AL_PITCH, &p, s_stream, sample);
}

static void gains_at(ALuint src, float l, float r, uint32_t sample) {
    ALfloat v[2] = { l, r };
    alXboxSourcefvAtSample(src, AL_XBOX_DIRECT_GAINS, v, s_stream, sample);
}

/* One engine frame with the stream at `pos` */
static void frame_at(uint32_t pos) {
    al_source_debug_clock(true, pos, 0u, false);
    alXboxUpdateVoices();
}

static ALuint sound(void) {
    ALuint src = 0;
    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)s_buf);
    assert(alGetError() == AL_NO_ERROR);
    return src;
}

static void setup(void) {
    static int16_t pcm[FRAMES];
    alGenBuffers(1, &s_ring);
    alBufferData(s_ring, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &s_stream);
    alSourceQueueBuffers(s_stream, 1, &s_ring);
    alSourcei(s_stream, AL_LOOPING, AL_TRUE);
    alGenBuffers(1, &s_buf);
    alBufferData(s_buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_checks(void) {
    ALuint a = sound(), b = sound();
    ALfloat one = 1.0f, bad[2] = { 0.5f, 1.5f };
    printf("[1] the checks\n");
    alXboxSourcefvAtSample(a, AL_PITCH, &one, 9999, 100);         /* no such stream */
    assert(alGetError() == AL_INVALID_NAME);
    alXboxSourcefvAtSample(9999, AL_PITCH, &one, s_stream, 100);  /* no such source */
    assert(alGetError() == AL_INVALID_NAME);
    alXboxSourcefvAtSample(a, AL_PITCH, &one, b, 100);            /* not a stream */
    assert(alGetError() == AL_INVALID_OPERATION);
    alXboxSourcefvAtSample(s_stream, AL_PITCH, &one, s_stream, 100);   /* itself */
    assert(alGetError() == AL_INVALID_OPERATION);
    alXboxSourcefvAtSample(a, AL_GAIN, &one, s_stream, 100);      /* not a timed parameter */
    assert(alGetError() == AL_INVALID_ENUM);
    alXboxSourcefvAtSample(a, AL_PITCH, NULL, s_stream, 100);
    assert(alGetError() == AL_INVALID_VALUE);
    pitch_at(a, 0.0f, 100);                                       /* as alSourcef: > 0 */
    assert(alGetError() == AL_INVALID_VALUE);
    alXboxSourcefvAtSample(a, AL_XBOX_DIRECT_GAINS, bad, s_stream, 100);   /* as alSourcefv: 0..1 */
    assert(alGetError() == AL_INVALID_VALUE);
    assert(al_source_debug_timed_params(a) == 0u);
    alDeleteSources(1, &a);
    alDeleteSources(1, &b);
    printf("    -> names, a stream that is not one, the stream itself, the parameter, the values [PASS]\n");
}

static void test_window(void) {
    ALuint a = sound();
    printf("[2] a change waits for the stream, then for its frame (half an update early at most)\n");
    pitch_at(a, 0.5f, 1000);                             /* the stream has not started: it waits */
    assert(alGetError() == AL_NO_ERROR && al_source_debug_timed_params(a) == 1u && pitch(a) == 1.0f);
    frame_at(0);
    assert(pitch(a) == 1.0f);
    alSourcePlay(s_stream);
    frame_at(0);                                         /* the first frame measures nothing */
    frame_at(400);                                       /* 400 a frame: the window is 200 */
    assert(pitch(a) == 1.0f);
    frame_at(790);                                       /* 210 ahead, 195 window */
    assert(pitch(a) == 1.0f && al_source_debug_timed_params(a) == 1u);
    frame_at(1010);
    assert(pitch(a) == 0.5f && al_source_debug_timed_params(a) == 0u);

    gains_at(a, 0.25f, 0.75f, 1500);                     /* 490 ahead, 110 window: waits */
    assert(gain_l(a) != 0.25f);
    frame_at(1410);                                      /* 90 ahead, 200 window */
    assert(gain_l(a) == 0.25f && gain_r(a) == 0.75f);
    alDeleteSources(1, &a);
    printf("    -> waits for the stream; acts within half an update [PASS]\n");
}

static void test_order_and_full(void) {
    ALuint a = sound();
    int i;
    printf("[3] several changes, in order; a full queue; one already due\n");
    frame_at(10000);
    frame_at(10400);                                     /* window 200 */
    pitch_at(a, 0.6f, 10800);
    gains_at(a, 0.1f, 0.2f, 10800);
    pitch_at(a, 0.7f, 11200);
    pitch_at(a, 0.8f, 11600);
    assert(al_source_debug_timed_params(a) == 4u);
    frame_at(10800);                                     /* the first two, not the rest */
    assert(pitch(a) == 0.6f && gain_l(a) == 0.1f && gain_r(a) == 0.2f && al_source_debug_timed_params(a) == 2u);
    frame_at(12000);                                     /* a frame missed: both late ones, the last wins */
    assert(pitch(a) == 0.8f && al_source_debug_timed_params(a) == 0u);

    for (i = 0; i < AL_XBOX_TIMED_PARAMS; i++) {
        pitch_at(a, 1.0f + 0.1f * (float)i, 20000u + 100u * (uint32_t)i);
    }
    assert(al_source_debug_timed_params(a) == AL_XBOX_TIMED_PARAMS && pitch(a) == 0.8f);
    pitch_at(a, 3.0f, 30000);                            /* full: the oldest acts now to make room */
    assert(alGetError() == AL_NO_ERROR && al_source_debug_timed_params(a) == AL_XBOX_TIMED_PARAMS);
    assert(pitch(a) == 1.0f);
    frame_at(30000);
    assert(pitch(a) == 3.0f && al_source_debug_timed_params(a) == 0u);

    pitch_at(a, 0.9f, 29000);                            /* already past: at once */
    assert(pitch(a) == 0.9f && al_source_debug_timed_params(a) == 0u);
    alDeleteSources(1, &a);
    printf("    -> in order, the latest wins; a full queue applies its oldest; a past one at once [PASS]\n");
}

static void test_with_start_and_drop(void) {
    ALuint a = sound();
    printf("[4] a change and a start on one frame; what drops them\n");
    frame_at(40000);
    frame_at(40400);                                     /* window 200 */
    alXboxSourcePlayAtSample(a, s_stream, 40800);
    gains_at(a, 0.3f, 0.4f, 40800);
    pitch_at(a, 0.5f, 40800);
    alSourcePlay(a);                                     /* a plain play keeps the changes */
    alSourceStop(a);                                     /* a stop drops them */
    assert(al_source_debug_timed_params(a) == 0u);

    alXboxSourcePlayAtSample(a, s_stream, 41200);
    gains_at(a, 0.3f, 0.4f, 41200);
    pitch_at(a, 0.5f, 41200);
    frame_at(40800);
    assert(state(a) == AL_STOPPED && gain_l(a) != 0.3f);
    frame_at(41200);                                     /* the start plays with them */
    assert(state(a) == AL_PLAYING && gain_l(a) == 0.3f && pitch(a) == 0.5f);
    alXboxSourceStopAtSample(a, s_stream, 41600);
    pitch_at(a, 0.7f, 41600);                            /* due with the stop: applied first */
    pitch_at(a, 0.9f, 42000);                            /* after it: dropped */
    frame_at(41600);
    assert(state(a) == AL_STOPPED && pitch(a) == 0.7f && al_source_debug_timed_params(a) == 0u);

    pitch_at(a, 0.6f, 50000);
    alSourceRewind(a);
    assert(al_source_debug_timed_params(a) == 0u);
    pitch_at(a, 0.6f, 50000);
    alSourcef(a, AL_PITCH, 2.0f);                        /* a plain set leaves them */
    assert(al_source_debug_timed_params(a) == 1u);
    alDeleteSources(1, &a);                              /* deleting drops them */
    a = sound();
    frame_at(50000);
    assert(pitch(a) == 1.0f && al_source_debug_timed_params(a) == 0u);   /* the new source has none */
    alDeleteSources(1, &a);
    printf("    -> applied before a start or a stop due with them; stop/rewind/delete drop, play keeps [PASS]\n");
}

static void test_stream_states(void) {
    ALuint a = sound();
    printf("[5] a paused stream holds them; a stopped one lets them act at once\n");
    alSourcePause(s_stream);
    pitch_at(a, 0.5f, 60000);                            /* due, but the stream is paused */
    frame_at(60000);
    assert(pitch(a) == 1.0f && al_source_debug_timed_params(a) == 1u);
    alSourcePlay(s_stream);                              /* resumes */
    frame_at(60000);
    assert(pitch(a) == 0.5f);
    alSourceStop(s_stream);
    pitch_at(a, 0.7f, 99999);                            /* the stream is stopped: at once */
    assert(pitch(a) == 0.7f && al_source_debug_timed_params(a) == 0u);
    alDeleteSources(1, &a);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> paused: waits; stopped: at once [PASS]\n");
}

/* Closing the device with a change and a start waiting leaves nothing counted */
static ALCdevice *test_close_waiting(ALCdevice *dev, ALCcontext *ctx) {
    ALuint a = sound();
    uint32_t ops = 9, params = 9;
    printf("[6] the device closed with a change and a start waiting\n");
    alSourcePlay(s_stream);
    frame_at(70000);
    frame_at(70400);
    pitch_at(a, 0.5f, 90000);
    alXboxSourcePlayAtSample(a, s_stream, 90000);
    al_source_debug_timed_counts(&ops, &params);
    assert(ops == 1u && params == 1u);
    al_source_debug_clock(false, 0u, 0u, false);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    al_source_debug_timed_counts(&ops, &params);
    assert(ops == 0u && params == 0u);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    printf("    -> both counts back to 0 [PASS]\n");
    return dev;
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    printf("=== pitch and direct gains timed to a stream ===\n");
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    alGetError();
    assert(alcGetProcAddress(dev, "alXboxSourcefvAtSample") == (void *)alXboxSourcefvAtSample);
    setup();
    test_checks();
    test_window();
    test_order_and_full();
    test_with_start_and_drop();
    test_stream_states();
    dev = test_close_waiting(dev, ctx);
    alcCloseDevice(dev);
    printf("=== pitch and direct gains timed to a stream: all passed ===\n");
    return 0;
}
