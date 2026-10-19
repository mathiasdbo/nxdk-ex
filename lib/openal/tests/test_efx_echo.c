/*
 * AL_EFFECT_ECHO on the library's GP kernel (docs/SOFT_GAP_PLAN.md phase9-05):
 * the effect object (defaults, ranges), the kernel a slot runs and the
 * parameter words it gets, the slot gain on the sends, a reverb beside it.
 * The kernel's sound is checked in tests/test_gp_programs.py [6].
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
#include "apu_voice.h"
#include "gp_kefx_echo.h"

static void test_object(ALuint e) {
    ALfloat f = 0.0f;
    printf("[1] the echo effect: defaults and ranges\n");
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_ECHO);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffectf(e, AL_ECHO_DELAY, &f);
    assert(f == AL_ECHO_DEFAULT_DELAY);
    alGetEffectf(e, AL_ECHO_LRDELAY, &f);
    assert(f == AL_ECHO_DEFAULT_LRDELAY);
    alGetEffectf(e, AL_ECHO_DAMPING, &f);
    assert(f == AL_ECHO_DEFAULT_DAMPING);
    alGetEffectf(e, AL_ECHO_FEEDBACK, &f);
    assert(f == AL_ECHO_DEFAULT_FEEDBACK);
    alGetEffectf(e, AL_ECHO_SPREAD, &f);
    assert(f == AL_ECHO_DEFAULT_SPREAD);
    alEffectf(e, AL_ECHO_DELAY, 0.3f);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffectf(e, AL_ECHO_SPREAD, -1.5f);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffectf(e, AL_REVERB_DECAY_HFRATIO, 1.0f);   /* 0x0006: not an echo parameter (1..5 are) */
    assert(alGetError() == AL_INVALID_ENUM);
}

static void test_slot(ALuint e) {
    ALuint slot = 0, src = 0, e2 = 0, slot2 = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;
    float s[APU_VOICE_SENDS];

    printf("[2] a slot runs the echo kernel with the EFX values as its words\n");
    alEffectf(e, AL_ECHO_DELAY, 0.1f);
    alEffectf(e, AL_ECHO_LRDELAY, 0.05f);
    alEffectf(e, AL_ECHO_FEEDBACK, 0.5f);
    alEffectf(e, AL_ECHO_DAMPING, 0.25f);
    alEffectf(e, AL_ECHO_SPREAD, -1.0f);
    alGenAuxiliaryEffectSlots(1, &slot);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR && al_efx_active_slot() == slot);
    assert(al_source_debug_gp_kernel(&k, &p));
    assert(k.words == GP_KEFX_ECHO_WORDS && k.scratch_bytes == GP_KEFX_ECHO_K_SCRATCH_BYTES);
    assert(p[0] == 4800u && p[1] == 7200u);               /* DELAY, DELAY + LRDELAY in samples */
    assert(p[2] == 0x400000u && p[3] == 0x200000u);       /* feedback, damping */
    assert(p[4] == 0x7FFFFFu && p[5] == 0u);              /* spread -1: tap 1 left */
    assert(p[6] == 0u && p[7] == 0x7FFFFFu);              /* tap 2 right */

    /* spread 0: both taps in the middle, constant power */
    alEffectf(e, AL_ECHO_SPREAD, 0.0f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(al_source_debug_gp_kernel(&k, &p));
    assert(p[4] == p[5] && p[6] == p[7] && p[4] == p[6] && fabs((double)p[4] / 0x800000 - 0.70710678) < 1e-6);

    /* the slot gain scales the sends into it */
    alGenSources(1, &src);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
    alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.5f);
    assert(al_source_debug_sends(src, s) && s[0] == 0.5f);
    alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 1.0f);

    /* a reverb beside it in another slot (phase9-04); AL_XBOX_REVERB stays exclusive */
    alGenEffects(1, &e2);
    alEffecti(e2, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    alGenAuxiliaryEffectSlots(1, &slot2);
    alAuxiliaryEffectSloti(slot2, AL_EFFECTSLOT_EFFECT, (ALint)e2);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_debug_gp_slot(slot - 1u, NULL, NULL, NULL) == AL_GP_KERNEL);
    assert(al_source_debug_gp_slot(slot2 - 1u, NULL, NULL, NULL) == AL_GP_REVERB);
    alEnable(AL_XBOX_REVERB);
    assert(alGetError() == AL_INVALID_OPERATION);
    alAuxiliaryEffectSloti(slot2, AL_EFFECTSLOT_EFFECT, 0);

    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
    alDeleteSources(1, &src);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    assert(!al_source_debug_gp_kernel(&k, &p) && al_efx_active_slot() == 0u);
    alDeleteAuxiliaryEffectSlots(1, &slot2);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    alDeleteEffects(1, &e2);
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
    test_slot(e);
    alDeleteEffects(1, &e);

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== AL_EFFECT_ECHO: all passed ===\n");
    return 0;
}
