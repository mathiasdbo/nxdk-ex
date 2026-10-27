/*
 * AL_EFFECT_EQUALIZER on the library's GP kernel (docs/SOFT_GAP_PLAN.md
 * phase9-07): the effect object (defaults, ranges), the kernel a slot runs and
 * its words: only the bands not at gain 1, in order, each OpenAL Soft's
 * shelf or peak designed with the square root of its gain, quartered. The
 * kernel's sound is checked in tests/test_gp_onchip.py [3].
 */
#define AL_ALEXT_PROTOTYPES
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_efx.h"
#include "al_source.h"
#include "gp_kefx_equalizer.h"

#define PI 3.14159265358979323846

static double q(uint32_t w) {
    return (double)((int32_t)(w << 8) >> 8) / 8388608.0;
}

static void test_object(ALuint e) {
    static const struct {
        ALenum param;
        ALfloat def, lo, hi;
    } t[] = {
        { AL_EQUALIZER_LOW_GAIN, 1.0f, 0.126f, 7.943f },     { AL_EQUALIZER_LOW_CUTOFF, 200.0f, 50.0f, 800.0f },
        { AL_EQUALIZER_MID1_GAIN, 1.0f, 0.126f, 7.943f },    { AL_EQUALIZER_MID1_CENTER, 500.0f, 200.0f, 3000.0f },
        { AL_EQUALIZER_MID1_WIDTH, 1.0f, 0.01f, 1.0f },      { AL_EQUALIZER_MID2_GAIN, 1.0f, 0.126f, 7.943f },
        { AL_EQUALIZER_MID2_CENTER, 3000.0f, 1000.0f, 8000.0f }, { AL_EQUALIZER_MID2_WIDTH, 1.0f, 0.01f, 1.0f },
        { AL_EQUALIZER_HIGH_GAIN, 1.0f, 0.126f, 7.943f },    { AL_EQUALIZER_HIGH_CUTOFF, 6000.0f, 4000.0f, 16000.0f },
    };
    ALfloat f = 0.0f;
    size_t i;
    printf("[1] the equalizer: defaults, ranges\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_EQUALIZER);
    assert(alGetError() == AL_NO_ERROR);
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        alGetEffectf(e, t[i].param, &f);
        assert(f == t[i].def);
        alEffectf(e, t[i].param, t[i].lo * 0.99f);
        assert(alGetError() == AL_INVALID_VALUE);
        alEffectf(e, t[i].param, t[i].hi * 1.01f);
        assert(alGetError() == AL_INVALID_VALUE);
        alEffectf(e, t[i].param, t[i].hi);
        assert(alGetError() == AL_NO_ERROR);
    }
    alEffectf(e, AL_EQUALIZER_HIGH_CUTOFF + 1, 1.0f);
    assert(alGetError() == AL_INVALID_ENUM);
}

/* OpenAL Soft's peaking filter (gain: the square root of the band's), -> the kernel's quartered words */
static void peak(double hz, double band_gain, double width, double w[5]) {
    double g = sqrt(band_gain), w0 = 2.0 * PI * hz / 48000.0, cw = cos(w0);
    double al = sin(w0) / 2.0 * 2.0 * sinh(log(2.0) / 2.0 * width * w0 / sin(w0)), a0 = 1.0 + al / g;
    w[0] = (1.0 + al * g) / a0 / 4.0;
    w[1] = -2.0 * cw / a0 / 4.0;
    w[2] = (1.0 - al * g) / a0 / 4.0;
    w[3] = 2.0 * cw / a0 / 4.0;
    w[4] = -(1.0 - al / g) / a0 / 4.0;
}

/* OpenAL Soft's low shelf (slope 0.75), the same way */
static void low_shelf(double hz, double band_gain, double w[5]) {
    double g = sqrt(band_gain), w0 = 2.0 * PI * hz / 48000.0, cw = cos(w0);
    double al = sin(w0) / 2.0 * sqrt((g + 1.0 / g) * (1.0 / 0.75 - 1.0) + 2.0), s = 2.0 * sqrt(g) * al;
    double a0 = (g + 1.0) + (g - 1.0) * cw + s;
    w[0] = g * ((g + 1.0) - (g - 1.0) * cw + s) / a0 / 4.0;
    w[1] = 2.0 * g * ((g - 1.0) - (g + 1.0) * cw) / a0 / 4.0;
    w[2] = g * ((g + 1.0) - (g - 1.0) * cw - s) / a0 / 4.0;
    w[3] = 2.0 * ((g - 1.0) + (g + 1.0) * cw) / a0 / 4.0;
    w[4] = -((g + 1.0) + (g - 1.0) * cw - s) / a0 / 4.0;
}

static void check(const uint32_t *p, const double w[5]) {
    int i;
    for (i = 0; i < 5; i++) {
        assert(fabs(q(p[i]) - w[i]) < 0.6 / 8388608.0);
    }
}

static void test_slot(void) {
    ALuint e = 0, slot = 0, src = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;
    double w[5];

    printf("[2] a slot runs the equalizer's kernel: the bands not at gain 1, in order, as its words\n");
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_EQUALIZER);
    alGenAuxiliaryEffectSlots(1, &slot);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(k.words == GP_KEFX_EQUALIZER_WORDS && k.x_words == GP_KEFX_EQUALIZER_K_XWORDS);
    assert(k.scratch_bytes == 0u && k.descriptors == 0u && k.y_words == 0u);
    assert(p[0] == 0u);                                        /* the defaults: every band at 1, no stage */

    alEffectf(e, AL_EQUALIZER_MID2_GAIN, 4.0f);
    alEffectf(e, AL_EQUALIZER_MID2_CENTER, 2000.0f);
    alEffectf(e, AL_EQUALIZER_MID2_WIDTH, 0.5f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(p[0] == 1u);
    peak(2000.0, 4.0, 0.5, w);
    check(&p[1], w);

    alEffectf(e, AL_EQUALIZER_LOW_GAIN, 0.25f);
    alEffectf(e, AL_EQUALIZER_LOW_CUTOFF, 100.0f);
    alEffectf(e, AL_EQUALIZER_HIGH_GAIN, 2.0f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(p[0] == 3u);                                        /* low, mid 2, high */
    low_shelf(100.0, 0.25, w);
    check(&p[1], w);
    peak(2000.0, 4.0, 0.5, w);
    check(&p[6], w);
    assert(p[16] == 0u && p[17] == 0u);                        /* nothing after the last stage */

    /* the gain at 7.943 (+18 dB) still fits the quartered words */
    alEffectf(e, AL_EQUALIZER_HIGH_GAIN, 7.943f);
    alEffectf(e, AL_EQUALIZER_HIGH_CUTOFF, 16000.0f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(q(p[11]) > 0.25 && q(p[11]) < 0.99);

    /* a linear effect: the slot's gain scales the sends into it */
    {
        float s[APU_VOICE_SENDS];
        alGenSources(1, &src);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.25f);
        assert(al_source_debug_sends(src, s) && s[0] == 0.25f);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
        alDeleteSources(1, &src);
    }

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    alDeleteEffects(1, &e);
    assert(alGetError() == AL_NO_ERROR);
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    ALuint e = 0;

    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    alGenEffects(1, &e);
    test_object(e);
    alDeleteEffects(1, &e);
    test_slot();

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== AL_EFFECT_EQUALIZER: all passed ===\n");
    return 0;
}
