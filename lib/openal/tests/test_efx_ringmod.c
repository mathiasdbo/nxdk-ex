/*
 * AL_EFFECT_RING_MODULATOR on the library's GP kernel (docs/SOFT_GAP_PLAN.md
 * phase9-07): the effect object (defaults, ranges, the integer waveform), the
 * kernel a slot runs and the parameter words it gets. The kernel's sound is
 * checked in tests/test_gp_onchip.py [1].
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
#include "gp_kefx_ringmod.h"

static double q(uint32_t w) {
    return (double)((int32_t)(w << 8) >> 8) / 8388608.0;
}

static void test_object(ALuint e) {
    ALfloat f = 0.0f;
    ALint i = -1;
    printf("[1] the ring modulator: defaults, ranges, the integer waveform\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_RING_MODULATOR);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffectf(e, AL_RING_MODULATOR_FREQUENCY, &f);
    assert(f == 440.0f);
    alGetEffectf(e, AL_RING_MODULATOR_HIGHPASS_CUTOFF, &f);
    assert(f == 800.0f);
    alGetEffecti(e, AL_RING_MODULATOR_WAVEFORM, &i);
    assert(i == AL_RING_MODULATOR_SINUSOID);
    assert(alGetError() == AL_NO_ERROR);

    alEffectf(e, AL_RING_MODULATOR_FREQUENCY, 8000.5f);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffectf(e, AL_RING_MODULATOR_FREQUENCY, -1.0f);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffectf(e, AL_RING_MODULATOR_HIGHPASS_CUTOFF, 24001.0f);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_RING_MODULATOR_WAVEFORM, 3);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_RING_MODULATOR_WAVEFORM, AL_RING_MODULATOR_SQUARE);
    assert(alGetError() == AL_NO_ERROR);
    alEffectf(e, AL_RING_MODULATOR_WAVEFORM, 1.0f);           /* an integer parameter */
    assert(alGetError() == AL_INVALID_ENUM);
    alEffecti(e, AL_RING_MODULATOR_FREQUENCY, 100);          /* a float one */
    assert(alGetError() == AL_INVALID_ENUM);
    alEffectf(e, 0x0004, 0.0f);                              /* not one of its parameters */
    assert(alGetError() == AL_INVALID_ENUM);
}

/* The highpass OpenAL Soft designs (slope 0.75 at gain 1), as the kernel's halved words */
static void highpass(double hz, double w[5]) {
    double w0 = 2.0 * 3.14159265358979323846 * hz / 48000.0, cw = cos(w0);
    double al = sin(w0) / 2.0 * sqrt(2.0 * (1.0 / 0.75 - 1.0) + 2.0), a0 = 1.0 + al;
    w[0] = (1.0 + cw) / 2.0 / a0 / 2.0;
    w[1] = -(1.0 + cw) / a0 / 2.0;
    w[2] = w[0];
    w[3] = 2.0 * cw / a0 / 2.0;
    w[4] = -(1.0 - al) / a0 / 2.0;
}

static void test_slot(void) {
    ALuint e = 0, slot = 0, src = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;
    double w[5];
    int i;

    printf("[2] a slot runs the ring modulator's kernel with the EFX values as its words\n");
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_RING_MODULATOR);
    alEffectf(e, AL_RING_MODULATOR_FREQUENCY, 1000.0f);
    alEffectf(e, AL_RING_MODULATOR_HIGHPASS_CUTOFF, 300.0f);
    alEffecti(e, AL_RING_MODULATOR_WAVEFORM, AL_RING_MODULATOR_SAWTOOTH);
    alGenAuxiliaryEffectSlots(1, &slot);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_efx_active_slot() == slot);
    assert(al_source_debug_gp_kernel(&k, &p));
    assert(k.words == GP_KEFX_RINGMOD_WORDS && k.scratch_bytes == 0u && k.descriptors == 0u && k.y_words == 0u);
    assert(k.x_words == GP_KEFX_RINGMOD_K_XWORDS);
    assert(p[0] == 349525u);                                   /* 1000 / 48000 of 2^24 */
    assert(p[1] == AL_RING_MODULATOR_SAWTOOTH);
    highpass(300.0, w);
    for (i = 0; i < 5; i++) {
        assert(fabs(q(p[2 + i]) - w[i]) < 0.6 / 8388608.0);
    }

    /* frequency 0: no modulation (OpenAL Soft's "one"); highpass 0 Hz: the identity */
    alEffectf(e, AL_RING_MODULATOR_FREQUENCY, 0.0f);
    alEffectf(e, AL_RING_MODULATOR_HIGHPASS_CUTOFF, 0.0f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(p[0] == 0u && p[1] == 3u && p[2] == 0x400000u && p[3] == 0u && p[4] == 0u && p[5] == 0u && p[6] == 0u);

    /* a linear effect: the slot's gain scales the sends into it */
    {
        float s[APU_VOICE_SENDS];
        alGenSources(1, &src);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.5f);
        assert(al_source_debug_sends(src, s) && s[0] == 0.5f);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
        alDeleteSources(1, &src);
    }

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    assert(!al_source_debug_gp_kernel(&k, &p) && al_efx_active_slot() == 0u);
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
    printf("=== AL_EFFECT_RING_MODULATOR: all passed ===\n");
    return 0;
}
