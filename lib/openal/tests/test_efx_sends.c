/*
 * EFX slots and sends (task phase8-03, lib/openal/docs/EFX_PLAN.md 3.2, 3.5):
 * which effect the GP runs (AL_XBOX_REVERB or the active EFX slot), the EFX
 * reverb's mapping onto the GP reverb, and the send gains a source hands its
 * voice. Null backend: the routing (outputs 4-7 to bins 2/3) is checked on a
 * console.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_efx.h"
#include "al_source.h"

static int near(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static float onepole_gain(float p, float hz) {
    double w = 2.0 * 3.14159265358979 * hz / 48000.0;
    return (float)((1.0 - p) / sqrt(1.0 - 2.0 * p * cos(w) + (double)p * p));
}

static void test_mapping(void) {
    ALuint e = 0, slot = 0;
    apu_reverb_params_t p;
    float lp;

    printf("[1] AL_EFFECT_REVERB / EAXREVERB onto the GP reverb\n");
    assert(!al_source_debug_gp_effect(&p));             /* nothing runs */
    alGenEffects(1, &e);
    alGenAuxiliaryEffectSlots(1, &slot);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_debug_gp_effect(&p));
    assert(near(p.decay_time, AL_REVERB_DEFAULT_DECAY_TIME, 1e-6f));
    assert(near(p.hf_ratio, AL_REVERB_DEFAULT_DECAY_HFRATIO, 1e-6f));
    assert(near(p.gain, AL_REVERB_DEFAULT_GAIN * AL_REVERB_DEFAULT_LATE_REVERB_GAIN, 1e-5f));
    assert(near(p.pre_delay, AL_REVERB_DEFAULT_REFLECTIONS_DELAY + AL_REVERB_DEFAULT_LATE_REVERB_DELAY, 1e-6f));
    assert(near(p.diffusion, AL_REVERB_DEFAULT_DIFFUSION, 1e-6f));
    assert(near(onepole_gain(p.input_lowpass, 5000.0f), AL_REVERB_DEFAULT_GAINHF, 1e-3f));   /* GAINHF at 5 kHz */
    assert(p.output_lowpass == 0.0f);

    /* The slot's gain scales the wet gain; the effect's values are copied when attached */
    alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, 0.5f);
    assert(al_source_debug_gp_effect(&p));
    assert(near(p.gain, 0.5f * AL_REVERB_DEFAULT_GAIN * AL_REVERB_DEFAULT_LATE_REVERB_GAIN, 1e-5f));
    alEffectf(e, AL_REVERB_DECAY_HFRATIO, 1.8f);          /* not attached yet */
    alEffectf(e, AL_REVERB_DECAY_TIME, 4.0f);
    alEffectf(e, AL_REVERB_GAIN, 1.0f);
    alEffectf(e, AL_REVERB_LATE_REVERB_GAIN, 5.0f);
    alEffectf(e, AL_REVERB_REFLECTIONS_DELAY, 0.3f);
    alEffectf(e, AL_REVERB_GAINHF, 1.0f);
    assert(al_source_debug_gp_effect(&p) && near(p.decay_time, AL_REVERB_DEFAULT_DECAY_TIME, 1e-6f));
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(al_source_debug_gp_effect(&p));
    assert(near(p.decay_time, 4.0f, 1e-6f));
    assert(p.hf_ratio == 1.0f);                           /* no HF boost on the GP reverb */
    assert(p.gain == 1.0f);                               /* 0.5 x 1 x 5, clamped */
    assert(p.pre_delay == 0.1f);                          /* 0.3 + 0.011, clamped */
    assert(p.input_lowpass == 0.0f);                      /* GAINHF 1: no input lowpass */

    /* EAX reverb: the HF reference moves the input lowpass */
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    alEffectf(e, AL_EAXREVERB_GAINHF, 0.5f);
    alEffectf(e, AL_EAXREVERB_HFREFERENCE, 2000.0f);
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(al_source_debug_gp_effect(&p));
    lp = p.input_lowpass;
    assert(lp > 0.0f && near(onepole_gain(lp, 2000.0f), 0.5f, 1e-3f));
    assert(al_efx_onepole_for_gain(0.0f, 5000.0f) == 0.95f && al_efx_onepole_for_gain(1.5f, 5000.0f) == 0.0f);

    /* AL_XBOX_REVERB owns the GP while it is on (and then no slot may hold an effect) */
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, 0);
    assert(!al_source_debug_gp_effect(&p));
    alEnable(AL_XBOX_REVERB);
    alXboxReverbPreset(AL_XBOX_REVERB_PRESET_HALL);
    assert(al_source_debug_gp_effect(&p) && near(p.decay_time, 2.5f, 1e-6f));
    alDisable(AL_XBOX_REVERB);
    assert(!al_source_debug_gp_effect(&p));

    alDeleteAuxiliaryEffectSlots(1, &slot);
    alDeleteEffects(1, &e);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> defaults, slot gain, copy on attach, clamps, GAINHF at HFREFERENCE, AL_XBOX_REVERB [PASS]\n");
}

