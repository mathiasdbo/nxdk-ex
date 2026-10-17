/*
 * ALC_EXT_EFX core (task phase8-01, lib/openal/docs/EFX_PLAN.md 3.1): the
 * extension's discovery, objects, parameter defaults / ranges / errors, the
 * one-effect rule, the source and listener parameters, and every entry point
 * through alGetProcAddress. Null backend; nothing reaches the APU.
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

static int near(float a, float b) {
    return fabsf(a - b) < 1e-6f;
}

static void expect_error(ALenum want, const char *what) {
    ALenum got = alGetError();
    if (got != want) {
        printf("    %s: error 0x%04x, wanted 0x%04x\n", what, (unsigned)got, (unsigned)want);
        assert(0);
    }
}

static void test_discovery(ALCdevice *dev) {
    static const char *const names[] = {
        "alGenEffects", "alDeleteEffects", "alIsEffect", "alEffecti", "alEffectiv", "alEffectf", "alEffectfv",
        "alGetEffecti", "alGetEffectiv", "alGetEffectf", "alGetEffectfv", "alGenFilters", "alDeleteFilters",
        "alIsFilter", "alFilteri", "alFilteriv", "alFilterf", "alFilterfv", "alGetFilteri", "alGetFilteriv",
        "alGetFilterf", "alGetFilterfv", "alGenAuxiliaryEffectSlots", "alDeleteAuxiliaryEffectSlots",
        "alIsAuxiliaryEffectSlot", "alAuxiliaryEffectSloti", "alAuxiliaryEffectSlotiv", "alAuxiliaryEffectSlotf",
        "alAuxiliaryEffectSlotfv", "alGetAuxiliaryEffectSloti", "alGetAuxiliaryEffectSlotiv",
        "alGetAuxiliaryEffectSlotf", "alGetAuxiliaryEffectSlotfv",
    };
    size_t i;
    ALCint v = -1;
    LPALGENEFFECTS gen;
    void *p;

    printf("[1] Discovery: extension string, versions, sends, entry points\n");
    assert(alcIsExtensionPresent(dev, "ALC_EXT_EFX") == ALC_TRUE);
    assert(strstr(alcGetString(dev, ALC_EXTENSIONS), "ALC_EXT_EFX") != NULL);
    alcGetIntegerv(dev, ALC_EFX_MAJOR_VERSION, 1, &v);
    assert(v == 1);
    alcGetIntegerv(dev, ALC_EFX_MINOR_VERSION, 1, &v);
    assert(v == 0);
    alcGetIntegerv(dev, ALC_MAX_AUXILIARY_SENDS, 1, &v);
    assert(v == 2);
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        assert(alGetProcAddress(names[i]) != NULL);
        assert(alcGetProcAddress(dev, names[i]) != NULL);
    }
    p = alGetProcAddress("alGenEffects");
    memcpy(&gen, &p, sizeof(gen));
    {
        ALuint e = 0;
        gen(1, &e);
        assert(e != 0 && alIsEffect(e));
        alDeleteEffects(1, &e);
    }
    assert(alGetProcAddress("alNoSuchFunction") == NULL);
    printf("    -> %u entry points [PASS]\n", (unsigned)(sizeof(names) / sizeof(names[0])));
}

static void test_effects(void) {
    ALuint e[2] = { 0, 0 }, bad = 999;
    ALint iv = -1;
    ALfloat fv = -1.0f, pan[3] = { 0.5f, -0.25f, 0.0f }, back[3];

    printf("[2] Effects: types, defaults, ranges, errors\n");
    alGenEffects(2, e);
    expect_error(AL_NO_ERROR, "gen");
    assert(e[0] && e[1] && e[0] != e[1]);
    assert(alIsEffect(0) == AL_TRUE && alIsEffect(bad) == AL_FALSE);
    alGetEffecti(e[0], AL_EFFECT_TYPE, &iv);
    assert(iv == AL_EFFECT_NULL);

    /* AL_EFFECT_NULL has no parameters */
    alEffectf(e[0], AL_REVERB_DECAY_TIME, 2.0f);
    expect_error(AL_INVALID_ENUM, "null effect parameter");

    /* Unsupported types */
    alEffecti(e[0], AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    expect_error(AL_INVALID_VALUE, "chorus");
    alEffecti(e[0], AL_EFFECT_TYPE, 0x1234);
    expect_error(AL_INVALID_VALUE, "unknown type");

    /* Reverb defaults and ranges */
    alEffecti(e[0], AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    expect_error(AL_NO_ERROR, "reverb type");
    alGetEffectf(e[0], AL_REVERB_DECAY_TIME, &fv);
    assert(near(fv, AL_REVERB_DEFAULT_DECAY_TIME));
    alGetEffectf(e[0], AL_REVERB_GAIN, &fv);
    assert(near(fv, AL_REVERB_DEFAULT_GAIN));
    alGetEffectf(e[0], AL_REVERB_AIR_ABSORPTION_GAINHF, &fv);
    assert(near(fv, AL_REVERB_DEFAULT_AIR_ABSORPTION_GAINHF));
    alGetEffecti(e[0], AL_REVERB_DECAY_HFLIMIT, &iv);
    assert(iv == AL_TRUE);
    alEffectf(e[0], AL_REVERB_DECAY_TIME, 2.8f);
    alGetEffectf(e[0], AL_REVERB_DECAY_TIME, &fv);
    assert(near(fv, 2.8f));
    alEffectf(e[0], AL_REVERB_DECAY_TIME, 25.0f);
    expect_error(AL_INVALID_VALUE, "decay above 20 s");
    alEffectf(e[0], AL_REVERB_DECAY_TIME, NAN);
    expect_error(AL_INVALID_VALUE, "decay NaN");
    alGetEffectf(e[0], AL_REVERB_DECAY_TIME, &fv);
    assert(near(fv, 2.8f));
    alEffecti(e[0], AL_REVERB_DECAY_HFLIMIT, 2);
    expect_error(AL_INVALID_VALUE, "hf limit 2");
    alEffecti(e[0], AL_REVERB_DECAY_TIME, 2);
    expect_error(AL_INVALID_ENUM, "decay time as an integer");
    alEffectf(e[0], AL_EAXREVERB_ECHO_TIME, 0.1f);   /* 0x000F: not a reverb parameter */
    expect_error(AL_INVALID_ENUM, "EAX parameter on a reverb");
    alEffectfv(e[0], AL_REVERB_DIFFUSION, pan);
    alGetEffectfv(e[0], AL_REVERB_DIFFUSION, back);
    assert(near(back[0], 0.5f));

    /* A new type resets the parameters */
    alEffecti(e[0], AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
    alGetEffectf(e[0], AL_EAXREVERB_DECAY_TIME, &fv);
    assert(near(fv, AL_EAXREVERB_DEFAULT_DECAY_TIME));
    alGetEffectf(e[0], AL_EAXREVERB_HFREFERENCE, &fv);
    assert(near(fv, AL_EAXREVERB_DEFAULT_HFREFERENCE));
    alGetEffectf(e[0], AL_EAXREVERB_ECHO_TIME, &fv);
    assert(near(fv, AL_EAXREVERB_DEFAULT_ECHO_TIME));
    alEffectf(e[0], AL_EAXREVERB_ECHO_TIME, 0.05f);
    expect_error(AL_INVALID_VALUE, "echo time below 0.075");
    alEffectfv(e[0], AL_EAXREVERB_REFLECTIONS_PAN, pan);
    expect_error(AL_NO_ERROR, "pan");
    alGetEffectfv(e[0], AL_EAXREVERB_REFLECTIONS_PAN, back);
    assert(near(back[0], 0.5f) && near(back[1], -0.25f) && near(back[2], 0.0f));
    alEffectf(e[0], AL_EAXREVERB_REFLECTIONS_PAN, 0.5f);
    expect_error(AL_INVALID_ENUM, "pan as one float");

    /* Names */
    alEffectf(bad, AL_REVERB_GAIN, 0.5f);
    expect_error(AL_INVALID_NAME, "bad name");
    alGetEffecti(e[0], AL_EFFECT_TYPE, NULL);
    expect_error(AL_INVALID_VALUE, "NULL out");
    {
        ALuint del[2] = { e[1], bad };
        alDeleteEffects(2, del);
        expect_error(AL_INVALID_NAME, "delete with a bad name");
        assert(alIsEffect(e[1]));   /* nothing deleted */
    }
    alDeleteEffects(2, e);
    expect_error(AL_NO_ERROR, "delete");
    assert(!alIsEffect(e[0]) && !alIsEffect(e[1]));

    /* All or nothing */
    {
        static ALuint many[257];
        alGenEffects(257, many);
        expect_error(AL_OUT_OF_MEMORY, "257 effects");
        alGenEffects(-1, many);
        expect_error(AL_INVALID_VALUE, "n < 0");
        alGenEffects(256, many);
        expect_error(AL_NO_ERROR, "256 effects");
        alDeleteEffects(256, many);
    }
    printf("    -> [PASS]\n");
}

static void test_filters(void) {
    ALuint f = 0;
    ALint iv = -1;
    ALfloat fv;

    printf("[3] Filters: AL_FILTER_NULL only until the VP filter is measured\n");
    alGenFilters(1, &f);
    assert(f && alIsFilter(f) && alIsFilter(0));
    alGetFilteri(f, AL_FILTER_TYPE, &iv);
    assert(iv == AL_FILTER_NULL);
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    expect_error(AL_INVALID_VALUE, "lowpass");
    alFilterf(f, AL_LOWPASS_GAIN, 0.5f);
    expect_error(AL_INVALID_ENUM, "null filter parameter");
    alGetFilterf(f, AL_LOWPASS_GAIN, &fv);
    expect_error(AL_INVALID_ENUM, "null filter get");
    alFilteri(f, AL_FILTER_TYPE, AL_FILTER_NULL);
    expect_error(AL_NO_ERROR, "null type");
    alDeleteFilters(1, &f);
    assert(!alIsFilter(f));
    printf("    -> [PASS]\n");
}

static void test_slots_and_sends(void) {
    ALuint slot[2] = { 0, 0 }, e = 0, src = 0, f = 0;
    ALint iv = -1;
    ALfloat fv = -1.0f;

    printf("[4] Slots, the one-effect rule, source sends\n");
    alGenAuxiliaryEffectSlots(2, slot);
    expect_error(AL_NO_ERROR, "gen slots");
    assert(alIsAuxiliaryEffectSlot(slot[0]) && !alIsAuxiliaryEffectSlot(0));
    alGetAuxiliaryEffectSlotf(slot[0], AL_EFFECTSLOT_GAIN, &fv);
    assert(near(fv, 1.0f));
    alGetAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_AUXILIARY_SEND_AUTO, &iv);
    assert(iv == AL_TRUE);
    alGetAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, &iv);
    assert(iv == 0);
    alAuxiliaryEffectSlotf(slot[0], AL_EFFECTSLOT_GAIN, 1.5f);
    expect_error(AL_INVALID_VALUE, "slot gain 1.5");
    alAuxiliaryEffectSlotf(slot[0], AL_EFFECTSLOT_GAIN, 0.7f);
    alGetAuxiliaryEffectSlotf(slot[0], AL_EFFECTSLOT_GAIN, &fv);
    assert(near(fv, 0.7f));
    {
        static ALuint more[4];
        alGenAuxiliaryEffectSlots(3, more);
        expect_error(AL_OUT_OF_MEMORY, "five slots");
    }

    /* The slot copies the effect */
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    alEffectf(e, AL_REVERB_DECAY_TIME, 3.0f);
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, (ALint)e);
    expect_error(AL_NO_ERROR, "attach");
    alGetAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, &iv);
    assert(iv == (ALint)e);
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, 999);
    expect_error(AL_INVALID_VALUE, "attach a bad effect");

    /* One effect at a time: not on a second slot, not with AL_XBOX_REVERB */
    alAuxiliaryEffectSloti(slot[1], AL_EFFECTSLOT_EFFECT, (ALint)e);
    expect_error(AL_INVALID_OPERATION, "second active slot");
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, (ALint)e);   /* the same slot again is fine */
    expect_error(AL_NO_ERROR, "re-attach");
    alEnable(AL_XBOX_REVERB);
    expect_error(AL_INVALID_OPERATION, "AL_XBOX_REVERB with an active slot");
    assert(alIsEnabled(AL_XBOX_REVERB) == AL_FALSE);
    alAuxiliaryEffectSloti(slot[0], AL_EFFECTSLOT_EFFECT, 0);
    alEnable(AL_XBOX_REVERB);
    expect_error(AL_NO_ERROR, "AL_XBOX_REVERB alone");
    alAuxiliaryEffectSloti(slot[1], AL_EFFECTSLOT_EFFECT, (ALint)e);
    expect_error(AL_INVALID_OPERATION, "slot while AL_XBOX_REVERB runs");
    alDisable(AL_XBOX_REVERB);
    alAuxiliaryEffectSloti(slot[1], AL_EFFECTSLOT_EFFECT, (ALint)e);
    expect_error(AL_NO_ERROR, "slot 2 after AL_XBOX_REVERB");
    alDeleteEffects(1, &e);   /* the slot keeps its copy */
    alGetAuxiliaryEffectSloti(slot[1], AL_EFFECTSLOT_EFFECT, &iv);
    assert(iv == (ALint)e);
    alAuxiliaryEffectSloti(slot[1], AL_EFFECTSLOT_EFFECT, 0);

    /* Sends */
    alGenSources(1, &src);
    alGenFilters(1, &f);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot[0], 0, AL_FILTER_NULL);
    expect_error(AL_NO_ERROR, "send 0");
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot[1], 1, (ALint)f);
    expect_error(AL_NO_ERROR, "send 1 with a filter");
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot[0], 2, 0);
    expect_error(AL_INVALID_VALUE, "send 2");
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 77, 0, 0);
    expect_error(AL_INVALID_VALUE, "bad slot");
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot[0], 0, 77);
    expect_error(AL_INVALID_VALUE, "bad filter");
    alSource3i(999, AL_AUXILIARY_SEND_FILTER, (ALint)slot[0], 0, 0);
    expect_error(AL_INVALID_NAME, "bad source");
    {
        ALint v[3] = { (ALint)slot[1], 0, 0 };
        alSourceiv(src, AL_AUXILIARY_SEND_FILTER, v);   /* send 0 moves to slot 2 */
        expect_error(AL_NO_ERROR, "send via iv");
    }

    /* A slot a source sends to cannot be deleted; after the source goes, it can */
    alDeleteAuxiliaryEffectSlots(1, &slot[1]);
    expect_error(AL_INVALID_OPERATION, "delete a slot in use");
    assert(alIsAuxiliaryEffectSlot(slot[1]));
    alDeleteAuxiliaryEffectSlots(1, &slot[0]);   /* slot 1 lost its only send above */
    expect_error(AL_NO_ERROR, "delete a free slot");
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 1, 0);
    alDeleteSources(1, &src);
    alDeleteAuxiliaryEffectSlots(1, &slot[1]);
    expect_error(AL_NO_ERROR, "delete after the source");
    alDeleteFilters(1, &f);
    printf("    -> [PASS]\n");
}

