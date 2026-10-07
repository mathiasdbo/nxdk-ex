#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"
#include "al_source.h"
#include "apu_voice.h"
#include "apu_hardware.h"

#define MOCK_MMIO_SIZE 0x80000
#define BAR0_SIZE      0x80000u

/*
 * Backend guard: by default (no alc_set_apu_base mock, no -DOPENAL_APU_REAL_MMIO)
 * the device must run on a RAM stand-in and never touch the real BAR0
 * (NV_PAPU_BASE = 0xFE800000), which does not exist on the host: any access
 * would fault. Surviving the default-base sections below therefore proves
 * that no real MMIO is accessed.
 */

/* True if any 32-bit word of the register space was written (model-agnostic bring-up check) */
static int region_has_nonzero(uintptr_t base, uint32_t size) {
    uint32_t off;
    for (off = 0; off < size; off += 4u) {
        if (apu_read32(base, off) != 0u) {
            return 1;
        }
    }
    return 0;
}

static ALint backend_query(void) {
    ALint v = -1;
    alXboxGetHardwareStatus(AL_XBOX_BACKEND, &v);
    assert(alGetError() == AL_NO_ERROR);
    return v;
}

static void check_renderer_matches_backend(void) {
    const ALchar *r = alGetString(AL_RENDERER);
    assert(r != NULL);
    assert(strncmp(r, "MCPX APU", 8) == 0);
    if (backend_query() == AL_XBOX_BACKEND_MMIO) {
        assert(strstr(r, "MMIO") != NULL);
        assert(strstr(r, "null") == NULL);
    } else {
        assert(strstr(r, "null") != NULL);
        assert(strstr(r, "MMIO") == NULL);
    }
}

/* Context, buffer, source, playback and frame tick on an already open device */
static void exercise_device(ALCdevice *dev) {
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    ALuint buf = 0, src = 0;
    int16_t pcm[256] = {0};
    ALint state = 0;

    assert(ctx != NULL);
    assert(alcMakeContextCurrent(ctx) == ALC_TRUE);

    alGenBuffers(1, &buf);
    assert(buf != 0);
    alBufferData(buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    assert(alGetError() == AL_NO_ERROR);

    alGenSources(1, &src);
    assert(src != 0);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    alSourcePlay(src);
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);

    /* The voice trigger landed in the device's register space (RAM stand-in or mock) */
    {
        ALsource *s = al_source_get(src);
        assert(s != NULL);
        assert(s->hw_voice_idx >= 0);
        assert(apu_voice_is_active(dev->apu_base, (uint32_t)s->hw_voice_idx) == 1);
    }

    alcProcessContext(ctx);
    alXboxUpdateVoices();
    assert(alGetError() == AL_NO_ERROR);
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING); /* looping source survives the tick */

    alSourceStop(src);
    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    assert(alcMakeContextCurrent(NULL) == ALC_TRUE);
    alcDestroyContext(ctx);
    assert(alGetError() == AL_NO_ERROR);
}

#ifndef OPENAL_APU_REAL_MMIO
/*
 * Buffer, source, playback and state queries with no device open. The source
 * subsystem has no session base then, so its voice control must not fall back
 * to the real BAR0: any access to 0xFE800000 would fault on the host.
 */
static void play_without_device(void) {
    ALuint buf = 0, src = 0;
    int16_t pcm[256] = {0};
    ALint state = 0;

    assert(al_source_get_apu_base() != NV_PAPU_BASE);

    alGenBuffers(1, &buf);
    assert(buf != 0);
    alBufferData(buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 48000);
    alGenSources(1, &src);
    assert(src != 0);
    alSourcei(src, AL_BUFFER, (ALint)buf);
    alSourcei(src, AL_LOOPING, AL_TRUE);
    assert(alGetError() == AL_NO_ERROR);

    alSourcePlay(src);
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);
    alSourcePause(src);
    alSourcePlay(src);  /* unpause path */
    alSourcePlay(src);  /* restart path (stops the voice first) */
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_PLAYING);
    alSourceStop(src);
    alGetSourcei(src, AL_SOURCE_STATE, &state);
    assert(state == AL_STOPPED);
    assert(alGetError() == AL_NO_ERROR);

    alDeleteSources(1, &src);
    alDeleteBuffers(1, &buf);
    assert(alGetError() == AL_NO_ERROR);
    assert(al_source_get_apu_base() != NV_PAPU_BASE);
}
#endif

int main(void) {
    printf("=== OpenAL Backend Guard (null/software-model backend) Test ===\n");

    /* ------------------------------------------------------------------ */
    printf("[1] status queries with no device open touch no MMIO...\n");
    {
        /* Default base on purpose: reading 0xFE801004 / 0xFE803000 would fault on the host */
        ALint v = -1;

        alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &v);
        assert(v == 0);
        v = -1;
        alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &v);
        assert(v == 0);
        assert(alGetError() == AL_NO_ERROR);

        v = -1;
        alXboxGetHardwareStatus(AL_XBOX_HW_VOICE_COUNT, &v);
        assert(v == 64);
        v = -1;
        alXboxGetHardwareStatus(AL_XBOX_AV_PACK_TYPE, &v);
        assert(v >= 0);

        /* No device, no backend: null in every build (also with OPENAL_APU_REAL_MMIO) */
        assert(backend_query() == AL_XBOX_BACKEND_NULL);
        check_renderer_matches_backend();
        assert(strstr(alGetString(AL_RENDERER), "null") != NULL);

        /* The frame tick is a no-op without a device (no stale/default base access) */
        alXboxUpdateVoices();
        assert(alGetError() == AL_NO_ERROR);
        assert(alc_get_apu_base() == NV_PAPU_BASE);

