/*
 * Buffer queueing (streaming sources, al_stream.c) on the software model: the
 * voice's play position is set by the test (apu_voice_debug_set_position()),
 * standing in for the VP's CBO, and the ring content is checked directly.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"
#include "al_source.h"
#include "al_buffer.h"
#include "al_stream.h"
#include "apu_voice.h"

#define MOCK_MMIO_SIZE 0x30000
#define CHUNK 4096   /* frames per test buffer */

static int16_t s_pcm[CHUNK];

/* Fill buffer b with CHUNK mono16 frames of the constant value v */
static void fill_buffer(ALuint b, int16_t v) {
    int i;
    for (i = 0; i < CHUNK; i++) s_pcm[i] = v;
    alBufferData(b, AL_FORMAT_MONO16, s_pcm, (ALsizei)sizeof(s_pcm), 48000);
    assert(alGetError() == AL_NO_ERROR);
}

static ALint geti(ALuint src, ALenum p) {
    ALint v = -1;
    alGetSourcei(src, p, &v);
    return v;
}

static const int16_t *ring_of(ALuint src) {
    const ALbuffer *rb = al_source_play_buffer(al_source_get(src));
    assert(rb != NULL);
    return (const int16_t *)rb->data_virt;
}

/* Play position of the source's voice (the ring index, as the VP's CBO) */
static void set_pos(ALuint src, uint32_t ring_index) {
    ALsource *s = al_source_get(src);
    assert(s->hw_voice_idx >= 0);
    apu_voice_debug_set_position((uint32_t)s->hw_voice_idx, ring_index % AL_STREAM_RING_FRAMES);
    alXboxUpdateVoices();
}

