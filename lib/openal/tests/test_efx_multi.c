/*
 * Several effects at once (docs/SOFT_GAP_PLAN.md phase9-04, EFX_PLAN.md 3.1-3.2):
 * every EFX slot runs its own GP kernel, an attach fails with
 * AL_INVALID_OPERATION only when the GP's shared budgets are full, send n of a
 * source to slot s feeds GP slot s - 1 (bins 2+2k / 3+2k), and a playing
 * voice's send moves to another slot's bins with a volume ramp. Null backend:
 * the loader is tests/test_apu_gp.c [7]-[9], three kernels running together
 * tests/test_gp_programs.py [10]; the on-chip effects (phase9-07) beside the
 * reverb and each other [4], their kernels together tests/test_gp_onchip_slots.py.
 */
#define AL_ALEXT_PROTOTYPES
#include <assert.h>
#include <stdio.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_efx.h"
#include "al_source.h"
#include "apu_voice.h"
#include "apu_vp.h"
#include "../dsp/gp_kecho.h"
#include "gp_kefx_autowah.h"
#include "gp_kefx_compressor.h"
#include "gp_kefx_distortion.h"
#include "gp_kefx_equalizer.h"
#include "gp_kefx_ringmod.h"

static ALuint s_fx[6], s_slot[4];

static void attach(ALuint slot, ALuint fx, ALenum want) {
    alAuxiliaryEffectSloti(s_slot[slot], AL_EFFECTSLOT_EFFECT, (ALint)fx);
    assert(alGetError() == want);
}

