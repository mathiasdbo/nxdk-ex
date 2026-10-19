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

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_buffer.h"
#include "al_source.h"
#include "apu_hardware.h"
#include "apu_voice.h"

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

static void test_deferred(void) {
    static const int16_t pcm[256] = { 0 };
    ALuint b = 0, s = 0, f = 0;
    ALsource *as;

    printf("[5] AL_SOFT_deferred_updates: changes wait for the process call\n");
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &s);
    alSourcei(s, AL_BUFFER, (ALint)b);
    alSourcei(s, AL_LOOPING, AL_TRUE);
    alSourcePlay(s);
    as = al_source_get(s);
    assert(as && as->hw_voice_idx >= 0);
    alGenFilters(1, &f);
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(f, AL_LOWPASS_GAINHF, 0.5f);

    alDeferUpdatesSOFT();
    alSourcei(s, AL_DIRECT_FILTER, (ALint)f);
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);   /* held back */
    alListenerf(AL_GAIN, 0.5f);                                            /* listener changes too */
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);
    alProcessUpdatesSOFT();
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 0.5f);
    assert(as->hw_pending == 0u);
    /* not deferred: at once */
    alSourcei(s, AL_DIRECT_FILTER, AL_FILTER_NULL);
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);
    alListenerf(AL_GAIN, 1.0f);

    alSourceStop(s);
    alDeleteFilters(1, &f);
    alDeleteSources(1, &s);
    alDeleteBuffers(1, &b);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_latency(void) {
    static int16_t pcm[48000];
    ALuint b = 0, s = 0;
    ALint v = 0;
    ALint64SOFT v64[2] = { 0, 0 };
    ALdouble d[2] = { 0.0, 0.0 };

    printf("[6] AL_SOFT_source_latency: 64-bit and double forms, offset and latency pairs\n");
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &s);
    alSourcei(s, AL_BUFFER, (ALint)b);

    alSourcedSOFT(s, AL_SEC_OFFSET, 0.5);
    alGetSourcei(s, AL_SAMPLE_OFFSET, &v);
    assert(v == 24000);
    alGetSourcedSOFT(s, AL_SEC_OFFSET, &d[0]);
    assert(d[0] == 0.5);
    alSourcei64SOFT(s, AL_SAMPLE_OFFSET, 12345);
    alGetSourcei64SOFT(s, AL_SAMPLE_OFFSET, &v64[0]);
    assert(v64[0] == 12345);
    alGetSourcei64vSOFT(s, AL_SAMPLE_OFFSET_LATENCY_SOFT, v64);
    assert(v64[0] == (ALint64SOFT)12345 << 32 && v64[1] == 0);   /* no output on the host */
    alGetSourcedvSOFT(s, AL_SEC_OFFSET_LATENCY_SOFT, d);
    assert(fabs(d[0] - 12345.0 / 48000.0) < 1e-12 && d[1] == 0.0);
    assert(alGetError() == AL_NO_ERROR);

    /* the other forms pass through */
    alSource3dSOFT(s, AL_POSITION, 1.0, 2.0, 3.0);
    alGetSource3i64SOFT(s, AL_POSITION, &v64[0], &v64[1], &v64[0]);
    {
        ALdouble p[3] = { 0.0, 0.0, 0.0 };
        alGetSourcedvSOFT(s, AL_POSITION, p);
        assert(p[0] == 1.0 && p[1] == 2.0 && p[2] == 3.0);
    }
    alSourcei64SOFT(s, AL_LOOPING, AL_TRUE);
    alGetSourcei(s, AL_LOOPING, &v);
    assert(v == AL_TRUE);
    assert(alGetError() == AL_NO_ERROR);

    /* errors */
    alSourcei64SOFT(s, AL_LOOPING, 0x100000000LL);
    assert(alGetError() == AL_INVALID_VALUE);
    alSourcedSOFT(s, AL_SEC_OFFSET_LATENCY_SOFT, 1.0);
    assert(alGetError() == AL_INVALID_OPERATION);
    alGetSourcedSOFT(s, AL_SEC_OFFSET_LATENCY_SOFT, d);
    assert(alGetError() == AL_INVALID_ENUM);
    alGetSourcei64vSOFT(s, AL_SEC_OFFSET_LATENCY_SOFT, v64);
    assert(alGetError() == AL_INVALID_ENUM);
    alGetSourcei64vSOFT(9999u, AL_SAMPLE_OFFSET_LATENCY_SOFT, v64);
    assert(alGetError() == AL_INVALID_NAME);

    alDeleteSources(1, &s);
    alDeleteBuffers(1, &b);
}

/* The null backend's pause bit of voice idx (NV_PAPU_VP_PAUSE_0/1) */
static int voice_paused(int idx) {
    uintptr_t base = al_source_get_apu_base();
    uint32_t reg = (idx < 32) ? NV_PAPU_VP_PAUSE_0 : NV_PAPU_VP_PAUSE_1;
    return (apu_read32(base, reg) >> (idx & 31)) & 1u;
}

