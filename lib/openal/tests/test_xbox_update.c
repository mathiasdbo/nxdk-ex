#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "alc_context.h"
#include "al_source.h"
#include "al_buffer.h"
#include "apu_voice.h"
#include "apu_voice_mgr.h"
#include "apu_hardware.h"

#define MOCK_MMIO_SIZE 0x80000

/* Simulate hardware completion of a voice: clear its ACTIVE bit in the mock MMIO */
static void mock_hw_voice_finished(uint8_t *mock_mmio, int voice) {
    uint32_t reg = (voice < 32) ? NV_PAPU_VP_ACTIVE_0 : NV_PAPU_VP_ACTIVE_1;
    uint32_t bit = 1u << ((uint32_t)voice & 31u);
    uintptr_t base = (uintptr_t)mock_mmio;
    apu_write32(base, reg, apu_read32(base, reg) & ~bit);
}

static ALuint make_buffer(void) {
    ALuint buf = 0;
    int16_t pcm[256] = {0};
    alGenBuffers(1, &buf);
    assert(buf != 0);
    alBufferData(buf, AL_FORMAT_MONO16, pcm, (ALsizei)sizeof(pcm), 44100);
    assert(alGetError() == AL_NO_ERROR);
    return buf;
}

/* Raw state read: alGetSourcei(AL_SOURCE_STATE) would reap on its own and hide the tick */
static ALint raw_state(ALuint id) {
    ALsource *s = al_source_get(id);
    assert(s != NULL);
    return s->state;
}

static int raw_voice(ALuint id) {
    ALsource *s = al_source_get(id);
    assert(s != NULL);
    return s->hw_voice_idx;
}