static void test_budgets(void) {
    static const ALuint abi1[5] = { AL_XBOX_GP_IMAGE_MAGIC, 1u, 0u, 0u, 0x00000Cu };   /* rts at P:$100 */
    ALint iv = -1;

    printf("[1] one kernel per slot; refused only when a budget is full\n");
    alEffecti(s_fx[0], AL_EFFECT_TYPE, AL_EFFECT_REVERB);
    alEffecti(s_fx[1], AL_EFFECT_TYPE, AL_EFFECT_ECHO);
    alEffecti(s_fx[2], AL_EFFECT_TYPE, AL_EFFECT_CHORUS);
    alEffecti(s_fx[3], AL_EFFECT_TYPE, AL_EFFECT_FLANGER);
    alEffecti(s_fx[4], AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    alXboxEffectProgram(s_fx[4], gp_kecho_image, GP_KECHO_IMAGE_WORDS, 0);
    alEffecti(s_fx[5], AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    alXboxEffectProgram(s_fx[5], abi1, 5, 0);
    assert(alGetError() == AL_NO_ERROR);

    /* reverb, echo and chorus at once, each in its own GP slot */
    attach(0, s_fx[0], AL_NO_ERROR);
    attach(1, s_fx[1], AL_NO_ERROR);
    attach(2, s_fx[2], AL_NO_ERROR);
    assert(al_source_debug_gp_slot(0, NULL, NULL, NULL) == AL_GP_REVERB);
    assert(al_source_debug_gp_slot(1, NULL, NULL, NULL) == AL_GP_KERNEL);
    assert(al_source_debug_gp_slot(2, NULL, NULL, NULL) == AL_GP_KERNEL);
    assert(al_source_debug_gp_slot(3, NULL, NULL, NULL) == AL_GP_NONE);
    /* a second reverb: X (two blocks of $8C0 in $B20); a flanger beside the chorus: Y (2 x 2048) */
    attach(3, s_fx[0], AL_INVALID_OPERATION);
    attach(3, s_fx[3], AL_INVALID_OPERATION);
    alGetAuxiliaryEffectSloti(s_slot[3], AL_EFFECTSLOT_EFFECT, &iv);
    assert(iv == 0 && al_source_debug_gp_slot(3, NULL, NULL, NULL) == AL_GP_NONE);   /* nothing changed */
    /* the flanger in the chorus's own slot replaces it: the slot's old kernel does not count */
    attach(2, s_fx[3], AL_NO_ERROR);
    /* the example echo beside the reverb and the echo: scratch 180 + 128 + 128 KiB of about 500 */
    attach(3, s_fx[4], AL_NO_ERROR);
    attach(2, s_fx[1], AL_INVALID_OPERATION);                       /* a second EFX echo: 128 KiB more */
    assert(al_source_debug_gp_slot(2, NULL, NULL, NULL) == AL_GP_KERNEL);   /* the flanger stays */
    /* an ABI 1 kernel runs only alone */
    attach(2, s_fx[5], AL_INVALID_OPERATION);
    attach(0, 0, AL_NO_ERROR);
    attach(1, 0, AL_NO_ERROR);
    attach(3, 0, AL_NO_ERROR);
    attach(2, s_fx[5], AL_NO_ERROR);
    attach(0, s_fx[2], AL_INVALID_OPERATION);
    alEnable(AL_XBOX_REVERB);
    assert(alGetError() == AL_INVALID_OPERATION);                   /* the mix's reverb: only with no EFX effect */
    attach(2, 0, AL_NO_ERROR);
    alEnable(AL_XBOX_REVERB);
    assert(alGetError() == AL_NO_ERROR && al_source_debug_gp_slot(0, NULL, NULL, NULL) == AL_GP_REVERB);
    attach(1, s_fx[1], AL_INVALID_OPERATION);                       /* and no EFX effect beside it */
    alDisable(AL_XBOX_REVERB);
    assert(al_source_debug_gp_slot(0, NULL, NULL, NULL) == AL_GP_NONE);
    printf("    -> reverb + echo + chorus together; a second reverb (X), a flanger beside the chorus (Y), "
           "a third 128 KiB line (scratch) and an ABI 1 kernel beside another refused; replacing a slot's "
           "own kernel fits [PASS]\n");
}

/* A source playing a looping buffer on a voice (null backend) */
static ALuint play_source(ALuint buf, uint32_t *voice) {
    ALuint src = 0;
    ALsource *as;
    alGenSources(1, &src);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    as = al_source_get(src);
    assert(as && as->hw_voice_idx >= 0);
    *voice = (uint32_t)as->hw_voice_idx;
    return src;
}

static void test_routing(void) {
    ALuint buf = 0, src, src2;
    uint32_t v, v2;
    int16_t pcm[512] = { 0 };
    float g[APU_VOICE_SENDS];
    uint8_t k[APU_VOICE_SENDS];
    uint16_t half = apu_vp_atten_from_gain(0.5f);

    printf("[2] send n to slot s: GP slot s - 1, bins 2+2k / 3+2k\n");
    attach(0, s_fx[0], AL_NO_ERROR);                                /* reverb: GP slot 0, bins 2/3 */
    attach(1, s_fx[1], AL_NO_ERROR);                                /* echo: GP slot 1, bins 4/5 */
    attach(3, s_fx[2], AL_NO_ERROR);                                /* chorus: GP slot 3, bins 8/9 */
    alGenBuffers(1, &buf);
    alBufferData(buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    src = play_source(buf, &v);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[1], 0, AL_FILTER_NULL);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[3], 1, AL_FILTER_NULL);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_debug_send_route(src, g, k) && g[0] == 1.0f && g[1] == 1.0f && k[0] == 1u && k[1] == 3u);
    /* a playing voice moves: muted first, the bins after two frames */
    assert(apu_voice_send_moving(v) && apu_voice_send_atten(v, 0) == 0xFFFu && apu_voice_send_atten(v, 1) == 0xFFFu);
    apu_voice_service(0);
    apu_voice_service(0);
    assert(!apu_voice_send_moving(v) && apu_voice_send_bin(v, 0) == 4u && apu_voice_send_bin(v, 1) == 8u);
    assert(apu_voice_send_atten(v, 0) == half && apu_voice_send_atten(v, 1) == half);
    /* a slot with no effect: no send, and the voice's outputs stay where they are */
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[2], 1, AL_FILTER_NULL);
    assert(al_source_debug_send_route(src, g, k) && g[1] == 0.0f && k[1] == 0xFFu);
    assert(!apu_voice_send_moving(v) && apu_voice_send_bin(v, 1) == 8u && apu_voice_send_atten(v, 1) == 0xFFFu);
    printf("    -> sends 0 / 1 on bins 4 / 8; a send to an empty slot is muted in place [PASS]\n");

    printf("[3] moving a playing voice's send: a volume ramp, then the bins\n");
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[0], 0, AL_FILTER_NULL);   /* echo -> reverb */
    assert(apu_voice_send_moving(v));
    assert(apu_voice_send_bin(v, 0) == 4u && apu_voice_send_atten(v, 0) == 0xFFFu);  /* still there, muted */
    apu_voice_service(0);
    assert(apu_voice_send_moving(v) && apu_voice_send_bin(v, 0) == 4u);            /* one frame: not yet */
    alSourcef(src, AL_GAIN, 1.0f);                                  /* an update during the ramp keeps it muted */
    assert(apu_voice_send_atten(v, 0) == 0xFFFu);
    apu_voice_service(0);
    assert(!apu_voice_send_moving(v) && apu_voice_send_bin(v, 0) == 2u);           /* two frames: moved */
    assert(apu_voice_send_atten(v, 0) == half);
    /* back within the ramp: the move is called off, nothing moves */
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[1], 0, AL_FILTER_NULL);
    assert(apu_voice_send_moving(v));
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[0], 0, AL_FILTER_NULL);
    assert(!apu_voice_send_moving(v) && apu_voice_send_bin(v, 0) == 2u && apu_voice_send_atten(v, 0) == half);
    /* the effect moves to another slot: its senders follow when they are pointed at it */
    attach(1, 0, AL_NO_ERROR);
    attach(2, s_fx[1], AL_NO_ERROR);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[2], 0, AL_FILTER_NULL);
    assert(apu_voice_send_moving(v) && apu_voice_send_bin(v, 0) == 2u);
    apu_voice_service(0);
    apu_voice_service(0);
    assert(apu_voice_send_bin(v, 0) == 6u && !apu_voice_send_moving(v));
    /* a voice that is not playing takes the new bins at once */
    src2 = play_source(buf, &v2);
    alSourceStop(src2);
    alSource3i(src2, AL_AUXILIARY_SEND_FILTER, (ALint)s_slot[3], 0, AL_FILTER_NULL);
    alSourcePlay(src2);
    v2 = (uint32_t)al_source_get(src2)->hw_voice_idx;
    assert(!apu_voice_send_moving(v2) && apu_voice_send_bin(v2, 0) == 8u && apu_voice_send_atten(v2, 0) == half);

    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
    alSource3i(src, AL_AUXILIARY_SEND_FILTER, 0, 1, AL_FILTER_NULL);
    alSource3i(src2, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
    alSourceStop(src);
    alSourceStop(src2);
    alDeleteSources(1, &src);
    alDeleteSources(1, &src2);
    alDeleteBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> muted, two frames, then the new bins and the volume; called off when it goes back; "
           "a stopped voice moves at once [PASS]\n");
}

