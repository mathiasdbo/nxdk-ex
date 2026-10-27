/*
 * AL_EFFECT_COMPRESSOR on the library's GP kernel (docs/SOFT_GAP_PLAN.md
 * phase9-07): the effect object (its one integer parameter), the kernel a
 * slot runs, its words (OpenAL Soft's fixed rates) and the slot's gain taken
 * at the kernel's output. The kernel's sound is checked in
 * tests/test_gp_onchip.py [4].
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
#include "gp_kefx_compressor.h"

static double q(uint32_t w) {
    return (double)((int32_t)(w << 8) >> 8) / 8388608.0;
}

static void test_object(ALuint e) {
    ALint i = -1;
    printf("[1] the compressor: default on, 0 / 1 only, an integer parameter\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_COMPRESSOR);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffecti(e, AL_COMPRESSOR_ONOFF, &i);
    assert(i == AL_TRUE);
    alEffecti(e, AL_COMPRESSOR_ONOFF, 2);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_COMPRESSOR_ONOFF, -1);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_COMPRESSOR_ONOFF, AL_FALSE);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffecti(e, AL_COMPRESSOR_ONOFF, &i);
    assert(i == AL_FALSE);
    alEffectf(e, AL_COMPRESSOR_ONOFF, 1.0f);
    assert(alGetError() == AL_INVALID_ENUM);
}

static void test_slot(void) {
    ALuint e = 0, slot = 0, src = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;

    printf("[2] a slot runs the compressor's kernel: on, the rates, the slot's gain at its output\n");
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_COMPRESSOR);
    alGenAuxiliaryEffectSlots(1, &slot);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(k.words == GP_KEFX_COMPRESSOR_WORDS && k.x_words == GP_KEFX_COMPRESSOR_K_XWORDS);
    assert(k.scratch_bytes == 0u && k.descriptors == 0u && k.y_words == 0u);
    assert(p[0] == 1u);
    /* 1/2 -> 2 in 0.1 s, 2 -> 1/2 in 0.2 s */
    assert(fabs(pow(1.0 + q(p[1]), 4800.0) - 4.0) < 0.01);
    assert(fabs(pow(q(p[2]), 9600.0) - 0.25) < 0.01);
    assert(p[3] == 0x7FFFFFu);

    alEffecti(e, AL_COMPRESSOR_ONOFF, AL_FALSE);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p) && p[0] == 0u);

    /* not linear: the slot's gain goes into the kernel, not into the sends */
    {
        float s[APU_VOICE_SENDS];
        alGenSources(1, &src);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.5f);
        assert(al_source_debug_sends(src, s) && s[0] == 1.0f);
        assert(al_source_debug_gp_kernel(&k, &p) && p[3] == 0x400000u);
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
    printf("=== AL_EFFECT_COMPRESSOR: all passed ===\n");
    return 0;
}
