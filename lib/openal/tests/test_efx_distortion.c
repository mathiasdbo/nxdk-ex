/*
 * AL_EFFECT_DISTORTION on the library's GP kernel (docs/SOFT_GAP_PLAN.md
 * phase9-07): the effect object (defaults, ranges), the kernel a slot runs,
 * its words (the filters, the folded waveshaper's d, the gain in the
 * bandpass's b words) and the slot's gain taken at the kernel's output. The
 * kernel's sound is checked in tests/test_gp_onchip.py [2].
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
#include "gp_kefx_distortion.h"

#define PI 3.14159265358979323846

static double q(uint32_t w) {
    return (double)((int32_t)(w << 8) >> 8) / 8388608.0;
}

static void test_object(ALuint e) {
    static const struct {
        ALenum param;
        ALfloat def, lo, hi;
    } t[] = {
        { AL_DISTORTION_EDGE, 0.2f, 0.0f, 1.0f },
        { AL_DISTORTION_GAIN, 0.05f, 0.01f, 1.0f },
        { AL_DISTORTION_LOWPASS_CUTOFF, 8000.0f, 80.0f, 24000.0f },
        { AL_DISTORTION_EQCENTER, 3600.0f, 80.0f, 24000.0f },
        { AL_DISTORTION_EQBANDWIDTH, 3600.0f, 80.0f, 24000.0f },
    };
    ALfloat f = 0.0f;
    size_t i;
    printf("[1] the distortion: defaults, ranges\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_DISTORTION);
    assert(alGetError() == AL_NO_ERROR);
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        alGetEffectf(e, t[i].param, &f);
        assert(f == t[i].def);
        alEffectf(e, t[i].param, t[i].lo * 0.99f - 0.001f);
        assert(alGetError() == AL_INVALID_VALUE);
        alEffectf(e, t[i].param, t[i].hi * 1.01f);
        assert(alGetError() == AL_INVALID_VALUE);
        alEffectf(e, t[i].param, t[i].lo);
        alEffectf(e, t[i].param, t[i].hi);
        assert(alGetError() == AL_NO_ERROR);
        alGetEffectf(e, t[i].param, &f);
        assert(f == t[i].hi);
    }
    alEffecti(e, AL_DISTORTION_EDGE, 0);                      /* a float parameter */
    assert(alGetError() == AL_INVALID_ENUM);
}

/* OpenAL Soft's three waveshaper stages */
static double soft_shaper(double x, double edge) {
    double e = sin(PI / 2.0 * edge), fc;
    if (e > 0.99) e = 0.99;
    fc = 2.0 * e / (1.0 - e);
    x = (1.0 + fc) * x / (1.0 + fc * fabs(x));
    x = (1.0 + fc) * x / (1.0 + fc * fabs(x)) * -1.0;
    return (1.0 + fc) * x / (1.0 + fc * fabs(x));
}

static void test_slot(void) {
    static const float edges[] = { 0.0f, 0.2f, 0.7f, 1.0f };
    ALuint e = 0, slot = 0, src = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;
    size_t i;
    int j;

    printf("[2] a slot runs the distortion's kernel; its words; the folded shaper = OpenAL Soft's three stages\n");
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_DISTORTION);
    alGenAuxiliaryEffectSlots(1, &slot);
    for (i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        double d;
        alEffectf(e, AL_DISTORTION_EDGE, edges[i]);
        alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
        assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
        assert(k.words == GP_KEFX_DISTORTION_WORDS && k.x_words == GP_KEFX_DISTORTION_K_XWORDS);
        assert(k.scratch_bytes == 0u && k.descriptors == 0u && k.y_words == 0u);
        d = q(p[10]);
        assert(p[10] + p[11] <= 0x7FFFFFu && fabs(q(p[11]) - (1.0 - d)) <= 1.0 / 8388608.0);
        /* x / (d + (1 - d) |x|), negated, is what the three stages make */
        for (j = -50; j <= 50; j++) {
            double x = j / 50.0, ref = soft_shaper(x, edges[i]);
            double got = -x / (d + (1.0 - d) * fabs(x));
            assert(fabs(got - ref) < (edges[i] > 0.9f ? 2e-3 : 1e-5));
        }
    }

    /* the default lowpass (8 kHz, OpenAL Soft's 0.746 octaves at 4x the rate) and bandpass (3.6 kHz, 1 octave
     * at 4x), gain 0.05 in the bandpass's b words */
    alEffectf(e, AL_DISTORTION_EDGE, 0.2f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    {
        double w0 = 2.0 * PI * 8000.0 / 48000.0, w4 = w0 / 4.0, cw = cos(w0);
        double rq = 2.0 * sinh(log(2.0) / 2.0 * (4000.0 / (8000.0 * 0.67)) * w4 / sin(w4));
        double al = sin(w0) / 2.0 * rq, a0 = 1.0 + al;
        assert(fabs(q(p[0]) - (1.0 - cw) / 2.0 / a0 / 2.0) < 1e-6);
        assert(fabs(q(p[1]) - (1.0 - cw) / a0 / 2.0) < 1e-6);
        assert(fabs(q(p[3]) - cw / a0) < 1e-6 && fabs(q(p[4]) + (1.0 - al) / a0 / 2.0) < 1e-6);
        w0 = 2.0 * PI * 3600.0 / 48000.0;
        w4 = w0 / 4.0;
        rq = 2.0 * sinh(log(2.0) / 2.0 * (3600.0 / (3600.0 * 0.67)) * w4 / sin(w4));
        al = sin(w0) / 2.0 * rq;
        assert(fabs(q(p[5]) - 0.05 * al / (1.0 + al) / 2.0) < 1e-6 && p[6] == 0u);
        assert(fabs(q(p[7]) + 0.05 * al / (1.0 + al) / 2.0) < 1e-6);
    }

    /* not linear: the slot's gain goes into the kernel's gain, not into the sends */
    {
        float s[APU_VOICE_SENDS];
        uint32_t b0 = p[5];
        alGenSources(1, &src);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.5f);
        assert(al_source_debug_sends(src, s) && s[0] == 1.0f);
        assert(al_source_debug_gp_kernel(&k, &p) && fabs(q(p[5]) - 0.5 * q(b0)) < 1e-6);
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
    printf("=== AL_EFFECT_DISTORTION: all passed ===\n");
    return 0;
}
