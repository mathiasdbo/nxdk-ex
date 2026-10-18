/*
 * The OpenAL Soft compatible extensions (docs/SOFT_GAP_PLAN.md phase9-03):
 * AL_EXT_FLOAT32, AL_SOFT_source_spatialize, AL_EXT_source_distance_model,
 * AL_SOFT_gain_clamp_ex, ALC_EXT_disconnect, alGetEnumValue / alcGetEnumValue.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_buffer.h"
#include "al_source.h"

static int near(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static void test_float32(void) {
    static const float in[6] = { 0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -0.25f };
    ALuint b = 0;
    ALint v = 0;
    const ALbuffer *buf;
    const int16_t *pcm;

    printf("[1] AL_EXT_FLOAT32: converted to 16-bit when loaded\n");
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO_FLOAT32, in, (ALsizei)sizeof(in), 22050);
    assert(alGetError() == AL_NO_ERROR);
    alGetBufferi(b, AL_BITS, &v);
    assert(v == 16);
    alGetBufferi(b, AL_SIZE, &v);
    assert(v == 12);
    alGetBufferi(b, AL_FREQUENCY, &v);
    assert(v == 22050);
    buf = al_buffer_get(b);
    assert(buf && buf->data_virt);
    pcm = (const int16_t *)buf->data_virt;
    assert(pcm[0] == 0 && pcm[1] == 32767 && pcm[2] == -32767 && pcm[3] == 16384);
    assert(pcm[4] == 32767 && pcm[5] == -8192);   /* clipped; rounded to nearest */
    alBufferData(b, AL_FORMAT_STEREO_FLOAT32, in, 4, 48000);   /* half a stereo frame */
    assert(alGetError() == AL_INVALID_VALUE);
    alBufferDataStatic(b, AL_FORMAT_MONO_FLOAT32, in, (ALsizei)sizeof(in), 48000);
    assert(alGetError() == AL_INVALID_ENUM);   /* a static buffer plays as it is */
    alDeleteBuffers(1, &b);
}

static ALuint make_source(ALenum format, ALuint *b) {
    static const int16_t pcm[64] = { 0 };
    ALuint s = 0;
    alGenBuffers(1, b);
    alBufferData(*b, format, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &s);
    alSourcei(s, AL_BUFFER, (ALint)*b);
    alSource3f(s, AL_POSITION, 0.0f, 0.0f, -10.0f);   /* inverse clamped, reference 1: gain 0.1 */
    assert(alGetError() == AL_NO_ERROR);
    return s;
}

static float gain_of(ALuint s) {
    AL_SPATIAL_CALC calc;
    al_source_calc_spatial(al_source_get(s), &calc);
    return calc.effective_gain;
}

