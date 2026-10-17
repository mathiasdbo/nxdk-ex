/*
 * ALXBOXSTATS' voice tick (update_calls, update_cycles, update_max_cycles) and
 * its split (update_stream_cycles, update_voice_cycles,
 * update_service_cycles) runs from alcOpenDevice like the other counters: a
 * device reopen starts it at zero, not at the last device's totals.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"

#define MOCK_MMIO_SIZE 0x30000

static ALXBOXSTATS stats(void) {
    ALXBOXSTATS s;
    memset(&s, 0, sizeof(s));
    s.size = sizeof(s);
    alXboxGetStats(&s);
    return s;
}

static void open_ctx(ALCdevice **dev, ALCcontext **ctx) {
    *dev = alcOpenDevice(NULL);
    assert(*dev);
    *ctx = alcCreateContext(*dev, NULL);
    assert(*ctx && alcMakeContextCurrent(*ctx));
}

static void close_ctx(ALCdevice *dev, ALCcontext *ctx) {
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
}

int main(void) {
    uint8_t *mock = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    ALCdevice *dev;
    ALCcontext *ctx;
    ALuint src;
    ALXBOXSTATS s;
    int i;

    printf("=== ALXBOXSTATS: the voice tick's split per device ===\n");
    assert(mock);
    alc_set_apu_base((uintptr_t)mock);

    printf("[1] ticks with a source accumulate the split...\n");
    open_ctx(&dev, &ctx);
    alGenSources(1, &src);
    for (i = 0; i < 16; i++) {
        alXboxUpdateVoices();
    }
    s = stats();
#if defined(__i386__) || defined(__x86_64__)
    assert(s.update_stream_cycles + s.update_voice_cycles > 0);
#endif
    assert(s.update_stream_cycles + s.update_voice_cycles <= s.update_cycles);
    printf("    -> [PASS]\n");

    printf("[2] a reopened device starts the split at zero...\n");
    alDeleteSources(1, &src);
    close_ctx(dev, ctx);
    open_ctx(&dev, &ctx);
    s = stats();
    assert(s.update_calls == 0 && s.update_cycles == 0 && s.update_max_cycles == 0);
    assert(s.update_stream_cycles == 0 && s.update_voice_cycles == 0 && s.update_service_cycles == 0);
    close_ctx(dev, ctx);
    printf("    -> [PASS]\n");

    free(mock);
    printf("=== All tick stats Tests Passed! ===\n");
    return 0;
}