int main(void) {
    uint8_t *mock = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint src, stat, buf[4], other;
    ALuint ids[4];
    int i;

    printf("=== OpenAL buffer queueing (streaming) host test ===\n");
    assert(mock);
    alc_set_apu_base((uintptr_t)mock);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));

    alGenSources(1, &src);
    alGenSources(1, &stat);
    alGenBuffers(4, buf);
    alGenBuffers(1, &other);
    for (i = 0; i < 4; i++) fill_buffer(buf[i], (int16_t)(i + 1));
    {
        int16_t st[2 * 16] = { 0 };
        alBufferData(other, AL_FORMAT_STEREO16, st, (ALsizei)sizeof(st), 48000);
    }
    assert(alGetError() == AL_NO_ERROR);

    /* [1] Queue validation */
    printf("[1] queue validation\n");
    assert(geti(src, AL_SOURCE_TYPE) == AL_UNDETERMINED);
    alSourcei(stat, AL_BUFFER, (ALint)buf[0]);
    alSourceQueueBuffers(stat, 1, &buf[1]);
    assert(alGetError() == AL_INVALID_OPERATION);            /* static source */
    alSourcei(stat, AL_BUFFER, 0);
    {
        ALuint bad[2] = { buf[0], 999 };
        alSourceQueueBuffers(src, 2, bad);
        assert(alGetError() == AL_INVALID_NAME);
        assert(geti(src, AL_BUFFERS_QUEUED) == 0);
    }
    alSourceQueueBuffers(src, 3, buf);
    assert(alGetError() == AL_NO_ERROR);
    alSourceQueueBuffers(src, 1, &other);
    assert(alGetError() == AL_INVALID_OPERATION);            /* stereo into a mono queue */
    assert(geti(src, AL_SOURCE_TYPE) == AL_STREAMING);
    assert(geti(src, AL_BUFFERS_QUEUED) == 3);
    assert(geti(src, AL_BUFFERS_PROCESSED) == 0);
    alSourceUnqueueBuffers(src, 1, ids);
    assert(alGetError() == AL_INVALID_VALUE);                /* nothing processed yet */
    alDeleteBuffers(1, &buf[0]);
    assert(alGetError() == AL_INVALID_OPERATION);            /* still queued */
    alSourcei(src, AL_BUFFER, (ALint)buf[0]);               /* AL_BUFFER replaces the queue... */
    assert(geti(src, AL_SOURCE_TYPE) == AL_STATIC);
    alSourcei(src, AL_BUFFER, 0);                           /* ...and 0 clears it */
    assert(geti(src, AL_SOURCE_TYPE) == AL_UNDETERMINED);
    alSourceQueueBuffers(src, 3, buf);
    assert(alGetError() == AL_NO_ERROR);

    /* [2] Play: the ring holds the lead of data, then silence */
    printf("[2] ring fill at play\n");
    alSourcePlay(src);
    assert(geti(src, AL_SOURCE_STATE) == AL_PLAYING);
    {
        const int16_t *r = ring_of(src);
        assert(r[0] == 1 && r[CHUNK - 1] == 1 && r[CHUNK] == 2 && r[2 * CHUNK - 1] == 2);
        assert(AL_STREAM_LEAD_FRAMES == 2 * CHUNK);
        assert(r[2 * CHUNK] == 0 && r[AL_STREAM_RING_FRAMES - 300] == 0);   /* not yet written: silent */
        assert(geti(src, AL_BUFFER) == (ALint)buf[0]);
    }

    /* [2b] The output thread's pass silences what the voice has played, and nothing ahead */
    printf("[2b] scrub behind the play position\n");
    {
        ALsource *s = al_source_get(src);
        const int16_t *r = ring_of(src);
        apu_voice_debug_set_position((uint32_t)s->hw_voice_idx, 3000);
        al_stream_scrub_tick();
        assert(r[0] == 0 && r[2999] == 0 && r[3000] == 1 && r[CHUNK] == 2);
        alSourcePause(src);                                  /* paused: the position holds */
        al_stream_scrub_tick();
        assert(r[3000] == 1);
        alSourcePlay(src);
    }

    /* [3] Progress: processed buffers, unqueue, requeue */
    printf("[3] processed / unqueue / requeue\n");
    set_pos(src, 5000);
    assert(geti(src, AL_BUFFERS_PROCESSED) == 1);
    assert(geti(src, AL_BUFFER) == (ALint)buf[1]);
    {
        const int16_t *r = ring_of(src);
        assert(r[2 * CHUNK] == 3 && r[3 * CHUNK - 1] == 3);   /* lead moved: buffer 3 written */
        assert(r[3 * CHUNK] == 0);                            /* end of the data: silence */
    }
    alSourceUnqueueBuffers(src, 1, ids);
    assert(alGetError() == AL_NO_ERROR && ids[0] == buf[0]);
    assert(geti(src, AL_BUFFERS_QUEUED) == 2 && geti(src, AL_BUFFERS_PROCESSED) == 0);
    fill_buffer(buf[0], 5);                                  /* free again: refill it */
    alSourceQueueBuffers(src, 1, &buf[0]);
    alSourceQueueBuffers(src, 1, &buf[3]);
    assert(alGetError() == AL_NO_ERROR && geti(src, AL_BUFFERS_QUEUED) == 4);
    set_pos(src, 6000);
    {
        const int16_t *r = ring_of(src);
        /* the requeued data follows on, up to the lead: 1904 + 8192 played/ahead */
        assert(r[3 * CHUNK] == 5 && r[3 * CHUNK + 1000] == 5 && r[3 * CHUNK + 2000] == 0);
    }

    /* [4] Play through the ring wrap to the end of the data */
    printf("[4] wrap and end of stream\n");
    {
        uint32_t p = 6000;   /* ring index == stream frames played since the voice started */
        while (geti(src, AL_SOURCE_STATE) == AL_PLAYING && p < 40000u) {
            p += 1500;
            set_pos(src, p);
            if (p == 13500u) {
                const int16_t *r = ring_of(src);
                /* the last buffer (value 4) starts 4 * CHUNK = 16384 frames after
                 * the voice started: ring index 0, written ahead across the wrap */
                assert(r[0] == 4 && r[CHUNK - 1] == 4);
            }
        }
        /* 1 + 4 buffers = 5 * CHUNK = 20480 frames from the start */
        assert(p >= 5u * CHUNK && p < 5u * CHUNK + 1500u);
    }
    assert(geti(src, AL_SOURCE_STATE) == AL_STOPPED);
    assert(geti(src, AL_BUFFERS_PROCESSED) == 4);
    assert(al_source_get(src)->hw_voice_idx < 0);           /* voice released */
    assert(al_stream_starved(al_source_get(src)->stream) == 0);
    alSourceUnqueueBuffers(src, 4, ids);
    assert(alGetError() == AL_NO_ERROR && geti(src, AL_BUFFERS_QUEUED) == 0);

    /* [5] Starvation: the application updates too late */
    printf("[5] starvation\n");
    for (i = 0; i < 3; i++) fill_buffer(buf[i], (int16_t)(10 + i));
    alSourceQueueBuffers(src, 3, buf);
    alSourcePlay(src);
    set_pos(src, 10000);   /* jumped past the 8192 frames of lead in one update */
    assert(al_stream_starved(al_source_get(src)->stream) == 1);
    assert(geti(src, AL_SOURCE_STATE) == AL_PLAYING);
    assert(ring_of(src)[11000] == 12);                      /* the rest of the data is written */
    set_pos(src, 12300);
    assert(geti(src, AL_SOURCE_STATE) == AL_STOPPED && geti(src, AL_BUFFERS_PROCESSED) == 3);

    /* [6] Stop marks everything processed; rewind clears it; play restarts */
    printf("[6] stop / rewind / play\n");
    alSourcePlay(src);                                      /* from STOPPED: from the start */
    assert(geti(src, AL_BUFFERS_PROCESSED) == 0 && ring_of(src)[0] == 10);
    set_pos(src, 4500);
    assert(geti(src, AL_BUFFERS_PROCESSED) == 1);
    alSourcePause(src);
    assert(geti(src, AL_SOURCE_STATE) == AL_PAUSED);
    alSourcePlay(src);                                      /* resume: not rewound */
    set_pos(src, 4600);
    assert(geti(src, AL_BUFFERS_PROCESSED) == 1);
    alSourceStop(src);
    assert(geti(src, AL_SOURCE_STATE) == AL_STOPPED && geti(src, AL_BUFFERS_PROCESSED) == 3);
    alSourceRewind(src);
    assert(geti(src, AL_SOURCE_STATE) == AL_INITIAL && geti(src, AL_BUFFERS_PROCESSED) == 0);

    /* [7] Looping stream: nothing is processed, the queue repeats */
    printf("[7] looping\n");
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    {
        uint32_t p = 0;
        for (i = 0; i < 30; i++) {
            p += 1500;
            set_pos(src, p);
        }
        assert(geti(src, AL_SOURCE_STATE) == AL_PLAYING);
        assert(geti(src, AL_BUFFERS_PROCESSED) == 0);
        /* 45000 frames played of a 12288-frame queue: lap 3, ring holds lap 4's start */
        alSourcei(src, AL_LOOPING, AL_FALSE);
        for (i = 0; i < 20 && geti(src, AL_SOURCE_STATE) == AL_PLAYING; i++) {
            p += 1500;
            set_pos(src, p);
        }
        assert(geti(src, AL_SOURCE_STATE) == AL_STOPPED);
        assert(p <= 4u * 3u * CHUNK + 1500u);              /* ended at the end of the current lap */
    }

    /* [8] Clearing the queue releases the buffers */
    /* [9] Xbox ADPCM: the ring holds 36-byte blocks of 64 frames; data, silence
     * and scrubbing move whole blocks */
    printf("[9] ADPCM queue\n");
    {
        enum { BLOCKS = 64, BB = 36 };          /* 4096 frames per buffer */
        static uint8_t data[3][BLOCKS * BB];
        ALuint ab[3], sa;
        const uint8_t *r;
        int b, k;
        alGenBuffers(3, ab);
        for (b = 0; b < 3; b++) {
            for (k = 0; k < BLOCKS * BB; k++) data[b][k] = (uint8_t)(1 + b * BLOCKS + k / BB);   /* block id */
            alBufferData(ab[b], AL_FORMAT_MONO_XBOX_ADPCM, data[b], BLOCKS * BB, 48000);
        }
        alGenSources(1, &sa);
        alSourceQueueBuffers(sa, 3, ab);
        assert(alGetError() == AL_NO_ERROR && geti(sa, AL_BUFFERS_QUEUED) == 3);
        alSourcePlay(sa);
        r = (const uint8_t *)al_source_play_buffer(al_source_get(sa))->data_virt;
        assert(al_source_play_buffer(al_source_get(sa))->size == (ALsizei)(AL_STREAM_RING_FRAMES / 64 * BB));
        /* the lead: 8192 frames = 128 blocks = buffers 0 and 1, then silent blocks */
        for (k = 0; k < 128; k++) assert(r[k * BB] == (uint8_t)(1 + k) && r[k * BB + BB - 1] == (uint8_t)(1 + k));
        assert(r[128 * BB] == 0 && r[200 * BB] == 0);
        /* the voice at frame 3000 (inside block 46): the lead advances by whole blocks */
        set_pos(sa, 3000);
        for (k = 128; k < 128 + 46; k++) assert(r[k * BB] == (uint8_t)(1 + k));   /* buffer 2's first 46 blocks */
        assert(r[(128 + 46) * BB] == 0);
        /* the output thread silences only blocks wholly played: 0..45 */
        al_stream_scrub_tick();
        assert(r[0] == 0 && r[45 * BB + BB - 1] == 0);
        assert(r[46 * BB] == (uint8_t)47);
        /* a seek while playing lands on a block start */
        alSourcei(sa, AL_SAMPLE_OFFSET, 4100);
        assert(geti(sa, AL_SAMPLE_OFFSET) == 4096 && geti(sa, AL_BUFFERS_PROCESSED) == 1);
        alSourceStop(sa);
        alSourcei(sa, AL_BUFFER, 0);
        alDeleteSources(1, &sa);
        alDeleteBuffers(3, ab);
        assert(alGetError() == AL_NO_ERROR);
    }
    printf("    -> blocks copied whole, the lead and scrub stop at block boundaries, seeks land on blocks [PASS]\n");

    printf("[8] AL_BUFFER 0 releases the queue\n");
    alSourcei(src, AL_BUFFER, 0);
    assert(geti(src, AL_BUFFERS_QUEUED) == 0);
    alDeleteBuffers(4, buf);
    alDeleteBuffers(1, &other);
    assert(alGetError() == AL_NO_ERROR);

    /* [10] The timed operations' clock (al_stream_clock): an application
     * unqueues buffers as the stream plays, which moves read_pos (and
     * AL_SAMPLE_OFFSET) back to the new start of the queue; the clock runs on
     * from the play, so a start timed to a frame of it happens there. On
     * read_pos it never did: the frame stayed ahead by everything unqueued
     * (the console, 2026-10-19). */
    printf("[10] the clock runs on through unqueues; a timed start lands on it\n");
    {
        ALuint cs, cb[4], snd, sb, done;
        const al_stream_t *cst;
        alGenSources(1, &cs);
        alGenSources(1, &snd);
        alGenBuffers(4, cb);
        alGenBuffers(1, &sb);
        for (i = 0; i < 4; i++) fill_buffer(cb[i], (int16_t)(i + 1));
        fill_buffer(sb, 9);
        alSourceQueueBuffers(cs, 4, cb);
        alSourcei(snd, AL_BUFFER, (ALint)sb);
        alSourcePlay(cs);
        assert(alGetError() == AL_NO_ERROR);
        cst = al_source_get(cs)->stream;
        assert(cst && al_stream_clock(cst) == 0u);
        set_pos(cs, 2048);
        set_pos(cs, CHUNK + 100);
        assert(geti(cs, AL_BUFFERS_PROCESSED) == 1);
        alSourceUnqueueBuffers(cs, 1, &done);
        assert(alGetError() == AL_NO_ERROR && done == cb[0]);
        assert(geti(cs, AL_SAMPLE_OFFSET) == 100);                 /* from the new start of the queue */
        assert(al_stream_clock(cst) == CHUNK + 100);               /* from the play */
        alXboxSourcePlayAtSample(snd, cs, 8000);
        assert(alGetError() == AL_NO_ERROR && geti(snd, AL_SOURCE_STATE) == AL_INITIAL);
        set_pos(cs, CHUNK + 100 + 2048);                           /* 1756 ahead; the window is still 0 */
        assert(geti(snd, AL_SOURCE_STATE) == AL_INITIAL);
        set_pos(cs, CHUNK + 100 + 4096);                           /* 292 late, window 1024: starts 292 in */
        assert(geti(snd, AL_SOURCE_STATE) == AL_PLAYING && geti(snd, AL_SAMPLE_OFFSET) == 292);
        assert(al_stream_clock(cst) == CHUNK + 100 + 4096 && geti(cs, AL_SAMPLE_OFFSET) == 100 + 4096);
        alSourceStop(snd);
        alSourceStop(cs);
        alSourcePlay(cs);                                          /* from a stop: the clock starts again */
        assert(al_stream_clock(cst) == 0u);
        alSourceStop(cs);
        alSourcei(cs, AL_BUFFER, 0);
        alSourcei(snd, AL_BUFFER, 0);
        alDeleteSources(1, &cs);
        alDeleteSources(1, &snd);
        alDeleteBuffers(1, &cb[0]);
        alDeleteBuffers(3, &cb[1]);
        alDeleteBuffers(1, &sb);
        assert(alGetError() == AL_NO_ERROR);
    }
    printf("    -> unqueues move the offset back and not the clock; the start lands on the clock's frame; a play from a stop starts it at 0 [PASS]\n");

    alDeleteSources(1, &src);
    alDeleteSources(1, &stat);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    free(mock);
    printf("=== all streaming tests passed ===\n");
    return 0;
}