static void test_pause_device(ALCdevice *dev) {
    static const int16_t pcm[256] = { 0 };
    ALuint b = 0, s1 = 0, s2 = 0;
    ALint st = 0;
    ALsource *a1, *a2;

    printf("[7] ALC_SOFT_pause_device: voices held, sources stay AL_PLAYING\n");
    alGenBuffers(1, &b);
    alBufferData(b, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &s1);
    alGenSources(1, &s2);
    alSourcei(s1, AL_BUFFER, (ALint)b);
    alSourcei(s2, AL_BUFFER, (ALint)b);
    alSourcei(s1, AL_LOOPING, AL_TRUE);
    alSourcei(s2, AL_LOOPING, AL_TRUE);
    alSourcePlay(s1);
    a1 = al_source_get(s1);
    assert(a1->hw_voice_idx >= 0 && !voice_paused(a1->hw_voice_idx));

    alcDevicePauseSOFT(dev);
    assert(alcGetError(dev) == ALC_NO_ERROR);
    assert(voice_paused(a1->hw_voice_idx));
    alGetSourcei(s1, AL_SOURCE_STATE, &st);
    assert(st == AL_PLAYING);
    alSourcePlay(s2);                       /* started while paused: waits */
    a2 = al_source_get(s2);
    assert(a2->hw_voice_idx >= 0 && voice_paused(a2->hw_voice_idx));
    alGetSourcei(s2, AL_SOURCE_STATE, &st);
    assert(st == AL_PLAYING);
    alSourcePause(s1);                      /* paused and resumed meanwhile: still waits */
    alSourcePlay(s1);
    assert(voice_paused(a1->hw_voice_idx));

    alcDeviceResumeSOFT(dev);
    assert(!voice_paused(a1->hw_voice_idx) && !voice_paused(a2->hw_voice_idx));
    alcDevicePauseSOFT(NULL);
    assert(alcGetError(NULL) == ALC_INVALID_DEVICE);

    alSourceStop(s1);
    alSourceStop(s2);
    alDeleteSources(1, &s1);
    alDeleteSources(1, &s2);
    alDeleteBuffers(1, &b);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_hrtf(ALCdevice *dev) {
    ALCint v = -1;
    const ALCint off[] = { ALC_HRTF_SOFT, ALC_FALSE, 0 };
    const ALCint on[] = { ALC_HRTF_SOFT, ALC_TRUE, 0 };
    const ALCint any[] = { ALC_HRTF_SOFT, ALC_DONT_CARE_SOFT, 0 };

    printf("[8] ALC_SOFT_HRTF: the APU's HRTF switch\n");
    assert(alcIsExtensionPresent(dev, "ALC_SOFT_HRTF") == ALC_TRUE);
    alcGetIntegerv(dev, ALC_HRTF_STATUS_SOFT, 1, &v);
    assert(v == ALC_HRTF_ENABLED_SOFT);             /* on by default, as AL_XBOX_HRTF */
    assert(alcResetDeviceSOFT(dev, off) == ALC_TRUE);
    assert(alIsEnabled(AL_XBOX_HRTF) == AL_FALSE);
    alcGetIntegerv(dev, ALC_HRTF_SOFT, 1, &v);
    assert(v == ALC_FALSE);
    alcGetIntegerv(dev, ALC_HRTF_STATUS_SOFT, 1, &v);
    assert(v == ALC_HRTF_DISABLED_SOFT);
    alcResetDeviceSOFT(dev, any);                   /* as it is */
    assert(alIsEnabled(AL_XBOX_HRTF) == AL_FALSE);
    alcResetDeviceSOFT(dev, on);
    assert(alIsEnabled(AL_XBOX_HRTF) == AL_TRUE);
    alcGetIntegerv(dev, ALC_NUM_HRTF_SPECIFIERS_SOFT, 1, &v);
    assert(v == 1);
    assert(strcmp(alcGetStringiSOFT(dev, ALC_HRTF_SPECIFIER_SOFT, 0), "MCPX APU HRTF") == 0);
    assert(alcGetStringiSOFT(dev, ALC_HRTF_SPECIFIER_SOFT, 1) == NULL);
    assert(alcGetError(dev) == ALC_INVALID_VALUE);
    {
        ALCcontext *c2 = alcCreateContext(dev, off);   /* at context creation too */
        assert(c2 && alIsEnabled(AL_XBOX_HRTF) == AL_FALSE);
        alcDestroyContext(c2);
        alcResetDeviceSOFT(dev, on);
    }
    assert(alcGetError(dev) == ALC_NO_ERROR);
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
    test_deferred();
    test_latency();
    test_pause_device(dev);
    test_hrtf(dev);

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== OpenAL Soft extensions: all passed ===\n");
    return 0;
}
