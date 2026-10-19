/*
 * AL_FILTER_LOWPASS (lib/openal/docs/EFX_PLAN.md 3.7): the filter object
 * (type, AL_LOWPASS_GAIN / GAINHF, ranges, defaults), the source's copy of it,
 * GAIN on the dry sound and on a send, GAINHF on the voice filter, and the fit
 * of GAINHF at 5 kHz to the VP's 2-pole Butterworth (apu_vp_lowpass).
 */
#include <assert.h>
#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "alc_context.h"
#include "al_efx.h"
#include "al_source.h"
#include "apu_voice.h"
#include "apu_vp.h"

#define MOCK_MMIO_SIZE 0x80000

static int near(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static int16_t cut_of(float gain_hf) {
    uint32_t fc = 0;
    uint32_t mode = apu_vp_lowpass(gain_hf, &fc);
    assert(mode == 1u && (fc >> 16) == APU_VP_LP_DAMPING);
    return (int16_t)(fc & 0xFFFFu);
}

static void test_object(void) {
    ALuint f = 0;
    ALint iv;
    ALfloat fv;

    printf("[1] the filter object\n");
    alGenFilters(1, &f);
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    assert(alGetError() == AL_NO_ERROR);
    alGetFilteri(f, AL_FILTER_TYPE, &iv);
    assert(iv == AL_FILTER_LOWPASS);
    alGetFilterf(f, AL_LOWPASS_GAIN, &fv);
    assert(fv == 1.0f);
    alGetFilterf(f, AL_LOWPASS_GAINHF, &fv);
    assert(fv == 1.0f);
    alFilterf(f, AL_LOWPASS_GAIN, 0.5f);
    alFilterf(f, AL_LOWPASS_GAINHF, 0.25f);
    assert(alGetError() == AL_NO_ERROR);
    alGetFilterf(f, AL_LOWPASS_GAINHF, &fv);
    assert(fv == 0.25f);
    alFilterf(f, AL_LOWPASS_GAIN, 1.5f);
    assert(alGetError() == AL_INVALID_VALUE);
    alFilterf(f, AL_LOWPASS_GAINHF, -0.1f);
    assert(alGetError() == AL_INVALID_VALUE);
    alGetFilterf(f, AL_LOWPASS_GAIN, &fv);
    assert(fv == 0.5f);                                     /* unchanged */
    alFilterf(f, AL_FILTER_TYPE, 1.0f);
    assert(alGetError() == AL_INVALID_ENUM);
    alFilterf(9999u, AL_LOWPASS_GAIN, 0.5f);
    assert(alGetError() == AL_INVALID_NAME);
    /* the type set again resets the parameters */
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alGetFilterf(f, AL_LOWPASS_GAIN, &fv);
    assert(fv == 1.0f);
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_NULL);
    alFilterf(f, AL_LOWPASS_GAIN, 0.5f);
    assert(alGetError() == AL_INVALID_ENUM);
    alDeleteFilters(1, &f);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_fit(void) {
    uint32_t fc;
    float g;
    int16_t prev = 0;

    printf("[2] GAINHF at 5 kHz -> the voice filter's cutoff (the levels measured on a console)\n");
    assert(apu_vp_lowpass(1.0f, &fc) == 0u && fc == 0u);       /* no high cut: no filter */
    assert(apu_vp_lowpass(0.999f, &fc) == 0u);                 /* within 0.05 dB */
    /* measured points: -1.6 dB at -0x400, -4.7 dB at -0x1000, -15.0 dB at -0x2000, -40.0 dB at -0x4000 */
    assert(abs(cut_of(powf(10.0f, -1.6f / 20.0f)) + 0x400) <= 2);
    assert(abs(cut_of(powf(10.0f, -4.7f / 20.0f)) + 0x1000) <= 2);
    assert(abs(cut_of(powf(10.0f, -15.0f / 20.0f)) + 0x2000) <= 2);
    assert(abs(cut_of(0.01f) + 0x4000) <= 2);
    assert(cut_of(0.5f) < -0x1000 && cut_of(0.5f) > -0x1800);  /* -6 dB: between -4.7 and -9.1 */
    assert(cut_of(0.0f) == APU_VP_LP_CUT_MIN && cut_of(1e-6f) == APU_VP_LP_CUT_MIN);
    assert(cut_of(0.99f) < 0);
    /* lower GAINHF, lower cutoff, all the way down */
    for (g = 0.99f; g > 0.0002f; g *= 0.9f) {
        int16_t c = cut_of(g);
        assert(c <= prev);
        prev = c;
    }
}

static void test_source(uint8_t *mmio) {
    ALuint f = 0, src = 0, buf = 0, e = 0, slot = 0;
    int16_t pcm[256] = { 0 };
    AL_SPATIAL_CALC calc;
    float s[APU_VOICE_SENDS];
    ALsource *as;
    (void)mmio;

    printf("[3] a source's direct and send filters\n");
    alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alGenFilters(1, &f);
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(f, AL_LOWPASS_GAIN, 0.5f);
    alFilterf(f, AL_LOWPASS_GAINHF, 0.25f);
    alSourcePlay(src);
    as = al_source_get(src);
    assert(as && as->hw_voice_idx >= 0);
    al_source_calc_spatial(as, &calc);
    assert(near(calc.effective_gain, 1.0f, 1e-6f));
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);

    /* the direct filter: GAIN on the dry sound, GAINHF on the voice, live */
    alSourcei(src, AL_DIRECT_FILTER, (ALint)f);
    assert(alGetError() == AL_NO_ERROR);
    al_source_calc_spatial(as, &calc);
    assert(near(calc.effective_gain, 0.5f, 1e-6f));
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 0.25f);
    /* a copy: changing the filter object does nothing until it is set again */
    alFilterf(f, AL_LOWPASS_GAINHF, 0.75f);
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 0.25f);
    alSourcei(src, AL_DIRECT_FILTER, (ALint)f);
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 0.75f);
    /* deleting the filter leaves the source's copy */
    alDeleteFilters(1, &f);
    al_source_calc_spatial(as, &calc);
    assert(near(calc.effective_gain, 0.5f, 1e-6f));
    /* a restart keeps it */
    alSourceStop(src);
    alSourcePlay(src);
    as = al_source_get(src);
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 0.75f);
    alSourcei(src, AL_DIRECT_FILTER, AL_FILTER_NULL);
    al_source_calc_spatial(as, &calc);
    assert(near(calc.effective_gain, 1.0f, 1e-6f));
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);

    /* a send filter: its GAIN on the send, not on the dry sound; its GAINHF is not applied */
    alGenFilters(1, &f);
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(f, AL_LOWPASS_GAIN, 0.25f);
    alFilterf(f, AL_LOWPASS_GAINHF, 0.1f);
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    alEffectf(e, AL_REVERB_ROOM_ROLLOFF_FACTOR, 0.0f);
    alGenAuxiliaryEffectSlots(1, &slot);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
    assert(al_source_debug_sends(src, s) && near(s[0], 1.0f, 1e-6f));
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, (ALint)f);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_debug_sends(src, s) && near(s[0], 0.25f, 1e-6f));
    al_source_calc_spatial(as, &calc);
    assert(near(calc.effective_gain, 1.0f, 1e-6f));
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);

    alSourceStop(src);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    alDeleteEffects(1, &e);
    alDeleteFilters(1, &f);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);
}

