/*
 * The engine-facing features (XASH3D_INTEGRATION_PLAN.md phase B) through the
 * OpenAL API on the software model: AL_SOFT_loop_points, AL_*_OFFSET,
 * AL_XBOX_source_control (direct gains, engine priority, virtualization) and
 * the hardware status queries. The voice's play position is set by the test
 * (apu_voice_debug_set_position), standing in for the VP's CBO.
 */
#include <assert.h>
#include <math.h>
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
#include "apu_voice.h"

#define MOCK_MMIO_SIZE 0x30000
#define FRAMES 1000

static int16_t s_pcm[4096];

static ALint geti(ALuint src, ALenum p) {
    ALint v = -12345;
    alGetSourcei(src, p, &v);
    return v;
}

static int slot_of(ALuint src) {
    return al_source_get(src)->hw_voice_idx;
}

static ALint hw_status(ALenum p) {
    ALint v = -1;
    alXboxGetHardwareStatus(p, &v);
    return v;
}

int main(void) {
    uint8_t *mock = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint buf, src;

    printf("=== engine features (loop points, offsets, direct gains, priority) host test ===\n");
    assert(mock);
    alc_set_apu_base((uintptr_t)mock);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    assert(alIsExtensionPresent("AL_SOFT_loop_points") && alIsExtensionPresent("AL_XBOX_source_control"));

    alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, s_pcm, FRAMES * 2, 48000);
    alGenSources(1, &src);
    assert(alGetError() == AL_NO_ERROR);

    /* [1] AL_SOFT_loop_points */
    printf("[1] loop points\n");
    {
        ALint v[2];
        const ALint ok[2] = { 100, 900 }, rev[2] = { 900, 100 }, big[2] = { 0, FRAMES + 1 };
        alGetBufferiv(buf, AL_LOOP_POINTS_SOFT, v);
        assert(v[0] == 0 && v[1] == FRAMES);                    /* default: the whole buffer */
        alBufferiv(buf, AL_LOOP_POINTS_SOFT, ok);
        assert(alGetError() == AL_NO_ERROR);
        alBufferiv(buf, AL_LOOP_POINTS_SOFT, rev);
        assert(alGetError() == AL_INVALID_VALUE);
        alBufferiv(buf, AL_LOOP_POINTS_SOFT, big);
        assert(alGetError() == AL_INVALID_VALUE);
        alGetBufferiv(buf, AL_LOOP_POINTS_SOFT, v);
        assert(v[0] == 100 && v[1] == 900);
        alSourcei(src, AL_BUFFER, (ALint)buf);
        alBufferiv(buf, AL_LOOP_POINTS_SOFT, ok);
        assert(alGetError() == AL_INVALID_OPERATION);           /* in use */
        alSourcei(src, AL_BUFFER, 0);
        alBufferData(buf, AL_FORMAT_MONO16, s_pcm, FRAMES * 2, 48000);
        alGetBufferiv(buf, AL_LOOP_POINTS_SOFT, v);
        assert(v[0] == 0 && v[1] == FRAMES);                    /* new data: reset */
        alSourcei(src, AL_BUFFER, (ALint)buf);
        assert(alGetError() == AL_NO_ERROR);
    }

    /* [2] Offsets on a static buffer */
    printf("[2] offsets\n");
    {
        ALfloat sec = -1.0f;
        alSourcei(src, AL_SAMPLE_OFFSET, 250);
        assert(geti(src, AL_SAMPLE_OFFSET) == 250 && geti(src, AL_BYTE_OFFSET) == 500);
        alGetSourcef(src, AL_SEC_OFFSET, &sec);
        assert(fabsf(sec - 250.0f / 48000.0f) < 1e-6f);
        alSourcePlay(src);
        assert(geti(src, AL_SOURCE_STATE) == AL_PLAYING);
        assert(apu_voice_get_position((uint32_t)slot_of(src)) == 250u);   /* the voice started there */
        apu_voice_debug_set_position((uint32_t)slot_of(src), 400);
        assert(geti(src, AL_SAMPLE_OFFSET) == 400);
        alSourcef(src, AL_SEC_OFFSET, 600.0f / 48000.0f);       /* seek while playing */
        assert(alGetError() == AL_NO_ERROR);
        assert(apu_voice_get_position((uint32_t)slot_of(src)) == 600u);
        alSourcei(src, AL_SAMPLE_OFFSET, FRAMES);
        assert(alGetError() == AL_INVALID_VALUE);               /* past the end */
        alSourcePause(src);
        alSourcei(src, AL_BYTE_OFFSET, 100);                    /* paused: moves, stays paused */
        assert(geti(src, AL_SOURCE_STATE) == AL_PAUSED && geti(src, AL_SAMPLE_OFFSET) == 50);
        alSourceStop(src);
        assert(geti(src, AL_SAMPLE_OFFSET) == 0);
        alSourcePlay(src);                                      /* no offset set: from the start */
        assert(apu_voice_get_position((uint32_t)slot_of(src)) == 0u);
        alSourceStop(src);
    }

    /* [3] Offsets on a streaming source: buffers before the offset are processed */
    printf("[3] stream offsets\n");
    {
        ALuint q[3], s2;
        int i;
        alGenBuffers(3, q);
        for (i = 0; i < 3; i++) alBufferData(q[i], AL_FORMAT_MONO16, s_pcm, (ALsizei)sizeof(s_pcm), 48000);
        alGenSources(1, &s2);
        alSourceQueueBuffers(s2, 3, q);
        alSourcei(s2, AL_SAMPLE_OFFSET, 5000);                  /* 4096-frame buffers: inside the second */
        alSourcePlay(s2);
        assert(geti(s2, AL_BUFFERS_PROCESSED) == 1 && geti(s2, AL_BUFFER) == (ALint)q[1]);
        assert(geti(s2, AL_SAMPLE_OFFSET) == 5000);
        alSourcei(s2, AL_SAMPLE_OFFSET, 100);                   /* back into the first while playing */
        assert(geti(s2, AL_BUFFERS_PROCESSED) == 0 && geti(s2, AL_SAMPLE_OFFSET) == 100);
        alSourceStop(s2);
        alSourcei(s2, AL_BUFFER, 0);
        alDeleteSources(1, &s2);
        alDeleteBuffers(3, q);
        assert(alGetError() == AL_NO_ERROR);
    }

    /* [4] Direct gains: the engine's pan, no distance, no Doppler */
    printf("[4] direct gains\n");
    {
        const ALfloat g[2] = { 1.0f, 0.5f }, bad[2] = { 1.5f, 0.0f };
        NVAPU_VOICE_CONTEXT_3D *vc;
        alSource3f(src, AL_POSITION, 100.0f, 0.0f, 0.0f);       /* far to the right */
        alSource3f(src, AL_VELOCITY, 50.0f, 0.0f, 0.0f);        /* receding: Doppler would lower the pitch */
        alSourcefv(src, AL_XBOX_DIRECT_GAINS, bad);
        assert(alGetError() == AL_INVALID_VALUE);
        alSourcefv(src, AL_XBOX_DIRECT_GAINS, g);
        assert(geti(src, AL_XBOX_DIRECT_MODE) == AL_TRUE);
        alSourcePlay(src);
        vc = apu_voice_get_context((uint32_t)slot_of(src));
        assert(vc->mixbin_gain[0] == 255 && vc->mixbin_gain[1] == 128);
        assert(vc->mixbin_gain[2] == 0 && vc->mixbin_gain[4] == 0);
        assert(vc->master_vol_left == 65535);                   /* no distance attenuation */
        assert(vc->pitch_step == 65536u);                       /* 48 kHz at pitch 1, no Doppler */
        alSourcei(src, AL_XBOX_DIRECT_MODE, AL_FALSE);          /* back to the 3D model */
        assert(vc->mixbin_gain[1] != 128 || vc->master_vol_left != 65535);
        alSourceStop(src);
        alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
        alSource3f(src, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    }

    /* [5] Engine priority and virtualization */
    printf("[5] priority / virtualize / voice count\n");
    {
        ALuint many[64], a, b, c, d;
        int i;
        assert(hw_status(AL_XBOX_FREE_VOICES) == 64);
        alGenSources(64, many);
        for (i = 0; i < 64; i++) {
            alSourcei(many[i], AL_BUFFER, (ALint)buf);
            alSourcef(many[i], AL_XBOX_PRIORITY, 1.0f);
            alSourcePlay(many[i]);
            assert(geti(many[i], AL_XBOX_HAS_VOICE) == AL_TRUE);
        }
        assert(hw_status(AL_XBOX_FREE_VOICES) == 0);
        alGenSources(1, &a);
        alGenSources(1, &b);
        alGenSources(1, &c);
        alGenSources(1, &d);
        alSourcei(a, AL_BUFFER, (ALint)buf);
        alSourcei(b, AL_BUFFER, (ALint)buf);
        alSourcei(c, AL_BUFFER, (ALint)buf);
        alSourcei(d, AL_BUFFER, (ALint)buf);

        alSourcef(a, AL_XBOX_PRIORITY, 0.5f);                   /* loses, and may not wait */
        alSourcei(a, AL_XBOX_VIRTUALIZE, AL_FALSE);
        alSourcePlay(a);
        assert(geti(a, AL_SOURCE_STATE) == AL_STOPPED && geti(a, AL_XBOX_HAS_VOICE) == AL_FALSE);

        alSourcef(b, AL_XBOX_PRIORITY, 0.5f);                   /* loses, waits in standby */
        alSourcePlay(b);
        assert(geti(b, AL_SOURCE_STATE) == AL_PLAYING && geti(b, AL_XBOX_HAS_VOICE) == AL_FALSE);

        /* c outranks them: it takes the oldest priority-1 voice (many[0]),
         * which goes to standby and remembers where it was */
        apu_voice_debug_set_position((uint32_t)slot_of(many[0]), 123);
        alSourcef(c, AL_XBOX_PRIORITY, 2.0f);
        alSourcePlay(c);
        assert(geti(c, AL_XBOX_HAS_VOICE) == AL_TRUE);
        assert(geti(many[0], AL_SOURCE_STATE) == AL_PLAYING && geti(many[0], AL_XBOX_HAS_VOICE) == AL_FALSE);
        assert(geti(many[0], AL_SAMPLE_OFFSET) == 123);

        /* a victim that may not wait stops instead */
        alSourcei(many[1], AL_XBOX_VIRTUALIZE, AL_FALSE);
        alSourcef(d, AL_XBOX_PRIORITY, 3.0f);
        alSourcePlay(d);
        assert(geti(d, AL_XBOX_HAS_VOICE) == AL_TRUE);
        assert(geti(many[1], AL_SOURCE_STATE) == AL_STOPPED);

        /* free a voice: the best waiting source (many[0], priority 1) is promoted and resumes */
        alSourceStop(c);
        alXboxUpdateVoices();
        assert(geti(many[0], AL_XBOX_HAS_VOICE) == AL_TRUE);
        assert(apu_voice_get_position((uint32_t)slot_of(many[0])) == 123u);
        assert(geti(b, AL_XBOX_HAS_VOICE) == AL_FALSE);

        for (i = 0; i < 64; i++) alSourceStop(many[i]);
        alSourceStop(b);
        alSourceStop(d);
        alXboxUpdateVoices();
        assert(hw_status(AL_XBOX_FREE_VOICES) == 64);
        assert(hw_status(AL_XBOX_SAMPLE_PAGES_TOTAL) == 2048);
        alDeleteSources(64, many);
        alDeleteSources(1, &a);
        alDeleteSources(1, &b);
        alDeleteSources(1, &c);
        alDeleteSources(1, &d);
        assert(alGetError() == AL_NO_ERROR);
    }

    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    free(mock);
    printf("=== all engine feature tests passed ===\n");
    return 0;
}