static void test_source_listener(void) {
    ALuint src = 0, f = 0;
    ALint iv = -1;
    ALfloat fv = -1.0f;

    printf("[5] Source and listener parameters\n");
    alGenSources(1, &src);
    alGetSourcef(src, AL_AIR_ABSORPTION_FACTOR, &fv);
    assert(near(fv, 0.0f));
    alGetSourcef(src, AL_ROOM_ROLLOFF_FACTOR, &fv);
    assert(near(fv, 0.0f));
    alGetSourcef(src, AL_CONE_OUTER_GAINHF, &fv);
    assert(near(fv, 1.0f));
    alGetSourcei(src, AL_DIRECT_FILTER_GAINHF_AUTO, &iv);
    assert(iv == AL_TRUE);
    alGetSourcei(src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, &iv);
    assert(iv == AL_TRUE);
    alGetSourcei(src, AL_AUXILIARY_SEND_FILTER_GAINHF_AUTO, &iv);
    assert(iv == AL_TRUE);

    alSourcef(src, AL_ROOM_ROLLOFF_FACTOR, 2.5f);
    alGetSourcef(src, AL_ROOM_ROLLOFF_FACTOR, &fv);
    assert(near(fv, 2.5f));
    {
        ALfloat v = 4.0f;
        alSourcefv(src, AL_AIR_ABSORPTION_FACTOR, &v);
        alGetSourcef(src, AL_AIR_ABSORPTION_FACTOR, &fv);
        assert(near(fv, 4.0f));
    }
    alSourcef(src, AL_AIR_ABSORPTION_FACTOR, 11.0f);
    expect_error(AL_INVALID_VALUE, "air absorption 11");
    alSourcef(src, AL_CONE_OUTER_GAINHF, -0.1f);
    expect_error(AL_INVALID_VALUE, "cone gain hf < 0");
    alSourcei(src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE);
    alGetSourcei(src, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, &iv);
    assert(iv == AL_FALSE);
    alSourcei(src, AL_DIRECT_FILTER_GAINHF_AUTO, 3);
    expect_error(AL_INVALID_VALUE, "auto flag 3");

    alGenFilters(1, &f);
    alSourcei(src, AL_DIRECT_FILTER, (ALint)f);
    expect_error(AL_NO_ERROR, "direct filter");
    alSourcei(src, AL_DIRECT_FILTER, 0);
    expect_error(AL_NO_ERROR, "no direct filter");
    alSourcei(src, AL_DIRECT_FILTER, 77);
    expect_error(AL_INVALID_VALUE, "bad direct filter");
    alSourcef(src, 0x2FFFF, 1.0f);
    expect_error(AL_INVALID_ENUM, "unknown source parameter");

    /* Reset on delete: a new source gets the defaults */
    alDeleteSources(1, &src);
    alGenSources(1, &src);
    alGetSourcef(src, AL_ROOM_ROLLOFF_FACTOR, &fv);
    assert(near(fv, 0.0f));
    alDeleteSources(1, &src);
    alDeleteFilters(1, &f);

    alGetListenerf(AL_METERS_PER_UNIT, &fv);
    assert(near(fv, 1.0f));
    alListenerf(AL_METERS_PER_UNIT, 0.3048f);
    alGetListenerf(AL_METERS_PER_UNIT, &fv);
    assert(near(fv, 0.3048f));
    alListenerf(AL_METERS_PER_UNIT, 0.0f);
    expect_error(AL_INVALID_VALUE, "0 meters per unit");
    printf("    -> [PASS]\n");
}