static void test_sends(void) {
    ALuint e = 0, slot[2] = { 0, 0 }, src = 0;
    float s[APU_VOICE_SENDS];

    printf("[2] Send gains\n");
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    alGenAuxiliaryEffectSlots(2, slot);
    alGenSources(1, &src);
    alSource3f(src, AL_POSITION, 0.0f, 0.0f, -5.0f);      /* 5 units in front: dry gain 1 / (1 + 4) */

    /* No active effect: no send is heard */
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot[0], 0, AL_FILTER_NULL);
    assert(al_source_debug_sends(src, s) && s[0] == 0.0f && s[1] == 0.0f);

    /* The active slot: room rolloff 0 means no distance attenuation for the send */
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(al_source_debug_sends(src, s) && near(s[0], 1.0f, 1e-6f) && s[1] == 0.0f);

    /* AL_ROOM_ROLLOFF_FACTOR (source) plus AL_REVERB_ROOM_ROLLOFF_FACTOR (effect, with send-auto) */
    alSourcef(src, AL_ROOM_ROLLOFF_FACTOR, 1.0f);
    assert(al_source_debug_sends(src, s) && near(s[0], 0.2f, 1e-5f));
    alEffectf(e, AL_REVERB_ROOM_ROLLOFF_FACTOR, 1.0f);
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(al_source_debug_sends(src, s) && near(s[0], 1.0f / 9.0f, 1e-5f));   /* rolloff 2 */
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_AUXILIARY_SEND_AUTO, AL_FALSE);
    assert(al_source_debug_sends(src, s) && near(s[0], 0.2f, 1e-5f));          /* the effect's part off */
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_AUXILIARY_SEND_AUTO, AL_TRUE);
    alSourcef(src, AL_ROOM_ROLLOFF_FACTOR, 0.0f);

    /* AL_GAIN and the listener gain count; the slot gain does not (the GP applies it) */
    alSourcef(src, AL_GAIN, 0.5f);
    alListenerf(AL_GAIN, 0.5f);
    alAuxiliaryEffectSlotf(slot[0], AL_EFFECTSLOT_GAIN, 0.1f);
    assert(al_source_debug_sends(src, s) && near(s[0], 0.25f * 0.2f, 1e-5f));   /* room rolloff 1 from the effect */
    alListenerf(AL_GAIN, 1.0f);
    alSourcef(src, AL_GAIN, 1.0f);

    /* The cone: only with AL_AUXILIARY_SEND_FILTER_GAIN_AUTO */
    alEffectf(e, AL_REVERB_ROOM_ROLLOFF_FACTOR, 0.0f);
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, (ALint)e);
    alSource3f(src, AL_DIRECTION, 0.0f, 0.0f, -1.0f);   /* facing away from the listener */
    alSourcef(src, AL_CONE_INNER_ANGLE, 30.0f);
    alSourcef(src, AL_CONE_OUTER_ANGLE, 60.0f);
    alSourcef(src, AL_CONE_OUTER_GAIN, 0.25f);
    assert(al_source_debug_sends(src, s) && near(s[0], 0.25f, 1e-5f));
    alSourcei(src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE);
    assert(al_source_debug_sends(src, s) && near(s[0], 1.0f, 1e-5f));
    alSourcei(src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_TRUE);
    alSourcef(src, AL_CONE_OUTER_GAIN, 1.0f);

    /* Send 1 to the same slot is heard too; a send to another (inactive) slot is not */
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot[0], 1, AL_FILTER_NULL);
    assert(al_source_debug_sends(src, s) && near(s[0], 1.0f, 1e-6f) && near(s[1], 1.0f, 1e-6f));
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot[1], 1, AL_FILTER_NULL);
    assert(al_source_debug_sends(src, s) && s[1] == 0.0f);
    /* Moving the effect to the other slot moves which send is heard */
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, 0);
    alAuxiliaryEffectSloti(slot[1], AL_EFFECTSLOT_EFFECT, (ALint)e);
    assert(al_source_debug_sends(src, s) && s[0] == 0.0f && near(s[1], 1.0f, 1e-6f));
    alAuxiliaryEffectSloti(slot[1], AL_EFFECTSLOT_EFFECT, 0);

    /* AL_XBOX_REVERB: send 0 is AL_XBOX_REVERB_SEND times the dry gain, EFX sends are silent */
    alSourcef(src, AL_XBOX_REVERB_SEND, 0.5f);
    assert(al_source_debug_sends(src, s) && s[0] == 0.0f && s[1] == 0.0f);   /* reverb off */
    alEnable(AL_XBOX_REVERB);
    assert(al_source_debug_sends(src, s) && near(s[0], 0.5f * 0.2f, 1e-5f) && s[1] == 0.0f);
    alDisable(AL_XBOX_REVERB);

    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, 0);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 1, 0);
    alDeleteSources(1, &src);
    alDeleteAuxiliaryEffectSlots(2, slot);
    alDeleteEffects(1, &e);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> active slot only, room rolloff (source + effect, send-auto), gains, cone with GAIN_AUTO, "
           "AL_XBOX_REVERB_SEND [PASS]\n");
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    printf("=== EFX slots and sends (phase8-03) ===\n");
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    alcMakeContextCurrent(ctx);
    alGetError();
    test_mapping();
    test_sends();
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== EFX slots and sends: all passed ===\n");
    return 0;
}
