/*
 * AL_EFFECT_CHORUS and AL_EFFECT_FLANGER on the library's GP kernel
 * (docs/SOFT_GAP_PLAN.md phase9-06): the effect objects (defaults, ranges,
 * integer parameters), the kernel a slot runs and the parameter words it gets.
 * The kernel's sound is checked in tests/test_gp_programs.py [7].
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
#include "gp_kefx_chorus.h"

static void test_chorus_object(ALuint e) {
    ALfloat f = 0.0f;
    ALint i = 0;
    printf("[1] the chorus effect: defaults, ranges, integer parameters\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffecti(e, AL_CHORUS_WAVEFORM, &i);
    assert(i == AL_CHORUS_WAVEFORM_TRIANGLE);
    alGetEffecti(e, AL_CHORUS_PHASE, &i);
    assert(i == 90);
    alGetEffectf(e, AL_CHORUS_RATE, &f);
    assert(f == AL_CHORUS_DEFAULT_RATE);
    alGetEffectf(e, AL_CHORUS_DEPTH, &f);
    assert(f == AL_CHORUS_DEFAULT_DEPTH);
    alGetEffectf(e, AL_CHORUS_FEEDBACK, &f);
    assert(f == AL_CHORUS_DEFAULT_FEEDBACK);
    alGetEffectf(e, AL_CHORUS_DELAY, &f);
    assert(f == AL_CHORUS_DEFAULT_DELAY);
    assert(alGetError() == AL_NO_ERROR);

    alEffecti(e, AL_CHORUS_WAVEFORM, 2);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_CHORUS_PHASE, -181);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_CHORUS_PHASE, -180);
    assert(alGetError() == AL_NO_ERROR);
    alEffectf(e, AL_CHORUS_DELAY, 0.017f);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffectf(e, AL_CHORUS_FEEDBACK, -1.0f);
    assert(alGetError() == AL_NO_ERROR);
    alEffectf(e, AL_CHORUS_WAVEFORM, 0.0f);         /* an integer parameter */
    assert(alGetError() == AL_INVALID_ENUM);
    alEffecti(e, AL_CHORUS_RATE, 1);                /* a float one */
    assert(alGetError() == AL_INVALID_ENUM);
}

static void test_flanger_object(ALuint e) {
    ALfloat f = 0.0f;
    ALint i = -1;
    printf("[2] the flanger effect: its own defaults and ranges on the same tokens\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_FLANGER);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffecti(e, AL_FLANGER_PHASE, &i);
    assert(i == 0);
    alGetEffectf(e, AL_FLANGER_DEPTH, &f);
    assert(f == AL_FLANGER_DEFAULT_DEPTH);
    alGetEffectf(e, AL_FLANGER_FEEDBACK, &f);
    assert(f == AL_FLANGER_DEFAULT_FEEDBACK);
    alGetEffectf(e, AL_FLANGER_DELAY, &f);
    assert(f == AL_FLANGER_DEFAULT_DELAY);
    alEffectf(e, AL_FLANGER_DELAY, 0.005f);         /* the chorus would take it */
    assert(alGetError() == AL_INVALID_VALUE);
}

static double q(uint32_t w) {
    return (double)((int32_t)(w << 8) >> 8) / 8388608.0;
}

static void test_slot(void) {
    ALuint e = 0, slot = 0, src = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;

    printf("[3] a slot runs the chorus kernel with the EFX values as its words\n");
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    alEffecti(e, AL_CHORUS_WAVEFORM, AL_CHORUS_WAVEFORM_SINUSOID);
    alEffecti(e, AL_CHORUS_PHASE, -90);
    alEffectf(e, AL_CHORUS_RATE, 2.0f);
    alEffectf(e, AL_CHORUS_DEPTH, 0.5f);
    alEffectf(e, AL_CHORUS_FEEDBACK, -0.25f);
    alEffectf(e, AL_CHORUS_DELAY, 0.01f);
    alGenAuxiliaryEffectSlots(1, &slot);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_efx_active_slot() == slot);
    assert(al_source_debug_gp_kernel(&k, &p));
    assert(k.words == GP_KEFX_CHORUS_WORDS && k.scratch_bytes == 0u);
    assert(fabs(q(p[0]) * 2048.0 - 480.0) < 0.01);             /* D: 10 ms */
    assert(fabs(q(p[1]) * 2048.0 - 240.0) < 0.01);             /* DD: DEPTH x D */
    assert(fabs(q(p[2]) + 0.25) < 1e-6);                       /* feedback */
    assert(fabs(q(p[3]) - 2.0 * 2.0 / 48000.0) < 1e-6);        /* the LFO step */
    assert(fabs(q(p[4]) + 0.5) < 1e-6);                        /* PHASE / 180 */
    assert(p[5] == 0u);                                        /* sine */

    /* the flanger runs the same kernel; the slot gain scales the sends into it */
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_FLANGER);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_kernel(&k, &p));
    assert(k.words == GP_KEFX_CHORUS_WORDS);
    assert(fabs(q(p[0]) * 2048.0 - 96.0) < 0.01 && fabs(q(p[1]) * 2048.0 - 96.0) < 0.01);   /* 2 ms, depth 1 */
    assert(fabs(q(p[2]) + 0.5) < 1e-6 && p[4] == 0u && p[5] == 1u);
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
    test_chorus_object(e);
    test_flanger_object(e);
    alDeleteEffects(1, &e);
    test_slot();

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== AL_EFFECT_CHORUS / AL_EFFECT_FLANGER: all passed ===\n");
    return 0;
}
