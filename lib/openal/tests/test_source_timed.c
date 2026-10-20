/*
 * alXboxSourcePlayAtSample / alXboxSourceStopAtSample (AL_XBOX_timed_source):
 * starts and stops timed to a streaming source's position. A test clock
 * stands in for the stream's voice (al_source_debug_clock); each
 * alXboxUpdateVoices is one engine frame. The checks, the half-update
 * window, a late start's offset (into the sound, past its end, wrapped in its
 * loop), the stop, what replaces or drops a waiting operation, and a paused
 * or stopped stream.
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

static ALint offset(ALuint src) {
    ALint v = -1;
    alGetSourcei(src, AL_SAMPLE_OFFSET, &v);
    return v;
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
    printf("[1] the checks\n");
    alXboxSourcePlayAtSample(a, 9999, 100);              /* no such stream */
    assert(alGetError() == AL_INVALID_NAME);
    alXboxSourcePlayAtSample(9999, s_stream, 100);       /* no such source */
    assert(alGetError() == AL_INVALID_NAME);
    alXboxSourcePlayAtSample(a, b, 100);                 /* not a stream */
    assert(alGetError() == AL_INVALID_OPERATION);
    alXboxSourceStopAtSample(s_stream, s_stream, 100);   /* itself */
    assert(alGetError() == AL_INVALID_OPERATION);
    assert(al_source_debug_timed_pending(a) == 0u);
    alDeleteSources(1, &a);
    alDeleteSources(1, &b);
    printf("    -> names, a stream that is not one, the stream itself [PASS]\n");
}

static void test_window(void) {
    ALuint a = sound();
    printf("[2] the start waits for the stream, then for its frame (half an update early at most)\n");
    alXboxSourcePlayAtSample(a, s_stream, 1000);         /* the stream has not started: it waits */
    assert(alGetError() == AL_NO_ERROR && al_source_debug_timed_pending(a) == 1u && state(a) == AL_INITIAL);
    frame_at(0);
    assert(state(a) == AL_INITIAL);
    alSourcePlay(s_stream);
    frame_at(0);                                         /* the first frame measures nothing */
    assert(state(a) == AL_INITIAL);
    frame_at(400);                                       /* 400 a frame: the window is 200 */
    assert(state(a) == AL_INITIAL);
    frame_at(790);                                       /* 210 ahead, 195 window */
    assert(state(a) == AL_INITIAL && al_source_debug_timed_pending(a) == 1u);
    frame_at(1010);                                      /* 10 late: starts 10 frames in */
    assert(state(a) == AL_PLAYING && al_source_debug_timed_pending(a) == 0u);
    assert(offset(a) == 10);
    alSourceStop(a);

    alXboxSourcePlayAtSample(a, s_stream, 1500);         /* 490 ahead, 110 window: waits */
    assert(state(a) == AL_STOPPED);
    frame_at(1410);                                      /* 90 ahead, 200 window: early, from the start */
    assert(state(a) == AL_PLAYING && offset(a) == 0);
    alDeleteSources(1, &a);
    printf("    -> waits for the stream; acts within half an update, early from the start, late into it [PASS]\n");
}

