/*
 * A streaming source's ring is sized by its format: AL_STREAM_RING_FRAMES of
 * 8-bit mono take 16 KiB, of 16-bit stereo 64 KiB, of Xbox ADPCM whole
 * blocks. The ring stays in the stream's slot for the next stream; a format
 * that needs more replaces it, one that needs less keeps it.
 */
#define AL_ALEXT_PROTOTYPES
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include "al_source.h"
#include "apu_vp.h"

static uint8_t s_pcm[8192];

/* A new streaming source with one buffer of `format` queued; its ring's bytes */
static uint32_t stream_of(ALenum format, ALsizei bytes, ALuint *src, ALuint *buf) {
    alGenBuffers(1, buf);
    alBufferData(*buf, format, s_pcm, bytes, 22050);
    alGenSources(1, src);
    alSourceQueueBuffers(*src, 1, buf);
    assert(alGetError() == AL_NO_ERROR);
    return al_stream_debug_ring_bytes(al_source_get(*src)->stream);
}

static void drop(ALuint src, ALuint buf) {
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    ALuint src, buf;
    const uint32_t f = AL_STREAM_RING_FRAMES;
    uint32_t adpcm = apu_vp_frames_to_bytes(f, 1, 4);
    printf("=== a stream's ring sized by its format ===\n");
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    alGetError();

    printf("[1] each format's ring\n");
    assert(stream_of(AL_FORMAT_MONO8, 4096, &src, &buf) == f);
    assert(al_stream_ring_buffer(al_source_get(src)->stream)->size == (ALsizei)f);
    drop(src, buf);
    /* the slot is reused: a ring for more replaces the 16 KiB one */
    assert(stream_of(AL_FORMAT_STEREO16, 4096, &src, &buf) == f * 4u);
    drop(src, buf);
    /* one that needs less keeps it */
    assert(stream_of(AL_FORMAT_MONO8, 4096, &src, &buf) == f * 4u);
    assert(al_stream_ring_buffer(al_source_get(src)->stream)->size == (ALsizei)f);
    drop(src, buf);
    printf("    -> 8-bit mono 16 KiB, 16-bit stereo 64 KiB; a reused slot grows, never shrinks [PASS]\n");

    printf("[2] two at once, and ADPCM\n");
    {
        ALuint a, ab, b, bb;
        assert(stream_of(AL_FORMAT_MONO8, 4096, &a, &ab) == f * 4u);    /* the kept 64 KiB */
        assert(stream_of(AL_FORMAT_MONO16, 4096, &b, &bb) == f * 2u);   /* a new slot: 32 KiB */
        drop(a, ab);
        drop(b, bb);
    }
    assert(adpcm == (f / 64u) * 36u && adpcm < f);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("    -> a second stream gets its own size; ADPCM's ring is %u bytes of whole blocks [PASS]\n", adpcm);
    printf("=== a stream's ring sized by its format: all passed ===\n");
    return 0;
}
