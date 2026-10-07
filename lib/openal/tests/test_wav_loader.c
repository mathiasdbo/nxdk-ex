#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <AL/al.h>
#include <AL/alc.h>
#include "alc_context.h"
#include "../../samples/openal_showcase/wav_loader.h"
#include "../../samples/openal_showcase/sound_assets.h"

#define MOCK_MMIO_SIZE 0x30000

int main(void) {
    printf("=== OpenAL WAV Loader & Asset Pipeline Unit Test ===\n");

    /* Initialize Mock APU MMIO */
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    ALCdevice *dev = alcOpenDevice(NULL);
    assert(dev != NULL);
    ALCcontext *ctx = alcCreateContext(dev, NULL);
    assert(ctx != NULL);
    alcMakeContextCurrent(ctx);

    /* 1. Validate Parsing of all 6 Standardized Sound Assets */
    printf("[1] Verifying WAV parsing for all 6 generated audio assets...\n");

    wav_info_t info;

    /* Orbit Mono 48k */
    int res = wav_parse(g_sound_orbit_mono_wav, sizeof(g_sound_orbit_mono_wav), &info);
    assert(res == 0);
    assert(info.format == AL_FORMAT_MONO16);
    assert(info.channels == 1);
    assert(info.sample_rate == 48000);
    assert(info.bits_per_sample == 16);
    assert(info.pcm_size == 96000);
    printf("    -> orbit_mono_48k: format=MONO16, rate=%d, size=%d bytes [PASS]\n",
           info.sample_rate, info.pcm_size);

    /* Siren Doppler 48k */
    res = wav_parse(g_sound_siren_doppler_wav, sizeof(g_sound_siren_doppler_wav), &info);
    assert(res == 0);
    assert(info.format == AL_FORMAT_MONO16);
    assert(info.channels == 1);
    assert(info.sample_rate == 48000);
    assert(info.bits_per_sample == 16);
    assert(info.pcm_size == 96000);
    printf("    -> siren_doppler_48k: format=MONO16, rate=%d, size=%d bytes [PASS]\n",
           info.sample_rate, info.pcm_size);

    /* Explosion LFE 48k */
    res = wav_parse(g_sound_explosion_lfe_wav, sizeof(g_sound_explosion_lfe_wav), &info);
    assert(res == 0);
    assert(info.format == AL_FORMAT_MONO16);
    assert(info.channels == 1);
    assert(info.sample_rate == 48000);
    assert(info.bits_per_sample == 16);
    assert(info.pcm_size == 96000);
    printf("    -> explosion_lfe_48k: format=MONO16, rate=%d, size=%d bytes [PASS]\n",
           info.sample_rate, info.pcm_size);

    /* Laser Stress 48k */
    res = wav_parse(g_sound_laser_stress_wav, sizeof(g_sound_laser_stress_wav), &info);
    assert(res == 0);
    assert(info.format == AL_FORMAT_MONO16);
    assert(info.channels == 1);
    assert(info.sample_rate == 48000);
    assert(info.bits_per_sample == 16);
    assert(info.pcm_size == 11520);
    printf("    -> laser_stress_48k: format=MONO16, rate=%d, size=%d bytes [PASS]\n",
           info.sample_rate, info.pcm_size);

    /* Voice Center 48k */
    res = wav_parse(g_sound_voice_center_wav, sizeof(g_sound_voice_center_wav), &info);
    assert(res == 0);
    assert(info.format == AL_FORMAT_MONO16);
    assert(info.channels == 1);
    assert(info.sample_rate == 48000);
    assert(info.bits_per_sample == 16);
    assert(info.pcm_size == 96000);
    printf("    -> voice_center_48k: format=MONO16, rate=%d, size=%d bytes [PASS]\n",
           info.sample_rate, info.pcm_size);

    /* BGM Stereo 48k */
    res = wav_parse(g_sound_bgm_stereo_wav, sizeof(g_sound_bgm_stereo_wav), &info);
    assert(res == 0);
    assert(info.format == AL_FORMAT_STEREO16);
    assert(info.channels == 2);
    assert(info.sample_rate == 48000);
    assert(info.bits_per_sample == 16);
    assert(info.pcm_size == 384000);
    printf("    -> bgm_stereo_48k: format=STEREO16, rate=%d, size=%d bytes [PASS]\n",
           info.sample_rate, info.pcm_size);

    /* 2. Validate OpenAL Buffer Upload via wav_load_to_buffer */
    printf("[2] Verifying OpenAL buffer loading via wav_load_to_buffer...\n");
    ALuint al_bufs[6];
    alGenBuffers(6, al_bufs);
    assert(alGetError() == AL_NO_ERROR);

    assert(wav_load_to_buffer(al_bufs[0], g_sound_orbit_mono_wav, sizeof(g_sound_orbit_mono_wav)) == 0);
    assert(wav_load_to_buffer(al_bufs[1], g_sound_siren_doppler_wav, sizeof(g_sound_siren_doppler_wav)) == 0);
    assert(wav_load_to_buffer(al_bufs[2], g_sound_explosion_lfe_wav, sizeof(g_sound_explosion_lfe_wav)) == 0);
    assert(wav_load_to_buffer(al_bufs[3], g_sound_laser_stress_wav, sizeof(g_sound_laser_stress_wav)) == 0);
    assert(wav_load_to_buffer(al_bufs[4], g_sound_voice_center_wav, sizeof(g_sound_voice_center_wav)) == 0);
    assert(wav_load_to_buffer(al_bufs[5], g_sound_bgm_stereo_wav, sizeof(g_sound_bgm_stereo_wav)) == 0);

    /* Check buffer attributes */
    ALint channels = 0, bits = 0, size_bytes = 0, freq = 0;
    alGetBufferi(al_bufs[0], AL_CHANNELS, &channels);
    alGetBufferi(al_bufs[0], AL_BITS, &bits);
    alGetBufferi(al_bufs[0], AL_SIZE, &size_bytes);
    alGetBufferi(al_bufs[0], AL_FREQUENCY, &freq);
    assert(channels == 1 && bits == 16 && size_bytes == 96000 && freq == 48000);

    alGetBufferi(al_bufs[5], AL_CHANNELS, &channels);
    alGetBufferi(al_bufs[5], AL_BITS, &bits);
    alGetBufferi(al_bufs[5], AL_SIZE, &size_bytes);
    alGetBufferi(al_bufs[5], AL_FREQUENCY, &freq);
    assert(channels == 2 && bits == 16 && size_bytes == 384000 && freq == 48000);
    printf("    -> OpenAL buffer queries verified for mono & stereo buffers [PASS]\n");

    /* 3. Error Case Handling */
    printf("[3] Verifying error handling for corrupt and invalid inputs...\n");
    assert(wav_parse(NULL, 100, &info) == -1);
    assert(wav_parse(g_sound_orbit_mono_wav, 20, &info) == -1);

    /* Corrupt Magic */
    uint8_t bad_hdr[44];
    memcpy(bad_hdr, g_sound_orbit_mono_wav, 44);
    bad_hdr[0] = 'B'; bad_hdr[1] = 'A'; bad_hdr[2] = 'D'; bad_hdr[3] = '!';
    assert(wav_parse(bad_hdr, sizeof(bad_hdr), &info) == -2);

    /* Non-PCM format (AudioFormat = 3 IEEE float) */
    memcpy(bad_hdr, g_sound_orbit_mono_wav, 44);
    bad_hdr[20] = 3; bad_hdr[21] = 0;
    assert(wav_parse(bad_hdr, sizeof(bad_hdr), &info) == -4);
    printf("    -> All parser guard checks verified [PASS]\n");

    /* Teardown */
    alDeleteBuffers(6, al_bufs);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(dev);
    free(mock_mmio);

    printf("=== All WAV Loader and Audio Asset Pipeline Tests Passed! ===\n");
    return 0;
}
