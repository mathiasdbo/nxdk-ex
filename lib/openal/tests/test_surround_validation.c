#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#include "apu_spatial.h"
#include "al_source.h"
#include "al_listener.h"
#include "al_buffer.h"
#include "alc_context.h"
#include "apu_voice.h"
#include "apu_hardware.h"
#include "apu_eeprom.h"
#include "apu_ep.h"

#define MOCK_MMIO_SIZE 0x40000

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Xbox OpenAL 1.1 Multichannel 5.1 Surround Validation Suite ===\n");

    /*
     * ------------------------------------------------------------------------
     * Environment Setup: Simulated APU MMIO page & Mock EEPROM/SMBus
     * ------------------------------------------------------------------------
     */
    uint8_t *mock_mmio = (uint8_t *)calloc(1, MOCK_MMIO_SIZE);
    assert(mock_mmio != NULL);
    alc_set_apu_base((uintptr_t)mock_mmio);

    /* Configure HDTV Component Pack with Dolby Digital AC-3 enabled */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_HDTV);

    /* Initialize ALC Device & Context in Surround 5.1 mode */
    ALCdevice *device = alcOpenDevice(NULL);
    assert(device != NULL);
    assert(device->is_open);
    assert(device->topology == APU_TOPOLOGY_SURROUND_51);

    ALCcontext *ctx = alcCreateContext(device, NULL);
    assert(ctx != NULL);
    assert(alcMakeContextCurrent(ctx) == ALC_TRUE);

    /* Verify listener initialized at origin */
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const ALfloat listener_ori[6] = { 0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f };
    alListenerfv(AL_ORIENTATION, listener_ori);
    alListenerf(AL_GAIN, 1.0f);

    /* Create sample mono PCM buffer (48 kHz, 256 samples) */
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    assert(alGetError() == AL_NO_ERROR);
    int16_t pcm_data[256];
    for (size_t i = 0; i < 256; ++i) {
        pcm_data[i] = (int16_t)(sin((double)i * 0.1) * 20000.0);
    }
    alBufferData(buffer, AL_FORMAT_MONO16, pcm_data, (ALsizei)sizeof(pcm_data), 48000);
    assert(alGetError() == AL_NO_ERROR);

    /* Create OpenAL source */
    ALuint source = 0;
    alGenSources(1, &source);
    assert(alGetError() == AL_NO_ERROR);
    alSourcei(source, AL_BUFFER, (ALint)buffer);
    alSourcei(source, AL_LOOPING, AL_TRUE);
    alSourcef(source, AL_REFERENCE_DISTANCE, 2.0f);
    alSourcef(source, AL_MAX_DISTANCE, 25.0f);
    alSourcef(source, AL_ROLLOFF_FACTOR, 1.0f);

    /*
     * ------------------------------------------------------------------------
     * Test 1: Validate Discrete Channel Isolation for all 5 Satellite Speakers
     * ------------------------------------------------------------------------
     * Speaker Geometry (ITU-R BS.775):
     *   - Center (0 deg):          Pos (0.0f, 0.0f, -5.0f)     -> MixBin 4 >= 254, others <= 1
     *   - Front Right (30 deg):    Pos (2.887f, 0.0f, -5.0f)   -> MixBin 1 >= 254, others <= 1
     *   - Surround Right (110 deg):Pos (4.698f, 0.0f, 1.710f)  -> MixBin 3 >= 254, others <= 1
     *   - Surround Left (250 deg): Pos (-4.698f, 0.0f, 1.710f) -> MixBin 2 >= 254, others <= 1
     *   - Front Left (330 deg):    Pos (-2.887f, 0.0f, -5.0f)  -> MixBin 0 >= 254, others <= 1
     *
     * QA Acceptance: >99% energy in designated MixBin, <1% in other bins.
     * ------------------------------------------------------------------------
     */
    printf("[1] Testing Discrete Channel Isolation across all 5 satellite positions...\n");

    struct {
        const char *name;
        int target_bin;
        float x;
        float y;
        float z;
    } satellite_positions[5] = {
        { "Center (0 deg)",          4,  0.0f,   0.0f, -5.0f },
        { "Front Right (30 deg)",    1,  2.887f, 0.0f, -5.0f },
        { "Surround Right (110 deg)",3,  4.698f, 0.0f,  1.710f },
        { "Surround Left (250 deg)", 2, -4.698f, 0.0f,  1.710f },
        { "Front Left (330 deg)",    0, -2.887f, 0.0f, -5.0f }
    };

    /* Start source playback */
    alSourcef(source, AL_XBOX_LFE_GAIN, 0.0f);
    alSourcePlay(source);
    assert(alGetError() == AL_NO_ERROR);

    ALsource *src_obj = al_source_get(source);
    assert(src_obj != NULL && src_obj->hw_voice_idx >= 0);
    NVAPU_VOICE_CONTEXT_3D *vctx = apu_voice_get_context((uint32_t)src_obj->hw_voice_idx);
    assert(vctx != NULL);

    for (int p = 0; p < 5; ++p) {
        int target = satellite_positions[p].target_bin;
        alSource3f(source, AL_POSITION,
                   satellite_positions[p].x,
                   satellite_positions[p].y,
                   satellite_positions[p].z);
        assert(alGetError() == AL_NO_ERROR);

        /* Validate Voice Context hardware mixbin gain multipliers */
        assert(vctx->mixbin_gain[target] >= 254);

        float total_energy = 0.0f;
        for (int b = 0; b < 6; ++b) {
            float g = (float)vctx->mixbin_gain[b] / 255.0f;
            total_energy += g * g;
            if (b != target) {
                assert(vctx->mixbin_gain[b] <= 1);
            }
        }

        /* Verify >99% energy concentration in target channel */
        float target_energy = ((float)vctx->mixbin_gain[target] / 255.0f) * ((float)vctx->mixbin_gain[target] / 255.0f);
        float energy_ratio = (total_energy > 1e-6f) ? (target_energy / total_energy) : 0.0f;
        assert(energy_ratio > 0.99f);

        printf("    -> %s: Target MixBin %d = %3u (Energy: %.2f%%) | Others <= 1 [PASS]\n",
               satellite_positions[p].name, target, vctx->mixbin_gain[target], energy_ratio * 100.0f);
    }

    /*
     * ------------------------------------------------------------------------
     * Test 2: Validate LFE Subwoofer Channel Isolation
     * ------------------------------------------------------------------------
     * AL_XBOX_LFE_GAIN = 1.0f -> MixBin 5 = 255
     * Dynamic muting / modulation to 0.0f -> MixBin 5 = 0
     * ------------------------------------------------------------------------
     */
    printf("[2] Testing LFE Subwoofer Channel Isolation & Dynamic Gain...\n");

    /* Set maximum LFE gain */
    alSourcef(source, AL_XBOX_LFE_GAIN, 1.0f);
    assert(alGetError() == AL_NO_ERROR);
    assert(vctx->mixbin_gain[5] == 255);
    printf("    -> AL_XBOX_LFE_GAIN = 1.0f -> Voice Context MixBin 5 = %u [PASS]\n", vctx->mixbin_gain[5]);

    /* Intermediate LFE gain */
    alSourcef(source, AL_XBOX_LFE_GAIN, 0.5f);
    assert(alGetError() == AL_NO_ERROR);
    assert(vctx->mixbin_gain[5] == 128);
    printf("    -> AL_XBOX_LFE_GAIN = 0.5f -> Voice Context MixBin 5 = %u [PASS]\n", vctx->mixbin_gain[5]);

    /* Mute LFE channel */
    alSourcef(source, AL_XBOX_LFE_GAIN, 0.0f);
    assert(alGetError() == AL_NO_ERROR);
    assert(vctx->mixbin_gain[5] == 0);
    printf("    -> AL_XBOX_LFE_GAIN = 0.0f -> Voice Context MixBin 5 = %u [PASS]\n", vctx->mixbin_gain[5]);

    /* Restore LFE gain to 1.0f */
    alSourcef(source, AL_XBOX_LFE_GAIN, 1.0f);
    assert(vctx->mixbin_gain[5] == 255);

    /*
     * ------------------------------------------------------------------------
     * Test 3: Validate Voice Context Hardware Context Memory
     * ------------------------------------------------------------------------
     * - Check struct size strictly 128 bytes.
     * - Check mixbin_routing_mask = 0x3F (enables MixBins 0..5).
     * - Check mixbin_gain[0..5] values directly in struct.
     * - Check unused MixBins 6..15 are strictly zeroed.
     * ------------------------------------------------------------------------
     */
    printf("[3] Testing Voice Context Hardware Context Memory Layout & Mask...\n");

    /* Verify struct memory layout alignments */
    assert(sizeof(NVAPU_VOICE_CONTEXT_3D) == 128);
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, mixbin_routing_mask) == 96);
    assert(offsetof(NVAPU_VOICE_CONTEXT_3D, mixbin_gain) == 112);

    /* Verify active 3D voice context routing mask */
    assert(vctx->mixbin_routing_mask == 0x0000003Fu);

    /* Place source back at Front Left and verify Voice Context struct directly */
    alSource3f(source, AL_POSITION, -2.887f, 0.0f, -5.0f);
    alSourcef(source, AL_XBOX_LFE_GAIN, 1.0f);

    assert(vctx->mixbin_gain[0] >= 254); /* FL */
    assert(vctx->mixbin_gain[1] <= 1);   /* FR */
    assert(vctx->mixbin_gain[2] <= 1);   /* SL */
    assert(vctx->mixbin_gain[3] <= 1);   /* SR */
    assert(vctx->mixbin_gain[4] <= 1);   /* C  */
    assert(vctx->mixbin_gain[5] == 255); /* LFE */

    /* Verify unused MixBins 6..15 are strictly zero */
    for (int k = 6; k < 16; ++k) {
        assert(vctx->mixbin_gain[k] == 0);
    }
    printf("    -> Context Size: %u B, Mask: 0x%08X, MixBins 6..15: 0 [PASS]\n",
           (unsigned int)sizeof(NVAPU_VOICE_CONTEXT_3D),
           vctx->mixbin_routing_mask);

    /*
     * ------------------------------------------------------------------------
     * Test 4: Validate Dolby Digital DSE Bitstream Activation on Optical Pack
     * ------------------------------------------------------------------------
     * - Mock HDTV Pack + XC_AUDIO_FLAGS_ENABLE_AC3 -> DOLBY_DIGITAL_ACTIVE returns 1.
     * - Mock Standard Pack + AC3 disabled -> returns 0.
     * - Optical packs (HDTV, S-Video, SCART) with AC3 enabled activate DSE.
     * - Non-optical packs or AC3 disabled remain in Stereo 2.0.
     * ------------------------------------------------------------------------
     */
    printf("[4] Testing Dolby Digital DSE Bitstream Activation & Detection...\n");

    /* Current active 5.1 context: Mock HDTV Pack + XC_AUDIO_FLAGS_ENABLE_AC3 */
    ALint dse_val = -1;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &dse_val);
    assert(alGetError() == AL_NO_ERROR);
    assert(dse_val == 1);
    assert(apu_is_dolby_digital_active(alc_get_apu_base()));
    printf("    -> Mock HDTV Pack + AC3 Enabled: DSE Active = %d [PASS]\n", dse_val);

    /* Verify optical pack detection for S-Video and SCART */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_SVIDEO);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_SCART);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_SURROUND_51);
    printf("    -> S-Video and SCART packs with AC3 resolve to APU_TOPOLOGY_SURROUND_51 [PASS]\n");

    /* Teardown 5.1 device and context before testing stereo lifecycle */
    alSourceStop(source);
    alDeleteSources(1, &source);
    alDeleteBuffers(1, &buffer);
    alcMakeContextCurrent(NULL);
    alcDestroyContext(ctx);
    alcCloseDevice(device);

    /* Case B: Standard Composite Pack + AC3 disabled -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_STANDARD);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);

    ALCdevice *dev_stereo = alcOpenDevice(NULL);
    assert(dev_stereo != NULL);
    assert(dev_stereo->topology == APU_TOPOLOGY_STEREO_20);

    dse_val = -1;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &dse_val);
    assert(alGetError() == AL_NO_ERROR);
    assert(dse_val == 0);
    assert(!apu_is_dolby_digital_active(alc_get_apu_base()));
    printf("    -> Mock Standard Pack + AC3 Disabled: DSE Active = %d [PASS]\n", dse_val);
    assert(alcCloseDevice(dev_stereo) == ALC_TRUE);

    /* Case C: HDTV Pack connected, but user disabled AC3 in dashboard -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_STEREO, APU_AV_PACK_HDTV);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);
    dev_stereo = alcOpenDevice(NULL);
    assert(dev_stereo != NULL);
    assert(dev_stereo->topology == APU_TOPOLOGY_STEREO_20);
    dse_val = -1;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &dse_val);
    assert(dse_val == 0);
    assert(!apu_is_dolby_digital_active(alc_get_apu_base()));
    printf("    -> Mock HDTV Pack + AC3 Disabled: DSE Active = %d [PASS]\n", dse_val);
    assert(alcCloseDevice(dev_stereo) == ALC_TRUE);

    /* Case D: Standard Composite Pack connected, but user enabled AC3 -> Stereo 2.0 */
    apu_eeprom_set_mock(true, XC_AUDIO_FLAGS_ENABLE_AC3, APU_AV_PACK_STANDARD);
    assert(apu_detect_audio_topology() == APU_TOPOLOGY_STEREO_20);
    dev_stereo = alcOpenDevice(NULL);
    assert(dev_stereo != NULL);
    assert(dev_stereo->topology == APU_TOPOLOGY_STEREO_20);
    dse_val = -1;
    alXboxGetHardwareStatus(AL_XBOX_DOLBY_DIGITAL_ACTIVE, &dse_val);
    assert(dse_val == 0);
    assert(!apu_is_dolby_digital_active(alc_get_apu_base()));
    printf("    -> Mock Standard Pack + AC3 Enabled (No Optical Port): DSE Active = %d [PASS]\n", dse_val);
    assert(alcCloseDevice(dev_stereo) == ALC_TRUE);

    /*
     * ------------------------------------------------------------------------
     * Final Cleanup
     * ------------------------------------------------------------------------
     */
    apu_eeprom_clear_mock();
    free(mock_mmio);

    printf("=== All Multichannel 5.1 Surround Validation Tests Passed Successfully! ===\n");
    return 0;
}