static void test_spatialize(void) {
    ALuint bm = 0, bs = 0, mono, stereo;
    AL_SPATIAL_CALC calc;
    ALint v = 0;

    printf("[2] AL_SOFT_source_spatialize\n");
    mono = make_source(AL_FORMAT_MONO16, &bm);
    stereo = make_source(AL_FORMAT_STEREO16, &bs);
    alGetSourcei(mono, AL_SOURCE_SPATIALIZE_SOFT, &v);
    assert(v == AL_AUTO_SOFT);

    /* AL_AUTO_SOFT: mono spatialized, stereo not (OpenAL 1.1) */
    assert(near(gain_of(mono), 0.1f, 1e-5f));
    assert(near(gain_of(stereo), 1.0f, 1e-6f));

    /* AL_TRUE: the stereo sound takes the distance too */
    alSourcei(stereo, AL_SOURCE_SPATIALIZE_SOFT, AL_TRUE);
    assert(near(gain_of(stereo), 0.1f, 1e-5f));

    /* AL_FALSE: the mono sound plays centred, AL_GAIN and the listener gain only */
    alSourcei(mono, AL_SOURCE_SPATIALIZE_SOFT, AL_FALSE);
    alSourcef(mono, AL_GAIN, 0.5f);
    al_source_calc_spatial(al_source_get(mono), &calc);
    assert(near(calc.effective_gain, 0.5f, 1e-6f) && calc.doppler_pitch == 1.0f);
    assert(calc.mixbin_gain[0] == 180 && calc.mixbin_gain[1] == 180);   /* 0.7071 a side */
    alSourcei(mono, AL_SOURCE_SPATIALIZE_SOFT, AL_AUTO_SOFT);
    alSourcef(mono, AL_GAIN, 1.0f);
    assert(near(gain_of(mono), 0.1f, 1e-5f));

    alSourcei(mono, AL_SOURCE_SPATIALIZE_SOFT, 5);
    assert(alGetError() == AL_INVALID_VALUE);
    alDeleteSources(1, &mono);
    alDeleteSources(1, &stereo);
    alDeleteBuffers(1, &bm);
    alDeleteBuffers(1, &bs);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_distance_model(void) {
    ALuint b = 0, s;

    printf("[3] AL_EXT_source_distance_model\n");
    s = make_source(AL_FORMAT_MONO16, &b);
    alSourcei(s, AL_DISTANCE_MODEL, AL_NONE);
    assert(near(gain_of(s), 0.1f, 1e-5f));   /* not enabled: the global model */
    alEnable(AL_SOURCE_DISTANCE_MODEL);
    assert(near(gain_of(s), 1.0f, 1e-6f));   /* the source's own: none */
    alSourcei(s, AL_DISTANCE_MODEL, AL_LINEAR_DISTANCE_CLAMPED);
    alSourcef(s, AL_MAX_DISTANCE, 20.0f);
    assert(near(gain_of(s), 1.0f - 9.0f / 19.0f, 1e-5f));
    alDisable(AL_SOURCE_DISTANCE_MODEL);
    assert(near(gain_of(s), 0.1f, 1e-5f));
    alDeleteSources(1, &s);
    alDeleteBuffers(1, &b);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_queries(ALCdevice *dev) {
    ALCint c = -1;

    printf("[4] gain limit, ALC_CONNECTED, enum values\n");
    assert(alGetFloat(AL_GAIN_LIMIT_SOFT) == 1.0f);
    alcGetIntegerv(dev, ALC_CONNECTED, 1, &c);
    assert(c == ALC_TRUE && alcGetError(dev) == ALC_NO_ERROR);
    assert(alcIsExtensionPresent(dev, "ALC_EXT_disconnect") == ALC_TRUE);
    assert(alIsExtensionPresent("AL_SOFT_source_spatialize") == AL_TRUE);
    assert(alGetEnumValue("AL_FORMAT_MONO_FLOAT32") == AL_FORMAT_MONO_FLOAT32);
    assert(alGetEnumValue("AL_SOURCE_SPATIALIZE_SOFT") == AL_SOURCE_SPATIALIZE_SOFT);
    assert(alGetEnumValue("AL_EFFECT_REVERB") == AL_EFFECT_REVERB);
    assert(alGetEnumValue("AL_INVERSE_DISTANCE_CLAMPED") == AL_INVERSE_DISTANCE_CLAMPED);
    assert(alGetEnumValue("AL_NOT_A_TOKEN") == 0 && alGetEnumValue(NULL) == 0);
    assert(alcGetEnumValue(dev, "ALC_CONNECTED") == ALC_CONNECTED);
    assert(alcGetEnumValue(dev, "ALC_MAX_AUXILIARY_SENDS") == ALC_MAX_AUXILIARY_SENDS);
    assert(alcGetEnumValue(dev, "ALC_NOPE") == 0);
    assert(alGetError() == AL_NO_ERROR);
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;

    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));

    test_float32();
    test_spatialize();
    test_distance_model();
    test_queries(dev);

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== OpenAL Soft extensions: all passed ===\n");
    return 0;
}
