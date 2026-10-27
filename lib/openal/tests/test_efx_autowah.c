/*
 * AL_EFFECT_AUTOWAH on the library's GP kernel (docs/SOFT_GAP_PLAN.md
 * phase9-07): the effect object (defaults, ranges), the kernel a slot runs,
 * its words (OpenAL Soft's derived values) and the slot's gain taken at the
 * kernel's output. The kernel's sound is checked in tests/test_gp_onchip.py [5].
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
#include "gp_kefx_autowah.h"

static double q(uint32_t w) {
    return (double)((int32_t)(w << 8) >> 8) / 8388608.0;
}

static void test_object(ALuint e) {
    static const struct {
        ALenum param;
        ALfloat def, lo, hi;
    } t[] = {
        { AL_AUTOWAH_ATTACK_TIME, 0.06f, 0.0001f, 1.0f },
        { AL_AUTOWAH_RELEASE_TIME, 0.06f, 0.0001f, 1.0f },
        { AL_AUTOWAH_RESONANCE, 1000.0f, 2.0f, 1000.0f },
        { AL_AUTOWAH_PEAK_GAIN, 11.22f, 0.00003f, 31621.0f },
    };
    ALfloat f = 0.0f;
    size_t i;
    printf("[1] the autowah: defaults, ranges\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_AUTOWAH);
    assert(alGetError() == AL_NO_ERROR);
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        alGetEffectf(e, t[i].param, &f);
        assert(f == t[i].def);
        alEffectf(e, t[i].param, t[i].lo * 0.99f);
        assert(alGetError() == AL_INVALID_VALUE);
        alEffectf(e, t[i].param, t[i].hi * 1.01f);
        assert(alGetError() == AL_INVALID_VALUE);
        alEffectf(e, t[i].param, t[i].lo);
        assert(alGetError() == AL_NO_ERROR);
    }
    alEffecti(e, AL_AUTOWAH_RESONANCE, 10);                  /* a float parameter */
    assert(alGetError() == AL_INVALID_ENUM);
}

static void test_slot(void) {
    ALuint e = 0, slot = 0, src = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;
    double res = sqrt(30.0 / 3.0);                             /* RESONANCE 1000: +20 dB */

    printf("[2] a slot runs the autowah's kernel: OpenAL Soft's derived values as its words\n");
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_AUTOWAH);
    alGenAuxiliaryEffectSlots(1, &slot);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(k.words == GP_KEFX_AUTOWAH_WORDS && k.x_words == GP_KEFX_AUTOWAH_K_XWORDS);
    assert(k.scratch_bytes == 0u && k.descriptors == 0u && k.y_words == 0u);
    assert(fabs(q(p[0]) - (1.0 - exp(-1.0 / (0.06 * 48000.0)))) < 1e-6);            /* 1 - the attack rate */
    assert(fabs(q(p[1]) - q(p[0])) < 1e-6);                                        /* the release, the same */
    assert(fabs(16.0 * q(p[2]) - (1.0 - log10(11.22 / 31621.0))) < 1e-5);           /* the peak gain, 4.45 */
    assert(fabs(q(p[3]) / 16.0 * 48000.0 - 2480.0) < 0.01 && fabs(q(p[4]) * 48000.0 - 20.0) < 0.01);
    assert(fabs(4.0 * q(p[5]) - res) < 1e-5 && fabs(q(p[6]) - 1.0 / res) < 1e-6);
    assert(p[7] == 0x7FFFFFu);

    /* the release is at least 1 ms (OpenAL Soft clamps it); the attack is not */
    alEffectf(e, AL_AUTOWAH_RELEASE_TIME, 0.0001f);
    alEffectf(e, AL_AUTOWAH_ATTACK_TIME, 0.0001f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(fabs(q(p[1]) - (1.0 - exp(-1.0 / 48.0))) < 1e-6 && fabs(q(p[0]) - (1.0 - exp(-1.0 / 4.8))) < 1e-6);

    /* not linear: the slot's gain goes into the kernel, not into the sends */
    {
        float s[APU_VOICE_SENDS];
        alGenSources(1, &src);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.25f);
        assert(al_source_debug_sends(src, s) && s[0] == 1.0f);
        assert(al_source_debug_gp_kernel(&k, &p) && p[7] == 0x200000u);
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
    printf("=== AL_EFFECT_AUTOWAH: all passed ===\n");
    return 0;
}
