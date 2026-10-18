/*
 * alXboxEffectSlotAtSample (AL_XBOX_gp_program): kernel parameters timed to a
 * streaming source's position. The checks, the GP frame each change is posted
 * for (a test clock stands in for the voice and the GP: al_source_debug_clock),
 * one change at a time on the GP and the rest queued in order, the cases that
 * wait or apply at once. The GP side (the frame the mailbox lands in) is in
 * tests/test_apu_gp.c and tests/test_gp_programs.py [3].
 */
#define AL_ALEXT_PROTOTYPES
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_efx.h"
#include "al_source.h"
#include "../dsp/gp_kecho.h"

static ALuint s_slot, s_src, s_buf, s_static_src;

static ALuint program(ALint p0) {
    ALuint e = 0;
    alGenEffects(1, &e);
    alEffecti(e, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    alXboxEffectProgram(e, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    alEffecti(e, AL_XBOX_GP_PARAM0, p0);
    assert(alGetError() == AL_NO_ERROR);
    return e;
}

static uint32_t param0(void) {
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;
    assert(al_source_debug_gp_kernel(&k, &p));
    return p[0];
}

static void setup(void) {
    static int16_t pcm[4800];
    ALuint b2 = 0;
    alGenAuxiliaryEffectSlots(1, &s_slot);
    alGenBuffers(1, &s_buf);
    alBufferData(s_buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &s_src);
    alSourceQueueBuffers(s_src, 1, &s_buf);
    alSourcei(s_src, AL_LOOPING, AL_TRUE);
    alGenSources(1, &s_static_src);
    alGenBuffers(1, &b2);
    alBufferData(b2, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alSourcei(s_static_src, AL_BUFFER, (ALint)b2);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_checks(ALuint e1) {
    ALuint other = 0, e2 = program(2);
    ALuint words[GP_KECHO_IMAGE_WORDS];

    printf("[1] the checks\n");
    alXboxEffectSlotAtSample(s_slot, e2, s_src, 100);       /* the slot runs nothing yet */
    assert(alGetError() == AL_INVALID_OPERATION);
    alAuxiliaryEffectSloti(s_slot, AL_EFFECTSLOT_EFFECT, (ALint)e1);
    assert(param0() == 1u);
    alXboxEffectSlotAtSample(s_slot, e2, 9999, 100);         /* no such source */
    assert(alGetError() == AL_INVALID_NAME);
    alXboxEffectSlotAtSample(s_slot, 9999, s_src, 100);
    assert(alGetError() == AL_INVALID_NAME);
    alXboxEffectSlotAtSample(9999, e2, s_src, 100);
    assert(alGetError() == AL_INVALID_NAME);
    alXboxEffectSlotAtSample(s_slot, e2, s_static_src, 100); /* not streaming */
    assert(alGetError() == AL_INVALID_OPERATION);
    /* another image: a restart, which no frame can time */
    memcpy(words, gp_kecho_image, sizeof(words));
    words[GP_KECHO_IMAGE_WORDS - 1u] = 0u;
    alGenEffects(1, &other);
    alEffecti(other, AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    alXboxEffectProgram(other, words, GP_KECHO_IMAGE_WORDS, 0);
    alXboxEffectSlotAtSample(s_slot, other, s_src, 100);
    assert(alGetError() == AL_INVALID_OPERATION);
    /* another copy of the same image is the same kernel */
    alXboxEffectProgram(other, gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    alEffecti(other, AL_XBOX_GP_PARAM0, 3);
    alSourcePlay(s_src);
    al_source_debug_clock(true, 0u, 100u, false);
    alXboxEffectSlotAtSample(s_slot, other, s_src, 0);
    assert(alGetError() == AL_NO_ERROR && param0() == 3u);
    al_source_debug_clock(true, 0u, 100u, false);
    alDeleteEffects(1, &other);
    alDeleteEffects(1, &e2);
    assert(alGetProcAddress("alXboxEffectSlotAtSample") != NULL);
}

static void test_timing(void) {
    ALuint e[6];
    int i;
    printf("[2] the frame each change is posted for, one at a time, in order\n");
    for (i = 0; i < 6; i++) {
        e[i] = program(10 + i);
    }
    /* 48 kHz, pitch 1: 32 frames of the stream a GP frame; 1000 ahead -> 32 frames on */
    al_source_debug_clock(true, 0u, 100u, false);
    alXboxEffectSlotAtSample(s_slot, e[0], s_src, 1000);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_debug_timed_frame() == 132u && param0() == 10u && al_efx_debug_timed_queued() == 0u);
    /* the GP holds it: the next two wait here */
    alXboxEffectSlotAtSample(s_slot, e[1], s_src, 2000);
    alXboxEffectSlotAtSample(s_slot, e[2], s_src, 3000);
    assert(al_efx_debug_timed_queued() == 2u && param0() == 10u);
    alXboxUpdateVoices();
    assert(al_efx_debug_timed_queued() == 2u);
    /* the GP took it: the next goes out at the next update, against the clock then */
    al_source_debug_clock(true, 1500u, 150u, false);
    alXboxUpdateVoices();
    assert(al_source_debug_timed_frame() == 150u + 16u && param0() == 11u && al_efx_debug_timed_queued() == 1u);
    al_source_debug_clock(true, 1500u, 151u, false);
    alXboxUpdateVoices();
    assert(al_source_debug_timed_frame() == 151u + 47u && param0() == 12u && al_efx_debug_timed_queued() == 0u);
    /* a sample already played: the next frame */
    al_source_debug_clock(true, 5000u, 300u, false);
    alXboxEffectSlotAtSample(s_slot, e[3], s_src, 4000);
    assert(al_source_debug_timed_frame() == 300u && param0() == 13u);
    /* across the stream position's wrap: 320 frames ahead */
    al_source_debug_clock(true, 0xFFFFFF00u, 400u, false);
    alXboxEffectSlotAtSample(s_slot, e[4], s_src, 0x40u);
    assert(al_source_debug_timed_frame() == 410u && param0() == 14u);
    /* pitch 2: 64 frames of the stream a GP frame */
    alSourcef(s_src, AL_PITCH, 2.0f);
    al_source_debug_clock(true, 0u, 500u, false);
    alXboxEffectSlotAtSample(s_slot, e[5], s_src, 640u);
    assert(al_source_debug_timed_frame() == 510u && param0() == 15u);
    alSourcef(s_src, AL_PITCH, 1.0f);
    al_source_debug_clock(true, 0u, 500u, false);
    for (i = 0; i < 6; i++) {
        alDeleteEffects(1, &e[i]);   /* the slot and the queue hold their own copies */
    }
    assert(alGetError() == AL_NO_ERROR);
}

static void test_queue(void) {
    ALuint e[17];
    int i;
    printf("[3] the queue: full, paused, stopped, dropped by an attach\n");
    for (i = 0; i < 17; i++) {
        e[i] = program(20 + i);
    }
    al_source_debug_clock(true, 0u, 0u, true);              /* the GP holds a change */
    for (i = 0; i < 16; i++) {
        alXboxEffectSlotAtSample(s_slot, e[i], s_src, 1000u + (ALuint)i);
    }
    assert(alGetError() == AL_NO_ERROR && al_efx_debug_timed_queued() == 16u);
    alXboxEffectSlotAtSample(s_slot, e[16], s_src, 5000u);
    assert(alGetError() == AL_INVALID_OPERATION);
    /* paused: they wait, even with the GP free */
    alSourcePause(s_src);
    al_source_debug_clock(true, 0u, 0u, false);
    alXboxUpdateVoices();
    assert(al_efx_debug_timed_queued() == 16u);
    /* playing again: one goes out; then an attach drops the rest */
    alSourcePlay(s_src);
    alXboxUpdateVoices();
    assert(al_efx_debug_timed_queued() == 15u && param0() == 20u);
    alAuxiliaryEffectSloti(s_slot, AL_EFFECTSLOT_EFFECT, (ALint)e[16]);
    assert(al_efx_debug_timed_queued() == 0u && param0() == 36u);
    /* stopped: at once */
    al_source_debug_clock(true, 0u, 0u, true);
    alXboxEffectSlotAtSample(s_slot, e[1], s_src, 100000u);
    assert(al_efx_debug_timed_queued() == 1u);
    alSourceStop(s_src);
    alXboxUpdateVoices();
    assert(al_efx_debug_timed_queued() == 0u && param0() == 21u);
    /* deleting the slot drops its queue */
    alSourcePlay(s_src);
    al_source_debug_clock(true, 0u, 0u, true);
    alXboxEffectSlotAtSample(s_slot, e[2], s_src, 100000u);
    assert(al_efx_debug_timed_queued() == 1u);
    alAuxiliaryEffectSloti(s_slot, AL_EFFECTSLOT_EFFECT, 0);
    assert(al_efx_debug_timed_queued() == 0u);
    for (i = 0; i < 17; i++) {
        alDeleteEffects(1, &e[i]);
    }
    al_source_debug_clock(false, 0u, 0u, false);
    assert(alGetError() == AL_NO_ERROR);
}

static void test_no_gp(void) {
    ALuint e1 = program(40), e2 = program(41);
    printf("[4] no GP to time it (the host): at once, like an attach\n");
    alSourcePlay(s_src);
    alAuxiliaryEffectSloti(s_slot, AL_EFFECTSLOT_EFFECT, (ALint)e1);
    alXboxEffectSlotAtSample(s_slot, e2, s_src, 100000u);
    assert(alGetError() == AL_NO_ERROR && al_efx_debug_timed_queued() == 0u && param0() == 41u);
    alAuxiliaryEffectSloti(s_slot, AL_EFFECTSLOT_EFFECT, 0);
    alDeleteEffects(1, &e1);
    alDeleteEffects(1, &e2);
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;
    ALuint e1;

    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    setup();
    e1 = program(1);
    test_checks(e1);
    test_timing();
    test_queue();
    test_no_gp();
    alDeleteEffects(1, &e1);
    alSourceStop(s_src);
    alDeleteSources(1, &s_src);
    alDeleteSources(1, &s_static_src);
    alDeleteAuxiliaryEffectSlots(1, &s_slot);
    assert(alGetError() == AL_NO_ERROR);

    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== alXboxEffectSlotAtSample: all passed ===\n");
    return 0;
}
