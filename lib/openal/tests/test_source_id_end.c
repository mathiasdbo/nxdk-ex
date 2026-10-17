/*
 * The per-frame tick walks the sources only up to the highest one in use
 * (al_source_id_end), not all AL_MAX_SOURCES: an engine with one streaming
 * source paid for 256 slots twice an update. The bound follows generation
 * and deletion, and a source beyond a deleted one still gets its tick.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"
#include "al_source.h"

#define MOCK_MMIO_SIZE 0x30000

int main(void) {
    uint8_t *mock = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint s[3], t;

    printf("=== al_source_id_end: the tick's bound ===\n");
    assert(mock);
    alc_set_apu_base((uintptr_t)mock);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));

    printf("[1] none, then three: the highest name...\n");
    assert(al_source_id_end() == 0);
    alGenSources(3, s);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_id_end() == s[2] && s[2] == 3);
    printf("    -> [PASS]\n");

    printf("[2] deleting a middle source keeps it; the highest lowers it...\n");
    alDeleteSources(1, &s[1]);
    assert(al_source_id_end() == 3);
    alDeleteSources(1, &s[2]);
    assert(al_source_id_end() == 1);
    printf("    -> [PASS]\n");

    printf("[3] a new source takes the free low slot; the bound follows...\n");
    alGenSources(1, &t);
    assert(t == 2 && al_source_id_end() == 2);
    alXboxUpdateVoices();
    assert(alGetError() == AL_NO_ERROR);
    printf("    -> [PASS]\n");

    printf("[4] all deleted, then a device reopen: none...\n");
    alDeleteSources(1, &s[0]);
    alDeleteSources(1, &t);
    assert(al_source_id_end() == 0);
    alGenSources(1, &t);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    dev = alcOpenDevice(NULL);
    assert(dev);
    ctx = alcCreateContext(dev, NULL);
    assert(ctx && alcMakeContextCurrent(ctx));
    alGenSources(1, &t);
    assert(t == 1 && al_source_id_end() == 1);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    printf("    -> [PASS]\n");

    free(mock);
    printf("=== All al_source_id_end Tests Passed! ===\n");
    return 0;
}