int main(void) {
    printf("=== AL_XBOX_update: alXboxUpdateVoices / alcProcessContext Frame Tick Test ===\n");

    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    ALCdevice *dev = alcOpenDevice(NULL);
    assert(dev != NULL);
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    assert(ctx != NULL);
    assert(alcMakeContextCurrent(ctx) == ALC_TRUE);

    ALuint buf = make_buffer();

    /* ---------------------------------------------------------------------- */
    printf("[1] alXboxUpdateVoices reaps a finished one-shot source...\n");
    {
        ALuint src = 0;
        alGenSources(1, &src);
        alSourcei(src, AL_BUFFER, (ALint)buf);
        alSourcei(src, AL_LOOPING, AL_FALSE);
        alSourcePlay(src);
        assert(alGetError() == AL_NO_ERROR);

        int v = raw_voice(src);
        assert(v >= 0);
        assert(raw_state(src) == AL_PLAYING);
        assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)v) == 1);

        mock_hw_voice_finished(mock_mmio, v);
        assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)v) == 0);

        /* Nothing has ticked yet: the library still believes the source is playing */
        assert(raw_state(src) == AL_PLAYING);
        assert(apu_voice_mgr_get_owner((uint32_t)v) == (int)src);

        alXboxUpdateVoices();
        assert(raw_state(src) == AL_STOPPED);
        assert(raw_voice(src) == AL_HW_VOICE_INVALID);
        assert(apu_voice_mgr_get_owner((uint32_t)v) == AL_HW_VOICE_INVALID);

        ALint st = 0;
        alGetSourcei(src, AL_SOURCE_STATE, &st);
        assert(st == AL_STOPPED);
        assert(alGetError() == AL_NO_ERROR);
        alDeleteSources(1, &src);
    }

    /* ---------------------------------------------------------------------- */
    printf("[2] alcProcessContext performs the same tick...\n");
    {
        ALuint src = 0;
        alGenSources(1, &src);
        alSourcei(src, AL_BUFFER, (ALint)buf);
        alSourcePlay(src);
        int v = raw_voice(src);
        assert(v >= 0);

        mock_hw_voice_finished(mock_mmio, v);
        assert(raw_state(src) == AL_PLAYING);

        alcProcessContext(ctx);
        assert(alcGetError(dev) == ALC_NO_ERROR);
        assert(raw_state(src) == AL_STOPPED);
        assert(raw_voice(src) == AL_HW_VOICE_INVALID);
        assert(apu_voice_mgr_get_owner((uint32_t)v) == AL_HW_VOICE_INVALID);
        alDeleteSources(1, &src);

        /* An invalid context is an ALC error and must not tick */
        alcProcessContext(NULL);
        assert(alcGetError(NULL) == ALC_INVALID_CONTEXT);
    }

    /* ---------------------------------------------------------------------- */
    printf("[3] a looping source is never reaped...\n");
    {
        ALuint src = 0;
        alGenSources(1, &src);
        alSourcei(src, AL_BUFFER, (ALint)buf);
        alSourcei(src, AL_LOOPING, AL_TRUE);
        alSourcePlay(src);
        int v = raw_voice(src);
        assert(v >= 0);

        mock_hw_voice_finished(mock_mmio, v);
        alXboxUpdateVoices();
        alcProcessContext(ctx);

        assert(raw_state(src) == AL_PLAYING);
        assert(raw_voice(src) == v);
        assert(apu_voice_mgr_get_owner((uint32_t)v) == (int)src);
        alSourceStop(src);
        alDeleteSources(1, &src);
    }

    /* ---------------------------------------------------------------------- */
    printf("[4] standby promotion into a slot freed by a finished voice...\n");
    {
        ALuint hw[NV_PAPU_NUM_3D_VOICES];
        ALuint standby = 0;
        int i;

        alGenSources((ALsizei)NV_PAPU_NUM_3D_VOICES, hw);
        for (i = 0; i < (int)NV_PAPU_NUM_3D_VOICES; i++) {
            alSourcei(hw[i], AL_BUFFER, (ALint)buf);
            alSource3f(hw[i], AL_POSITION, 0.0f, 0.0f, 1.0f + 0.1f * (float)i);
            alSourcei(hw[i], AL_LOOPING, AL_FALSE);
            alSourcePlay(hw[i]);
            assert(raw_voice(hw[i]) >= 0);
        }
        assert(apu_voice_mgr_get_active_hw_count() == NV_PAPU_NUM_3D_VOICES);

        /* 65th, lower-priority source (distant, quiet) is virtualized in standby */
        alGenSources(1, &standby);
        alSourcei(standby, AL_BUFFER, (ALint)buf);
        alSourcef(standby, AL_GAIN, 0.05f);
        alSource3f(standby, AL_POSITION, 0.0f, 0.0f, 50.0f);
        alSourcei(standby, AL_LOOPING, AL_FALSE);
        alSourcePlay(standby);
        assert(raw_state(standby) == AL_PLAYING);
        assert(raw_voice(standby) == AL_HW_VOICE_INVALID);
        assert(apu_voice_mgr_get_virtual_standby_count() == 1);

        /* A tick with every voice still active changes nothing */
        alXboxUpdateVoices();
        assert(raw_voice(standby) == AL_HW_VOICE_INVALID);
        assert(apu_voice_mgr_get_virtual_standby_count() == 1);

        /* One hardware voice finishes; the tick frees its slot and promotes the standby source */
        int slot = raw_voice(hw[10]);
        assert(slot >= 0);
        mock_hw_voice_finished(mock_mmio, slot);

        alXboxUpdateVoices();
        assert(raw_state(hw[10]) == AL_STOPPED);
        assert(raw_voice(hw[10]) == AL_HW_VOICE_INVALID);
        assert(raw_state(standby) == AL_PLAYING);
        assert(raw_voice(standby) == slot);
        assert(apu_voice_mgr_get_owner((uint32_t)slot) == (int)standby);
        assert(apu_voice_is_active((uintptr_t)mock_mmio, (uint32_t)slot) == 1);
        assert(apu_voice_mgr_get_active_hw_count() == NV_PAPU_NUM_3D_VOICES);
        assert(apu_voice_mgr_get_virtual_standby_count() == 0);

        alDeleteSources((ALsizei)NV_PAPU_NUM_3D_VOICES, hw);
        alDeleteSources(1, &standby);
        assert(apu_voice_mgr_get_active_hw_count() == 0);
    }

    /* ---------------------------------------------------------------------- */
    printf("[5] proc address resolution...\n");
    {
        void *p_al = alGetProcAddress("alXboxUpdateVoices");
        void *p_alc = alcGetProcAddress(dev, "alXboxUpdateVoices");
        assert(p_al != NULL);
        assert(p_al == p_alc);
        assert(alIsExtensionPresent("AL_XBOX_update") == AL_TRUE);
        assert(strstr(alGetString(AL_EXTENSIONS), "AL_XBOX_update") != NULL);
    }

    alDeleteBuffers(1, &buf);
    assert(alcMakeContextCurrent(NULL) == ALC_TRUE);
    alcDestroyContext(ctx);
    assert(alcCloseDevice(dev) == ALC_TRUE);

    /* With no device open the tick is a no-op: it must not touch the (now freed) mock base */
    free(mock_mmio);
    alXboxUpdateVoices();
    assert(alGetError() == AL_NO_ERROR);

    printf("=== All AL_XBOX_update Tests Passed Successfully! ===\n");
    return 0;
}