/* An ABI 2 image of `code` words (nop ..., rts) with an X block of `x_words` */
static ALuint *filler_image(uint32_t code, uint32_t x_words, ALsizei *count) {
    static ALuint img[7u + 0x700u];
    uint32_t i;
    img[0] = AL_XBOX_GP_IMAGE_MAGIC;
    img[1] = 2u;
    img[2] = 0u;            /* scratch */
    img[3] = 0u;            /* descriptors */
    img[4] = x_words;
    img[5] = 0u;            /* Y */
    img[6] = 0u;            /* relocs */
    for (i = 0; i < code; i++) {
        img[7u + i] = (i + 1u < code) ? 0x000000u : 0x00000Cu;
    }
    *count = (ALsizei)(7u + code);
    return img;
}

static void test_onchip(void) {
    static const ALenum types[5] = { AL_EFFECT_RING_MODULATOR, AL_EFFECT_DISTORTION, AL_EFFECT_EQUALIZER,
                                     AL_EFFECT_COMPRESSOR, AL_EFFECT_AUTOWAH };
    static const uint32_t x_words[5] = { GP_KEFX_RINGMOD_K_XWORDS, GP_KEFX_DISTORTION_K_XWORDS,
                                         GP_KEFX_EQUALIZER_K_XWORDS, GP_KEFX_COMPRESSOR_K_XWORDS,
                                         GP_KEFX_AUTOWAH_K_XWORDS };
    const apu_gp_kernel_t *rv = apu_gp_reverb_kernel();
    ALuint fx[6];
    ALsizei n;
    const ALuint *img;
    uint32_t a, b, c, d, combos = 0, s;

    printf("[4] the on-chip effects: any three beside the reverb, any four together; refused when full\n");
    for (s = 0; s < 4; s++) {
        attach(s, 0, AL_NO_ERROR);
    }
    alGenEffects(6, fx);
    for (a = 0; a < 5; a++) {
        alEffecti(fx[a], AL_EFFECT_TYPE, types[a]);
    }
    alEffecti(fx[5], AL_EFFECT_TYPE, AL_EFFECT_XBOX_GP_PROGRAM);
    assert(alGetError() == AL_NO_ERROR);

    /* the reverb in slot 0 and every three of the five in slots 1-3 */
    attach(0, s_fx[0], AL_NO_ERROR);
    for (a = 0; a < 5; a++) {
        for (b = a + 1; b < 5; b++) {
            for (c = b + 1; c < 5; c++) {
                attach(1, fx[a], AL_NO_ERROR);
                attach(2, fx[b], AL_NO_ERROR);
                attach(3, fx[c], AL_NO_ERROR);
                assert(al_source_debug_gp_slot(0, NULL, NULL, NULL) == AL_GP_REVERB);
                for (s = 1; s < 4; s++) {
                    assert(al_source_debug_gp_slot(s, NULL, NULL, NULL) == AL_GP_KERNEL);
                }
                for (s = 1; s < 4; s++) {
                    attach(s, 0, AL_NO_ERROR);
                }
                combos++;
            }
        }
    }
    attach(0, 0, AL_NO_ERROR);
    /* every four of the five */
    for (d = 0; d < 5; d++) {
        for (a = 0, s = 0; a < 5; a++) {
            if (a != d) attach(s++, fx[a], AL_NO_ERROR);
        }
        for (s = 0; s < 4; s++) {
            assert(al_source_debug_gp_slot(s, NULL, NULL, NULL) == AL_GP_KERNEL);
            attach(s, 0, AL_NO_ERROR);
        }
        combos++;
    }
    assert(combos == 15u);

    /* X: the reverb, a program that leaves room for exactly the ring modulator and the compressor;
     * then the equalizer does not fit (nothing changes), in the ring modulator's own slot it does */
    img = filler_image(1u, APU_GP_KERNEL_MAX_XWORDS - rv->x_words - x_words[0] - x_words[3], &n);
    alXboxEffectProgram(fx[5], img, n, 0);
    assert(alGetError() == AL_NO_ERROR);
    attach(0, s_fx[0], AL_NO_ERROR);
    attach(1, fx[5], AL_NO_ERROR);
    attach(2, fx[0], AL_NO_ERROR);
    attach(3, fx[2], AL_INVALID_OPERATION);
    assert(al_source_debug_gp_slot(3, NULL, NULL, NULL) == AL_GP_NONE);
    attach(3, fx[3], AL_NO_ERROR);
    attach(2, fx[2], AL_NO_ERROR);                                  /* $44 where the $53 was */
    attach(3, fx[0], AL_INVALID_OPERATION);
    assert(al_source_debug_gp_slot(3, NULL, NULL, NULL) == AL_GP_KERNEL);   /* the compressor stays */
    for (s = 0; s < 4; s++) {
        attach(s, 0, AL_NO_ERROR);
    }
    /* P ($700 words): the reverb, the autowah and a program filling the rest; one more refused */
    img = filler_image(APU_GP_KERNEL_MAX_WORDS - rv->words - GP_KEFX_AUTOWAH_WORDS, 0x28u, &n);
    alXboxEffectProgram(fx[5], img, n, 0);
    assert(alGetError() == AL_NO_ERROR);
    attach(0, s_fx[0], AL_NO_ERROR);
    attach(1, fx[4], AL_NO_ERROR);
    attach(2, fx[5], AL_NO_ERROR);
    for (a = 0; a < 4; a++) {
        attach(3, fx[a], AL_INVALID_OPERATION);
    }
    for (s = 0; s < 4; s++) {
        attach(s, 0, AL_NO_ERROR);
    }
    alDeleteEffects(6, fx);
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> %u sets fit (the five use no Y, scratch or descriptors; X $%X..$%X, P %u..%u words); "
           "X and P full: AL_INVALID_OPERATION, the slot unchanged [PASS]\n",
           (unsigned)combos, (unsigned)GP_KEFX_COMPRESSOR_K_XWORDS, (unsigned)GP_KEFX_RINGMOD_K_XWORDS,
           (unsigned)GP_KEFX_EQUALIZER_WORDS, (unsigned)GP_KEFX_AUTOWAH_WORDS);
}

int main(void) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx;

    printf("=== Several effects at once (phase9-04) ===\n");
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    alGenEffects(6, s_fx);
    alGenAuxiliaryEffectSlots(4, s_slot);
    assert(alGetError() == AL_NO_ERROR && s_slot[0] == 1u && s_slot[3] == 4u);
    test_budgets();
    test_routing();
    test_onchip();
    alDeleteAuxiliaryEffectSlots(4, s_slot);
    alDeleteEffects(6, s_fx);
    assert(alGetError() == AL_NO_ERROR);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("=== Several effects at once: all passed ===\n");
    return 0;
}
