/*
 * AL_XBOX_gp_program (task phase8-04, lib/openal/docs/EFX_PLAN.md 3.6): the
 * effect type, alXboxEffectProgram's checks and copy, AL_XBOX_GP_PARAM0 + n
 * as integers and Q23 fractions, the slot's copy of image and parameters,
 * and the one-effect rule. Null backend: the load into the GP is
 * tests/test_apu_gp.c [8], the kernel's arithmetic tests/test_gp_programs.py [5].
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_efx.h"
#include "al_source.h"
#include "apu_voice.h"
#include "apu_vp.h"
#include "../dsp/gp_kecho.h"

static void test_effect(ALuint e) {
    static ALuint bad[GP_KECHO_IMAGE_WORDS];
    ALint iv;
    ALfloat fv;

    printf("[1] the effect type and the image checks\n");
    alXboxEffectProgram(e, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    assert(alGetError() == AL_INVALID_OPERATION);          /* not a GP program effect yet */
    alEffecti(e, AL_XBOX_GP_PARAM0, 1);
    assert(alGetError() == AL_INVALID_ENUM);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffecti(e, AL_EFFECT_TYPE, &iv);
    assert(iv == AL_EFFECT_XBOX_GP_PROGRAM);
    alXboxEffectProgram(e, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 1);
    assert(alGetError() == AL_INVALID_VALUE);              /* flags */
    alXboxEffectProgram(e, NULL, GP_KECHO_IMAGE_WORDS, 0);
    assert(alGetError() == AL_INVALID_VALUE);
    alXboxEffectProgram(e, gp_kecho_image, -1, 0);
    assert(alGetError() == AL_INVALID_VALUE);
    memcpy(bad, gp_kecho_image, sizeof(bad));
    bad[0] = 0;                                             /* magic */
    alXboxEffectProgram(e, bad, GP_KECHO_IMAGE_WORDS, 0);
    assert(alGetError() == AL_INVALID_VALUE);
    bad[0] = AL_XBOX_GP_IMAGE_MAGIC;
    bad[2] = 4u * 1024u * 1024u;                            /* more scratch than there is */
    alXboxEffectProgram(e, bad, GP_KECHO_IMAGE_WORDS, 0);
    assert(alGetError() == AL_INVALID_VALUE);
    alXboxEffectProgram(9999u, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    assert(alGetError() == AL_INVALID_NAME);
    alXboxEffectProgram(e, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    assert(alGetError() == AL_NO_ERROR);

    printf("[2] parameters: 24-bit integers and Q23 fractions\n");
    alEffecti(e, AL_XBOX_GP_PARAM0, 100);
    alEffecti(e, AL_XBOX_GP_PARAM0 + 3, -5);
    alEffecti(e, AL_XBOX_GP_PARAM0 + 31, 0xFFFFFF);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffecti(e, AL_XBOX_GP_PARAM0, &iv);
    assert(iv == 100);
    alGetEffecti(e, AL_XBOX_GP_PARAM0 + 3, &iv);
    assert(iv == -5);
    alGetEffecti(e, AL_XBOX_GP_PARAM0 + 31, &iv);
    assert(iv == -1);                                       /* read back sign-extended */
    alEffecti(e, AL_XBOX_GP_PARAM0, 0x1000000);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_XBOX_GP_PARAM0, -0x800001);
    assert(alGetError() == AL_INVALID_VALUE);
    alEffecti(e, AL_XBOX_GP_PARAM0 + AL_XBOX_GP_PARAMS, 1);
    assert(alGetError() == AL_INVALID_ENUM);
    alEffectf(e, AL_XBOX_GP_PARAM0 + 2, 0.5f);
    alEffectf(e, AL_XBOX_GP_PARAM0 + 1, -0.25f);
    assert(alGetError() == AL_NO_ERROR);
    alGetEffecti(e, AL_XBOX_GP_PARAM0 + 2, &iv);
    assert(iv == 0x400000);
    alGetEffectf(e, AL_XBOX_GP_PARAM0 + 1, &fv);
    assert(fv == -0.25f);
    alEffectf(e, AL_XBOX_GP_PARAM0 + 4, 1.0f);
    alGetEffecti(e, AL_XBOX_GP_PARAM0 + 4, &iv);
    assert(iv == 0x7FFFFF);                                 /* 1.0 saturates */
    alEffectf(e, AL_XBOX_GP_PARAM0, 1.5f);
    assert(alGetError() == AL_INVALID_VALUE);
    alGetEffecti(e, AL_XBOX_GP_PARAM0, &iv);
    assert(iv == 100);                                      /* unchanged */
    alEffectf(e, AL_REVERB_DECAY_TIME, 2.0f);
    assert(alGetError() == AL_INVALID_ENUM);                /* not a reverb */
}