#ifndef OPENAL_APU_REAL_MMIO
        /* Before the first alcOpenDevice() */
        play_without_device();
#endif
    }

    /* ------------------------------------------------------------------ */
    printf("[2] mock base: null backend, everything works...\n");
    {
        uint8_t *mock = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
        ALCdevice *dev;
        assert(mock != NULL);
        alc_set_apu_base((uintptr_t)mock);

        dev = alcOpenDevice(NULL);
        assert(dev != NULL);
        assert(dev->apu_base == (uintptr_t)mock);
        assert(alc_get_apu_base() == (uintptr_t)mock);
        assert(backend_query() == AL_XBOX_BACKEND_NULL);
        check_renderer_matches_backend();
        assert(strstr(alGetString(AL_RENDERER), "null") != NULL);

        /* Bring-up went to the mock */
        assert(region_has_nonzero((uintptr_t)mock, MOCK_MMIO_SIZE));

        exercise_device(dev);

        {
            ALint v = 0;
            alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &v);
            assert((uint32_t)v == dev->voice_table_phys && v != 0);
        }

        assert(alcCloseDevice(dev) == ALC_TRUE);
        assert(alc_get_apu_base() == (uintptr_t)mock);

        /* Closed: cached queries fall back to 0 */
        {
            ALint v = -1;
            alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &v);
            assert(v == 0);
            v = -1;
            alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &v);
            assert(v == 0);
        }
        alc_set_apu_base(0);
        free(mock);

        /* The source subsystem may still hold the freed mock: the tick must not use it */
        alXboxUpdateVoices();
        assert(alGetError() == AL_NO_ERROR);
    }

    /* ------------------------------------------------------------------ */
#ifdef OPENAL_APU_REAL_MMIO
    printf("[3] default base: skipped (built with OPENAL_APU_REAL_MMIO, no MMIO on the host)\n");
#else
    printf("[3] default base: opens on a RAM stand-in, never the real BAR0...\n");
    {
        ALCdevice *dev;
        uintptr_t base;
        int cycle;

        assert(alc_get_apu_base() == NV_PAPU_BASE);
        dev = alcOpenDevice(NULL); /* would fault on the host if it wrote to 0xFE800000 */
        assert(dev != NULL);
        assert(alcGetError(dev) == ALC_NO_ERROR);

        base = dev->apu_base;
        assert(base != 0);
        assert(base != NV_PAPU_BASE);
        assert(alc_get_apu_base() == base);
        assert(al_source_get_apu_base() == base);
        assert(backend_query() == AL_XBOX_BACKEND_NULL);
        check_renderer_matches_backend();
        assert(strncmp(alGetString(AL_RENDERER), "MCPX APU", 8) == 0);

        /* Zeroed, BAR0-sized region: the whole range is addressable (ASan/valgrind checks bounds) */
        apu_write32(base, BAR0_SIZE - 4u, 0xA5A5A5A5u);
        assert(apu_read32(base, BAR0_SIZE - 4u) == 0xA5A5A5A5u);
        apu_write32(base, BAR0_SIZE - 4u, 0u);

        /* Bring-up programmed the stand-in */
        assert(region_has_nonzero(base, BAR0_SIZE));

        {
            ALint v = -1;
            alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &v);
            assert((uint32_t)v == dev->voice_table_phys && v != 0);
        }

        exercise_device(dev);

        /* A second device shares the session (and its stand-in) */
        {
            ALCdevice *dev2 = alcOpenDevice(NULL);
            assert(dev2 != NULL);
            assert(dev2->apu_base == base);
            assert(alcCloseDevice(dev) == ALC_TRUE);
            assert(alc_get_apu_base() == base); /* still open through dev2 */
            exercise_device(dev2);
            assert(alcCloseDevice(dev2) == ALC_TRUE);
        }

        /* Last close released the stand-in and reset the configured base view */
        assert(alc_get_apu_base() == NV_PAPU_BASE);
        assert(al_source_get_apu_base() != NV_PAPU_BASE); /* parked, not the real BAR0 */
        assert(backend_query() == AL_XBOX_BACKEND_NULL);
        {
            ALint v = -1;
            alXboxGetHardwareStatus(AL_XBOX_VP_BASE_PHYS, &v);
            assert(v == 0);
            alXboxUpdateVoices(); /* still a no-op */
        }

        /* After the last alcCloseDevice() */
        play_without_device();

        /* Repeated open/close cycles (run under valgrind/ASan to check for leaks) */
        for (cycle = 0; cycle < 20; cycle++) {
            ALCdevice *d = alcOpenDevice(NULL);
            assert(d != NULL);
            assert(d->apu_base != NV_PAPU_BASE);
            if ((cycle & 3) == 0) {
                exercise_device(d);
            }
            assert(alcCloseDevice(d) == ALC_TRUE);
        }
        assert(alGetError() == AL_NO_ERROR);
    }
#endif

    printf("=== All Backend Guard Tests Passed Successfully! ===\n");
    return 0;
}