/* AL_DIRECT_FILTER_GAINHF_AUTO: cone outer GAINHF and air absorption on the voice filter */
static void test_auto_hf(void) {
    ALuint f = 0, src = 0, buf = 0;
    int16_t pcm[256] = { 0 };
    const float air10 = powf(0.99426f, 10.0f);   /* 10 m beyond the reference distance */
    ALsource *as;
    float lp;

    printf("[4] cone outer GAINHF and air absorption (GAINHF auto)\n");
    alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -11.0f);   /* 11 units ahead, reference distance 1 */
    alSourcePlay(src);
    as = al_source_get(src);
    assert(as && as->hw_voice_idx >= 0);
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);   /* defaults: factor 0, GAINHF 1 */

    alSourcef(src, AL_AIR_ABSORPTION_FACTOR, 1.0f);
    lp = apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx);
    assert(near(lp, air10, 1e-5f));
    alListenerf(AL_METERS_PER_UNIT, 2.0f);   /* 20 m now */
    assert(near(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx), air10 * air10, 1e-5f));
    alListenerf(AL_METERS_PER_UNIT, 1.0f);
    alSourcef(src, AL_MAX_DISTANCE, 6.0f);   /* clamped to 5 m */
    assert(near(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx), powf(0.99426f, 5.0f), 1e-5f));
    alSourcef(src, AL_MAX_DISTANCE, FLT_MAX);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -0.5f);   /* inside the reference distance: none */
    assert(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx) == 1.0f);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -11.0f);

    /* the cone: pointing away, AL_CONE_OUTER_GAIN 1 (the volume stays), GAINHF 0.5 */
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, -1.0f);
    alSourcef(src, AL_CONE_INNER_ANGLE, 30.0f);
    alSourcef(src, AL_CONE_OUTER_ANGLE, 90.0f);
    alSourcef(src, AL_CONE_OUTER_GAINHF, 0.5f);
    assert(alGetError() == AL_NO_ERROR);
    assert(near(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx), 0.5f * air10, 1e-5f));
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, 1.0f);   /* facing the listener: inside the cone */
    assert(near(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx), air10, 1e-5f));
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, -1.0f);

    /* with the direct filter: the gains multiply */
    alGenFilters(1, &f);
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    alFilterf(f, AL_LOWPASS_GAINHF, 0.5f);
    alSourcei(src, AL_DIRECT_FILTER, (ALint)f);
    assert(near(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx), 0.25f * air10, 1e-5f));

    /* GAINHF auto off: the filter only */
    alSourcei(src, AL_DIRECT_FILTER_GAINHF_AUTO, AL_FALSE);
    assert(near(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx), 0.5f, 1e-6f));
    alSourcei(src, AL_DIRECT_FILTER_GAINHF_AUTO, AL_TRUE);

    /* direct mode: the engine spatializes, the filter only */
    {
        const ALfloat g[2] = { 1.0f, 1.0f };
        alSourcefv(src, AL_XBOX_DIRECT_GAINS, g);
    }
    assert(near(apu_voice_debug_lowpass((uint32_t)as->hw_voice_idx), 0.5f, 1e-6f));

    alSourceStop(src);
    alDeleteFilters(1, &f);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);
}

int main(void) {
    uint8_t *mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    ALCdevice *dev;
    ALCcontext *ctx;

    assert(mmio);
    alc_set_apu_base((uintptr_t)mmio);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));

    test_object();
    test_fit();
    test_source(mmio);
    test_auto_hf();

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    free(mmio);
    printf("=== AL_FILTER_LOWPASS: all passed ===\n");
    return 0;
}