static void test_slot(ALuint e) {
    ALuint slot = 0, e2 = 0;
    apu_gp_kernel_t k;
    const uint32_t *params = NULL;
    ALint iv;

    printf("[3] the slot runs a copy of image and parameters\n");
    alGenAuxiliaryEffectSlots(1, &slot);
    assert(!al_source_debug_gp_kernel(&k, &params));
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_efx_active_slot() == slot);
    assert(al_source_debug_gp_kernel(&k, &params));
    assert(k.words == GP_KECHO_WORDS && k.scratch_bytes == GP_KECHO_K_SCRATCH_BYTES);
    assert(memcmp(k.code, gp_kecho_image + 4, GP_KECHO_WORDS * sizeof(uint32_t)) == 0);
    assert(k.code != gp_kecho_image + 4);                   /* the library's copy */
    assert(params[0] == 100u && params[2] == 0x400000u && params[3] == 0xFFFFFBu);

    /* AL_EFFECTSLOT_GAIN scales the sends into a custom kernel (it has no word for it), live */
    {
        ALuint src = 0;
        float s[APU_VOICE_SENDS];
        alGenSources(1, &src);
        alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        assert(al_source_debug_sends(src, s) && s[0] == 1.0f && s[1] == 0.0f);
        alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.25f);
        assert(alGetError() == AL_NO_ERROR);
        assert(al_source_debug_sends(src, s) && s[0] == 0.25f);
        alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 1.0f);
        assert(al_source_debug_sends(src, s) && s[0] == 1.0f);
        alDeleteSources(1, &src);
    }

    /* A stereo source into a custom kernel sends each channel at the whole send
     * (exactly 0 dB at send 1); a mono one half on each output (their sum is the send) */
    {
        ALuint st = 0, mo = 0, bst = 0, bmo = 0;
        int16_t pcm[512] = { 0 };
        ALsource *as;
        alGenBuffers(1, &bst);
        alBufferData(bst, AL_FORMAT_STEREO16, pcm, (ALsizei)sizeof(pcm), 48000);
        alGenBuffers(1, &bmo);
        alBufferData(bmo, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
        alGenSources(1, &st);
        alGenSources(1, &mo);
        alSourcei(st, AL_BUFFER, (ALint)bst);
        alSourcei(mo, AL_BUFFER, (ALint)bmo);
        alSourcei(st, AL_LOOPING, AL_TRUE);
        alSourcei(mo, AL_LOOPING, AL_TRUE);
        alSource3i(st, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        alSource3i(mo, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 0, AL_FILTER_NULL);
        alSourcePlay(st);
        alSourcePlay(mo);
        as = al_source_get(st);
        assert(as && as->hw_voice_idx >= 0);
        assert(apu_voice_send_atten((uint32_t)as->hw_voice_idx, 0) == 0u);
        assert(apu_voice_send_atten((uint32_t)as->hw_voice_idx, 1) == 0xFFFu);   /* send 1 unused */
        as = al_source_get(mo);
        assert(as && as->hw_voice_idx >= 0);
        assert(apu_voice_send_atten((uint32_t)as->hw_voice_idx, 0) == apu_vp_atten_from_gain(0.5f));
        alSourceStop(st);
        alSourceStop(mo);
        alDeleteSources(1, &st);
        alDeleteSources(1, &mo);
        alDeleteBuffers(1, &bst);
        alDeleteBuffers(1, &bmo);
        assert(alGetError() == AL_NO_ERROR);
    }

    /* changing the effect does nothing until it is attached again */
    alEffecti(e, AL_XBOX_GP_PARAM0, 200);
    assert(al_source_debug_gp_kernel(&k, &params) && params[0] == 100u);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(al_source_debug_gp_kernel(&k, &params) && params[0] == 200u);

    /* one effect per device */
    alEnable(AL_XBOX_REVERB);
    assert(alGetError() == AL_INVALID_OPERATION);
    alGenEffects(1, &e2);
    alEffecti(e2, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    {
        ALuint slot2 = 0;
        alGenAuxiliaryEffectSlots(1, &slot2);
        alAuxiliaryEffectSloti(slot2, AL_EFFECTSLOT_EFFECT, (ALint)e2);
        assert(alGetError() == AL_INVALID_OPERATION);
        alDeleteAuxiliaryEffectSlots(1, &slot2);
    }

    /* deleting the effect leaves the slot's copy running */
    alDeleteEffects(1, &e);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_debug_gp_kernel(&k, &params) && k.words == GP_KECHO_WORDS && params[0] == 200u);
    alGetAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, &iv);
    assert(iv == (ALint)e);

    /* a GP program effect without an image holds the slot but runs nothing */
    alEffecti(e2, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e2);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_efx_active_slot() == slot && !al_source_debug_gp_kernel(&k, &params));
    /* an image, then the type set again: image and parameters are dropped */
    alXboxEffectProgram(e2, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    alEffecti(e2, AL_XBOX_GP_PARAM0, 7);
    alEffecti(e2, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    alGetEffecti(e2, AL_XBOX_GP_PARAM0, &iv);
    assert(iv == 0);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e2);
    assert(!al_source_debug_gp_kernel(&k, &params));
    /* an empty image clears it too */
    alXboxEffectProgram(e2, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    alXboxEffectProgram(e2, NULL, 0, 0);
    assert(alGetError() == AL_NO_ERROR);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e2);
    assert(!al_source_debug_gp_kernel(&k, &params));

    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    assert(al_efx_active_slot() == 0u);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    alDeleteEffects(1, &e2);
    assert(alGetError() == AL_NO_ERROR);
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    ALuint e = 0;
    ALint load = -1;

    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    assert(alGetProcAddress("alXboxEffectProgram") != NULL);

    alGenEffects(1, &e);
    test_effect(e);
    test_slot(e);

    printf("[4] the GP load status\n");
    alXboxGetHardwareStatus(AL_XBOX_GP_LOAD, &load);
    assert(alGetError() == AL_NO_ERROR && load == 0);
    alXboxGetHardwareStatus(AL_XBOX_GP_LOAD_PEAK, &load);
    assert(alGetError() == AL_NO_ERROR && load == 0);

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== AL_XBOX_gp_program: all passed ===\n");
    return 0;
}