static void test_late(void) {
    ALuint a = sound(), b = sound();
    ALint lp[2] = { 1000, 3000 };
    printf("[3] a late start: into the sound, at its rate; over; wrapped in its loop\n");
    frame_at(10000);
    frame_at(10000);                                     /* window 0 */
    alXboxSourcePlayAtSample(a, s_stream, 9400);         /* 600 late: at once, 600 in */
    assert(state(a) == AL_PLAYING && offset(a) == 600);
    alSourceStop(a);
    alSourcef(a, AL_PITCH, 0.5f);                        /* plays at half the stream's rate */
    alXboxSourcePlayAtSample(a, s_stream, 9400);
    assert(state(a) == AL_PLAYING && offset(a) == 300);
    alSourceStop(a);
    alSourcef(a, AL_PITCH, 1.0f);
    alXboxSourcePlayAtSample(a, s_stream, 10000 - FRAMES - 5);   /* over by 5 */
    assert(state(a) == AL_STOPPED && al_source_debug_timed_pending(a) == 0u);
    /* looping in [1000, 3000): 4805 in is 1000 + 3805 % 2000 */
    alSourcei(b, AL_BUFFER, 0);
    {
        ALuint lb = 0;
        static int16_t pcm[FRAMES];
        alGenBuffers(1, &lb);
        alBufferData(lb, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
        alBufferiv(lb, AL_LOOP_POINTS_SOFT, lp);
        alSourcei(b, AL_BUFFER, (ALint)lb);
        alSourcei(b, AL_LOOPING, AL_TRUE);
        assert(alGetError() == AL_NO_ERROR);
        alXboxSourcePlayAtSample(b, s_stream, 10000 - 4805);
        assert(state(b) == AL_PLAYING && offset(b) == 1000 + 3805 % 2000);
        alSourceStop(b);
        alDeleteSources(1, &b);
        alDeleteBuffers(1, &lb);
    }
    alDeleteSources(1, &a);
    printf("    -> offset by the lateness at the sound's own rate, a sound already over stops, a loop wraps [PASS]\n");
}

static void test_stop_and_replace(void) {
    ALuint a = sound();
    printf("[4] the stop; what replaces or drops a waiting operation\n");
    frame_at(20000);
    frame_at(20400);                                     /* window 200 */
    alSourcei(a, AL_LOOPING, AL_TRUE);
    alSourcePlay(a);
    alXboxSourceStopAtSample(a, s_stream, 21300);
    assert(state(a) == AL_PLAYING && al_source_debug_timed_pending(a) == 2u);
    frame_at(20800);                                     /* 500 ahead */
    assert(state(a) == AL_PLAYING);
    frame_at(21200);                                     /* 100 ahead, window 200: stops */
    assert(state(a) == AL_STOPPED && al_source_debug_timed_pending(a) == 0u);

    alXboxSourcePlayAtSample(a, s_stream, 30000);
    alXboxSourceStopAtSample(a, s_stream, 30000);        /* replaces the start */
    assert(al_source_debug_timed_pending(a) == 2u);
    alXboxSourcePlayAtSample(a, s_stream, 30000);
    assert(al_source_debug_timed_pending(a) == 1u);
    alSourceStop(a);                                     /* drops it */
    assert(al_source_debug_timed_pending(a) == 0u);
    alXboxSourcePlayAtSample(a, s_stream, 30000);
    alSourceRewind(a);
    assert(al_source_debug_timed_pending(a) == 0u);
    alXboxSourcePlayAtSample(a, s_stream, 30000);
    alSourcePlay(a);                                     /* a plain play wins */
    assert(al_source_debug_timed_pending(a) == 0u && state(a) == AL_PLAYING);
    alSourceStop(a);
    alXboxSourcePlayAtSample(a, s_stream, 30000);
    alDeleteSources(1, &a);                              /* deleting drops it too */
    a = sound();
    frame_at(30000);
    assert(state(a) == AL_INITIAL && al_source_debug_timed_pending(a) == 0u);   /* the new source has none */
    alDeleteSources(1, &a);
    printf("    -> the stop lands in its window; a new one replaces, play/stop/rewind/delete drop [PASS]\n");
}

static void test_stream_states(void) {
    ALuint a = sound();
    printf("[5] a paused stream holds them; a stopped one lets them act at once\n");
    alSourcePause(s_stream);
    alXboxSourcePlayAtSample(a, s_stream, 40000);        /* due, but the stream is paused */
    frame_at(40000);
    assert(state(a) == AL_INITIAL && al_source_debug_timed_pending(a) == 1u);
    alSourcePlay(s_stream);                              /* resumes */
    frame_at(40000);
    assert(state(a) == AL_PLAYING);
    alSourceStop(a);
    alSourceStop(s_stream);
    alXboxSourcePlayAtSample(a, s_stream, 99999);        /* the stream is stopped: at once */
    assert(state(a) == AL_PLAYING && offset(a) == 0);
    alDeleteSources(1, &a);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> paused: waits; stopped: at once, from the start [PASS]\n");
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    printf("=== starts and stops timed to a stream ===\n");
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    alGetError();
    setup();
    test_checks();
    test_window();
    test_late();
    test_stop_and_replace();
    test_stream_states();
    al_source_debug_clock(false, 0u, 0u, false);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== starts and stops timed to a stream: all passed ===\n");
    return 0;
}
