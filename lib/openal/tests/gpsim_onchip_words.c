/*
 * The parameter words the library sends a slot's GP kernel for an EFX effect
 * (al_efx.c), for tests/test_gp_onchip.py: it checks that the words its
 * floating-point models and the kernels run on are the library's own.
 *
 *   gpsim_onchip_words TYPE [f:PARAM:VALUE | i:PARAM:VALUE | g:SLOT_GAIN]...
 *
 * TYPE and PARAM in decimal or 0x hex. Prints the 32 words in hex, one line.
 */
#define AL_ALEXT_PROTOTYPES
#include <stdio.h>
#include <stdlib.h>

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include <AL/efx.h>

#include "al_efx.h"
#include "al_source.h"

int main(int argc, char **argv) {
    ALCdevice *dev = alcOpenDevice(NULL);
    ALCcontext *ctx = dev ? alcCreateContext(dev, NULL) : NULL;
    ALuint e = 0, slot = 0;
    apu_gp_kernel_t k;
    const uint32_t *p = NULL;
    int i;

    if (argc < 2 || !ctx || !alcMakeContextCurrent(ctx)) {
        fprintf(stderr, "usage: gpsim_onchip_words TYPE [f:PARAM:VALUE | i:PARAM:VALUE | g:GAIN]...\n");
        return 2;
    }
    alGenEffects(1, &e);
    alGenAuxiliaryEffectSlots(1, &slot);
    alEffecti(e, AL_EFFECT_TYPE, (ALint)strtol(argv[1], NULL, 0));
    for (i = 2; i < argc; i++) {
        char *rest = argv[i] + 2;
        if (argv[i][0] == 'g') {
            alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, (ALfloat)strtod(rest, NULL));
        } else {
            ALenum param = (ALenum)strtol(rest, &rest, 0);
            if (argv[i][0] == 'i') {
                alEffecti(e, param, (ALint)strtol(rest + 1, NULL, 0));
            } else {
                alEffectf(e, param, (ALfloat)strtod(rest + 1, NULL));
            }
        }
        if (alGetError() != AL_NO_ERROR) {
            fprintf(stderr, "gpsim_onchip_words: %s refused\n", argv[i]);
            return 1;
        }
    }
    alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)e);
    if (alGetError() != AL_NO_ERROR || !al_source_debug_gp_kernel(&k, &p)) {
        fprintf(stderr, "gpsim_onchip_words: no kernel\n");
        return 1;
    }
    for (i = 0; i < (int)AL_EFX_GP_PARAMS; i++) {
        printf("%06x%c", (unsigned)p[i], (i + 1 < (int)AL_EFX_GP_PARAMS) ? ' ' : '\n');
    }
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    return 0;
}