static void test_context_sends(ALCdevice *dev) {
    static const ALCint one_send[] = { ALC_MAX_AUXILIARY_SENDS, 1, 0 };
    static const ALCint four_sends[] = { ALC_MAX_AUXILIARY_SENDS, 4, 0 };
    ALCcontext *ctx1 = alcCreateContext(dev, one_send), *ctx4, *old = alcGetCurrentContext();
    ALuint src = 0, slot = 0;
    ALCint v = -1;

    printf("[6] ALC_MAX_AUXILIARY_SENDS per context\n");
    assert(ctx1);
    alcMakeContextCurrent(ctx1);
    alcGetIntegerv(dev, ALC_MAX_AUXILIARY_SENDS, 1, &v);
    assert(v == 1);
    alGenSources(1, &src);
    alGenAuxiliaryEffectSlots(1, &slot);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)slot, 1, 0);
    expect_error(AL_INVALID_VALUE, "send 1 with one send");
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, 0);
    expect_error(AL_NO_ERROR, "send 0 cleared");
    ctx4 = alcCreateContext(dev, four_sends);
    alcMakeContextCurrent(ctx4);
    alcGetIntegerv(dev, ALC_MAX_AUXILIARY_SENDS, 1, &v);
    assert(v == 2);   /* clamped to the hardware's two */
    alDeleteSources(1, &src);
    alDeleteAuxiliaryEffectSlots(1, &slot);
    alcMakeContextCurrent(old);
    alcDestroyContext(ctx1);
    alcDestroyContext(ctx4);
    printf("    -> [PASS]\n");
}

int main(void) {
    ALCdevice *dev;
    ALCcontext *ctx;

    printf("=== ALC_EXT_EFX core (phase8-01) ===\n");
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx);
    alcMakeContextCurrent(ctx);
    alGetError();

    test_discovery(dev);
    test_effects();
    test_filters();
    test_slots_and_sends();
    test_source_listener();
    test_context_sends(dev);

    /* Closing the last device frees every EFX object */
    {
        ALuint e = 0, e2 = 0;
        alGenEffects(1, &e);
        alcMakeContextCurrent(NULL);
        alcDestroyContext(ctx);
        alcCloseDevice(dev);
        dev = alcOpenDevice(NULL);
        ctx = alcCreateContext(dev, NULL);
        alcMakeContextCurrent(ctx);
        assert(!alIsEffect(e));
        alGenEffects(1, &e2);
        assert(e2 == 1u);
        alDeleteEffects(1, &e2);
    }
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== ALC_EXT_EFX core: all passed ===\n");
    return 0;
}
