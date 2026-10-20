/*
 * Five minutes of a fed stream: 8-bit mono at 11025 Hz, as an engine feeds a
 * speech channel onto a streaming source - a piece of 48 ms queued whenever
 * less than 100 ms is left ahead of the voice, the played ones unqueued every
 * update, the update ~30 times a second with a 60 ms hitch every 30 s. The
 * voice's play position is set by the test (apu_voice_debug_set_position, as
 * the VP's CBO). Every update: the ring holds exactly the frames fed, from the
 * play position to the end of what was fed (or the lead); the stream clock is
 * the frames played, though every unqueue moves the positions back; nothing
 * starves. The ring is 16 KiB (sized by the format), so the play position
 * laps it about 200 times.
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
#include "al_stream.h"
#include "apu_voice.h"

#define MOCK_MMIO_SIZE 0x30000
#define RATE      11025u
#define SECONDS   300u
#define PIECE     529u    /* 48 ms */
#define LEAD_MIN  1103u   /* 100 ms */
#define NBUF      16

/* Frame f of the fed data: never the silence byte, so silence in its place shows */
static uint8_t value_of(uint32_t f) {
    uint8_t v = (uint8_t)(f * 13u + (f >> 11));
    return v == 0x80u ? 0x81u : v;
}

int main(void) {
    uint8_t *mock = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    static uint8_t piece[PIECE];
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint src, bufs[NBUF], free_bufs[NBUF];
    uint32_t n_free = NBUF, fed = 0, played = 0, updates = 0, unqueued = 0, laps;
    const uint32_t total = RATE * SECONDS;
    ALsource *s;
    int i;

    printf("=== five minutes of a fed 11025 Hz 8-bit mono stream ===\n");
    assert(mock);
    alc_set_apu_base((uintptr_t)mock);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    alGenSources(1, &src);
    alGenBuffers(NBUF, bufs);
    for (i = 0; i < NBUF; i++) {
        free_bufs[i] = bufs[NBUF - 1 - i];
    }
    assert(alGetError() == AL_NO_ERROR);

    /* the first 100 ms, then play */
    while (fed < LEAD_MIN) {
        ALuint b = free_bufs[--n_free];
        uint32_t k;
        for (k = 0; k < PIECE; k++) piece[k] = value_of(fed + k);
        alBufferData(b, AL_FORMAT_MONO8, piece, PIECE, RATE);
        alSourceQueueBuffers(src, 1, &b);
        fed += PIECE;
    }
    alSourcePlay(src);
    assert(alGetError() == AL_NO_ERROR);
    s = al_source_get(src);
    assert(s->hw_voice_idx >= 0 && s->stream);
    assert(al_stream_debug_ring_bytes(s->stream) == AL_STREAM_RING_FRAMES);   /* 1 byte a frame */

    while (played < total) {
        const ALbuffer *rb;
        const uint8_t *ring;
        uint32_t step = (updates % 2u) ? 368u : 367u, end, f;
        ALint processed = 0;

        if (updates > 0u && updates % 900u == 0u) {
            step = 661u;   /* a 60 ms hitch */
        }
        played += step;
        apu_voice_debug_set_position((uint32_t)s->hw_voice_idx, played % AL_STREAM_RING_FRAMES);
        alXboxUpdateVoices();
        updates++;
        alGetSourcei(src, AL_SOURCE_STATE, &processed);
        assert(processed == AL_PLAYING);

        /* the played pieces back, the lead topped up */
        processed = 0;
        alGetSourcei(src, AL_BUFFERS_PROCESSED, &processed);
        while (processed-- > 0) {
            ALuint b = 0;
            alSourceUnqueueBuffers(src, 1, &b);
            assert(alGetError() == AL_NO_ERROR && n_free < NBUF);
            free_bufs[n_free++] = b;
            unqueued++;
        }
        while (fed - played < LEAD_MIN) {
            ALuint b;
            uint32_t k;
            assert(n_free > 0u);
            b = free_bufs[--n_free];
            for (k = 0; k < PIECE; k++) piece[k] = value_of(fed + k);
            alBufferData(b, AL_FORMAT_MONO8, piece, PIECE, RATE);
            alSourceQueueBuffers(src, 1, &b);
            assert(alGetError() == AL_NO_ERROR);
            fed += PIECE;
        }
        alXboxUpdateVoices();   /* the new pieces into the ring (the position has not moved) */

        assert(al_stream_clock(s->stream) == played);
        assert(al_stream_starved(s->stream) == 0u);
        rb = al_source_play_buffer(s);
        assert(rb && rb->size == (ALsizei)AL_STREAM_RING_FRAMES);
        ring = (const uint8_t *)rb->data_virt;
        end = fed;
        if (end - played > AL_STREAM_LEAD_FRAMES) {
            end = played + AL_STREAM_LEAD_FRAMES;
        }
        for (f = played; f < end; f++) {
            if (ring[f % AL_STREAM_RING_FRAMES] != value_of(f)) {
                printf("    frame %u (update %u): ring 0x%02x, fed 0x%02x\n", f, updates,
                       ring[f % AL_STREAM_RING_FRAMES], value_of(f));
                assert(0);
            }
        }
    }
    laps = played / AL_STREAM_RING_FRAMES;
    alSourceStop(src);
    alDeleteSources(1, &src);
    alDeleteBuffers(NBUF, bufs);
    assert(alGetError() == AL_NO_ERROR);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("    -> %u updates, %u frames played (%u laps of the ring), %u pieces unqueued: every frame fed as fed, "
           "the clock the frames played, nothing starved [PASS]\n", updates, played, laps, unqueued);
    printf("=== five minutes of a fed stream: all passed ===\n");
    free(mock);
    return 0;
}
